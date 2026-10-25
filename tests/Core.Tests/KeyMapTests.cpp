#include <windows.h>

#include "TestUtil.h"
#include "Ui/KeyMap.h"

using namespace fastlook::core;

TEST(KeyMap, CloseKeysAndPinnedSpace)
{
    EXPECT_EQ(MapPreviewKey(VK_SPACE, false, false, false, false), Command::Close);
    EXPECT_FALSE(MapPreviewKey(VK_SPACE, false, false, false, true));
    EXPECT_EQ(MapPreviewKey(VK_ESCAPE, false, false, false, true), Command::Close);
    EXPECT_EQ(MapPreviewKey(VK_ESCAPE, false, false, false, false), Command::Close);
    EXPECT_EQ(MapPreviewKey('W', true, false, false, false), Command::Close);
    EXPECT_EQ(MapPreviewKey('W', true, false, false, true), Command::Close);
    EXPECT_FALSE(MapPreviewKey('W', false, false, false, false));
    EXPECT_FALSE(MapPreviewKey(VK_SPACE, true, false, false, false));
}

TEST(KeyMap, NavigationOpenFullscreen)
{
    EXPECT_EQ(MapPreviewKey(VK_LEFT, false, false, false, false), Command::Prev);
    EXPECT_EQ(MapPreviewKey(VK_UP, false, false, false, false), Command::Prev);
    EXPECT_EQ(MapPreviewKey(VK_RIGHT, false, false, false, false), Command::Next);
    EXPECT_EQ(MapPreviewKey(VK_DOWN, false, false, false, false), Command::Next);
    EXPECT_EQ(MapPreviewKey(VK_RETURN, false, false, false, false), Command::OpenDefault);
    EXPECT_EQ(MapPreviewKey(VK_F11, false, false, false, false), Command::ToggleFullscreen);
    EXPECT_FALSE(MapPreviewKey(VK_RIGHT, false, true, false, false));
    EXPECT_FALSE(MapPreviewKey(VK_LEFT, false, false, true, false));
    EXPECT_FALSE(MapPreviewKey(VK_RETURN, false, true, false, false));
    EXPECT_FALSE(MapPreviewKey(VK_ESCAPE, false, true, false, false));
    EXPECT_FALSE(MapPreviewKey(VK_LEFT, true, false, false, false));
}

TEST(KeyMap, ZoomAndCopy)
{
    EXPECT_EQ(MapPreviewKey(VK_OEM_PLUS, true, false, false, false), Command::ZoomIn);
    EXPECT_EQ(MapPreviewKey(VK_ADD, true, false, false, false), Command::ZoomIn);
    EXPECT_EQ(MapPreviewKey(VK_OEM_MINUS, true, false, false, false), Command::ZoomOut);
    EXPECT_EQ(MapPreviewKey(VK_SUBTRACT, true, false, false, false), Command::ZoomOut);
    EXPECT_EQ(MapPreviewKey('0', true, false, false, false), Command::ZoomReset);
    EXPECT_EQ(MapPreviewKey(VK_NUMPAD0, true, false, false, false), Command::ZoomReset);
    EXPECT_EQ(MapPreviewKey('C', true, false, false, false), Command::Copy);
    EXPECT_FALSE(MapPreviewKey('C', false, false, false, false));
    EXPECT_FALSE(MapPreviewKey(VK_OEM_PLUS, false, false, false, false));
    EXPECT_FALSE(MapPreviewKey('C', true, true, false, false));
    EXPECT_FALSE(MapPreviewKey(VK_ADD, true, false, true, false));
}
