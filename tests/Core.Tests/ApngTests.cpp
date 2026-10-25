#include "TestUtil.h"
#include "Imaging/Apng.h"
#include "Imaging/DecodeMath.h"

using namespace fastlook::core;

TEST(Apng, ParsesFixture)
{
    auto f = ParseApng(ReadBytes("image/anim-apng.png"));
    ASSERT_TRUE(f);
    EXPECT_GE(f->frames.size(), 2u);
    EXPECT_EQ(f->canvas, (SizeU{64, 48}));
    for (auto const& frame : f->frames)
    {
        EXPECT_GT(frame.info.delayMs, 0u);
        EXPECT_GE(frame.png.size(), 8u + 25u);
        EXPECT_EQ(frame.png[1], 'P');
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

    // A syntactically valid two-frame APNG (4x4 frames, 100 ms, blend source) whose IHDR claims `w` x `h`.
    std::string Crafted(uint32_t w, uint32_t h)
    {
        std::string f = Bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
        Chunk(f, "IHDR", U32(w) + U32(h) + Bytes({8, 6, 0, 0, 0}));
        Chunk(f, "acTL", U32(2) + U32(0));
        auto fctl = [&](uint32_t seq) { return U32(seq) + U32(4) + U32(4) + U32(0) + U32(0) + Bytes({0, 10, 0, 100, 0, 0}); };
        Chunk(f, "fcTL", fctl(0));
        Chunk(f, "IDAT", "abc");
        Chunk(f, "fcTL", fctl(1));
        Chunk(f, "fdAT", U32(2) + "def");
        Chunk(f, "IEND", "");
        return f;
    }
}

TEST(Apng, HugeIhdrParsesWithoutAllocatingACanvasAndIsNotAnimatable)
{
    auto f = ParseApng(B(Crafted(0xFFFFFFFFu, 0xFFFFFFFFu)));
    ASSERT_TRUE(f); // the parser only keeps the frame streams
    EXPECT_EQ(f->canvas, (SizeU{0xFFFFFFFFu, 0xFFFFFFFFu}));
    EXPECT_FALSE(CanAnimateCanvas(f->canvas)); // the Viewer stops here, before any canvas exists
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
