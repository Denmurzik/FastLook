#include "pch.h"
#include "SettingsGeneralPage.xaml.h"
#if __has_include("SettingsGeneralPage.g.cpp")
#include "SettingsGeneralPage.g.cpp"
#endif

#include <algorithm>
#include <array>

#include "Peek/PeekDetect.h"
#include "SettingsLauncher.h"
#include "System/AppPaths.h"
#include "System/Autostart.h"
#include "Util/SettingsUi.h"
#include "Util/Str.h"
#include "ViewerHost.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace fastlook;
using namespace fastlook::viewer;

namespace
{
    constexpr std::array kActivationKeys{core::ActivationKey::Space, core::ActivationKey::CtrlSpace, core::ActivationKey::AltSpace,
                                         core::ActivationKey::ShiftSpace};
    constexpr std::array<wchar_t const*, 4> kActivationKeyStrings{L"KeySpace", L"KeyCtrlSpace", L"KeyAltSpace", L"KeyShiftSpace"};
    constexpr std::array<uint32_t, 5> kIdleMinutes{5, 15, 30, 60, 0}; // 0 = never
    constexpr std::array kThemes{core::ThemePref::System, core::ThemePref::Light, core::ThemePref::Dark};
    constexpr std::array kLanguages{core::LanguagePref::System, core::LanguagePref::English, core::LanguagePref::Russian};

    template <typename T, size_t N>
    int IndexOf(std::array<T, N> const& values, T value)
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (values[i] == value)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    hstring KeyName(core::ActivationKey key)
    {
        return Str(L"Settings", kActivationKeyStrings[static_cast<size_t>(std::max(IndexOf(kActivationKeys, key), 0))]);
    }

    hstring IdleName(uint32_t minutes)
    {
        return minutes == 0 ? Str(L"Settings", L"IdleNever") : FormatOne(Str(L"Settings", L"IdleMinutes"), std::to_wstring(minutes));
    }
}

namespace winrt::FastLook::implementation
{
    SettingsGeneralPage::SettingsGeneralPage()
    {
        InitializeComponent();
        Build();
    }

