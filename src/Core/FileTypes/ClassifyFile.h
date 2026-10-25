#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "FileTypes/Classify.h"

namespace fastlook::core
{
    struct FileClassification
    {
        PreviewerKind kind = PreviewerKind::Fallback;
        std::optional<std::wstring> shellHandlerClsid;
        bool cloudPlaceholder = false;
    };

    // Reads attributes and at most 4 KB of head (never for cloud placeholders); errors give Fallback.
    // Callers pass item.path / item.isVirtual. The caller has COM initialised (WIC lookup).
    FileClassification ClassifyFile(std::wstring_view path, bool isVirtual, bool useShellHandlers);
}
