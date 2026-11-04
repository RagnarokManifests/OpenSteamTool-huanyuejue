#include "ReleaseUpdater.h"

#include "OpenSteamToolBuildInfo.h"
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Hash.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Logging/Log.h"

#include <toml++/toml.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef OST_UPDATE_REPO
#define OST_UPDATE_REPO "RagnarokManifests/OpenSteamTool-huanyuejue"
#endif

namespace fs = std::filesystem;

namespace ReleaseUpdater {

namespace {

    constexpr const wchar_t* kHeaders = L"User-Agent: OpenSteamTool-Updater\r\n";
    // Let Steam finish starting before a question box shows up.
    constexpr auto kStartDelay = std::chrono::seconds(20);
    constexpr const char* kStagedSuffix = ".ost-new";
    constexpr const char* kBackupSuffix = ".ost-old";
    constexpr size_t kMinDllBytes = 40 * 1024;
    constexpr size_t kMaxDllBytes = 16 * 1024 * 1024;

    // Assets a release may carry, by published name. version.dll ships
    // disabled; when the user enabled it, the live file is version.dll.
    constexpr const char* kKnownAssets[] = {
        "OpenSteamTool.dll", "dwmapi.dll", "xinput1_4.dll", "version.dll.disabled",
    };

    struct Asset {
        std::string name;    // published asset name
        std::string sha256;  // lower-case hex
    };

    struct Manifest {
        std::string version;
        std::vector<Asset> assets;
    };

    std::string ReleaseUrl(const std::string& path) {
        return std::string("https://github.com/") + OST_UPDATE_REPO + "/releases/" + path;
    }

    std::string Lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // "1.5.4.10" -> {1,5,4,10}. Non-numeric parts end the parse.
    std::vector<uint32_t> ParseVersion(std::string_view v) {
        std::vector<uint32_t> parts;
        uint32_t cur = 0;
        bool any = false;
        for (char c : v) {
            if (std::isdigit(static_cast<unsigned char>(c))) {
                cur = cur * 10 + static_cast<uint32_t>(c - '0');
                any = true;
            } else if (c == '.' && any) {
                parts.push_back(cur);
                cur = 0;
                any = false;
            } else {
                break;
            }
        }
        if (any) parts.push_back(cur);
        return parts;
    }

    bool IsNewer(std::string_view remote, std::string_view local) {
        auto r = ParseVersion(remote);
        auto l = ParseVersion(local);
        if (r.empty()) return false;
        const size_t n = std::max(r.size(), l.size());
        r.resize(n, 0);
        l.resize(n, 0);
        return r > l;
    }

    std::optional<Manifest> FetchManifest() {
        const std::string url = ReleaseUrl("latest/download/update.toml");
        auto res = OSTPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, kHeaders,
                                              5000, 5000, 10000, 15000, 64 * 1024);
        if (!res.ok || res.status != 200 || res.body.empty()) {
            LOG_INFO("ReleaseUpdater: no update.toml at {} (HTTP {})", url, res.status);
            return std::nullopt;
        }

        Manifest m;
        try {
            const toml::table tbl = toml::parse(res.body);
            m.version = tbl["version"].value_or(std::string());
            if (const auto* files = tbl["files"].as_table()) {
                for (const char* name : kKnownAssets) {
                    if (auto sha = (*files)[name].value<std::string>(); sha && sha->size() == 64)
                        m.assets.push_back({ name, Lower(*sha) });
                }
            }
        } catch (const toml::parse_error& e) {
            LOG_WARN("ReleaseUpdater: update.toml parse error: {}", e.description());
            return std::nullopt;
        }
        if (m.version.empty() || m.assets.empty()) {
            LOG_WARN("ReleaseUpdater: update.toml has no version or files");
            return std::nullopt;
        }
        return m;
    }

    fs::path SkipFile(const std::string& steamDir) {
        return fs::path(steamDir) / "opensteamtool" / "update-skip.txt";
    }

    bool IsSkipped(const std::string& steamDir, const std::string& version) {
        std::ifstream in(SkipFile(steamDir));
        std::string skipped;
        return in && std::getline(in, skipped) && skipped == version;
    }

