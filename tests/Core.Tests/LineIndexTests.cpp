#include <thread>

#include "TestUtil.h"
#include "Text/LineIndex.h"

using namespace fastlook::core;

namespace
{
    std::string Slice(std::string const& s, LineIndex::LineSpan span)
    {
        return s.substr(static_cast<size_t>(span.begin), static_cast<size_t>(span.end - span.begin));
    }

    std::string U16(std::wstring const& w, bool bigEndian = false)
    {
        std::string out;
        for (wchar_t c : w)
        {
            char lo = static_cast<char>(c & 0xFF), hi = static_cast<char>((c >> 8) & 0xFF);
            out += bigEndian ? hi : lo;
            out += bigEndian ? lo : hi;
        }
        return out;
    }
}

TEST(LineIndex, MixedEolAndTrailingNewline)
{
    LineIndex ix({});
    auto c = B("a\nb\r\nc\rd");
    ix.Append(c, true);
    EXPECT_EQ(ix.LineCount(), 4u);
    EXPECT_TRUE(ix.Complete());
    auto l1 = ix.Line(1, c);
    EXPECT_EQ(l1.begin, 2u);
    EXPECT_EQ(l1.end, 3u);
    auto l3 = ix.Line(3, c);
    EXPECT_EQ(l3.begin, 7u);
    EXPECT_EQ(l3.end, 8u);
    LineIndex t({});
    t.Append(B("a\n"), true);
    EXPECT_EQ(t.LineCount(), 2u);
    LineIndex e({});
    e.Append({}, true);
    EXPECT_EQ(e.LineCount(), 1u);
    EXPECT_EQ(e.Line(0, {}).end, 0u);
}

TEST(LineIndex, TrailingCrCountsOnce)
{
    LineIndex ix({});
    ix.Append(B("a\r"), true);
    EXPECT_EQ(ix.LineCount(), 2u);
    LineIndex dbl({});
    dbl.Append(B("a\r\r\nb"), true);
    EXPECT_EQ(dbl.LineCount(), 3u);
}

TEST(LineIndex, CrLfSplitAcrossChunks)
{
    LineIndex ix({});
    ix.Append(B("a\r"), false);
    ix.Append(B("\nb"), true);
    EXPECT_EQ(ix.LineCount(), 2u);
    auto c = B("a\r\nb");
    EXPECT_EQ(ix.Line(1, c).begin, 3u);
}

TEST(LineIndex, ChunkingDoesNotChangeTheResult)
{
    std::string s;
    for (int i = 0; i < 500; ++i) s += "line " + std::to_string(i) + ((i % 3 == 0) ? "\r\n" : (i % 3 == 1) ? "\n" : "\r");
    s += std::string(40000, 'q') + "\ntail";
    auto c = B(s);
    LineIndex whole({});
    whole.Append(c, true);
    for (size_t step : {1u, 7u, 100u, 16384u})
    {
        LineIndex ix({});
        for (size_t at = 0; at < s.size(); at += step)
        {
            auto n = (std::min)(step, s.size() - at);
            ix.Append(c.subspan(at, n), at + n >= s.size());
        }
        ASSERT_EQ(ix.LineCount(), whole.LineCount()) << step;
        for (uint64_t n = 0; n < whole.LineCount(); ++n)
        {
            auto a = ix.Line(n, c), b = whole.Line(n, c);
            ASSERT_TRUE(a.begin == b.begin && a.end == b.end && a.continuation == b.continuation) << step << " line " << n;
        }
    }
}

TEST(LineIndex, CheckpointsResolveEveryLine)
{
    std::string s;
    for (int i = 0; i < 10000; ++i) s += "L" + std::to_string(i) + "\n";
    auto c = B(s);
    LineIndex ix({});
    ix.Append(c, true);
    ASSERT_EQ(ix.LineCount(), 10001u);
    for (uint64_t n : {0, 1, 63, 64, 65, 127, 128, 5000, 9999})
    {
        EXPECT_EQ(Slice(s, ix.Line(n, c)), "L" + std::to_string(n)) << n;
    }
    EXPECT_EQ(Slice(s, ix.Line(10000, c)), "");
    EXPECT_EQ(ix.Line(10001, c).begin, s.size());
}

TEST(LineIndex, LongLineSplitsAt16384)
{
    std::string s(40000, 'x');
    LineIndex ix({});
    ix.Append(B(s), true);
    auto c = B(s);
    ASSERT_EQ(ix.LineCount(), 3u);
    EXPECT_EQ(ix.Line(0, c).end, 16384u);
    EXPECT_FALSE(ix.Line(0, c).continuation);
    EXPECT_TRUE(ix.Line(1, c).continuation);
    EXPECT_EQ(ix.Line(2, c).end - ix.Line(2, c).begin, 40000u - 2 * 16384u);
}

