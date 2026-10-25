#include "TestUtil.h"
#include "Text/Encoding.h"

using namespace fastlook::core;

TEST(Encoding, Boms)
{
    EXPECT_EQ(DetectEncoding(B("\xEF\xBB\xBF" "a"), 1251, false), (EncodingInfo{TextEncoding::Utf8, 65001, 3}));
    EXPECT_EQ(DetectEncoding(B(std::string("\xFF\xFE" "a\0", 4)), 1251, false).encoding, TextEncoding::Utf16LE);
    EXPECT_EQ(DetectEncoding(B(std::string("\xFE\xFF\0a", 4)), 1251, false).encoding, TextEncoding::Utf16BE);
    EXPECT_EQ(DetectEncoding(B(std::string("\xFF\xFE\0\0", 4)), 1251, false).encoding, TextEncoding::Unsupported);
    EXPECT_EQ(DetectEncoding(B(std::string("\0\0\xFE\xFF", 4)), 1251, false).encoding, TextEncoding::Unsupported);
}

TEST(Encoding, Utf8VsAnsi)
{
    EXPECT_EQ(DetectEncoding(B("plain ascii"), 1251, false).encoding, TextEncoding::Utf8);
    EXPECT_EQ(DetectEncoding(B("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"), 1251, false).encoding, TextEncoding::Utf8); // "Привет"
    EXPECT_EQ(DetectEncoding(B("\xCF\xF0\xE8\xE2\xE5\xF2"), 1251, false), (EncodingInfo{TextEncoding::Ansi, 1251, 0}));       // cp1251
    EXPECT_EQ(DetectEncoding(B("\xED\xA0\x80"), 1251, false).encoding, TextEncoding::Ansi);                                   // surrogate
    EXPECT_EQ(DetectEncoding(B("ok \xD0"), 1251, true).encoding, TextEncoding::Utf8);                                         // truncated tail
    EXPECT_EQ(DetectEncoding(B("ok \xD0"), 1251, false).encoding, TextEncoding::Ansi);
}

TEST(Encoding, RejectsInvalidUtf8Sequences)
{
    for (auto bad : {"\xC0\x80", "\xC1\xBF", "\xF5\x80\x80\x80", "\xFF", "\xE0\x80\x80", "\xE0\x9F\xBF", "\xF0\x80\x80\x80",
                     "\xF0\x8F\xBF\xBF", "\xF4\x90\x80\x80", "\x80", "\xD0\x41"})
    {
        EXPECT_EQ(DetectEncoding(B(bad), 1251, false).encoding, TextEncoding::Ansi) << bad;
    }
    // A bad byte inside the cut tail is still invalid for a prefix sample.
    EXPECT_EQ(DetectEncoding(B("a\xE0\x80"), 1251, true).encoding, TextEncoding::Ansi);
    EXPECT_EQ(DetectEncoding(B("a\xF4\x90"), 1251, true).encoding, TextEncoding::Ansi);
    EXPECT_EQ(DetectEncoding(B("a\xF0\x9F\x98"), 1251, true).encoding, TextEncoding::Utf8);
}

TEST(Encoding, AcceptsBoundaryUtf8)
{
    EXPECT_EQ(DetectEncoding(B("\xC2\x80"), 1251, false).encoding, TextEncoding::Utf8);
    EXPECT_EQ(DetectEncoding(B("\xE0\xA0\x80"), 1251, false).encoding, TextEncoding::Utf8);
    EXPECT_EQ(DetectEncoding(B("\xED\x9F\xBF"), 1251, false).encoding, TextEncoding::Utf8);
    EXPECT_EQ(DetectEncoding(B("\xF0\x90\x80\x80"), 1251, false).encoding, TextEncoding::Utf8);
    EXPECT_EQ(DetectEncoding(B("\xF4\x8F\xBF\xBF"), 1251, false).encoding, TextEncoding::Utf8);
    EXPECT_EQ(DetectEncoding({}, 1251, false), (EncodingInfo{TextEncoding::Utf8, 65001, 0}));
}

TEST(Encoding, DecodeCp1251)
{
    EXPECT_EQ(DecodeToUtf16(B("\xCF\xF0\xE8\xE2\xE5\xF2"), {TextEncoding::Ansi, 1251, 0}), L"Привет");
}

TEST(Encoding, DecodeUtf8AndUtf16)
{
    EXPECT_EQ(DecodeToUtf16(B("\xD0\x9F\xD1\x80"), {TextEncoding::Utf8, 65001, 0}), L"Пр");
    EXPECT_EQ(DecodeToUtf16(B("\xFF"), {TextEncoding::Utf8, 65001, 0}), L"�");
    EXPECT_EQ(DecodeToUtf16(B(std::string("a\0\x1F\x04", 4)), {TextEncoding::Utf16LE, 1200, 0}), L"aП");
    EXPECT_EQ(DecodeToUtf16(B(std::string("\0a\x04\x1F", 4)), {TextEncoding::Utf16BE, 1201, 0}), L"aП");
    EXPECT_EQ(DecodeToUtf16(B(std::string("a\0b", 3)), {TextEncoding::Utf16LE, 1200, 0}), L"a�");
    EXPECT_EQ(DecodeToUtf16(B("abc"), {TextEncoding::Unsupported, 0, 4}), L"");
    EXPECT_EQ(DecodeToUtf16({}, {TextEncoding::Ansi, 1251, 0}), L"");
}
