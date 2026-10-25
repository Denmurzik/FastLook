#include "TestUtil.h"
#include "Cli/ViewerArgs.h"

using namespace fastlook::core;

TEST(ViewerArgs, PreviewFilesIconRectAndScript)
{
    std::wstring_view a[] = {L"--preview", L"a.txt", L"b.txt", L"--icon-rect", L"1,2,3,4", L"--script", L"next;wait:300", L"--perf-log", L"p.log"};
    auto r = ParseViewerArgs(a);
    EXPECT_EQ(r.mode, ViewerMode::Preview);
    EXPECT_EQ(r.files, (std::vector<std::wstring>{L"a.txt", L"b.txt"}));
    EXPECT_EQ(r.iconRect, (RectI{1, 2, 3, 4}));
    EXPECT_EQ(r.script, L"next;wait:300");
    EXPECT_EQ(r.perfLog, L"p.log");
}

TEST(ViewerArgs, ServerBenchSettingsAndDefaults)
{
    {
        std::wstring_view a[] = {L"--background", L"--pipe", L"X"};
        auto r = ParseViewerArgs(a);
        EXPECT_EQ(r.mode, ViewerMode::Server);
        EXPECT_TRUE(r.background);
        EXPECT_EQ(r.pipeName, L"X");
    }
    {
        std::wstring_view a[] = {L"--bench", L"d", L"--iterations", L"50", L"--out", L"o.json"};
        auto r = ParseViewerArgs(a);
        EXPECT_EQ(r.mode, ViewerMode::Bench);
        EXPECT_EQ(r.benchDir, L"d");
        EXPECT_EQ(r.iterations, 50u);
        EXPECT_EQ(r.benchOut, L"o.json");
    }
    {
        auto r = ParseViewerArgs({});
        EXPECT_EQ(r.mode, ViewerMode::Settings);
        EXPECT_EQ(r.iterations, 1000u);
    }
    {
        std::wstring_view a[] = {L"--settings", L"--settings-page", L"about"};
        auto r = ParseViewerArgs(a);
        EXPECT_EQ(r.mode, ViewerMode::Settings);
        EXPECT_EQ(r.settingsPage, L"about");
    }
    {
        std::wstring_view a[] = {L"--icon-rect", L"1,2"};
        EXPECT_FALSE(ParseViewerArgs(a).iconRect.has_value());
    }
    {
        std::wstring_view a[] = {L"--lang", L"ru", L"--theme", L"dark"};
        auto r = ParseViewerArgs(a);
        EXPECT_EQ(r.lang, LanguagePref::Russian);
        EXPECT_EQ(r.theme, ThemePref::Dark);
    }
    {
        std::wstring_view a[] = {L"--bogus"};
        EXPECT_EQ(ParseViewerArgs(a).unknown, (std::vector<std::wstring>{L"--bogus"}));
    }
    {
        std::wstring_view a[] = {L"--preview", L"a.txt", L"--background"};
        auto r = ParseViewerArgs(a);
        EXPECT_EQ(r.mode, ViewerMode::Preview);
        EXPECT_TRUE(r.background);
    }
}

TEST(ViewerArgs, ModePrecedenceAndMalformedValues)
{
    std::wstring_view a[] = {L"--pipe", L"X", L"--preview", L"a", L"--bench", L"d", L"--iterations", L"zero"};
    auto r = ParseViewerArgs(a);
    EXPECT_EQ(r.mode, ViewerMode::Bench);
    EXPECT_EQ(r.iterations, 1000u);
    std::wstring_view b[] = {L"--pipe", L"X", L"--preview", L"a"};
    EXPECT_EQ(ParseViewerArgs(b).mode, ViewerMode::Preview);
}
