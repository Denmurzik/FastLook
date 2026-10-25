#pragma once

#include <string_view>

namespace Scintilla
{
    class ILexer5;
}

namespace fastlook::core
{
    // Installs the keyword lists Lexilla does not ship (it only knows the grammar). Lexers without an entry keep their defaults.
    void ApplyKeywords(Scintilla::ILexer5* lexer, std::string_view lexerName);
}
