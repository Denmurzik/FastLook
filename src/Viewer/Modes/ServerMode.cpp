#include "pch.h"
#include "Modes/Modes.h"

#include "Diagnostics/Log.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    // Stub: replaced by the server mode task.
    void RunServerMode(core::ViewerArgs const&)
    {
        FL_LOG_INFO(L"Server mode: not implemented");
        ViewerHost::Get().ExitApp();
    }
}
