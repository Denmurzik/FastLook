#include "pch.h"
#include "Previewers/Factories.h"
#include "Previewers/Placeholder.h"

namespace fastlook::viewer
{
    // Stub: replaced by the real previewer in a later task.
    std::shared_ptr<IPreviewer> CreateArchivePreviewer()
    {
        return MakePlaceholderPreviewer(L"Archive");
    }
}
