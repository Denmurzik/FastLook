#include "Strings.h"

#include <windows.h>

#include <atomic>

namespace fastlook::daemon
{
    namespace
    {
        struct Entry
        {
            std::wstring_view en;
            std::wstring_view ru;
        };

        // Order matches StrId.
        constexpr Entry kTable[] = {
            {L"Settings", L"Настройки"},
            {L"Pause", L"Пауза"},
            {L"Resume", L"Продолжить"},
            {L"Exit", L"Выход"},
            {L"FastLook", L"FastLook"},
            {L"FastLook (paused)", L"FastLook (пауза)"},
            {L"FastLook", L"FastLook"},
            {L"PowerToys Peek uses the same shortcut. Turn it off: PowerToys → Peek → Off.",
             L"PowerToys Peek использует то же сочетание клавиш. Отключите его: PowerToys → Peek → выключить."},
        };

        std::atomic<bool> g_russian{false};
    }

    std::wstring_view S(StrId id)
    {
        auto const& e = kTable[static_cast<size_t>(id)];
        return g_russian.load(std::memory_order_relaxed) ? e.ru : e.en;
    }

    void SetLanguage(core::LanguagePref pref)
    {
        bool russian = false;
        switch (pref)
        {
        case core::LanguagePref::Russian:
            russian = true;
            break;
        case core::LanguagePref::English:
            break;
        case core::LanguagePref::System:
            russian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
            break;
        }
        g_russian.store(russian, std::memory_order_relaxed);
    }
}
