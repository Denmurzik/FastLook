#pragma once

#include <string>
#include <string_view>

namespace fastlook::core
{
    // Invalid UTF-8 input decodes to U+FFFD.
    std::string ToUtf8(std::wstring_view text);
    std::wstring FromUtf8(std::string_view text);
}
