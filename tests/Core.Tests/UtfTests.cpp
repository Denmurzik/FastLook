#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "Text/Utf.h"

TEST(Utf, RoundTripsCyrillicAndEmoji)
{
    std::wstring w = L"Привет, мир \U0001F600";
    EXPECT_EQ(fastlook::core::FromUtf8(fastlook::core::ToUtf8(w)), w);
    EXPECT_EQ(fastlook::core::ToUtf8(L"я"), "\xD1\x8F");
}

TEST(Utf, InvalidUtf8BecomesReplacementChar)
{
    EXPECT_EQ(fastlook::core::FromUtf8("a\xFF" "b"), L"a�b");
}

TEST(Utf, FixturesDirExists)
{
    EXPECT_TRUE(std::filesystem::is_directory(FASTLOOK_FIXTURES_DIR));
}
