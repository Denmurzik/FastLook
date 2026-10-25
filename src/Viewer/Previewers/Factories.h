#pragma once

#include <memory>

#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    // Each is defined in Previewers/<Kind>/<Kind>Previewer.cpp.
    std::shared_ptr<IPreviewer> CreateImagePreviewer();
    std::shared_ptr<IPreviewer> CreateTextPreviewer();
    std::shared_ptr<IPreviewer> CreateMarkdownPreviewer();
    std::shared_ptr<IPreviewer> CreateMediaPreviewer();
    std::shared_ptr<IPreviewer> CreatePdfPreviewer();
    std::shared_ptr<IPreviewer> CreateArchivePreviewer();
    std::shared_ptr<IPreviewer> CreateFolderPreviewer();
    std::shared_ptr<IPreviewer> CreateShellPreviewer();
    std::shared_ptr<IPreviewer> CreateFallbackPreviewer();
}
