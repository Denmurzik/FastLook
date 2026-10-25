#include "pch.h"
#include "Previewers/PreviewerRegistry.h"

#include "Previewers/Factories.h"

namespace fastlook::viewer
{
    namespace
    {
        using core::PreviewerKind;
    }

    std::shared_ptr<IPreviewer> CreatePreviewer(PreviewerKind kind)
    {
        switch (kind)
        {
        case PreviewerKind::Image:
            return CreateImagePreviewer();
        case PreviewerKind::Text:
            return CreateTextPreviewer();
        case PreviewerKind::Markdown:
            return CreateMarkdownPreviewer();
        case PreviewerKind::Media:
            return CreateMediaPreviewer();
        case PreviewerKind::Pdf:
            return CreatePdfPreviewer();
        case PreviewerKind::Archive:
            return CreateArchivePreviewer();
        case PreviewerKind::Folder:
            return CreateFolderPreviewer();
        case PreviewerKind::Shell:
            return CreateShellPreviewer();
        case PreviewerKind::Fallback:
            break;
        }
        return CreateFallbackPreviewer();
    }
}
