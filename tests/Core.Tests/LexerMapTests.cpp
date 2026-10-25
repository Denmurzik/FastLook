#include <gtest/gtest.h>

#include <set>
#include <string>

#include "FileTypes/Extensions.h"

using namespace fastlook::core;

// The vcpkg lexilla port ships headers only (no library to call CreateLexer on), so the mapped names are
// checked against the lexer names registered by Lexilla 5.4.6 (Lexilla.h / LexicalStyles.iface, SCLEX_*).
TEST(LexerMap, AllMappedLexersExistInLexilla)
{
    static const std::set<std::string, std::less<>> lexilla{
        "asm", "bash", "batch", "cmake", "cpp", "css", "diff", "fortran", "haskell", "hypertext", "json", "latex",
        "lua", "makefile", "markdown", "pascal", "perl", "phpscript", "powershell", "props", "python", "r", "ruby",
        "rust", "sql", "toml", "vb", "xml", "yaml"};
    for (auto ext : MappedTextExtensions())
    {
        auto name = LexerNameFor(std::wstring(L"f.") + std::wstring(ext));
        if (!name.empty())
            EXPECT_TRUE(lexilla.contains(name)) << std::string(name);
    }
}

TEST(LexerMap, AtLeast40Extensions)
{
    EXPECT_GE(MappedTextExtensions().size(), 40u);
}

TEST(LexerMap, SpotChecks)
{
    EXPECT_EQ(LexerNameFor(L"a.cpp"), "cpp");
    EXPECT_EQ(LexerNameFor(L"A.CS"), "cpp");
    EXPECT_EQ(LexerNameFor(L"a.rs"), "rust");
    EXPECT_EQ(LexerNameFor(L"a.csproj"), "xml");
    EXPECT_EQ(LexerNameFor(L"a.htm"), "hypertext");
    EXPECT_EQ(LexerNameFor(L"a.ps1"), "powershell");
    EXPECT_EQ(LexerNameFor(L"a.reg"), "props");
    EXPECT_EQ(LexerNameFor(L"Makefile"), "makefile");
    EXPECT_EQ(LexerNameFor(L"CMakeLists.txt"), "cmake");
    EXPECT_EQ(LexerNameFor(L"Dockerfile"), "bash");
    EXPECT_EQ(LexerNameFor(L"C:\\src\\a.py"), "python");
    EXPECT_EQ(LexerNameFor(L"x.txt"), "");
    EXPECT_EQ(LexerNameFor(L"x.log"), "");
    EXPECT_EQ(LexerNameFor(L"x.unknown"), "");
}

TEST(LexerMap, KindForExtensionTable)
{
    EXPECT_EQ(KindForExtension(L"x.TAR.GZ"), PreviewerKind::Archive);
    EXPECT_EQ(KindForExtension(L"x.svg"), PreviewerKind::Image);
    EXPECT_EQ(KindForExtension(L"x.md"), PreviewerKind::Markdown);
    EXPECT_EQ(KindForExtension(L"x.tsv"), PreviewerKind::Text);
    EXPECT_EQ(KindForExtension(L"x.editorconfig"), PreviewerKind::Text);
    EXPECT_EQ(KindForExtension(L"makefile"), PreviewerKind::Text);
    EXPECT_FALSE(KindForExtension(L"x.docx"));
    EXPECT_FALSE(KindForExtension(L"README"));
    EXPECT_FALSE(KindForExtension(L"x."));
}