    void RememberSkip(const std::string& steamDir, const std::string& version) {
        std::error_code ec;
        fs::create_directories(SkipFile(steamDir).parent_path(), ec);
        std::ofstream(SkipFile(steamDir), std::ios::trunc) << version << '\n';
    }

    // Where an asset lives in the Steam folder.
    fs::path TargetFor(const std::string& steamDir, const std::string& asset) {
        const fs::path dir(steamDir);
        if (asset == "version.dll.disabled") {
            std::error_code ec;
            if (fs::exists(dir / "version.dll", ec)) return dir / "version.dll";
        }
        return dir / asset;
    }

    fs::path WithSuffix(const fs::path& p, const char* suffix) {
        fs::path out = p;
        out += suffix;
        return out;
    }

    // Leftovers of a previous update: old images (free once Steam restarted)
    // and half-downloaded files.
    void Cleanup(const std::string& steamDir) {
        for (const char* asset : kKnownAssets) {
            const fs::path target = TargetFor(steamDir, asset);
            for (const char* suffix : { kBackupSuffix, kStagedSuffix }) {
                const fs::path p = WithSuffix(target, suffix);
                std::error_code ec;
                if (fs::exists(p, ec) && DeleteFileW(p.wstring().c_str()))
                    LOG_INFO("ReleaseUpdater: removed {}", p.string());
            }
        }
    }

    // Download one asset next to its target as "<name>.ost-new" and verify it.
    bool Download(const std::string& steamDir, const std::string& version, const Asset& asset) {
        const std::string url = ReleaseUrl("download/" + version + "/" + asset.name);
        auto res = OSTPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, kHeaders,
                                              5000, 5000, 15000, 60000, kMaxDllBytes + 1);
        const std::string& body = res.body;
        if (!res.ok || res.status != 200) {
            LOG_WARN("ReleaseUpdater: download {} failed (HTTP {})", url, res.status);
            return false;
        }
        if (body.size() < kMinDllBytes || body.size() > kMaxDllBytes ||
            body[0] != 'M' || body[1] != 'Z') {
            LOG_WARN("ReleaseUpdater: {} is not a DLL ({} bytes)", asset.name, body.size());
            return false;
        }

