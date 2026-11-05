#pragma once

#include <string>

namespace ConfigFileWatcher {
    void Start(const std::string& configPath, const std::string& defaultLuaDir);
    void Stop();
    // Process exit: let go of the watcher thread without joining it. The OS
    // has already ended the thread, and a std::thread still joinable when the
    // module's globals are destroyed calls std::terminate (abort).
    void AbandonOnProcessExit();
}