TEST(LineIndex, ExactlyMaxLengthLineIsNotSplit)
{
    std::string s = std::string(16384, 'x') + "\nnext";
    LineIndex ix({});
    ix.Append(B(s), true);
    EXPECT_EQ(ix.LineCount(), 2u);
    EXPECT_EQ(ix.Line(0, B(s)).end, 16384u);
    LineIndex plain({});
    plain.Append(B(std::string(16384, 'x')), true);
    EXPECT_EQ(plain.LineCount(), 1u);
}

TEST(LineIndex, LongLineSplitsNeverCutUtf8)
{
    std::string s;
    for (int i = 0; i < 10000; ++i) s += "\xD1\x8F"; // "я"
    auto c = B(s);
    LineIndex ix({TextEncoding::Utf8, 65001, 0});
    ix.Append(c, true);
    auto l0 = ix.Line(0, c);
    EXPECT_EQ(l0.end % 2, 0u);
    EXPECT_LE(l0.end, 16384u);
    // Three-byte characters force a back-off to the lead byte.
    std::string t = "a";
    for (int i = 0; i < 9000; ++i) t += "\xE4\xB8\x96";
    auto tc = B(t);
    LineIndex jx({TextEncoding::Utf8, 65001, 0});
    jx.Append(tc, true);
    ASSERT_GE(jx.LineCount(), 2u);
    for (uint64_t n = 0; n < jx.LineCount(); ++n)
    {
        auto sp = jx.Line(n, tc);
        EXPECT_NE(tc[static_cast<size_t>(sp.begin)] & 0xC0, 0x80) << n;
        EXPECT_LE(sp.end - sp.begin, 16384u);
    }
}

TEST(LineIndex, Utf16Lines)
{
    auto s = U16(L"a\nb");
    auto c = B(s);
    LineIndex ix({TextEncoding::Utf16LE, 1200, 0});
    ix.Append(c, true);
    ASSERT_EQ(ix.LineCount(), 2u);
    EXPECT_EQ(ix.Line(1, c).begin, 4u);
    EXPECT_EQ(ix.Line(0, c).end, 2u);

    auto be = U16(L"a\r\nb", true);
    LineIndex bx({TextEncoding::Utf16BE, 1201, 0});
    bx.Append(B(be), true);
    EXPECT_EQ(bx.LineCount(), 2u);
    EXPECT_EQ(bx.Line(1, B(be)).begin, 6u);
}

TEST(LineIndex, Utf16SplitsKeepSurrogatePairsAndUnitsAcrossChunks)
{
    std::wstring w(16383, L'x');
    w += L"\xD83D\xDE00"; // a pair straddling the limit
    w += std::wstring(20, L'y');
    auto s = U16(w);
    auto c = B(s);
    LineIndex ix({TextEncoding::Utf16LE, 1200, 0});
    for (size_t at = 0; at < s.size(); at += 3) // odd chunk size splits code units
    {
        auto n = (std::min<size_t>)(3, s.size() - at);
        ix.Append(c.subspan(at, n), at + n >= s.size());
    }
    ASSERT_EQ(ix.LineCount(), 2u);
    EXPECT_EQ(ix.Line(0, c).end, 16383u * 2);
    EXPECT_TRUE(ix.Line(1, c).continuation);
    EXPECT_EQ(ix.Line(1, c).begin, 16383u * 2);
}

TEST(LineIndex, LineFromOffset)
{
    LineIndex ix({});
    auto c = B("aa\nbb\ncc");
    ix.Append(c, true);
    EXPECT_EQ(ix.LineFromOffset(4, c), 1u);
    EXPECT_EQ(ix.LineFromOffset(0, c), 0u);
    EXPECT_EQ(ix.LineFromOffset(2, c), 0u); // the EOL belongs to its line
    EXPECT_EQ(ix.LineFromOffset(3, c), 1u);
    EXPECT_EQ(ix.LineFromOffset(7, c), 2u);
    EXPECT_EQ(ix.LineFromOffset(1000, c), 2u);

    std::string s;
    for (int i = 0; i < 1000; ++i) s += "row\n";
    LineIndex big({});
    big.Append(B(s), true);
    EXPECT_EQ(big.LineFromOffset(4 * 700 + 2, B(s)), 700u);
}

TEST(LineIndex, ConcurrentReadersDuringAppend)
{
    std::string s;
    for (int i = 0; i < 20000; ++i) s += "L" + std::to_string(i) + "\n";
    auto c = B(s);
    LineIndex ix({});
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done)
        {
            auto n = ix.LineCount();
            auto sp = ix.Line(n - 1, c);
            EXPECT_LE(sp.begin, sp.end);
        }
    });
    for (size_t at = 0; at < s.size(); at += 1000)
    {
        auto n = (std::min<size_t>)(1000, s.size() - at);
        ix.Append(c.subspan(at, n), at + n >= s.size());
    }
    done = true;
    reader.join();
    EXPECT_EQ(ix.LineCount(), 20001u);
}
