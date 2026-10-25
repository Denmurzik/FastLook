#include <Windows.h>

#include <cwchar>

#include "Hook/HookFilter.h"
#include "TestUtil.h"

using namespace fastlook::core;
using namespace fastlook::core::hook;

namespace
{
    FocusSnapshot ListFocus(ForegroundKind fg = ForegroundKind::Explorer)
    {
        FocusSnapshot f{.fg = fg, .focusUnderDefView = true};
        wcscpy_s(f.focusClass, L"DirectUIHWND");
        return f;
    }

    KeySnapshot Space(bool down, uint32_t t = 5000)
    {
        return {.vk = VK_SPACE, .keyDown = down, .timeMs = t, .fgThreadId = 42};
    }

    KeySnapshot Key(uint32_t vk, uint32_t t, uint32_t thread = 42, bool ctrl = false)
    {
        return {.vk = vk, .keyDown = true, .ctrl = ctrl, .timeMs = t, .fgThreadId = thread};
    }

    // Space press+release that must not trigger, leaving the filter ready for the next press.
    void Release(HookFilter& h, FocusSnapshot const& f, HookConfig const& c, uint32_t t = 9000)
    {
        h.Decide(Space(false, t), &f, c);
    }
}

TEST(HookFilter, SpaceInExplorerListTriggersAndSwallowsKeyUp)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Trigger);
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Swallow); // autorepeat
    EXPECT_EQ(h.Decide(Space(false), &f, c), HookVerdict::Swallow);
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Trigger); // next press
}

TEST(HookFilter, LegacyListViewTriggers)
{
    HookFilter h;
    auto f = ListFocus();
    wcscpy_s(f.focusClass, L"SysListView32");
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Trigger);
}

TEST(HookFilter, ClassNamesAreCaseInsensitive)
{
    HookFilter h;
    auto f = ListFocus();
    wcscpy_s(f.focusClass, L"directuihwnd");
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Trigger);
}

TEST(HookFilter, AutorepeatAfterPassIsPassed)
{
    HookFilter h;
    auto f = ListFocus();
    f.hasCaret = true;
    wcscpy_s(f.caretClass, L"Edit");
    HookConfig c;
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(false), &f, c), HookVerdict::Pass);
}

TEST(HookFilter, AutorepeatDoesNotRetriggerEvenIfFocusChanges)
{
    HookFilter h;
    auto list = ListFocus();
    HookConfig c;
    EXPECT_EQ(h.Decide(Space(true), &list, c), HookVerdict::Trigger);
    auto edit = ListFocus();
    edit.hasCaret = true;
    wcscpy_s(edit.caretClass, L"Edit");
    EXPECT_EQ(h.Decide(Space(true), &edit, c), HookVerdict::Swallow);
    EXPECT_EQ(h.Decide(Space(false), &edit, c), HookVerdict::Swallow);
}

TEST(HookFilter, KeyUpWithoutPriorDownPasses)
{
    HookFilter h;
    auto f = ListFocus();
    EXPECT_EQ(h.Decide(Space(false), &f, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, RenameEditPasses)
{
    HookFilter h;
    auto f = ListFocus();
    f.hasCaret = true;
    wcscpy_s(f.caretClass, L"Edit");
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, RichEditAndCaseVariantsPass)
{
    for (wchar_t const* cls : {L"RichEditD2DPT", L"RICHEDIT50W", L"edit", L"EditControl"})
    {
        HookFilter h;
        auto f = ListFocus();
        f.hasCaret = true;
        wcscpy_s(f.caretClass, cls);
        EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass) << "class " << fastlook::core::ToUtf8(cls);
    }
}

TEST(HookFilter, CaretInNonEditClassStillTriggersInList)
{
    // Explorer's list view can own a caret-less focus rect; a non-edit caret owner must not block.
    HookFilter h;
    auto f = ListFocus();
    f.hasCaret = true;
    wcscpy_s(f.caretClass, L"DirectUIHWND");
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Trigger);
}

// The caret class is only consulted when GUI_CARETBLINKING-style caret info exists (hasCaret); a stale class buffer
// must not block by itself.
TEST(HookFilter, EditClassWithoutCaretFlagDoesNotBlockByItself)
{
    HookFilter h;
    auto f = ListFocus();
    f.hasCaret = false;
    wcscpy_s(f.caretClass, L"Edit");
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Trigger);
}

