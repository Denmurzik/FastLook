#include "TestUtil.h"
#include "Imaging/FrameCompositor.h"

using namespace fastlook::core;

namespace
{
    constexpr uint32_t kRed = 0xFFFF0000, kGreen = 0xFF00FF00, kBlue = 0xFF0000FF;

    std::vector<uint32_t> Solid(int w, int h, uint32_t color)
    {
        return std::vector<uint32_t>(static_cast<size_t>(w) * h, color);
    }

    uint32_t At(FrameCompositor const& c, int x, int y, int width = 4)
    {
        return c.Canvas()[static_cast<size_t>(y) * width + x];
    }
}

TEST(FrameCompositor, DisposeBackgroundClearsRect)
{
    FrameCompositor c;
    c.Begin({4, 4});
    c.Apply({{0, 0, 4, 4}, 100, Dispose::Background, Blend::Over}, Solid(4, 4, kRed));
    EXPECT_EQ(At(c, 3, 3), kRed);
    c.Apply({{0, 0, 1, 1}, 100, Dispose::None, Blend::Over}, Solid(1, 1, kGreen));
    EXPECT_EQ(At(c, 0, 0), kGreen);
    EXPECT_EQ(At(c, 3, 3), 0u);
}

TEST(FrameCompositor, DisposePreviousRestores)
{
    FrameCompositor c;
    c.Begin({4, 4});
    c.Apply({{0, 0, 4, 4}, 100, Dispose::None, Blend::Over}, Solid(4, 4, kRed));
    c.Apply({{0, 0, 2, 2}, 100, Dispose::Previous, Blend::Over}, Solid(2, 2, kBlue));
    EXPECT_EQ(At(c, 0, 0), kBlue);
    c.Apply({{3, 3, 1, 1}, 100, Dispose::None, Blend::Over}, Solid(1, 1, kGreen));
    EXPECT_EQ(At(c, 0, 0), kRed);
    EXPECT_EQ(At(c, 3, 3), kGreen);
}

TEST(FrameCompositor, BlendOverRespectsAlpha)
{
    FrameCompositor c;
    c.Begin({1, 1});
    c.Apply({{0, 0, 1, 1}, 100, Dispose::None, Blend::Source}, Solid(1, 1, kRed));
    // Premultiplied half-transparent blue: A=128, B=128.
    c.Apply({{0, 0, 1, 1}, 100, Dispose::None, Blend::Over}, Solid(1, 1, 0x80000080));
    EXPECT_EQ(c.Canvas()[0], 0xFF7F0080u); // A=255, R=255*127/255, B=128
}

TEST(FrameCompositor, BlendSourceReplacesPixels)
{
    FrameCompositor c;
    c.Begin({2, 1});
    c.Apply({{0, 0, 2, 1}, 100, Dispose::None, Blend::Source}, Solid(2, 1, kRed));
    c.Apply({{1, 0, 1, 1}, 100, Dispose::None, Blend::Source}, Solid(1, 1, 0));
    EXPECT_EQ(At(c, 0, 0, 2), kRed);
    EXPECT_EQ(At(c, 1, 0, 2), 0u);
}

TEST(FrameCompositor, ClipsFramesOutsideTheCanvas)
{
    FrameCompositor c;
    c.Begin({2, 2});
    c.Apply({{1, 1, 3, 3}, 100, Dispose::None, Blend::Source}, Solid(3, 3, kBlue));
    EXPECT_EQ(At(c, 1, 1, 2), kBlue);
    EXPECT_EQ(At(c, 0, 0, 2), 0u);
    c.Apply({{5, 5, 2, 2}, 100, Dispose::None, Blend::Source}, Solid(2, 2, kRed)); // fully outside: ignored
    c.Apply({{0, 0, 2, 2}, 100, Dispose::None, Blend::Source}, Solid(1, 1, kRed)); // short pixel data: ignored
    EXPECT_EQ(At(c, 1, 1, 2), kBlue);
}

TEST(FrameCompositor, RestartClearsCanvasAndDisposalState)
{
    FrameCompositor c;
    c.Begin({2, 2});
    c.Apply({{0, 0, 2, 2}, 100, Dispose::Background, Blend::Source}, Solid(2, 2, kRed));
    c.Restart();
    EXPECT_EQ(At(c, 0, 0, 2), 0u);
    EXPECT_EQ(c.Canvas().size(), 4u);
    c.Apply({{0, 0, 1, 1}, 100, Dispose::None, Blend::Source}, Solid(1, 1, kGreen)); // no stale disposal of the old frame
    EXPECT_EQ(At(c, 0, 0, 2), kGreen);
    EXPECT_EQ(At(c, 1, 1, 2), 0u);
}
