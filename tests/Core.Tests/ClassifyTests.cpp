#include <gtest/gtest.h>

#include <vector>

#include "FileTypes/Classify.h"

using namespace fastlook::core;

namespace
{
    PreviewerKind K(std::wstring_view n, ClassifyInput in = {})
    {
        in.fileName = n;
        return ClassifyPreview(in);
    }
}

TEST(Classify, ImagesByExtensionCaseInsensitive)
{
    for (auto n : {L"a.jpg", L"PHOTO.JPG", L"x.heic", L"x.avif", L"x.dds", L"x.svg"})
        EXPECT_EQ(K(n), PreviewerKind::Image) << n;
}

TEST(Classify, MarkdownPdfArchiveMedia)
{
    EXPECT_EQ(K(L"r.md"), PreviewerKind::Markdown);
    EXPECT_EQ(K(L"d.pdf"), PreviewerKind::Pdf);
    EXPECT_EQ(K(L"a.tar.gz"), PreviewerKind::Archive);
    EXPECT_EQ(K(L"x.iso"), PreviewerKind::Archive);
    EXPECT_EQ(K(L"v.mkv"), PreviewerKind::Media);
    EXPECT_EQ(K(L"s.opus"), PreviewerKind::Media);
}

TEST(Classify, CodeAndSpecialNames)
{
    for (auto n : {L"a.cpp", L"a.ps1", L"Makefile", L"Dockerfile", L"CMakeLists.txt", L"x.log", L".gitignore"})
        EXPECT_EQ(K(n), PreviewerKind::Text) << n;
}

TEST(Classify, DirectoryVirtualPlaceholder)
{
    EXPECT_EQ(K(L"dir", {.isDirectory = true}), PreviewerKind::Folder);
    EXPECT_EQ(K(L"This PC", {.isVirtual = true}), PreviewerKind::Fallback);
}

TEST(Classify, CloudPlaceholderIsFallback)
{
    EXPECT_EQ(K(L"notes.txt", {.isCloudPlaceholder = true}), PreviewerKind::Fallback);
    EXPECT_EQ(K(L"dir", {.isDirectory = true, .isCloudPlaceholder = true}), PreviewerKind::Fallback);
}

TEST(Classify, PerceivedAndWicForUnknownExtension)
{
    EXPECT_EQ(K(L"a.xyz", {.perceived = PerceivedKind::Image}), PreviewerKind::Image);
    EXPECT_EQ(K(L"a.xyz", {.hasWicDecoder = true}), PreviewerKind::Image);
    EXPECT_EQ(K(L"a.xyz", {.perceived = PerceivedKind::Video}), PreviewerKind::Media);
    EXPECT_EQ(K(L"a.xyz", {.perceived = PerceivedKind::Audio}), PreviewerKind::Media);
    EXPECT_EQ(K(L"a.xyz", {.perceived = PerceivedKind::Text}), PreviewerKind::Text);
}

TEST(Classify, SniffThenShellThenFallback)
{
    std::vector<uint8_t> text{'h', 'i', '\n'}, bin{'P', 'K', 3, 4, 0, 0};
    EXPECT_EQ(K(L"a.nfo2", {.head = text}), PreviewerKind::Text);
    EXPECT_EQ(K(L"a.docx", {.head = bin, .hasShellHandler = true}), PreviewerKind::Shell);
    EXPECT_EQ(K(L"a.docx", {.head = bin, .hasShellHandler = true, .useShellHandlers = false}), PreviewerKind::Fallback);
    EXPECT_EQ(K(L"a.docx", {.head = bin}), PreviewerKind::Fallback);
}

TEST(Classify, BinaryTxtGoesToShellOrFallback)
{
    std::vector<uint8_t> text{'h', 'i', '\n'}, bin{'M', 'Z', 0, 0, 1, 2};
    EXPECT_EQ(K(L"x.txt", {.head = bin}), PreviewerKind::Fallback);
    EXPECT_EQ(K(L"x.txt", {.head = bin, .hasShellHandler = true}), PreviewerKind::Shell);
    EXPECT_EQ(K(L"x.txt", {.head = bin, .hasShellHandler = true, .useShellHandlers = false}), PreviewerKind::Fallback);
    EXPECT_EQ(K(L"x.md", {.head = bin}), PreviewerKind::Fallback);
    EXPECT_EQ(K(L"x.txt", {.head = text}), PreviewerKind::Text);
    EXPECT_EQ(K(L"x.cpp"), PreviewerKind::Text);
}

TEST(Classify, TsIsTextWhenHeadIsTextOtherwiseMedia)
{
    std::vector<uint8_t> text{'l', 'e', 't', ' ', 'x', '\n'}, ts{0x47, 0x40, 0x00, 0x10, 0x00};
    EXPECT_EQ(K(L"a.ts", {.head = text}), PreviewerKind::Text);
    EXPECT_EQ(K(L"a.ts", {.head = ts}), PreviewerKind::Media);
    EXPECT_EQ(K(L"a.ts"), PreviewerKind::Media);
}

TEST(Classify, ExtensionlessAndTrailingDotUseSniff)
{
    std::vector<uint8_t> text{'a', 'b'};
    EXPECT_EQ(K(L"README", {.head = text}), PreviewerKind::Text);
    EXPECT_EQ(K(L"weird.", {.head = text}), PreviewerKind::Text);
}
