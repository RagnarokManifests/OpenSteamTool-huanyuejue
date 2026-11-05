#pragma once

#include <string>
#include <vector>

namespace LuaFileWatcher {
    void Start(const std::vector<std::string>& directories);
    void Stop();
    // Process exit: let go of the watcher thread without joining it. The OS
    // has already ended the thread, and a std::thread still joinable when the
    // module's globals are destroyed calls std::terminate (abort).
    void AbandonOnProcessExit();
}
