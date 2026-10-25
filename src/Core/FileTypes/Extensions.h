#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "FileTypes/Classify.h"

namespace fastlook::core
{
    // Extension table plus special names; case-insensitive; "a.tar.gz" -> Archive.
    std::optional<PreviewerKind> KindForExtension(std::wstring_view fileName);
    // Lexilla lexer name for a text file; "" = no lexer.
    std::string_view LexerNameFor(std::wstring_view fileName);
    // Extensions (no dot, lower case) of the lexer map.
    std::vector<std::wstring_view> MappedTextExtensions();
}
