#include "DepotManifestSync.h"

#include "Utils/Logging/Log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace DepotManifestSync {

namespace {

    bool EndsWithInsensitive(std::string_view text, std::string_view suffix) {
        if (text.size() < suffix.size()) return false;
        return std::equal(suffix.begin(), suffix.end(), text.end() - suffix.size(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    }

    // "<digits>_<digits>.manifest" — the only name Steam looks up in depotcache.
    bool IsDepotManifestName(const std::string& name) {
        constexpr std::string_view kExt = ".manifest";
        if (!EndsWithInsensitive(name, kExt)) return false;
        const std::string_view stem(name.data(), name.size() - kExt.size());
        const size_t sep = stem.find('_');
        if (sep == std::string_view::npos || sep == 0 || sep + 1 >= stem.size()) return false;
        auto allDigits = [](std::string_view s) {
            return std::all_of(s.begin(), s.end(),
                               [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        };
        return allDigits(stem.substr(0, sep)) && allDigits(stem.substr(sep + 1));
    }

} // namespace

bool IsManifestFile(const std::string& path) {
    return EndsWithInsensitive(path, ".manifest");
}

bool InstallFile(const std::string& manifestPath, const std::string& steamInstallPath) {
    if (steamInstallPath.empty()) return false;

    const fs::path src(manifestPath);
    const std::string name = src.filename().string();
    if (!IsDepotManifestName(name)) {
        LOG_MANIFEST_DEBUG("DepotManifestSync: skipping {} (not <depot>_<gid>.manifest)", manifestPath);
        return false;
    }

    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) return false;
    const auto srcSize = fs::file_size(src, ec);
    if (ec || srcSize == 0) return false;

    const fs::path cacheDir = fs::path(steamInstallPath) / "depotcache";
    const fs::path dst = cacheDir / name;

    // Same name and size: already installed. Manifests are content-addressed
    // by their gid, so a different file under the same name is not expected.
    if (fs::exists(dst, ec) && !ec) {
        const auto dstSize = fs::file_size(dst, ec);
        if (!ec && dstSize == srcSize) return false;
    }

    fs::create_directories(cacheDir, ec);

    // Copy to a temp name first so Steam never reads a half-written manifest.
    fs::path tmp = dst;
    tmp += ".ost-tmp";
    ec.clear();
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        LOG_MANIFEST_WARN("DepotManifestSync: copy {} -> {} failed ({})",
                          manifestPath, tmp.string(), ec.message());
        return false;
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        LOG_MANIFEST_WARN("DepotManifestSync: rename {} -> {} failed ({})",
                          tmp.string(), dst.string(), ec.message());
        std::error_code cleanup;
        fs::remove(tmp, cleanup);
        return false;
    }

    LOG_MANIFEST_INFO("DepotManifestSync: installed {} into depotcache", name);
    return true;
}

int InstallFromDirectory(const std::string& directory, const std::string& steamInstallPath) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return 0;

    int copied = 0;
    fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        const std::string path = it->path().string();
        if (IsManifestFile(path) && InstallFile(path, steamInstallPath))
            ++copied;
    }
    if (copied > 0)
        LOG_MANIFEST_INFO("DepotManifestSync: installed {} manifest(s) from {}", copied, directory);
    return copied;
}

} // namespace DepotManifestSync
