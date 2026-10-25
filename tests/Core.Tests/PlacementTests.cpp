#include "TestUtil.h"
#include "Ui/Placement.h"

using namespace fastlook::core;

TEST(Placement, LargePhotoFitsKeepingAspect)
{
    EXPECT_EQ(FitPreviewWindow({6000, 4000}, true, {0, 0, 1920, 1040}, 1.0), (RectI{372, 104, 1176, 832}));
}

TEST(Placement, SmallContentClampedToMinimum)
{
    EXPECT_EQ(FitPreviewWindow({100, 100}, false, {0, 0, 1920, 1040}, 1.0), (RectI{720, 360, 480, 320}));
}

TEST(Placement, SecondMonitorScaled)
{
    EXPECT_EQ(FitPreviewWindow({960, 720}, false, {1920, 0, 2560, 1400}, 1.25), (RectI{2600, 220, 1200, 960}));
}

TEST(Placement, TextDefaultSizeAtScale1)
{
    EXPECT_EQ(FitPreviewWindow(kDefaultTextSize, false, {0, 0, 1920, 1040}, 1.0), (RectI{480, 136, 960, 768}));
}

TEST(Placement, OversizedWithoutAspectClampsPerAxis)
{
    EXPECT_EQ(FitPreviewWindow({5000, 5000}, false, {0, 0, 1920, 1040}, 1.0), (RectI{192, 104, 1536, 832}));
}
