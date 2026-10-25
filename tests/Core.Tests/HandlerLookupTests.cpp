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
    EXPECT_FALSE(IsBlockedPreviewHandler(L"{x}", L"C:\\Windows\\System32\\shell32.dll"));
}

TEST(HandlerLookup, UnresolvableServerPathIsBlocked)
{
    EXPECT_TRUE(IsBlockedPreviewHandler(L"{x}", L""));
}

TEST(HandlerLookup, KnownWebView2ClsidsAreBlockedWhateverThePath)
{
    constexpr wchar_t innocent[] = L"C:\\Windows\\System32\\fontext.dll";
    for (auto clsid : {L"{60789D87-9C3C-44AF-B18C-3DE2C2820ED3}",   // PowerToys Markdown
                       L"{FCDD4EED-41AA-492F-8A84-31A1546226E0}",   // PowerToys SVG
                       L"{D8034CFA-F34B-41FE-AD45-62FCBB52A6DA}",   // PowerToys Monaco
                       L"{A5A41CC7-02CB-41D4-8C9B-9087040D6098}",   // PowerToys PDF
                       L"{45769bcc-e8fd-42d0-947e-02beef77a1f5}",   // PowerToys managed Markdown
                       L"{3A84F9C2-6164-485C-A7D9-4B27F8AC009E}"})  // Edge PDF previewer
        EXPECT_TRUE(IsBlockedPreviewHandler(clsid, innocent)) << "clsid";
}

TEST(HandlerLookup, ManagedHandlersViaMscoreeAreBlocked)
{
    EXPECT_TRUE(IsBlockedPreviewHandler(L"{x}", L"C:\\Windows\\System32\\MSCOREE.DLL"));
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
