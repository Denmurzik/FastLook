#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>

#include "Ipc/Protocol.h"

namespace fastlook::viewer
{
    struct ShellInfo
    {
        std::wstring displayName, typeName;
        std::optional<uint64_t> size;
        std::optional<FILETIME> created, modified;
        DWORD attributes = 0;
    };

    // Background thread. Virtual items are resolved through their parsing name. A cloud placeholder (decision 56) only gets
    // GetFileAttributesEx data plus the type name from SHGetFileInfo(SHGFI_USEFILEATTRIBUTES); no property store, no content.
    ShellInfo QueryShellInfo(core::ipc::Item const& item, bool cloudPlaceholder);

    // Call on the UI thread (the result is a XAML object); completes on it. IShellItemImageFactory. cacheOnly asks for
    // SIIGBF_INCACHEONLY and on a miss falls back to SIIGBF_ICONONLY, which never reads the content (decision 56: a cloud
    // placeholder is never hydrated). Null when the shell has no image.
    winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Media::ImageSource> LoadThumbnailAsync(
        core::ipc::Item item, int sizePx, bool cacheOnly);

    // User-locale formatting.
    std::wstring FormatBytes(uint64_t bytes);
    std::wstring FormatDate(FILETIME time);
    // "R H S A": the letters of the attributes that are set; empty when none.
    std::wstring FormatAttributes(DWORD attributes);
    // Replaces {0}, {1}, ... with the arguments.
    std::wstring FormatPattern(std::wstring_view pattern, std::initializer_list<std::wstring> args);
}
