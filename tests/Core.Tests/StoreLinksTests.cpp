#include <gtest/gtest.h>

#include <string>

#include "FileTypes/StoreLinks.h"

using namespace fastlook::core;

TEST(StoreLinks, Ids)
{
    EXPECT_EQ(StoreProductId(*MissingCodecForImage(L"a.HEIC")), L"9PMMSR1CGPWG");
    EXPECT_EQ(StoreProductId(*MissingCodecForImage(L"a.heif")), L"9PMMSR1CGPWG");
    EXPECT_EQ(StoreProductId(*MissingCodecForImage(L"a.avif")), L"9MVZQVXJBQ9V");
    EXPECT_EQ(StoreProductId(*MissingCodecForImage(L"a.nef")), L"9NCTDW2W1BH8");
    EXPECT_EQ(StoreProductId(MissingCodec::Hevc), L"9NMZLZ57R3T7");
    EXPECT_EQ(StoreUri(L"9PMMSR1CGPWG"), L"ms-windows-store://pdp/?productid=9PMMSR1CGPWG");
    EXPECT_FALSE(MissingCodecForImage(L"a.png"));
    EXPECT_FALSE(MissingCodecForImage(L"noext"));
}

TEST(StoreLinks, EveryRawExtension)
{
    for (auto ext : {L"cr2", L"cr3", L"nef", L"nrw", L"arw", L"srf", L"sr2", L"dng", L"orf", L"rw2", L"raf", L"srw",
                     L"pef", L"rwl", L"3fr", L"erf", L"kdc", L"mrw", L"x3f"})
    {
        auto codec = MissingCodecForImage(std::wstring(L"IMG_0001.") + ext);
        ASSERT_TRUE(codec) << "extension";
        EXPECT_EQ(*codec, MissingCodec::Raw);
    }
}
