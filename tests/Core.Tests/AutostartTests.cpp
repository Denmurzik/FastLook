#include "System/Autostart.h"
#include "TestUtil.h"

using namespace fastlook::core;

TEST(Autostart, SetAndClearTestValue)
{
    ASSERT_TRUE(SetAutostart(true, L"C:\\X\\FastLook.exe", L"FastLookUnitTest"));
    EXPECT_TRUE(IsAutostartEnabled(L"FastLookUnitTest"));
    ASSERT_TRUE(SetAutostart(false, L"", L"FastLookUnitTest"));
    EXPECT_FALSE(IsAutostartEnabled(L"FastLookUnitTest"));
}

TEST(Autostart, ClearingAbsentValueSucceeds)
{
    EXPECT_TRUE(SetAutostart(false, L"", L"FastLookUnitTestAbsent"));
    EXPECT_FALSE(IsAutostartEnabled(L"FastLookUnitTestAbsent"));
}
