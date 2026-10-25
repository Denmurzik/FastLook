#include <gtest/gtest.h>

#include <ILexer.h>
#include <Lexilla.h>

#include <string>

#include "FileTypes/Extensions.h"

using namespace fastlook::core;

TEST(LexerMap, AllMappedLexersExistInLexilla)
{
    for (auto ext : MappedTextExtensions())
    {
        auto name = LexerNameFor(std::wstring(L"f.") + std::wstring(ext));
        if (name.empty())
            continue;
        auto* lx = CreateLexer(std::string(name).c_str());
        EXPECT_NE(lx, nullptr) << std::string(name);
        if (lx)
            lx->Release();
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
