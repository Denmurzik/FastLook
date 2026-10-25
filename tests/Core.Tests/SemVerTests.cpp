#include <gtest/gtest.h>

#include <iterator>

#include "Update/SemVer.h"

using namespace fastlook::core;

TEST(SemVer, ParsesAndRejects)
{
    EXPECT_EQ(ParseSemVer("v1.2.3"), (SemVer{1, 2, 3, ""}));
    EXPECT_EQ(ParseSemVer("1.0.0-rc.1+b5")->prerelease, "rc.1");
    for (auto bad : {"1.0", "01.0.0", "1.0.0.0", "", "v", "1.x.0"})
    {
        EXPECT_FALSE(ParseSemVer(bad)) << bad;
    }
}

TEST(SemVer, RejectsMalformedPrereleaseAndOverflow)
{
    for (auto bad : {"1.0.0-", "1.0.0-a..b", "1.0.0-01", "1.0.0+", "4294967296.0.0", "1.0.0-a_b"})
    {
        EXPECT_FALSE(ParseSemVer(bad)) << bad;
    }
    EXPECT_TRUE(ParseSemVer("4294967295.0.0"));
    EXPECT_TRUE(ParseSemVer("1.0.0-0.3.7"));
}

TEST(SemVer, Precedence)
{
    const char* order[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
                           "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0"};
    for (size_t i = 0; i + 1 < std::size(order); ++i)
    {
        EXPECT_LT(CompareSemVer(*ParseSemVer(order[i]), *ParseSemVer(order[i + 1])), 0) << order[i];
        EXPECT_GT(CompareSemVer(*ParseSemVer(order[i + 1]), *ParseSemVer(order[i])), 0) << order[i];
    }
    EXPECT_EQ(CompareSemVer(*ParseSemVer("1.0.0+a"), *ParseSemVer("1.0.0+b")), 0);
    EXPECT_LT(CompareSemVer(*ParseSemVer("1.0.0-1"), *ParseSemVer("1.0.0-a")), 0);
    EXPECT_LT(CompareSemVer(*ParseSemVer("1.0.0-9"), *ParseSemVer("1.0.0-10")), 0);
}
