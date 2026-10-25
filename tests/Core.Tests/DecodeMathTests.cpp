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
