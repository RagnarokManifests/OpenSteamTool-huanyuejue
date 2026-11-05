#pragma once

#include <string>

// Update notice for builds published on this fork's GitHub Releases.
//
// Every release ships an "update.toml" asset (version + SHA-256 of each DLL)
// next to the raw DLLs. On Steam start a background thread reads
// releases/latest/download/update.toml; when it names a newer version it asks
// the user first, and only on "Yes" downloads the DLLs, checks their SHA-256
// and stages them for the next Steam start (the loaded images are renamed
// aside, never overwritten in place). Nothing is installed without consent,
// and a declined version is not offered again.
namespace ReleaseUpdater {

    // Start the check on its own detached thread. Returns immediately.
    void StartAsync(const std::string& steamInstallPath);

}
