#include "TestUtil.h"
#include "Imaging/Apng.h"

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
