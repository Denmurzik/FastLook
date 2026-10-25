#pragma once

#include <memory>
#include <string>

#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    // Shows "<label>: <file name>" and, when ctx->fallback is set, its message below. Used by the previewer stubs and as the
    // last resort when even the Fallback previewer fails. Test hook: FASTLOOK_TEST_PREPARE_DELAY_MS makes PrepareAsync
    // sleep that long off the UI thread (cancellable).
    std::shared_ptr<IPreviewer> MakePlaceholderPreviewer(std::wstring label);
}
