#include "Hook/HookFilter.h"

#include <Windows.h>

#include <utility>

namespace fastlook::core::hook
{
    namespace
    {
        constexpr uint32_t kTypeAheadWindowMs = 1000;
        constexpr uint32_t kStaleHeldMs = 1500; // longer than any keyboard autorepeat delay or interval

        bool ModifiersMatch(ActivationKey key, KeySnapshot const& k) noexcept
        {
            if (k.win) return false;
            return k.ctrl == (key == ActivationKey::CtrlSpace) && k.alt == (key == ActivationKey::AltSpace) &&
                   k.shift == (key == ActivationKey::ShiftSpace);
        }

        wchar_t Lower(wchar_t c) noexcept
        {
            return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }

        // ASCII case-insensitive prefix test; s is NUL-terminated (fixed buffers in FocusSnapshot).
        bool StartsWithI(wchar_t const* s, wchar_t const* prefix) noexcept
        {
            for (; *prefix; ++s, ++prefix)
            {
                if (Lower(*s) != Lower(*prefix)) return false;
            }
            return true;
        }

        bool EqualsI(wchar_t const* s, wchar_t const* other) noexcept
        {
            for (; *s && *other; ++s, ++other)
            {
                if (Lower(*s) != Lower(*other)) return false;
            }
            return *s == *other;
        }

        bool IsEditClass(wchar_t const* c) noexcept
        {
            return StartsWithI(c, L"Edit") || StartsWithI(c, L"RichEdit");
        }

        bool IsItemViewClass(wchar_t const* c) noexcept
        {
            return EqualsI(c, L"DirectUIHWND") || EqualsI(c, L"SysListView32");
        }
    }

    bool IsTypeAheadKey(uint32_t vk) noexcept
    {
        if (vk >= 'A' && vk <= 'Z') return true;
        if (vk >= '0' && vk <= '9') return true;
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return true;
        if (vk >= VK_OEM_1 && vk <= VK_OEM_3) return true; // ; = , - . / `
        if (vk >= VK_OEM_4 && vk <= VK_OEM_8) return true; // [ \ ] ' and layout-specific
        return vk == VK_OEM_102 || vk == VK_PACKET || vk == VK_PROCESSKEY; // PROCESSKEY: IME composition in progress
    }

    void HookFilter::Reset() noexcept
    {
        *this = HookFilter{};
    }

    HookVerdict HookFilter::Decide(KeySnapshot const& k, FocusSnapshot const* f, HookConfig const& c) noexcept
    {
        if (k.extraInfo == kInjectedMarker) return HookVerdict::Pass;
        if (k.vk != VK_SPACE)
        {
            if (k.keyDown && !k.ctrl && !k.alt && !k.win && IsTypeAheadKey(k.vk))
            {
                m_typeThread = k.fgThreadId;
                m_typeTime = k.timeMs;
                m_typeValid = true;
            }
            return HookVerdict::Pass;
        }
        if (!k.keyDown)
        {
            m_held = false;
            bool s = std::exchange(m_swallowing, false);
            return s ? HookVerdict::Swallow : HookVerdict::Pass;
        }
        if (m_held && k.timeMs - m_lastDownTime > kStaleHeldMs)
        {
            m_held = false; // no keydown for longer than any autorepeat gap: the previous key-up was lost
            m_swallowing = false;
        }
        m_lastDownTime = k.timeMs;
        if (m_held) return m_swallowing ? HookVerdict::Swallow : HookVerdict::Pass; // autorepeat
        m_held = true;
        if (c.paused || !f || !ModifiersMatch(c.key, k)) return HookVerdict::Pass;
        if (f->fg == ForegroundKind::Desktop ? !c.desktopEnabled : f->fg != ForegroundKind::Explorer) return HookVerdict::Pass;
        if (f->guiFlags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE)) return HookVerdict::Pass;
        if (f->hasCaret && IsEditClass(f->caretClass)) return HookVerdict::Pass;
        // Keyboard-only desktop use (Win+D, arrows) leaves focus on the desktop's SHELLDLL_DefView itself, which is the item view there.
        bool const desktopView = f->fg == ForegroundKind::Desktop && EqualsI(f->focusClass, L"SHELLDLL_DefView");
        if (!f->focusUnderDefView || !(IsItemViewClass(f->focusClass) || desktopView)) return HookVerdict::Pass;
        if (m_typeValid && m_typeThread == k.fgThreadId && k.timeMs - m_typeTime < kTypeAheadWindowMs) // uint32 wrap-safe
        {
            m_typeTime = k.timeMs; // a space typed into the search string extends the window
            return HookVerdict::Pass;
        }
        m_swallowing = true;
        return HookVerdict::Trigger;
    }
}
