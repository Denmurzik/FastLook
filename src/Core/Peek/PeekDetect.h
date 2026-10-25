#pragma once

#include <string_view>

#include "Settings/Settings.h"

namespace fastlook::core
{
    struct PeekStatus
    {
        bool peekEnabled = false;
        bool conflict = false;
    };

    // Never throws. Missing or malformed global file yields {false,false}; missing or malformed Peek file means Peek's default Ctrl+Space.
    PeekStatus DetectPeekConflict(std::string_view globalSettingsJson, std::string_view peekSettingsJson, ActivationKey ours);

    // Reads <dir>\settings.json and <dir>\Peek\settings.json (read-only);
    // dir = env FASTLOOK_POWERTOYS_DIR or %LOCALAPPDATA%\Microsoft\PowerToys.
    PeekStatus DetectPeekConflictOnDisk(ActivationKey ours);
}
