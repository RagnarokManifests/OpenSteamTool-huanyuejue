#pragma once

#include <string>

// Depot manifests dropped next to Lua scripts (the SteamTools layout: a game
// folder holding "<appid>.lua" plus "<depotid>_<manifestgid>.manifest" files).
// Steam reads manifests from <Steam>\depotcache, so a manifest found there
// skips the manifest download and its request code. These helpers copy them
// across; nothing is ever deleted from depotcache.
namespace DepotManifestSync {

    // True for "*.manifest" (case-insensitive).
    bool IsManifestFile(const std::string& path);

    // Copy one "<depotid>_<gid>.manifest" into <steamInstallPath>\depotcache.
    // Skips names that do not follow that pattern and files already present
    // with the same size. Returns true when a file was copied.
    bool InstallFile(const std::string& manifestPath, const std::string& steamInstallPath);

    // Recursively install every manifest under `directory`. Returns the number
    // of files copied.
    int InstallFromDirectory(const std::string& directory, const std::string& steamInstallPath);

}