TEST(HookFilter, SearchBoxPasses)
{
    HookFilter h;
    FocusSnapshot f{.fg = ForegroundKind::Explorer, .focusUnderDefView = false};
    wcscpy_s(f.focusClass, L"Microsoft.UI.Content.DesktopChildSiteBridge");
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, AddressBarAndNavigationPanePass)
{
    for (wchar_t const* cls : {L"Edit", L"ToolbarWindow32", L"SysTreeView32", L"NamespaceTreeControl", L"ComboBox", L"Breadcrumb Parent"})
    {
        HookFilter h;
        FocusSnapshot f{.fg = ForegroundKind::Explorer, .focusUnderDefView = false};
        wcscpy_s(f.focusClass, cls);
        EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass) << "class " << fastlook::core::ToUtf8(cls);
    }
}

TEST(HookFilter, ItemViewClassOutsideDefViewPasses)
{
    // A DirectUIHWND that is not under the shell view (e.g. the navigation pane host) must not trigger.
    HookFilter h;
    auto f = ListFocus();
    f.focusUnderDefView = false;
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, UnderDefViewButOtherClassPasses)
{
    HookFilter h;
    auto f = ListFocus();
    wcscpy_s(f.focusClass, L"Edit");
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, EmptyFocusClassPasses)
{
    HookFilter h;
    FocusSnapshot f{.fg = ForegroundKind::Explorer, .focusUnderDefView = true};
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, MissingFocusSnapshotPasses)
{
    HookFilter h;
    EXPECT_EQ(h.Decide(Space(true), nullptr, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, MenuModePasses)
{
    for (uint32_t flag : {GUI_INMENUMODE, GUI_POPUPMENUMODE, GUI_SYSTEMMENUMODE, GUI_INMOVESIZE})
    {
        HookFilter h;
        auto f = ListFocus();
        f.guiFlags = flag;
        EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass) << "flag " << flag;
    }
}

TEST(HookFilter, HarmlessGuiFlagsDoNotBlock)
{
    HookFilter h;
    auto f = ListFocus();
    f.guiFlags = GUI_CARETBLINKING;
    EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Trigger);
}

TEST(HookFilter, ModifiersAndConfiguredKey)
{
    auto f = ListFocus();
    auto spaceWith = [](bool ctrl, bool alt, bool shift, bool win) {
        KeySnapshot k = Space(true);
        k.ctrl = ctrl;
        k.alt = alt;
        k.shift = shift;
        k.win = win;
        return k;
    };
    {
        HookFilter h;
        HookConfig c; // Space
        EXPECT_EQ(h.Decide(spaceWith(true, false, false, false), &f, c), HookVerdict::Pass);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(false, true, false, false), &f, c), HookVerdict::Pass);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(false, false, true, false), &f, c), HookVerdict::Pass);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(false, false, false, true), &f, c), HookVerdict::Pass);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(false, false, false, false), &f, c), HookVerdict::Trigger);
    }
    {
        HookFilter h;
        HookConfig c{.key = ActivationKey::CtrlSpace};
        EXPECT_EQ(h.Decide(spaceWith(true, false, false, false), &f, c), HookVerdict::Trigger);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(false, false, false, false), &f, c), HookVerdict::Pass);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(true, false, true, false), &f, c), HookVerdict::Pass);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(true, true, false, false), &f, c), HookVerdict::Pass);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(true, false, false, true), &f, c), HookVerdict::Pass);
    }
    {
        HookFilter h;
        HookConfig c{.key = ActivationKey::AltSpace};
        EXPECT_EQ(h.Decide(spaceWith(false, true, false, false), &f, c), HookVerdict::Trigger);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(false, false, false, false), &f, c), HookVerdict::Pass);
    }
    {
        HookFilter h;
        HookConfig c{.key = ActivationKey::ShiftSpace};
        EXPECT_EQ(h.Decide(spaceWith(false, false, true, false), &f, c), HookVerdict::Trigger);
        Release(h, f, c);
        EXPECT_EQ(h.Decide(spaceWith(true, false, true, false), &f, c), HookVerdict::Pass);
    }
}

TEST(HookFilter, ModifiedSpaceKeyUpIsNotSwallowed)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    KeySnapshot down = Space(true);
    down.ctrl = true;
    EXPECT_EQ(h.Decide(down, &f, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(false), &f, c), HookVerdict::Pass);
}

