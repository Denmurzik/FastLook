#include "TestUtil.h"
#include "Imaging/Apng.h"
#include "Imaging/DecodeMath.h"

#include <chrono>
#include <string_view>
#include <utility>
#include <vector>

using namespace fastlook::core;

TEST(Apng, ParsesFixture)
{
    auto f = ParseApng(ReadBytes("image/anim-apng.png"));
    ASSERT_TRUE(f);
    EXPECT_GE(f->frames.size(), 2u);
    EXPECT_EQ(f->canvas, (SizeU{64, 48}));
    for (size_t i = 0; i < f->frames.size(); ++i)
    {
        EXPECT_GT(f->frames[i].info.delayMs, 0u);
        auto const png = BuildApngFramePng(*f, i);
        EXPECT_GE(png.size(), 8u + 25u);
        EXPECT_EQ(png[1], 'P');
    }
}

TEST(Apng, PlainPngIsNotAnimated)
{
    EXPECT_FALSE(ParseApng(ReadBytes("image/sample.png")));
}

TEST(Apng, GarbageIsNotAnimated)
{
    EXPECT_FALSE(ParseApng({}));
    EXPECT_FALSE(ParseApng(B("not a png at all, just text")));
    auto truncated = ReadBytes("image/anim-apng.png");
    truncated.resize(truncated.size() / 2);
    auto f = ParseApng(truncated); // must not crash; a cut stream is either rejected or yields fewer frames
    if (f)
    {
        EXPECT_FALSE(f->frames.empty());
    }
}

namespace
{
    void Chunk(std::string& out, char const* type, std::string const& data)
    {
        auto be = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>(v >> s)); };
        be(static_cast<uint32_t>(data.size()));
        out.append(type, 4);
        out += data;
        be(0); // the parser does not verify CRCs
    }

    std::string U32(uint32_t v) { std::string s; for (int i = 24; i >= 0; i -= 8) s.push_back(static_cast<char>(v >> i)); return s; }

    std::string Bytes(std::initializer_list<int> values)
    {
        std::string s;
        for (int v : values)
        {
            s.push_back(static_cast<char>(v));
        }
        return s;
    }

    // A syntactically valid APNG (4x4 frames, 100 ms, blend source) whose IHDR claims `w` x `h`; `ancillary` chunks go
    // between acTL and the first fcTL.
    std::string Crafted(uint32_t w, uint32_t h, uint32_t frames = 2, std::vector<std::pair<char const*, std::string>> const& ancillary = {})
    {
        std::string f = Bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
        Chunk(f, "IHDR", U32(w) + U32(h) + Bytes({8, 6, 0, 0, 0}));
        Chunk(f, "acTL", U32(frames) + U32(0));
        for (auto const& [type, data] : ancillary)
        {
            Chunk(f, type, data);
        }
        auto fctl = [&](uint32_t seq) { return U32(seq) + U32(4) + U32(4) + U32(0) + U32(0) + Bytes({0, 10, 0, 100, 0, 0}); };
        uint32_t seq = 0;
        Chunk(f, "fcTL", fctl(seq++));
        Chunk(f, "IDAT", "abc");
        for (uint32_t i = 1; i < frames; ++i)
        {
            Chunk(f, "fcTL", fctl(seq++));
            Chunk(f, "fdAT", U32(seq++) + "def");
        }
        Chunk(f, "IEND", "");
        return f;
    }

    size_t Find(std::vector<uint8_t> const& png, std::string_view needle)
    {
        return std::string_view(reinterpret_cast<char const*>(png.data()), png.size()).find(needle);
    }
}

TEST(Apng, CanvasOverTheAnimationLimitIsRejectedAtIhdr)
{
    EXPECT_FALSE(ParseApng(B(Crafted(0xFFFFFFFFu, 0xFFFFFFFFu))));
    EXPECT_FALSE(ParseApng(B(Crafted(2000, 2000)))); // 4 MP > kMaxAnimatedPixels: the still path shows the first frame
    auto f = ParseApng(B(Crafted(1920, 1080)));
    ASSERT_TRUE(f);
    ASSERT_EQ(f->frames.size(), 2u);
    EXPECT_EQ(f->frames[0].info.delayMs, 100u);
    EXPECT_EQ(f->frames[0].info.dispose, Dispose::None);
    EXPECT_EQ(f->frames[0].info.blend, Blend::Source);
}

TEST(Apng, FrameOutsideTheCanvasIsRejected)
{
    EXPECT_TRUE(ParseApng(B(Crafted(8, 8))));
    EXPECT_FALSE(ParseApng(B(Crafted(3, 3)))); // 4x4 frames do not fit a 3x3 canvas
}

TEST(Apng, ColourChunksAreKeptOnceAndSplicedIntoEveryFrame)
{
    auto f = ParseApng(B(Crafted(8, 8, 3, {{"gAMA", U32(45455)}, {"tEXt", std::string("Comment\0hello", 13)}})));
    ASSERT_TRUE(f);
    ASSERT_EQ(f->frames.size(), 3u);
    EXPECT_EQ(f->shared.size(), 12u + 4u); // gAMA only: text and private chunks are not needed to decode a frame
    for (size_t i = 0; i < f->frames.size(); ++i)
    {
        auto const png = BuildApngFramePng(*f, i);
        auto const gama = Find(png, "gAMA"), idat = Find(png, "IDAT");
        ASSERT_NE(gama, std::string_view::npos);
        ASSERT_NE(idat, std::string_view::npos);
        EXPECT_LT(gama, idat);
        EXPECT_EQ(Find(png, "tEXt"), std::string_view::npos);
    }
    EXPECT_TRUE(BuildApngFramePng(*f, 3).empty());
}

TEST(Apng, HugeAncillaryChunkTimesManyFramesStaysBoundedByTheFileSize)
{
    // Final review A#1/B2#1: one large pre-IDAT chunk used to be copied into every frame (32 MB x 100 000 frames).
    std::string const big(16u * 1024 * 1024, 'x');
    auto const bytes = Crafted(8, 8, 20000, {{"zTXt", big}, {"prIv", big}});
    auto const start = std::chrono::steady_clock::now();
    auto f = ParseApng(B(bytes));
    auto const elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(f);
    EXPECT_EQ(f->frames.size(), 20000u);
    size_t held = f->shared.size();
    for (auto const& frame : f->frames)
    {
        held += frame.idat.size();
    }
    EXPECT_LE(held, bytes.size() / 100); // the 32 MB of text and private data is not kept at all
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(Apng, OversizedColourProfileIsRejected)
{
    std::string const icc(kMaxApngSharedBytes + 1, 'p');
    EXPECT_FALSE(ParseApng(B(Crafted(8, 8, 2, {{"iCCP", icc}}))));
    EXPECT_TRUE(ParseApng(B(Crafted(8, 8, 2, {{"iCCP", std::string(4096, 'p')}}))));
}

TEST(Apng, DeclaresAnimationLooksForActlBeforeTheImageData)
{
    EXPECT_TRUE(DeclaresApngAnimation(B(Crafted(8, 8))));
    EXPECT_FALSE(DeclaresApngAnimation(B(Crafted(2000, 2000)))); // canvas over the animation limit
    EXPECT_FALSE(DeclaresApngAnimation(ReadBytes("image/sample.png")));
    EXPECT_TRUE(DeclaresApngAnimation(ReadBytes("image/anim-apng.png")));
    EXPECT_FALSE(DeclaresApngAnimation(B("garbage")));
}
