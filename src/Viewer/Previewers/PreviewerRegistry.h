#pragma once

#include <memory>
#include <optional>

#include "FileTypes/Classify.h"
#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    std::shared_ptr<IPreviewer> CreatePreviewer(core::PreviewerKind kind);
}
