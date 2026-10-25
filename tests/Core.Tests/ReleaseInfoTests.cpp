#include "TestUtil.h"

#include "Update/ReleaseInfo.h"

using namespace fastlook::core;

TEST(ReleaseInfo, ParsesFixture)
{
    auto r = ParseLatestRelease(Fixture("update/latest.json"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->version, (SemVer{1, 2, 0, ""}));
    EXPECT_EQ(r->tag, "v1.2.0");
    EXPECT_EQ(r->setupName, "FastLook-Setup-1.2.0.exe");
    EXPECT_TRUE(r->setupUrl.ends_with("/FastLook-Setup-1.2.0.exe"));
    EXPECT_TRUE(r->shaUrl.ends_with(".exe.sha256"));
    EXPECT_EQ(r->htmlUrl, "https://github.com/Denmurzik/FastLook/releases/tag/v1.2.0");
}

TEST(ReleaseInfo, UpdateUrlAllowlist)
{
    EXPECT_TRUE(IsAllowedUpdateUrl("https://api.github.com/repos/x"));
    EXPECT_TRUE(IsAllowedUpdateUrl("https://release-assets.githubusercontent.com/a?b"));
    EXPECT_TRUE(IsAllowedUpdateUrl("https://github.com"));
    EXPECT_FALSE(IsAllowedUpdateUrl("http://github.com/x"));
    EXPECT_FALSE(IsAllowedUpdateUrl("https://github.com.evil.io/x"));
    EXPECT_FALSE(IsAllowedUpdateUrl("https://github.com@evil.io/x"));
    EXPECT_FALSE(IsAllowedUpdateUrl("https://evil.io/github.com"));
    EXPECT_FALSE(IsAllowedUpdateUrl("https://github.com:444/x"));
    EXPECT_FALSE(IsAllowedUpdateUrl("https://example.com/"));
    EXPECT_FALSE(IsAllowedUpdateUrl(""));
}

TEST(ReleaseInfo, MissingAssetsRejected)
{
    EXPECT_FALSE(ParseLatestRelease(R"({"tag_name":"v1.0.0","assets":[]})"));
    EXPECT_FALSE(ParseLatestRelease("not json"));
    EXPECT_FALSE(ParseLatestRelease(R"({"tag_name":"nightly","assets":[]})"));
}

TEST(ReleaseInfo, RejectsAssetsOutsideOwnRepositoryOrTag)
{
    constexpr char kOtherRepo[] = R"({"tag_name":"v1.0.0","assets":[
        {"name":"FastLook-Setup-1.0.0.exe","browser_download_url":"https://github.com/evil/FastLook/releases/download/v1.0.0/FastLook-Setup-1.0.0.exe"},
        {"name":"FastLook-Setup-1.0.0.exe.sha256","browser_download_url":"https://github.com/evil/FastLook/releases/download/v1.0.0/FastLook-Setup-1.0.0.exe.sha256"}]})";
    EXPECT_FALSE(ParseLatestRelease(kOtherRepo));
    constexpr char kOtherTag[] = R"({"tag_name":"v1.0.0","assets":[
        {"name":"FastLook-Setup-1.0.0.exe","browser_download_url":"https://github.com/Denmurzik/FastLook/releases/download/v0.9.0/FastLook-Setup-1.0.0.exe"},
        {"name":"FastLook-Setup-1.0.0.exe.sha256","browser_download_url":"https://github.com/Denmurzik/FastLook/releases/download/v0.9.0/FastLook-Setup-1.0.0.exe.sha256"}]})";
    EXPECT_FALSE(ParseLatestRelease(kOtherTag));
}

TEST(ReleaseInfo, RejectsAssetsFromForeignHostOrWrongVersion)
{
    constexpr char kForeign[] = R"({"tag_name":"v1.0.0","assets":[
        {"name":"FastLook-Setup-1.0.0.exe","browser_download_url":"https://evil.io/FastLook-Setup-1.0.0.exe"},
        {"name":"FastLook-Setup-1.0.0.exe.sha256","browser_download_url":"https://github.com/a/FastLook-Setup-1.0.0.exe.sha256"}]})";
    EXPECT_FALSE(ParseLatestRelease(kForeign));
    constexpr char kWrongVersion[] = R"({"tag_name":"v1.0.0","assets":[
        {"name":"FastLook-Setup-9.9.9.exe","browser_download_url":"https://github.com/a/FastLook-Setup-9.9.9.exe"},
        {"name":"FastLook-Setup-9.9.9.exe.sha256","browser_download_url":"https://github.com/a/FastLook-Setup-9.9.9.exe.sha256"}]})";
    EXPECT_FALSE(ParseLatestRelease(kWrongVersion));
}
