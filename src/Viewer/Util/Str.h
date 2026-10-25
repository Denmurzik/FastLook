#pragma once

#include <string_view>

#include <winrt/base.h>

namespace fastlook::viewer
{
    // Looks `key` up in the .resw map `map` (file name without extension, e.g. L"Resources", L"Common").
    // A missing key yields L"!" + key and is reported once through OutputDebugString.
    winrt::hstring Str(std::wstring_view map, std::wstring_view key);
}