TEST(HookFilter, OurMarkerPassesForeignInjectedTriggers)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    KeySnapshot ours = Space(true);
    ours.extraInfo = kInjectedMarker;
    EXPECT_EQ(h.Decide(ours, &f, c), HookVerdict::Pass);
    ours.keyDown = false;
    EXPECT_EQ(h.Decide(ours, &f, c), HookVerdict::Pass);
    KeySnapshot foreign = Space(true);
    foreign.extraInfo = 0x1234;
    EXPECT_EQ(h.Decide(foreign, &f, c), HookVerdict::Trigger);
}

TEST(HookFilter, OurMarkerKeyUpDoesNotCancelSwallow)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Trigger);
    KeySnapshot ours = Space(false);
    ours.extraInfo = kInjectedMarker;
    EXPECT_EQ(h.Decide(ours, &f, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(false), &f, c), HookVerdict::Swallow); // the real key-up is still swallowed
}

TEST(HookFilter, DesktopNeedsSetting)
{
    auto f = ListFocus(ForegroundKind::Desktop);
    {
        HookFilter h;
        HookConfig c{.desktopEnabled = false};
        EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Pass);
    }
    {
        HookFilter h;
        HookConfig c{.desktopEnabled = true};
        EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Trigger);
    }
}

TEST(HookFilter, DesktopSettingDoesNotAffectExplorer)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c{.desktopEnabled = false};
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Trigger);
}

TEST(HookFilter, OurPreviewOtherAppPausedPass)
{
    {
        HookFilter h;
        auto f = ListFocus(ForegroundKind::OurPreview);
        EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{}), HookVerdict::Pass);
    }
    {
        HookFilter h;
        auto f = ListFocus(ForegroundKind::Other);
        EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{.desktopEnabled = true}), HookVerdict::Pass);
    }
    {
        HookFilter h;
        auto f = ListFocus();
        EXPECT_EQ(h.Decide(Space(true), &f, HookConfig{.paused = true}), HookVerdict::Pass);
    }
}

TEST(HookFilter, PausedAutorepeatAndKeyUpPass)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c{.paused = true};
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(false), &f, c), HookVerdict::Pass);
    c.paused = false;
    EXPECT_EQ(h.Decide(Space(true), &f, c), HookVerdict::Trigger);
}

TEST(HookFilter, NonSpaceKeysAlwaysPass)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    for (uint32_t vk : {static_cast<uint32_t>(VK_RETURN), static_cast<uint32_t>(VK_ESCAPE), static_cast<uint32_t>(VK_F2), uint32_t{'A'}})
    {
        EXPECT_EQ(h.Decide(Key(vk, 100), &f, c), HookVerdict::Pass);
        KeySnapshot up = Key(vk, 120);
        up.keyDown = false;
        EXPECT_EQ(h.Decide(up, &f, c), HookVerdict::Pass);
    }
}

TEST(HookFilter, TypeAheadWindowIs1000ms)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    h.Decide({.vk = 'B', .keyDown = true, .timeMs = 1000, .fgThreadId = 42}, nullptr, c);
    EXPECT_EQ(h.Decide(Space(true, 1999), &f, c), HookVerdict::Pass);
    HookFilter h2; // a passing Space extends the window, so the exact boundary needs a fresh filter
    h2.Decide({.vk = 'B', .keyDown = true, .timeMs = 1000, .fgThreadId = 42}, nullptr, c);
    EXPECT_EQ(h2.Decide(Space(true, 2000), &f, c), HookVerdict::Trigger);
}

TEST(HookFilter, TypeAheadAppliesToDigitsAndNumpad)
{
    for (uint32_t vk : {uint32_t{'0'}, uint32_t{'9'}, static_cast<uint32_t>(VK_NUMPAD5)})
    {
        HookFilter h;
        auto f = ListFocus();
        h.Decide(Key(vk, 1000), nullptr, HookConfig{});
        EXPECT_EQ(h.Decide(Space(true, 1300), &f, HookConfig{}), HookVerdict::Pass) << "vk " << vk;
    }
}

