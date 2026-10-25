#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "FileTypes/Classify.h"

namespace fastlook::core
{
    // ext: ".png" or "png". AssocGetPerceivedType; Unknown on failure.
    PerceivedKind GetPerceivedKind(std::wstring_view ext);
    // Enumerates the WIC decoders' extensions once (cached). The caller has COM initialised.
    bool HasWicDecoder(std::wstring_view ext);
    // Registered IPreviewHandler CLSID for the extension (AssocQueryStringW ASSOCSTR_SHELLEXTENSION).
    std::optional<std::wstring> FindPreviewHandlerClsid(std::wstring_view ext);
    // Default value of InprocServer32 or LocalServer32 (expanded, command line reduced to the exe); empty if absent.
    std::wstring HandlerServerPath(std::wstring_view clsid);
    // Handlers that bring in a browser engine or live in PowerToys/Edge are never hosted.
    bool IsBlockedPreviewHandler(std::wstring_view clsid, std::wstring_view serverPath);
}