    void SettingsGeneralPage::Build()
    {
        auto const general = ViewerHost::Get().Settings().general;
        m_key = general.activationKey;
        m_peekPattern = Str(L"Settings", L"PeekWarning");

        auto rows = Rows().Children();
        m_error = MakeErrorBar();
        rows.Append(m_error);

        m_peek = InfoBar();
        m_peek.Severity(InfoBarSeverity::Warning);
        m_peek.IsClosable(true);
        Button dismiss;
        dismiss.Content(box_value(Str(L"Settings", L"PeekDismiss")));
        dismiss.Click([this](auto const&, auto const&) {
            m_peek.IsOpen(false);
            ShowSaveResult(m_error, CommitSettings([](core::Settings& s) { s.general.peekWarning = false; }));
        });
        m_peek.ActionButton(dismiss);
        m_peek.IsOpen(false);
        rows.Append(m_peek);
        RefreshPeekWarning();

        // Launch with Windows lives in the Run key, not in settings.json.
        rows.Append(MakeSettingsRow(
            Str(L"Settings", L"LaunchAtLogin"), Str(L"Settings", L"LaunchAtLoginDesc"),
            MakeToggle(core::IsAutostartEnabled(), [this](bool on) {
                bool const ok = core::SetAutostart(on, core::CurrentAppPaths().exeDir / L"FastLook.exe");
                if (ok)
                {
                    m_error.IsOpen(false);
                }
                else
                {
                    ShowError(m_error, L"AutostartFailed");
                }
                return ok;
            })));

        rows.Append(MakeSettingsRow(Str(L"Settings", L"Desktop"), Str(L"Settings", L"DesktopDesc"),
                                    MakeToggle(general.desktopEnabled, [this](bool on) {
                                        bool const ok = CommitSettings([on](core::Settings& s) { s.general.desktopEnabled = on; });
                                        ShowSaveResult(m_error, ok);
                                        return ok;
                                    })));

        auto keyCombo = MakeCombo({KeyName(kActivationKeys[0]), KeyName(kActivationKeys[1]), KeyName(kActivationKeys[2]),
                                   KeyName(kActivationKeys[3])},
                                  IndexOf(kActivationKeys, general.activationKey));
        keyCombo.SelectionChanged([this, keyCombo](auto const&, auto const&) {
            int const index = keyCombo.SelectedIndex();
            if (index < 0)
            {
                return;
            }
            auto const key = kActivationKeys[static_cast<size_t>(index)];
            ShowSaveResult(m_error, CommitSettings([key](core::Settings& s) { s.general.activationKey = key; }));
            m_key = key;
            RefreshPeekWarning();
        });
        rows.Append(MakeSettingsRow(Str(L"Settings", L"ActivationKey"), Str(L"Settings", L"ActivationKeyDesc"), keyCombo));

        rows.Append(MakeSettingsRow(Str(L"Settings", L"Prewarm"), Str(L"Settings", L"PrewarmDesc"),
                                    MakeToggle(general.prewarm, [this](bool on) {
                                        bool const ok = CommitSettings([on](core::Settings& s) { s.general.prewarm = on; });
                                        ShowSaveResult(m_error, ok);
                                        return ok;
                                    })));

        auto idleCombo = MakeCombo({IdleName(kIdleMinutes[0]), IdleName(kIdleMinutes[1]), IdleName(kIdleMinutes[2]),
                                    IdleName(kIdleMinutes[3]), IdleName(kIdleMinutes[4])},
                                   IndexOf(kIdleMinutes, general.idleUnloadMinutes));
        idleCombo.SelectionChanged([this, idleCombo](auto const&, auto const&) {
            int const index = idleCombo.SelectedIndex();
            if (index < 0)
            {
                return;
            }
            uint32_t const minutes = kIdleMinutes[static_cast<size_t>(index)];
            ShowSaveResult(m_error, CommitSettings([minutes](core::Settings& s) { s.general.idleUnloadMinutes = minutes; }));
        });
        rows.Append(MakeSettingsRow(Str(L"Settings", L"Idle"), Str(L"Settings", L"IdleDesc"), idleCombo));

        auto themeCombo = MakeCombo({Str(L"Settings", L"ThemeSystem"), Str(L"Settings", L"ThemeLight"), Str(L"Settings", L"ThemeDark")},
                                    IndexOf(kThemes, general.theme));
        themeCombo.SelectionChanged([this, themeCombo](auto const&, auto const&) {
            int const index = themeCombo.SelectedIndex();
            if (index < 0)
            {
                return;
            }
            auto const theme = kThemes[static_cast<size_t>(index)];
            ShowSaveResult(m_error, CommitSettings([theme](core::Settings& s) { s.general.theme = theme; }));
            RefreshSettingsWindowTheme();
        });
        rows.Append(MakeSettingsRow(Str(L"Settings", L"Theme"), Str(L"Settings", L"ThemeDesc"), themeCombo));

        InfoBar languageNote;
        languageNote.Severity(InfoBarSeverity::Informational);
        languageNote.IsClosable(false);
        languageNote.Title(Str(L"Settings", L"LanguageRestart"));
        languageNote.IsOpen(false);
        auto languageCombo = MakeCombo({Str(L"Settings", L"LanguageSystem"), Str(L"Settings", L"LanguageEnglish"),
                                        Str(L"Settings", L"LanguageRussian")},
                                       IndexOf(kLanguages, general.language));
        languageCombo.SelectionChanged([this, languageCombo, languageNote](auto const&, auto const&) {
            int const index = languageCombo.SelectedIndex();
            if (index < 0)
            {
                return;
            }
            auto const language = kLanguages[static_cast<size_t>(index)];
            ShowSaveResult(m_error, CommitSettings([language](core::Settings& s) { s.general.language = language; }));
            languageNote.IsOpen(true);
        });
        rows.Append(MakeSettingsRow(Str(L"Settings", L"Language"), Str(L"Settings", L"LanguageDesc"), languageCombo));
        rows.Append(languageNote);
    }

    void SettingsGeneralPage::RefreshPeekWarning()
    {
        bool const show = ViewerHost::Get().Settings().general.peekWarning && core::DetectPeekConflictOnDisk(m_key).conflict;
        if (show)
        {
            m_peek.Title(FormatOne(m_peekPattern, std::wstring{KeyName(m_key)}));
        }
        m_peek.IsOpen(show);
    }
}
