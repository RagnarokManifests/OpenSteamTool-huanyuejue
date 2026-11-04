#include "ReleaseUpdater.h"

#include "OpenSteamToolBuildInfo.h"
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
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
        std::vector<Asset> assets;  // DLLs to install, with their SHA-256
        // Release zip carrying those DLLs. When set, the zip is downloaded and
        // unpacked instead of fetching each DLL as its own release asset.
        std::string zipName;
        std::string zipSha256;
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
            m.zipName = tbl["zip"].value_or(std::string());
            m.zipSha256 = Lower(tbl["zip_sha256"].value_or(std::string()));
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
        // A zip name must be a plain file name with a full SHA-256.
        if (!m.zipName.empty() &&
            (m.zipSha256.size() != 64 || m.zipName.find_first_of("/\\") != std::string::npos)) {
            LOG_WARN("ReleaseUpdater: update.toml zip entry is invalid");
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

    fs::path WorkDir(const std::string& steamDir) {
        return fs::path(steamDir) / "opensteamtool" / "update-work";
    }

    // Leftovers of a previous update: old images (free once Steam restarted),
    // half-downloaded files and the zip work folder.
    void Cleanup(const std::string& steamDir) {
        {
            std::error_code ec;
            if (fs::exists(WorkDir(steamDir), ec))
                fs::remove_all(WorkDir(steamDir), ec);
        }
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

    bool ValidDll(const fs::path& path) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec || size < kMinDllBytes || size > kMaxDllBytes) return false;
        char mz[2] = {};
        std::ifstream in(path, std::ios::binary);
        return in.read(mz, 2) && mz[0] == 'M' && mz[1] == 'Z';
    }

    // Run Windows' own tar.exe (bundled since Windows 10 1803), which also
    // reads zip archives, to unpack `zip` into `dir`.
    bool Extract(const fs::path& zip, const fs::path& dir) {
        wchar_t sysDir[MAX_PATH] = {};
        if (!GetSystemDirectoryW(sysDir, MAX_PATH)) return false;
        const fs::path tar = fs::path(sysDir) / L"tar.exe";
        std::error_code ec;
        if (!fs::exists(tar, ec)) {
            LOG_WARN("ReleaseUpdater: {} not found, cannot unpack the update", tar.string());
            return false;
        }
        std::wstring cmd = L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() +
                           L"\" -C \"" + dir.wstring() + L"\"";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            LOG_WARN("ReleaseUpdater: tar.exe failed to start (error={})", GetLastError());
            return false;
        }
        const DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code != 0) LOG_WARN("ReleaseUpdater: tar.exe exited with {}", code);
        return code == 0;
    }

    // Download the release zip, check it against zip_sha256, unpack it and
    // stage every DLL listed in [files] as "<target>.ost-new" after checking
    // each one's own SHA-256.
    bool StageFromZip(const std::string& steamDir, const Manifest& m) {
        const fs::path work = WorkDir(steamDir);
        std::error_code ec;
        fs::remove_all(work, ec);
        ec.clear();
        fs::create_directories(work / "files", ec);
        if (ec) {
            LOG_WARN("ReleaseUpdater: cannot create {} ({})", work.string(), ec.message());
            return false;
        }

        const std::string url = ReleaseUrl("download/" + m.version + "/" + m.zipName);
        constexpr size_t kMaxZipBytes = 64 * 1024 * 1024;
        auto res = OSTPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, kHeaders,
                                              5000, 5000, 15000, 120000, kMaxZipBytes + 1);
        if (!res.ok || res.status != 200 || res.body.size() < 4 || res.body.size() > kMaxZipBytes ||
            res.body.compare(0, 2, "PK") != 0) {
            LOG_WARN("ReleaseUpdater: download {} failed (HTTP {}, {} bytes)",
                     url, res.status, res.body.size());
            return false;
        }

        const fs::path zip = work / "update.zip";
        {
            std::ofstream out(zip, std::ios::binary | std::ios::trunc);
            out.write(res.body.data(), static_cast<std::streamsize>(res.body.size()));
            if (!out) {
                LOG_WARN("ReleaseUpdater: cannot write {}", zip.string());
                return false;
            }
        }
        const std::string zipSha = Lower(OSTPlatform::Hash::Sha256OfFile(zip));
        if (zipSha != m.zipSha256) {
            LOG_WARN("ReleaseUpdater: SHA-256 mismatch for {} (expected {}, got {})",
                     m.zipName, m.zipSha256, zipSha);
            return false;
        }
        if (!Extract(zip, work / "files")) return false;

        for (const auto& asset : m.assets) {
            const fs::path unpacked = work / "files" / asset.name;
            if (!ValidDll(unpacked)) {
                LOG_WARN("ReleaseUpdater: {} missing or not a DLL in {}", asset.name, m.zipName);
                return false;
            }
            const std::string actual = Lower(OSTPlatform::Hash::Sha256OfFile(unpacked));
            if (actual != asset.sha256) {
                LOG_WARN("ReleaseUpdater: SHA-256 mismatch for {} in {} (expected {}, got {})",
                         asset.name, m.zipName, asset.sha256, actual);
                return false;
            }
            const fs::path staged = WithSuffix(TargetFor(steamDir, asset.name), kStagedSuffix);
            if (!MoveFileExW(unpacked.wstring().c_str(), staged.wstring().c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                LOG_WARN("ReleaseUpdater: cannot stage {} (error={})", staged.string(), GetLastError());
                return false;
            }
        }
        fs::remove_all(work, ec);
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

    // Base64 of the UTF-16LE bytes, the form powershell -EncodedCommand takes.
    std::wstring Base64Utf16(const std::wstring& text) {
        static constexpr wchar_t kAlphabet[] =
            L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const auto* b = reinterpret_cast<const unsigned char*>(text.data());
        const size_t n = text.size() * sizeof(wchar_t);
        std::wstring out;
        out.reserve((n + 2) / 3 * 4);
        for (size_t i = 0; i < n; i += 3) {
            const uint32_t v = (uint32_t{b[i]} << 16) |
                               (i + 1 < n ? uint32_t{b[i + 1]} << 8 : 0) |
                               (i + 2 < n ? uint32_t{b[i + 2]} : 0);
            out += kAlphabet[(v >> 18) & 63];
            out += kAlphabet[(v >> 12) & 63];
            out += i + 1 < n ? kAlphabet[(v >> 6) & 63] : L'=';
            out += i + 2 < n ? kAlphabet[v & 63] : L'=';
        }
        return out;
    }

    // Restart Steam so a staged update loads now.
    //
    // Steam runs inside a Windows job object, and every process it starts
    // lands in that job and dies with it the moment Steam exits, before it can
    // start Steam again; leaving the job (CREATE_BREAKAWAY_FROM_JOB) is not
    // allowed. So the restart runs from a script that WMI starts: processes
    // created through Win32_Process.Create belong to the WMI service, outside
    // Steam's job. The short-lived PowerShell started here only asks WMI to
    // run the script and exits. The script logs to ost-restart.log.
    void RestartSteam(const std::string& steamDir) {
        const fs::path steamExe = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
        if (steamExe.empty()) {
            LOG_WARN("ReleaseUpdater: steam.exe path unknown; restart Steam manually");
            return;
        }
        const fs::path dir = fs::path(steamDir) / "opensteamtool";
        const fs::path script = dir / "ost-restart.ps1";
        const fs::path log = dir / "ost-restart.log";
        std::error_code ec;
        fs::create_directories(dir, ec);

        // Single-quoted PowerShell literals escape ' by doubling it.
        auto quote = [](std::wstring v) {
            for (size_t i = v.find(L'\''); i != std::wstring::npos; i = v.find(L'\'', i + 2))
                v.insert(i, 1, L'\'');
            return L"'" + v + L"'";
        };

        {
            const std::wstring text =
                L"$exe = " + quote(steamExe.wstring()) + L"\n"
                L"$log = " + quote(log.wstring()) + L"\n"
                L"function Note($m) { Add-Content -Path $log -Value (\"{0:HH:mm:ss} {1}\" -f (Get-Date), $m) }\n"
                L"Note 'restart requested'\n"
                L"& $exe -shutdown\n"
                L"Wait-Process -Name steam -Timeout 90 -ErrorAction SilentlyContinue\n"
                L"if (Get-Process -Name steam -ErrorAction SilentlyContinue) { Note 'steam still running after 90 s, not starting a second copy'; exit 1 }\n"
                L"Start-Sleep -Seconds 2\n"
                L"Start-Process -FilePath $exe\n"
                L"Note 'steam started'\n";
            // UTF-8 with a BOM: Windows PowerShell reads BOM-less scripts in
            // the ANSI code page, which would mangle non-ASCII paths.
            std::ofstream out(script, std::ios::binary | std::ios::trunc);
            out << "\xEF\xBB\xBF" << OSTPlatform::Encoding::WideToUtf8(text);
            if (!out) {
                LOG_WARN("ReleaseUpdater: cannot write {}; restart Steam manually", script.string());
                return;
            }
        }

        // The hand-off goes in as -EncodedCommand: nesting the script path's
        // quotes inside a quoted -Command cut the command line short, and the
        // script never ran. WMI starts the script with its window hidden.
        const std::wstring handOff =
            L"$si = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly "
            L"-Property @{ ShowWindow = [uint16]0 }\n"
            L"$cl = 'powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"' + " +
            quote(script.wstring()) + L" + '\"'\n"
            L"$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create "
            L"-Arguments @{ CommandLine = $cl; ProcessStartupInformation = $si }\n"
            L"exit [int]$r.ReturnValue\n";
        std::wstring cmd =
            L"powershell -NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand " +
            Base64Utf16(handOff);

        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        // No DETACHED_PROCESS: powershell started without a console exits 0
        // without running its -Command.
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            LOG_WARN("ReleaseUpdater: restart helper failed (error={})", GetLastError());
            return;
        }
        // Wait for the hand-off so a failure is visible in the log.
        DWORD code = 1;
        if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0)
            GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code == 0)
            LOG_INFO("ReleaseUpdater: Steam restart scheduled through WMI ({})", script.string());
        else
            LOG_WARN("ReleaseUpdater: restart hand-off exited with {}", code);
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

        bool downloaded = true;
        if (!manifest->zipName.empty()) {
            downloaded = StageFromZip(steamDir, *manifest);
        } else {
            for (const auto& asset : manifest->assets) {
                if (!Download(steamDir, manifest->version, asset)) {
                    downloaded = false;
                    break;
                }
            }
        }
        if (!downloaded) {
            Cleanup(steamDir);
            OSTPlatform::Dialog::ShowWarning(title,
                "No se pudo descargar la actualización " + manifest->version +
                ".\nSe volverá a intentar la próxima vez que abras Steam.");
            return;
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
            RestartSteam(steamDir);
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