        const fs::path staged = WithSuffix(TargetFor(steamDir, asset.name), kStagedSuffix);
        {
            std::ofstream out(staged, std::ios::binary | std::ios::trunc);
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!out) {
                LOG_WARN("ReleaseUpdater: cannot write {}", staged.string());
                return false;
            }
        }
        const std::string actual = Lower(OSTPlatform::Hash::Sha256OfFile(staged));
        if (actual != asset.sha256) {
            LOG_WARN("ReleaseUpdater: SHA-256 mismatch for {} (expected {}, got {})",
                     asset.name, asset.sha256, actual);
            std::error_code ec;
            fs::remove(staged, ec);
            return false;
        }
        return true;
    }

    // Swap every staged file into place. A loaded DLL cannot be overwritten,
    // but NTFS lets it be renamed, so the live file moves to ".ost-old" and
    // the new one takes its name; it loads on the next Steam start. All or
    // nothing: on any failure the files already swapped are put back.
    bool Install(const std::string& steamDir, const std::vector<Asset>& assets) {
        std::vector<std::pair<fs::path, bool>> done;  // target, had an old file
        auto rollback = [&] {
            for (auto it = done.rbegin(); it != done.rend(); ++it) {
                const fs::path& target = it->first;
                DeleteFileW(target.wstring().c_str());
                if (it->second)
                    MoveFileExW(WithSuffix(target, kBackupSuffix).wstring().c_str(),
                                target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        };

        for (const auto& asset : assets) {
            const fs::path target = TargetFor(steamDir, asset.name);
            const fs::path staged = WithSuffix(target, kStagedSuffix);
            std::error_code ec;
            const bool hadOld = fs::exists(target, ec);
            if (hadOld && !MoveFileExW(target.wstring().c_str(),
                                       WithSuffix(target, kBackupSuffix).wstring().c_str(),
                                       MOVEFILE_REPLACE_EXISTING)) {
                LOG_WARN("ReleaseUpdater: cannot move {} aside (error={})", target.string(), GetLastError());
                rollback();
                return false;
            }
            if (!MoveFileExW(staged.wstring().c_str(), target.wstring().c_str(),
                             MOVEFILE_REPLACE_EXISTING)) {
                LOG_WARN("ReleaseUpdater: cannot place {} (error={})", target.string(), GetLastError());
                if (hadOld)
                    MoveFileExW(WithSuffix(target, kBackupSuffix).wstring().c_str(),
                                target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
                rollback();
                return false;
            }
            done.push_back({ target, hadOld });
        }
        return true;
    }

    // Detached, hidden PowerShell: graceful -shutdown, wait for Steam to exit,
    // start it again.
    void RestartSteam() {
        const fs::path steamExe = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
        if (steamExe.empty()) return;
        std::wstring exe = steamExe.wstring();
        for (size_t p = exe.find(L'\''); p != std::wstring::npos; p = exe.find(L'\'', p + 2))
            exe.insert(p, 1, L'\'');
        std::wstring cmd =
            L"powershell -NoProfile -WindowStyle Hidden -Command \"& '" + exe + L"' -shutdown; "
            L"Wait-Process -Name steam -Timeout 60 -ErrorAction SilentlyContinue; "
            L"Start-Process '" + exe + L"'\"";

        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        // No DETACHED_PROCESS: powershell started without a console exits 0
        // without running its -Command. Leave Steam's job object when it is
        // allowed, so the helper outlives the Steam it is shutting down.
        const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                 flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
        if (!ok && GetLastError() == ERROR_ACCESS_DENIED)
            ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                flags, nullptr, nullptr, &si, &pi);
        if (ok) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            LOG_INFO("ReleaseUpdater: Steam restart helper launched");
        } else {
            LOG_WARN("ReleaseUpdater: restart helper failed (error={})", GetLastError());
        }
    }

    void Run(const std::string& steamDir) {
        std::this_thread::sleep_for(kStartDelay);
        Cleanup(steamDir);

        const std::string current = OPENSTEAMTOOL_VERSION;
        const auto manifest = FetchManifest();
        if (!manifest) return;
        if (!IsNewer(manifest->version, current)) {
            LOG_INFO("ReleaseUpdater: up to date ({}, latest {})", current, manifest->version);
            return;
        }
        if (IsSkipped(steamDir, manifest->version)) {
            LOG_INFO("ReleaseUpdater: {} available but skipped by the user", manifest->version);
            return;
        }

        const std::string title = "OpenSteamTool";
        if (!OSTPlatform::Dialog::AskYesNo(title,
                "Hay una nueva versión de OpenSteamTool: " + current + " -> " + manifest->version +
                ".\n\n¿Descargar e instalar ahora?\n\n"
                "Se aplica al reiniciar Steam. Si eliges No, no se volverá a preguntar por esta versión.\n\n"
                "Novedades: " + ReleaseUrl("tag/" + manifest->version))) {
            RememberSkip(steamDir, manifest->version);
            LOG_INFO("ReleaseUpdater: user declined {}", manifest->version);
            return;
        }

        for (const auto& asset : manifest->assets) {
            if (!Download(steamDir, manifest->version, asset)) {
                Cleanup(steamDir);
                OSTPlatform::Dialog::ShowWarning(title,
                    "No se pudo descargar la actualización " + manifest->version +
                    ".\nSe volverá a intentar la próxima vez que abras Steam.");
                return;
            }
        }
        if (!Install(steamDir, manifest->assets)) {
            Cleanup(steamDir);
            OSTPlatform::Dialog::ShowWarning(title,
                "No se pudo instalar la actualización " + manifest->version +
                ".\nLos archivos actuales no se tocaron.");
            return;
        }
        LOG_INFO("ReleaseUpdater: staged {} -> {}", current, manifest->version);

        if (OSTPlatform::Dialog::AskYesNo(title,
                "OpenSteamTool " + manifest->version + " quedó instalado.\n\n"
                "¿Reiniciar Steam ahora para aplicarlo?\n"
                "(Si eliges No, se aplica la próxima vez que abras Steam.)"))
            RestartSteam();
    }

} // namespace

void StartAsync(const std::string& steamInstallPath) {
    OSTPlatform::Thread::StartDetached([steamDir = steamInstallPath]() -> uint32_t {
        try {
            Run(steamDir);
        } catch (...) {
            LOG_WARN("ReleaseUpdater: check aborted by an exception");
        }
        return 0;
    });
}

} // namespace ReleaseUpdater
