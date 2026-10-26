#pragma once

#include <memory>
#include <string>

#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    // The last resort when even the Fallback previewer fails: the file name and, below it, ctx->fallback's message or the
    // localized "Can't preview this file". Test hook: FASTLOOK_TEST_PREPARE_DELAY_MS makes PrepareAsync sleep that long off
    // the UI thread (cancellable).
    std::shared_ptr<IPreviewer> MakePlaceholderPreviewer();
}
