#include <gtest/gtest.h>

#include <Windows.h>
#include <objbase.h>

#include "Shell/HandlerLookup.h"

using namespace fastlook::core;

namespace
{
    struct CoInit
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~CoInit()
        {
            if (SUCCEEDED(hr))
                CoUninitialize();
        }
    };
}

TEST(HandlerLookup, BlocklistByPathAndClsid)
{
    EXPECT_TRUE(IsBlockedPreviewHandler(L"{00000000-0000-0000-0000-000000000001}", L"C:\\Program Files\\PowerToys\\PowerToys.MarkdownPreviewHandler.comhost.dll"));
    EXPECT_TRUE(IsBlockedPreviewHandler(L"{x}", L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\x.dll"));
    EXPECT_TRUE(IsBlockedPreviewHandler(L"{x}", L"C:\\Windows\\System32\\inetcomm.dll"));
    EXPECT_TRUE(IsBlockedPreviewHandler(L"{x}", L"c:\\windows\\system32\\MSHTML.DLL"));
    EXPECT_TRUE(IsBlockedPreviewHandler(L"{x}", L"c:\\program files\\POWERTOYS\\x.dll"));
    EXPECT_FALSE(IsBlockedPreviewHandler(L"{x}", L"C:\\Windows\\System32\\fontext.dll"));
    EXPECT_FALSE(IsBlockedPreviewHandler(L"{x}", L""));
}

TEST(HandlerLookup, WicKnowsPng)
{
    CoInit com;
    EXPECT_TRUE(HasWicDecoder(L".png"));
    EXPECT_TRUE(HasWicDecoder(L"JPG"));
    EXPECT_FALSE(HasWicDecoder(L".fastlookzz"));
}

TEST(HandlerLookup, PerceivedKindOfKnownExtensions)
{
    EXPECT_EQ(GetPerceivedKind(L".png"), PerceivedKind::Image);
    EXPECT_EQ(GetPerceivedKind(L".txt"), PerceivedKind::Text);
    EXPECT_EQ(GetPerceivedKind(L".fastlookzz"), PerceivedKind::Unknown);
}

TEST(HandlerLookup, NoPreviewHandlerForUnknownExtension)
{
    EXPECT_FALSE(FindPreviewHandlerClsid(L".fastlookzz"));
    EXPECT_TRUE(HandlerServerPath(L"{00000000-0000-0000-0000-000000000000}").empty());
}
