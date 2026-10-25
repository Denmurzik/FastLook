#include "pch.h"
#include "Modes/Modes.h"

#include "Diagnostics/Log.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    // Stub: replaced by the performance task.
    void RunBenchMode(core::ViewerArgs const&)
    {
        FL_LOG_INFO(L"Bench mode: not implemented");
        ViewerHost::Get().ExitApp();
    }
}
