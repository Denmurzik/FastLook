#include "TestUtil.h"
#include "Imaging/DecodeMath.h"

using namespace fastlook::core;

TEST(DecodeMath, FitsWithoutUpscale)
{
    EXPECT_EQ(ComputeDecodeSize({6000, 4000}, {1176, 784}, 1.5), (SizeU{1764, 1176}));
    EXPECT_EQ(ComputeDecodeSize({100, 100}, {1176, 784}, 1.0), (SizeU{100, 100}));
    EXPECT_EQ(ComputeDecodeSize({10000, 1}, {500, 500}, 1.0), (SizeU{500, 1}));
}

TEST(DecodeMath, DegenerateInputsGiveOnePixel)
{
    EXPECT_EQ(ComputeDecodeSize({0, 0}, {500, 500}, 1.0), (SizeU{1, 1}));
    EXPECT_EQ(ComputeDecodeSize({100, 100}, {0, 0}, 1.0), (SizeU{1, 1}));
}

TEST(DecodeMath, OrientationTable)
{
    EXPECT_EQ(OrientedSize({400, 300}, 6), (SizeU{300, 400}));
    EXPECT_EQ(OrientedSize({400, 300}, 3), (SizeU{400, 300}));
    EXPECT_EQ(OrientedSize({400, 300}, 0), (SizeU{400, 300}));
    EXPECT_EQ(OrientedSize({400, 300}, 9), (SizeU{400, 300}));
    int deg[] = {0, 0, 0, 180, 180, 90, 90, 270, 270};
    bool flip[] = {false, false, true, false, true, true, false, true, false};
    for (uint16_t o = 1; o <= 8; ++o)
    {
        EXPECT_EQ(ExifRotationDegrees(o), deg[o]) << o;
        EXPECT_EQ(ExifFlipsHorizontally(o), flip[o]) << o;
    }
}

TEST(DecodeMath, FrameDelayNormalization)
{
    EXPECT_EQ(NormalizeFrameDelayMs(0), 100u);
    EXPECT_EQ(NormalizeFrameDelayMs(10), 100u);
    EXPECT_EQ(NormalizeFrameDelayMs(19), 100u);
    EXPECT_EQ(NormalizeFrameDelayMs(20), 20u);
    EXPECT_EQ(NormalizeFrameDelayMs(70), 70u);
}

TEST(DecodeMath, AnimationCanvasLimit)
{
    EXPECT_TRUE(CanAnimateCanvas({64, 48}));
    EXPECT_TRUE(CanAnimateCanvas({2048, 2048}));
    EXPECT_TRUE(CanAnimateCanvas({4200, 1000}));
    EXPECT_FALSE(CanAnimateCanvas({4096, 4096}));
    EXPECT_FALSE(CanAnimateCanvas({2050, 2050}));
    EXPECT_FALSE(CanAnimateCanvas({65535, 65535}));
    EXPECT_FALSE(CanAnimateCanvas({0xFFFFFFFFu, 0xFFFFFFFFu}));
    EXPECT_FALSE(CanAnimateCanvas({0, 10}));
}

namespace
{
    std::vector<uint8_t> GifHeader(unsigned w, unsigned h)
    {
        return {'G', 'I', 'F', '8', '9', 'a', static_cast<uint8_t>(w), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8)};
    }
}

TEST(DecodeMath, GifLogicalScreenFromHeader)
{
    EXPECT_EQ(GifLogicalScreenSize(GifHeader(64, 48)), (SizeU{64, 48}));
    EXPECT_EQ(GifLogicalScreenSize(GifHeader(65535, 65535)), (SizeU{65535, 65535}));
    EXPECT_FALSE(GifLogicalScreenSize(B("GIF90a1234")));
    EXPECT_FALSE(GifLogicalScreenSize(B("GIF89a")));
    EXPECT_FALSE(GifLogicalScreenSize({}));
    auto real = ReadBytes("image/anim.gif");
    ASSERT_FALSE(real.empty());
    EXPECT_EQ(GifLogicalScreenSize(real), (SizeU{64, 48}));
}

TEST(DecodeMath, SvgLengthUnits)
{
    EXPECT_EQ(ParseSvgLength(L"24"), 24.0);
    EXPECT_EQ(ParseSvgLength(L" 24px "), 24.0);
    EXPECT_EQ(ParseSvgLength(L"1in"), 96.0);
    EXPECT_EQ(ParseSvgLength(L"72pt"), 96.0);
    EXPECT_EQ(ParseSvgLength(L"1pc"), 16.0);
    EXPECT_NEAR(*ParseSvgLength(L"25.4mm"), 96.0, 1e-9);
    EXPECT_NEAR(*ParseSvgLength(L"2.54cm"), 96.0, 1e-9);
    EXPECT_EQ(ParseSvgLength(L"1.5e1"), 15.0);
    EXPECT_FALSE(ParseSvgLength(L"100%"));
    EXPECT_FALSE(ParseSvgLength(L"2em"));
    EXPECT_FALSE(ParseSvgLength(L"3ex"));
    EXPECT_FALSE(ParseSvgLength(L"auto"));
    EXPECT_FALSE(ParseSvgLength(L""));
    EXPECT_FALSE(ParseSvgLength(L"px"));
}

TEST(DecodeMath, SvgIntrinsicSize)
{
    EXPECT_EQ(SvgIntrinsicSize(320, 240, SizeD{10, 10}), (SizeD{320, 240}));
    EXPECT_EQ(SvgIntrinsicSize(std::nullopt, std::nullopt, SizeD{24, 12}), (SizeD{24, 12}));
    EXPECT_EQ(SvgIntrinsicSize(200, std::nullopt, SizeD{24, 12}), (SizeD{200, 100}));
    EXPECT_EQ(SvgIntrinsicSize(std::nullopt, 100, SizeD{24, 12}), (SizeD{200, 100}));
    EXPECT_EQ(SvgIntrinsicSize(std::nullopt, std::nullopt, std::nullopt), (SizeD{512, 512}));
    EXPECT_EQ(SvgIntrinsicSize(0, -5, SizeD{0, 0}), (SizeD{512, 512}));
    EXPECT_EQ(SvgIntrinsicSize(100000, 50000, std::nullopt), (SizeD{16384, 8192}));
}
