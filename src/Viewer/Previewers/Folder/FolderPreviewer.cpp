#include "pch.h"
#include "Previewers/Factories.h"
#include "Previewers/Placeholder.h"

namespace fastlook::viewer
{
    // Stub: replaced by the real previewer in a later task.
    std::shared_ptr<IPreviewer> CreateFolderPreviewer()
    {
        return MakePlaceholderPreviewer(L"Folder");
    }
}