TEST(HookFilter, TypeAheadRussianOemKeyCounts)
{
    // Russian letters sit on OEM keys: х = [, ъ = ], ж = ;, э = ', б = comma, ю = period, ё = `
    for (uint32_t vk : {static_cast<uint32_t>(VK_OEM_4), static_cast<uint32_t>(VK_OEM_6), static_cast<uint32_t>(VK_OEM_1),
                        static_cast<uint32_t>(VK_OEM_7), static_cast<uint32_t>(VK_OEM_COMMA), static_cast<uint32_t>(VK_OEM_PERIOD),
                        static_cast<uint32_t>(VK_OEM_3)})
    {
        HookFilter h;
        auto f = ListFocus();
        h.Decide(Key(vk, 1000), nullptr, HookConfig{});
        EXPECT_EQ(h.Decide(Space(true, 1200), &f, HookConfig{}), HookVerdict::Pass) << "vk " << vk;
    }
}

TEST(HookFilter, TypeAheadImePacketCounts)
{
    HookFilter h;
    auto f = ListFocus();
    h.Decide(Key(VK_PACKET, 1000), nullptr, HookConfig{});
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, TypeAheadOtherThreadIgnored)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    h.Decide({.vk = 'B', .keyDown = true, .timeMs = 1000, .fgThreadId = 7}, nullptr, c);
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, c), HookVerdict::Trigger);
}

TEST(HookFilter, CtrlLetterIsNotTypeAhead)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    h.Decide(Key('C', 1000, 42, true), nullptr, c);
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, c), HookVerdict::Trigger);
}

TEST(HookFilter, AltAndWinLettersAreNotTypeAhead)
{
    for (int which = 0; which < 2; ++which)
    {
        HookFilter h;
        auto f = ListFocus();
        KeySnapshot k = Key('D', 1000);
        (which == 0 ? k.alt : k.win) = true;
        h.Decide(k, nullptr, HookConfig{});
        EXPECT_EQ(h.Decide(Space(true, 1100), &f, HookConfig{}), HookVerdict::Trigger);
    }
}

TEST(HookFilter, ShiftedLetterIsTypeAhead)
{
    HookFilter h;
    auto f = ListFocus();
    KeySnapshot k = Key('B', 1000);
    k.shift = true;
    h.Decide(k, nullptr, HookConfig{});
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, HookConfig{}), HookVerdict::Pass);
}

TEST(HookFilter, KeyUpDoesNotArmTypeAhead)
{
    HookFilter h;
    auto f = ListFocus();
    KeySnapshot up = Key('B', 1000);
    up.keyDown = false;
    h.Decide(up, nullptr, HookConfig{});
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, HookConfig{}), HookVerdict::Trigger);
}

TEST(HookFilter, NonTextKeysDoNotArmTypeAhead)
{
    for (uint32_t vk : {static_cast<uint32_t>(VK_DOWN), static_cast<uint32_t>(VK_RETURN), static_cast<uint32_t>(VK_F2),
                        static_cast<uint32_t>(VK_BACK), static_cast<uint32_t>(VK_TAB), static_cast<uint32_t>(VK_SHIFT)})
    {
        HookFilter h;
        auto f = ListFocus();
        h.Decide(Key(vk, 1000), nullptr, HookConfig{});
        EXPECT_EQ(h.Decide(Space(true, 1100), &f, HookConfig{}), HookVerdict::Trigger) << "vk " << vk;
    }
}

TEST(HookFilter, OurInjectedLetterDoesNotArmTypeAhead)
{
    HookFilter h;
    auto f = ListFocus();
    KeySnapshot k = Key('B', 1000);
    k.extraInfo = kInjectedMarker;
    h.Decide(k, nullptr, HookConfig{});
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, HookConfig{}), HookVerdict::Trigger);
}

TEST(HookFilter, NoTypeAheadHistoryMeansNoBlock)
{
    // A fresh filter must not treat (thread 0, time 0) as a recent keystroke.
    HookFilter h;
    auto f = ListFocus();
    KeySnapshot k = Space(true, 10);
    k.fgThreadId = 0;
    EXPECT_EQ(h.Decide(k, &f, HookConfig{}), HookVerdict::Trigger);
}

TEST(HookFilter, TickCountWrap)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    h.Decide({.vk = 'B', .keyDown = true, .timeMs = 0xFFFFFF00u, .fgThreadId = 42}, nullptr, c);
    EXPECT_EQ(h.Decide(Space(true, 0x00000100u), &f, c), HookVerdict::Pass);
    HookFilter h2;
    h2.Decide({.vk = 'B', .keyDown = true, .timeMs = 0xFFFFFF00u, .fgThreadId = 42}, nullptr, c);
    EXPECT_EQ(h2.Decide(Space(true, 0x00000400u), &f, c), HookVerdict::Trigger); // diff 1280 ms
}

