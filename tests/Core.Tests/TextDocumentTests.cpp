#include <Windows.h>

#include <stop_token>

#include "TestUtil.h"
#include "Text/TextDocument.h"

using namespace fastlook::core;

namespace
{
    std::unique_ptr<TextDocument> OpenIndexed(std::filesystem::path const& p)
    {
        auto doc = TextDocument::Open(p);
        if (doc) doc->BuildIndex({}, {});
        return doc;
    }
}

TEST(TextDocument, OpensFixturesWithRightEncoding)
{
    auto cp = OpenIndexed(FixturePath("text/cp1251.txt"));
    ASSERT_TRUE(cp);
    EXPECT_EQ(cp->Encoding().encoding, TextEncoding::Ansi);
    EXPECT_EQ(cp->Encoding().codePage, GetACP());
    if (GetACP() == 1251) EXPECT_EQ(cp->LineText(0), L"Привет");

    auto le = OpenIndexed(FixturePath("text/utf16le-bom.txt"));
    ASSERT_TRUE(le);
    EXPECT_EQ(le->Encoding().encoding, TextEncoding::Utf16LE);
    EXPECT_EQ(le->LineText(0), L"Привет");
    EXPECT_EQ(le->LineCount(), 2u);

    auto be = OpenIndexed(FixturePath("text/utf16be-bom.txt"));
    ASSERT_TRUE(be);
    EXPECT_EQ(be->Encoding().encoding, TextEncoding::Utf16BE);
    EXPECT_EQ(be->LineText(0), L"Привет");

    auto bom = OpenIndexed(FixturePath("text/utf8-bom.txt"));
    ASSERT_TRUE(bom);
    EXPECT_EQ(bom->Encoding().bomLength, 3u);
    EXPECT_EQ(bom->LineText(0), L"hello");
    EXPECT_EQ(bom->LineText(1), L"world");

    auto src = OpenIndexed(FixturePath("text/utf8.cpp"));
    ASSERT_TRUE(src);
    EXPECT_EQ(src->Encoding().encoding, TextEncoding::Utf8);
    EXPECT_EQ(src->LineText(2), L"// Привет, 世界");

    auto crlf = OpenIndexed(FixturePath("text/crlf.txt"));
    ASSERT_TRUE(crlf);
    EXPECT_EQ(crlf->LineCount(), 4u);
    EXPECT_EQ(crlf->LineText(1), L"two");
}

TEST(TextDocument, MissingFileFailsCleanly)
{
    EXPECT_EQ(TextDocument::Open(MakeTempDir() / "nope.txt"), nullptr);
}

TEST(TextDocument, EmptyFile)
{
    auto f = MakeTempDir() / "empty.txt";
    WriteFile(f, "");
    auto doc = TextDocument::Open(f);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->SizeBytes(), 0u);
    EXPECT_TRUE(doc->Content().empty());
    doc->BuildIndex({}, {});
    EXPECT_TRUE(doc->IndexComplete());
    EXPECT_EQ(doc->LineCount(), 1u);
    EXPECT_EQ(doc->LineText(0), L"");
    EXPECT_EQ(doc->LineText(5), L"");
}

TEST(TextDocument, LockedFileFailsCleanly)
{
    auto f = MakeTempDir() / "locked.txt";
    WriteFile(f, "data");
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    std::unique_ptr<TextDocument> doc;
    EXPECT_NO_THROW(doc = TextDocument::Open(f));
    EXPECT_EQ(doc, nullptr);
    CloseHandle(h);
    EXPECT_NE(TextDocument::Open(f), nullptr);
}

TEST(TextDocument, OpenDocumentDoesNotBlockDelete)
{
    auto f = MakeTempDir() / "shared.txt";
    WriteFile(f, "one\ntwo");
    auto doc = OpenIndexed(f);
    ASSERT_TRUE(doc);
    EXPECT_TRUE(std::filesystem::remove(f)); // FILE_SHARE_DELETE
    EXPECT_EQ(doc->LineText(1), L"two");
}

TEST(TextDocument, LongPath)
{
    auto dir = MakeTempDir();
    std::filesystem::path deep = LR"(\\?\)" + dir.wstring();
    while (deep.native().size() < 300)
    {
        deep /= std::wstring(60, L'd');
    }
    ASSERT_TRUE(std::filesystem::create_directories(deep));
    auto f = deep / L"long.txt";
    WriteFile(f, "alpha\nbeta");
    ASSERT_GE(f.native().size(), 300u);
    auto doc = OpenIndexed(f);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->LineText(0), L"alpha");
    EXPECT_EQ(doc->LineText(1), L"beta");

    // Without the prefix the document adds it itself.
    std::filesystem::path plain = f.native().substr(4);
    auto doc2 = OpenIndexed(plain);
    ASSERT_TRUE(doc2);
    EXPECT_EQ(doc2->LineText(1), L"beta");
}

TEST(TextDocument, ExtractRangeAcrossLines)
{
    auto f = MakeTempDir() / "range.txt";
    WriteFile(f, "ab\ncd\nef");
    auto doc = OpenIndexed(f);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->ExtractRange({0, 1}, {2, 1}), L"b\r\ncd\r\ne");
    EXPECT_EQ(doc->ExtractRange({1, 0}, {1, 2}), L"cd");
    EXPECT_EQ(doc->ExtractRange({0, 0}, {99, 0}), L"ab\r\ncd\r\nef");
    EXPECT_EQ(doc->ExtractRange({2, 0}, {0, 0}), L"");
    EXPECT_EQ(doc->ExtractRange({9, 0}, {9, 3}), L"");
}

TEST(TextDocument, ExtractRangeJoinsLongLineSegmentsWithoutBreak)
{
    auto f = MakeTempDir() / "long-line.txt";
    WriteFile(f, std::string(20000, 'z'));
    auto doc = OpenIndexed(f);
    ASSERT_TRUE(doc);
    ASSERT_EQ(doc->LineCount(), 2u);
    EXPECT_EQ(doc->ExtractRange({0, 0}, {1, 100000}).size(), 20000u);
}

TEST(TextDocument, BuildIndexIsChunkedStoppableAndResumable)
{
    auto f = MakeTempDir() / "big.txt";
    std::string row = "0123456789abcdef\n"; // 17 bytes
    std::string data;
    while (data.size() < 9 * 1024 * 1024) data += row;
    WriteFile(f, data);
    auto doc = TextDocument::Open(f);
    ASSERT_TRUE(doc);

    std::stop_source src;
    int calls = 0;
    doc->BuildIndex(src.get_token(), [&](uint64_t, bool) {
        if (++calls == 1) src.request_stop();
    });
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(doc->IndexComplete());

    std::vector<bool> completes;
    doc->BuildIndex({}, [&](uint64_t, bool complete) { completes.push_back(complete); });
    EXPECT_TRUE(doc->IndexComplete());
    ASSERT_EQ(completes.size(), 2u); // 9 MiB+ = 4 + 4 + rest; the first chunk is already done
    EXPECT_TRUE(completes.back());
    EXPECT_EQ(doc->LineCount(), data.size() / row.size() + 1);
    EXPECT_EQ(doc->LineText(doc->LineCount() - 2), L"0123456789abcdef");
}
