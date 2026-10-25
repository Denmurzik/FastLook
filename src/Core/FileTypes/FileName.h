#pragma once

#include <string>
#include <string_view>

namespace fastlook::core
{
    // Last path component of a name or path (accepts both separators).
    std::wstring_view BaseName(std::wstring_view path);
    // Extension without the dot, ASCII-lower-cased; empty when there is none ("README", "x.").
    std::wstring LowerExtension(std::wstring_view fileName);
    // ASCII lower-casing; extensions and special names are ASCII.
    std::wstring LowerAscii(std::wstring_view text);
}
