#include "System/AppPaths.h"
#include "TestUtil.h"

using namespace fastlook::core;

TEST(AppPaths, PortableWhenMarkerExists)
{
    auto exe = MakeTempDir();
    WriteFile(exe / "portable.txt", "");
    auto p = ResolveAppPaths(exe, MakeTempDir(), std::nullopt);
    EXPECT_EQ(p.dataDir, exe);
    EXPECT_EQ(p.logsDir, exe / "logs");
    EXPECT_EQ(p.settingsFile, exe / "settings.json");
    EXPECT_TRUE(p.portable);
}

TEST(AppPaths, LocalAppDataByDefault)
{
    auto exe = MakeTempDir();
    auto lad = MakeTempDir();
    auto p = ResolveAppPaths(exe, lad, std::nullopt);
    EXPECT_EQ(p.exeDir, exe);
    EXPECT_EQ(p.dataDir, lad / "FastLook");
    EXPECT_EQ(p.settingsFile, lad / "FastLook" / "settings.json");
    EXPECT_EQ(p.logsDir, lad / "FastLook" / "logs");
    EXPECT_FALSE(p.portable);
}

TEST(AppPaths, OverrideWinsOverPortable)
{
    auto exe = MakeTempDir();
    WriteFile(exe / "portable.txt", "");
    auto over = MakeTempDir() / "data";
    auto p = ResolveAppPaths(exe, MakeTempDir(), over);
    EXPECT_EQ(p.dataDir, over);
    EXPECT_EQ(p.settingsFile, over / "settings.json");
    EXPECT_FALSE(p.portable);
}
