#include <gtest/gtest.h>

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "Ipc/HostProtocol.h"

#pragma comment(lib, "shell32.lib")

using namespace fastlook::core::host;

TEST(HostProtocol, ArgsRoundTrip)
{
    HostArgs a{.parentHwnd = 0xABCDEF,
               .clsid = L"{8895B1C6-B41F-4C1C-A562-0D564250836F}",
               .file = L"C:\\Тест\\a b.docx",
               .rect = {0, 48, 800, 600},
               .dark = true,
               .debugHangAfterReady = true};
    auto s = FormatHostArgs(a);
    int n;
    auto argv = CommandLineToArgvW((L"x " + s).c_str(), &n);
    std::vector<std::wstring_view> v(argv + 1, argv + n);
    auto p = ParseHostArgs(v);
    LocalFree(argv);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->file, a.file);
    EXPECT_EQ(p->rect, a.rect);
    EXPECT_TRUE(p->dark);
    EXPECT_TRUE(p->debugHangAfterReady);
    EXPECT_FALSE(p->debugHang);
    EXPECT_EQ(p->parentHwnd, a.parentHwnd);
    EXPECT_EQ(p->clsid, a.clsid);
}

TEST(HostProtocol, ArgsQuotingEdgeCases)
{
    for (std::wstring file : {std::wstring(L"C:\\dir with space\\"), std::wstring(L"C:\\a\"b\\\\c.txt"), std::wstring(L"x")})
    {
        HostArgs a{.parentHwnd = 1, .clsid = L"{X}", .file = file, .rect = {1, 2, 3, 4}};
        auto s = FormatHostArgs(a);
        int n;
        auto argv = CommandLineToArgvW((L"x " + s).c_str(), &n);
        std::vector<std::wstring_view> v(argv + 1, argv + n);
        auto p = ParseHostArgs(v);
        LocalFree(argv);
        ASSERT_TRUE(p) << file.size();
        EXPECT_EQ(p->file, file);
        EXPECT_FALSE(p->dark);
    }
}

TEST(HostProtocol, ArgsRejectMissingOrMalformed)
{
    std::vector<std::wstring_view> none;
    EXPECT_FALSE(ParseHostArgs(none));
    std::vector<std::wstring_view> bad{L"--parent", L"zz", L"--clsid", L"{X}", L"--file", L"a", L"--rect", L"1,2,3,4"};
    EXPECT_FALSE(ParseHostArgs(bad));
    std::vector<std::wstring_view> shortRect{L"--parent", L"1", L"--clsid", L"{X}", L"--file", L"a", L"--rect", L"1,2,3"};
    EXPECT_FALSE(ParseHostArgs(shortRect));
}

TEST(HostProtocol, Commands)
{
    EXPECT_EQ(ParseHostCommand("rect 1 2 3 4")->rect, (fastlook::core::RectI{1, 2, 3, 4}));
    EXPECT_EQ(ParseHostCommand("quit")->cmd, HostCmd::Quit);
    EXPECT_FALSE(ParseHostCommand("rect 1 2"));
    EXPECT_FALSE(ParseHostCommand("rect 1 2 3 x"));
    EXPECT_EQ(ParseHostCommand(FormatHostCommand({HostCmd::Rect, {-5, 6, 7, 8}}))->rect, (fastlook::core::RectI{-5, 6, 7, 8}));
}

TEST(HostProtocol, Replies)
{
    EXPECT_EQ(FormatHostReply({HostStatus::Ready}), "ready\n");
    auto r = ParseHostReply("error 0x80004005 boom");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->hr, 0x80004005u);
    EXPECT_EQ(r->text, "boom");
    EXPECT_EQ(FormatHostReply({.status = HostStatus::Error, .hr = 0x80004005, .text = "a\nb"}), "error 0x80004005 a b\n");
    EXPECT_TRUE(ParseHostReply("ready\r\n"));
    EXPECT_FALSE(ParseHostReply("nonsense"));
}

TEST(HostProtocol, KeyReplies)
{
    EXPECT_EQ(FormatHostReply({.status = HostStatus::Key, .vk = VK_ESCAPE, .mods = kModCtrl}), "key 27 1\n");
    auto k = ParseHostReply("key 37 0");
    ASSERT_TRUE(k);
    EXPECT_EQ(k->status, HostStatus::Key);
    EXPECT_EQ(k->vk, 37u);
    EXPECT_EQ(k->mods, 0u);
    EXPECT_FALSE(ParseHostReply("key x"));
}

TEST(HostProtocol, ForwardedKeySet)
{
    for (uint32_t vk : {VK_SPACE, VK_ESCAPE, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_RETURN, VK_F11})
        EXPECT_TRUE(IsForwardedHostKey(vk, 0)) << vk;
    EXPECT_TRUE(IsForwardedHostKey('W', kModCtrl));
    EXPECT_FALSE(IsForwardedHostKey('C', kModCtrl));
    EXPECT_FALSE(IsForwardedHostKey('A', 0));
    EXPECT_FALSE(IsForwardedHostKey(VK_LEFT, kModShift));
}
