#pragma once

#include "Settings/Settings.h"
#include "Tray.h"

namespace fastlook::daemon
{
    // True (and logged) when the user wants the warning and PowerToys Peek is enabled on our activation key.
    // Reads PowerToys settings only.
    bool DetectPeekConflict(core::Settings const& settings);
    // Shows the Peek balloon; the tray icon must already be added.
    void ShowPeekNotice(Tray& tray);
}
