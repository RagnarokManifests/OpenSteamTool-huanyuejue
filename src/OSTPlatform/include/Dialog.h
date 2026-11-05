#pragma once

#include <string>

namespace OSTPlatform::Dialog {

    void ShowWarning(std::string title, std::string message);

    // Yes/No question box (UTF-8 text). Returns true only for "Yes".
    bool AskYesNo(std::string title, std::string message);

} // namespace OSTPlatform::Dialog
