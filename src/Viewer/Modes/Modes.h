#pragma once

#include "Cli/ViewerArgs.h"
#include "Ipc/Protocol.h"

namespace fastlook::viewer
{
    // One per process mode. Each runs on the UI thread from App::OnLaunched.
    void RunPreviewMode(core::ViewerArgs const& args);
    void RunServerMode(core::ViewerArgs const& args);
    void RunSettingsMode(core::ViewerArgs const& args);
    void RunBenchMode(core::ViewerArgs const& args);

    // The files of `--preview` as a Selection-mode show request (paths starting with "::" or "shell:" are virtual items).
    core::ipc::ShowPayload PayloadFromArgs(core::ViewerArgs const& args);
}
