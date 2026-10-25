#include "pch.h"
#include "Modes/Modes.h"

#include "Diagnostics/Log.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    // Stub: replaced by the settings window task.
    void RunSettingsMode(core::ViewerArgs const&)
    {
        FL_LOG_INFO(L"Settings mode: not implemented");
        ViewerHost::Get().ExitApp();
    }
}
