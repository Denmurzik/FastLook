#include <nlohmann/json.hpp>

#include "Settings/Settings.h"
#include "TestUtil.h"

using namespace fastlook::core;

TEST(Settings, DefaultsFromEmptyObject)
{
    auto d = ParseSettings("{}");
    EXPECT_EQ(d.values, Settings{});
    EXPECT_EQ(d.values.general.idleUnloadMinutes, 30u);
    EXPECT_FALSE(d.recoveredFromCorrupt);
}

TEST(Settings, InvalidIdleFallsBackTo30_ZeroMeansNever)
{
    EXPECT_EQ(ParseSettings(R"({"general":{"idleUnloadMinutes":7}})").values.general.idleUnloadMinutes, 30u);
    EXPECT_EQ(ParseSettings(R"({"general":{"idleUnloadMinutes":0}})").values.general.idleUnloadMinutes, 0u);
}

TEST(Settings, ClampsAndEnumFallbacks)
{
    auto v = ParseSettings(R"({"preview":{"codeFontSize":99,"mediaVolume":-1},"general":{"theme":"pink","activationKey":"ctrl+space"}})").values;
    EXPECT_EQ(v.preview.codeFontSize, 32u);
    EXPECT_EQ(v.preview.mediaVolume, 0.0);
    EXPECT_EQ(v.general.theme, ThemePref::System);
    EXPECT_EQ(v.general.activationKey, ActivationKey::CtrlSpace);
}

TEST(Settings, WrongTypesFallBackToDefaults)
{
    auto v = ParseSettings(R"({"general":{"prewarm":"yes","idleUnloadMinutes":"15"},"preview":{"codeFontSize":true,"wordWrap":1},"logLevel":5})").values;
    EXPECT_TRUE(v.general.prewarm);
    EXPECT_EQ(v.general.idleUnloadMinutes, 30u);
    EXPECT_EQ(v.preview.codeFontSize, 13u);
    EXPECT_FALSE(v.preview.wordWrap);
    EXPECT_EQ(v.logLevel, LogLevel::Info);
}

TEST(Settings, ParsesEveryKey)
{
    auto v = ParseSettings(R"({"version":1,"general":{"desktopEnabled":true,"activationKey":"alt+space","prewarm":false,
        "idleUnloadMinutes":60,"theme":"dark","language":"ru","peekWarning":false},
        "preview":{"wordWrap":true,"codeFontSize":16,"autoplayMedia":false,"useShellHandlers":false,
        "markdownView":"source","mediaVolume":0.25},"logLevel":"debug"})").values;
    EXPECT_TRUE(v.general.desktopEnabled);
    EXPECT_EQ(v.general.activationKey, ActivationKey::AltSpace);
    EXPECT_FALSE(v.general.prewarm);
    EXPECT_EQ(v.general.idleUnloadMinutes, 60u);
    EXPECT_EQ(v.general.theme, ThemePref::Dark);
    EXPECT_EQ(v.general.language, LanguagePref::Russian);
    EXPECT_FALSE(v.general.peekWarning);
    EXPECT_TRUE(v.preview.wordWrap);
    EXPECT_EQ(v.preview.codeFontSize, 16u);
    EXPECT_FALSE(v.preview.autoplayMedia);
    EXPECT_FALSE(v.preview.useShellHandlers);
    EXPECT_EQ(v.preview.markdownView, MarkdownView::Source);
    EXPECT_EQ(v.preview.mediaVolume, 0.25);
    EXPECT_EQ(v.logLevel, LogLevel::Debug);
}

TEST(Settings, SerializedShapeUsesExactKeys)
{
    auto j = nlohmann::json::parse(SerializeSettings(ParseSettings("{}")));
    EXPECT_EQ(j, nlohmann::json::parse(R"({"version":1,"general":{"desktopEnabled":false,"activationKey":"space","prewarm":true,
        "idleUnloadMinutes":30,"theme":"system","language":"system","peekWarning":true},
        "preview":{"wordWrap":false,"codeFontSize":13,"autoplayMedia":true,"useShellHandlers":true,
        "markdownView":"rendered","mediaVolume":0.8},"logLevel":"info"})"));
}

TEST(Settings, UnknownFieldsSurviveRoundTrip)
{
    auto d = ParseSettings(R"({"version":1,"future":{"x":1},"general":{"theme":"dark","newKey":true}})");
    auto j = nlohmann::json::parse(SerializeSettings(d));
    EXPECT_EQ(j["future"]["x"], 1);
    EXPECT_EQ(j["general"]["newKey"], true);
    EXPECT_EQ(j["general"]["theme"], "dark");
}

TEST(Settings, FutureVersionIsKept)
{
    EXPECT_EQ(nlohmann::json::parse(SerializeSettings(ParseSettings(R"({"version":3})")))["version"], 3);
}

TEST(Settings, MissingVersionBecomes1)
{
    EXPECT_EQ(ParseSettings("{}").values.version, 1u);
}

TEST(Settings, NonObjectDocumentIsCorrupt)
{
    EXPECT_TRUE(ParseSettings("[1,2]").recoveredFromCorrupt);
    EXPECT_TRUE(ParseSettings("").recoveredFromCorrupt);
}

TEST(Settings, MissingFileGivesDefaultsWithoutRecovery)
{
    auto d = LoadSettings(MakeTempDir() / "settings.json");
    EXPECT_EQ(d.values, Settings{});
    EXPECT_FALSE(d.recoveredFromCorrupt);
}

TEST(Settings, CorruptFileBackedUpAndDefaultsUsed)
{
    auto dir = MakeTempDir();
    auto f = dir / "settings.json";
    WriteFile(f, "{not json");
    auto d = LoadSettings(f);
    EXPECT_TRUE(d.recoveredFromCorrupt);
    EXPECT_EQ(d.values, Settings{});
    EXPECT_EQ(ReadFile(dir / "settings.json.bak"), "{not json");
}

TEST(Settings, SaveThenLoadRoundTrips)
{
    auto dir = MakeTempDir() / "nested";
    auto f = dir / "settings.json";
    auto d = ParseSettings(R"({"extra":[1,2]})");
    d.values.general.theme = ThemePref::Light;
    d.values.preview.codeFontSize = 20;
    d.values.logLevel = LogLevel::Debug;
    ASSERT_TRUE(SaveSettings(f, d));
    auto loaded = LoadSettings(f);
    EXPECT_EQ(loaded.values, d.values);
    EXPECT_FALSE(loaded.recoveredFromCorrupt);
    EXPECT_EQ(nlohmann::json::parse(SerializeSettings(loaded))["extra"][1], 2);
    EXPECT_FALSE(std::filesystem::exists(dir / "settings.json.tmp"));
}
