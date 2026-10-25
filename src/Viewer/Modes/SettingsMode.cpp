#include "pch.h"
#include "Modes/Modes.h"

#include "SettingsLauncher.h"
#include "System/AppPaths.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    void RunSettingsMode(core::ViewerArgs const& args)
    {
        auto& host = ViewerHost::Get();
        host.Initialize(core::CurrentAppPaths(), CloseBehavior::ExitProcess, args);
        ShowSettingsWindow(args.settingsPage, [] { ViewerHost::Get().ExitApp(); });
    }
}
