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

    struct PeekState
    {
        fastlook::viewer::WeakBar bar;
        core::ActivationKey key;
        hstring pattern;
    };

    void RefreshPeekWarning(PeekState const& state)
    {
        auto const bar = state.bar.get();
        if (!bar)
        {
            return;
        }
        bool const show = ViewerHost::Get().Settings().general.peekWarning && core::DetectPeekConflictOnDisk(state.key).conflict;
        if (show)
        {
            bar.Title(FormatOne(state.pattern, std::wstring{KeyName(state.key)}));
        }
        bar.IsOpen(show);
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

        auto rows = Rows().Children();
        m_error = MakeErrorBar();
        rows.Append(m_error);
        WeakBar const error = make_weak(m_error);

        // The Peek warning (shown only while PowerToys Peek is on and shares the activation key); its toggle is always there.
        InfoBar peek;
        peek.Severity(InfoBarSeverity::Warning);
        peek.IsClosable(true);
        peek.IsOpen(false);
        rows.Append(peek);
        auto const peekState = std::make_shared<PeekState>(PeekState{make_weak(peek), general.activationKey, Str(L"Settings", L"PeekWarning")});
        RefreshPeekWarning(*peekState);

        // Launch with Windows lives in the Run key, not in settings.json.
        rows.Append(MakeSettingsRow(
            Str(L"Settings", L"LaunchAtLogin"), Str(L"Settings", L"LaunchAtLoginDesc"),
            MakeToggle(core::IsAutostartEnabled(), [error](bool on) {
                bool const ok = core::SetAutostart(on, core::CurrentAppPaths().exeDir / L"FastLook.exe");
                if (ok)
                {
                    ShowSaveResult(error, true);
                }
                else
                {
                    ShowError(error, L"AutostartFailed");
                }
                return ok;
            })));

        rows.Append(MakeSettingsRow(Str(L"Settings", L"Desktop"), Str(L"Settings", L"DesktopDesc"),
                                    MakeToggle(general.desktopEnabled, [error](bool on) {
                                        bool const ok = CommitSettings([on](core::Settings& s) { s.general.desktopEnabled = on; });
                                        ShowSaveResult(error, ok);
                                        return ok;
                                    })));

        rows.Append(MakeSettingsRow(
            Str(L"Settings", L"ActivationKey"), Str(L"Settings", L"ActivationKeyDesc"),
            MakeCombo({KeyName(kActivationKeys[0]), KeyName(kActivationKeys[1]), KeyName(kActivationKeys[2]), KeyName(kActivationKeys[3])},
                      IndexOf(kActivationKeys, general.activationKey), [error, peekState](int index) {
                          auto const key = kActivationKeys[static_cast<size_t>(index)];
                          bool const ok = CommitSettings([key](core::Settings& s) { s.general.activationKey = key; });
                          ShowSaveResult(error, ok);
                          if (ok)
                          {
                              peekState->key = key;
                              RefreshPeekWarning(*peekState);
                          }
                          return ok;
                      })));

        rows.Append(MakeSettingsRow(Str(L"Settings", L"PeekToggle"), Str(L"Settings", L"PeekToggleDesc"),
                                    MakeToggle(general.peekWarning, [error, peekState](bool on) {
                                        bool const ok = CommitSettings([on](core::Settings& s) { s.general.peekWarning = on; });
                                        ShowSaveResult(error, ok);
                                        if (ok)
                                        {
                                            RefreshPeekWarning(*peekState);
                                        }
                                        return ok;
                                    })));

        rows.Append(MakeSettingsRow(Str(L"Settings", L"Prewarm"), Str(L"Settings", L"PrewarmDesc"),
                                    MakeToggle(general.prewarm, [error](bool on) {
                                        bool const ok = CommitSettings([on](core::Settings& s) { s.general.prewarm = on; });
                                        ShowSaveResult(error, ok);
                                        return ok;
                                    })));

        rows.Append(MakeSettingsRow(
            Str(L"Settings", L"Idle"), Str(L"Settings", L"IdleDesc"),
            MakeCombo({IdleName(kIdleMinutes[0]), IdleName(kIdleMinutes[1]), IdleName(kIdleMinutes[2]), IdleName(kIdleMinutes[3]),
                       IdleName(kIdleMinutes[4])},
                      IndexOf(kIdleMinutes, general.idleUnloadMinutes), [error](int index) {
                          uint32_t const minutes = kIdleMinutes[static_cast<size_t>(index)];
                          bool const ok = CommitSettings([minutes](core::Settings& s) { s.general.idleUnloadMinutes = minutes; });
                          ShowSaveResult(error, ok);
                          return ok;
                      })));

        rows.Append(MakeSettingsRow(
            Str(L"Settings", L"Theme"), Str(L"Settings", L"ThemeDesc"),
            MakeCombo({Str(L"Settings", L"ThemeSystem"), Str(L"Settings", L"ThemeLight"), Str(L"Settings", L"ThemeDark")},
                      IndexOf(kThemes, general.theme), [error](int index) {
                          auto const theme = kThemes[static_cast<size_t>(index)];
                          bool const ok = CommitSettings([theme](core::Settings& s) { s.general.theme = theme; });
                          ShowSaveResult(error, ok);
                          if (ok)
                          {
                              RefreshSettingsWindowTheme();
                          }
                          return ok;
                      })));

        InfoBar languageNote;
        languageNote.Severity(InfoBarSeverity::Informational);
        languageNote.IsClosable(false);
        languageNote.Title(Str(L"Settings", L"LanguageRestart"));
        languageNote.IsOpen(false);
        auto const weakNote = make_weak(languageNote);
        rows.Append(MakeSettingsRow(
            Str(L"Settings", L"Language"), Str(L"Settings", L"LanguageDesc"),
            MakeCombo({Str(L"Settings", L"LanguageSystem"), Str(L"Settings", L"LanguageEnglish"), Str(L"Settings", L"LanguageRussian")},
                      IndexOf(kLanguages, general.language), [error, weakNote](int index) {
                          auto const language = kLanguages[static_cast<size_t>(index)];
                          bool const ok = CommitSettings([language](core::Settings& s) { s.general.language = language; });
                          ShowSaveResult(error, ok);
                          if (ok)
                          {
                              if (auto const note = weakNote.get())
                              {
                                  note.IsOpen(true);
                              }
                          }
                          return ok;
                      })));
        rows.Append(languageNote);
    }
}
