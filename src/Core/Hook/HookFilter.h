#pragma once

#include <cstdint>

#include "Settings/Settings.h"

namespace fastlook::core::hook
{
    inline constexpr uintptr_t kInjectedMarker = 0x464C4B31; // 'FLK1' in dwExtraInfo of our own SendInput

    enum class ForegroundKind
    {
        Explorer,
        Desktop,
        OurPreview,
        Other
    };

    struct KeySnapshot
    {
        uint32_t vk = 0;
        bool keyDown = false;
        uintptr_t extraInfo = 0;
        bool ctrl = false, alt = false, shift = false, win = false;
        uint32_t timeMs = 0;
        uint32_t fgThreadId = 0;
    };

    struct FocusSnapshot
    {
        ForegroundKind fg = ForegroundKind::Other;
        wchar_t focusClass[64]{};
        bool focusUnderDefView = false;
        bool hasCaret = false;
        wchar_t caretClass[64]{};
        uint32_t guiFlags = 0;
    };

    struct HookConfig
    {
        ActivationKey key = ActivationKey::Space;
        bool desktopEnabled = false;
        bool paused = false;
    };

    enum class HookVerdict
    {
        Pass,
        Swallow,
        Trigger
    };

    // Keys Explorer may consume as type-ahead search: letters, digits, OEM punctuation (Russian layout letters live on OEM keys).
    bool IsTypeAheadKey(uint32_t vk) noexcept;

    // Runs inside the low-level keyboard hook: no allocation, no locks, no COM.
    class HookFilter
    {
    public:
        // focus: needed only for an activation keydown. Class-name buffers in FocusSnapshot must be NUL-terminated within 64 chars.
        HookVerdict Decide(KeySnapshot const& k, FocusSnapshot const* focus /*only for activation keydown*/, HookConfig const& c) noexcept;

        // Forgets held/swallow/type-ahead state. Call on first install, un-pause, and wherever a Space key-up may have
        // been lost (hook thread recreated, resume, unlock/reconnect, TaskbarCreated), not on the gapless periodic rehook. Not thread-safe: Decide() and Reset() must both run on the hook
        // thread (Task 27 posts a message to that thread to request a reset).
        void Reset() noexcept;

    private:
        bool m_held = false;
        bool m_swallowing = false;
        bool m_typeValid = false;
        uint32_t m_typeThread = 0;
        uint32_t m_typeTime = 0;
        uint32_t m_lastDownTime = 0;
    };
}