TEST(HookFilter, TypeAheadStillBlocksAcrossRepeatedLetters)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    h.Decide(Key('A', 1000), nullptr, c);
    h.Decide(Key('B', 1900), nullptr, c);
    EXPECT_EQ(h.Decide(Space(true, 2800), &f, c), HookVerdict::Pass); // 900 ms after the last letter
}

TEST(HookFilter, IsTypeAheadKeyTable)
{
    EXPECT_TRUE(IsTypeAheadKey('A'));
    EXPECT_TRUE(IsTypeAheadKey('Z'));
    EXPECT_TRUE(IsTypeAheadKey('5'));
    EXPECT_TRUE(IsTypeAheadKey(VK_NUMPAD0));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_1));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_PLUS));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_COMMA));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_MINUS));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_PERIOD));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_2));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_3));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_4));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_5));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_6));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_7));
    EXPECT_TRUE(IsTypeAheadKey(VK_OEM_102));
    EXPECT_FALSE(IsTypeAheadKey(VK_SPACE));
    EXPECT_FALSE(IsTypeAheadKey(VK_RETURN));
    EXPECT_FALSE(IsTypeAheadKey(VK_ESCAPE));
    EXPECT_FALSE(IsTypeAheadKey(VK_LEFT));
    EXPECT_FALSE(IsTypeAheadKey(VK_F1));
    EXPECT_FALSE(IsTypeAheadKey(VK_CONTROL));
    EXPECT_FALSE(IsTypeAheadKey(0));
}

TEST(HookFilter, LostKeyUpWhileSwallowingDoesNotSwallowNextTypedSpace)
{
    HookFilter h;
    auto list = ListFocus();
    HookConfig c;
    EXPECT_EQ(h.Decide(Space(true), &list, c), HookVerdict::Trigger);
    // key-up lost (Win+L); later the user renames: the first Space press must reach the edit box
    auto edit = ListFocus();
    edit.hasCaret = true;
    wcscpy_s(edit.caretClass, L"Edit");
    h.Reset();
    EXPECT_EQ(h.Decide(Space(true, 20000), &edit, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(false, 20010), &edit, c), HookVerdict::Pass);
}

TEST(HookFilter, ResetClearsHeldAndTypeAhead)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    h.Decide(Key('B', 1000), nullptr, c);
    h.Reset();
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, c), HookVerdict::Trigger);
}

TEST(HookFilter, StaleSwallowIsHealedOnNextFreshKeyDown)
{
    HookFilter h;
    auto list = ListFocus();
    HookConfig c;
    EXPECT_EQ(h.Decide(Space(true), &list, c), HookVerdict::Trigger);
    EXPECT_EQ(h.Decide(Space(false), &list, c), HookVerdict::Swallow);
    // m_held is clear; a fresh keydown that passes must not be reported as swallowed afterwards
    auto edit = ListFocus();
    edit.hasCaret = true;
    wcscpy_s(edit.caretClass, L"Edit");
    EXPECT_EQ(h.Decide(Space(true, 6000), &edit, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(true, 6050), &edit, c), HookVerdict::Pass);
    EXPECT_EQ(h.Decide(Space(false, 6100), &edit, c), HookVerdict::Pass);
}

TEST(HookFilter, SpaceInsideTypeAheadWindowExtendsIt)
{
    HookFilter h;
    auto f = ListFocus();
    HookConfig c;
    h.Decide(Key('B', 0), nullptr, c);
    EXPECT_EQ(h.Decide(Space(true, 600), &f, c), HookVerdict::Pass);
    h.Decide(Space(false, 650), &f, c);
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, c), HookVerdict::Pass);
    h.Decide(Space(false, 1150), &f, c);
    EXPECT_EQ(h.Decide(Space(true, 2200), &f, c), HookVerdict::Trigger);
}

TEST(HookFilter, ImeProcessKeyArmsTypeAhead)
{
    HookFilter h;
    auto f = ListFocus();
    h.Decide(Key(VK_PROCESSKEY, 1000), nullptr, HookConfig{});
    EXPECT_EQ(h.Decide(Space(true, 1100), &f, HookConfig{}), HookVerdict::Pass);
}
