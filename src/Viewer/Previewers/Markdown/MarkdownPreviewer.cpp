#include "pch.h"
#include "Previewers/Factories.h"
#include "Previewers/Placeholder.h"

namespace fastlook::viewer
{
    // Stub: replaced by the real previewer in a later task.
    std::shared_ptr<IPreviewer> CreateMarkdownPreviewer()
    {
        return MakePlaceholderPreviewer(L"Markdown");
    }
}
