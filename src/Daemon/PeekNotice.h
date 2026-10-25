#pragma once

#include "Settings/Settings.h"
#include "Tray.h"

namespace fastlook::daemon
{
    // Shows the PowerToys Peek balloon when Peek is enabled on our activation key and the user wants the warning.
    // Returns true when a conflict was detected (the balloon was shown). Reads PowerToys settings only.
    bool ShowPeekNoticeIfConflicting(core::Settings const& settings, Tray& tray);
}
