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

    // Call on the UI thread (the result is a XAML object); completes on it. Default: IShellItemImageFactory (thumbnail, else
    // icon). iconOnly (required for cloud placeholders, decision 56) never goes through the factory, whose icon handlers may
    // open the file: the icon comes from SHGetFileInfo(SHGFI_USEFILEATTRIBUTES) and the system image list, i.e. from the name
    // and attributes alone. Null when there is no image.
    winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Media::ImageSource> LoadThumbnailAsync(
        core::ipc::Item item, int sizePx, bool iconOnly);

    // User-locale formatting.
    std::wstring FormatBytes(uint64_t bytes);
    // Integer with the user locale's digit grouping.
    std::wstring FormatNumber(uint64_t value);
    std::wstring FormatDate(FILETIME time);
    // "R H S A": the letters of the attributes that are set; empty when none.
    std::wstring FormatAttributes(DWORD attributes);
    // Replaces {0}, {1}, ... with the arguments.
    std::wstring FormatPattern(std::wstring_view pattern, std::initializer_list<std::wstring> args);
}
