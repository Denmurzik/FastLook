#include "pch.h"
#include "ViewerHost.h"

#include <winrt/Windows.UI.ViewManagement.h>

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "PreviewSession.h"
#include "PreviewWindow.xaml.h"
#include "SettingsLauncher.h"
#include "WindowActions.h"

namespace fastlook::viewer
{
    ViewerHost& ViewerHost::Get()
    {
        // Intentionally leaked: it holds WinRT objects that CRT static destruction would release after WinRT teardown.
        static ViewerHost& host = *new ViewerHost;
        return host;
    }

    void ViewerHost::Initialize(core::AppPaths const& paths, CloseBehavior behavior, core::ViewerArgs const& args)
    {
        m_paths = paths;
        m_behavior = behavior;
        m_args = args;
        m_ui = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        m_doc = core::LoadSettings(paths.settingsFile);
        ApplySettings();
        m_session = std::make_shared<PreviewSession>();
    }

    void ViewerHost::ApplySettings()
    {
        m_settings = m_doc.values;
        if (m_args.theme)
        {
            m_settings.general.theme = *m_args.theme;
        }
        core::log::SetLevel(m_settings.logLevel);
        ApplyTheme();
    }

    void ViewerHost::ApplyTheme()
    {
        if (!m_window)
        {
            return;
        }
        using winrt::Microsoft::UI::Xaml::ElementTheme;
        auto theme = ElementTheme::Default;
        if (m_settings.general.theme == core::ThemePref::Light)
        {
            theme = ElementTheme::Light;
        }
        else if (m_settings.general.theme == core::ThemePref::Dark)
        {
            theme = ElementTheme::Dark;
        }
        winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(m_window)->SetTheme(theme);
    }

    bool ViewerHost::Dark() const
    {
        switch (m_settings.general.theme)
        {
        case core::ThemePref::Dark:
            return true;
        case core::ThemePref::Light:
            return false;
        case core::ThemePref::System:
            break;
        }
        winrt::Windows::UI::ViewManagement::UISettings ui;
        auto const background = ui.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background);
        return background.R < 128;
    }

    void ViewerHost::SaveSettings(core::Settings const& settings)
    {
        m_doc.values = settings;
        if (m_args.theme && settings.general.theme == *m_args.theme)
        {
            // The command-line theme is a session override, not a user choice: keep what is on disk.
            m_doc.values.general.theme = core::LoadSettings(m_paths.settingsFile).values.general.theme;
        }
        bool const saved = core::SaveSettings(m_paths.settingsFile, m_doc);
        if (!saved)
        {
            FL_LOG_INFO(L"Saving settings failed");
        }
        ApplySettings();
        if (saved && m_callbacks.settingsSaved)
        {
            m_callbacks.settingsSaved();
        }
    }

    void ViewerHost::ReloadSettings()
    {
        m_doc = core::LoadSettings(m_paths.settingsFile);
        ApplySettings();
    }

    winrt::FastLook::implementation::PreviewWindow* ViewerHost::Window()
    {
        if (!m_window)
        {
            m_window = winrt::make<winrt::FastLook::implementation::PreviewWindow>();
            m_window.AppWindow().Closing([this](auto const&, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
                if (!m_exiting)
                {
                    // The close button hides the preview (or ends a one-shot process) instead of destroying the warm window.
                    args.Cancel(true);
                    Hide();
                }
            });
            ApplyTheme();
        }
        return winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(m_window);
    }

    void ViewerHost::ExitApp()
    {
        m_exiting = true;
        winrt::Microsoft::UI::Xaml::Application::Current().Exit();
    }

    void ViewerHost::Show(core::ipc::ShowPayload const& payload)
    {
        m_session->Show(payload);
    }

    bool ViewerHost::Toggle(core::ipc::ShowPayload const& payload)
    {
        return m_session->Toggle(payload);
    }

    void ViewerHost::UpdateItems(core::ipc::ItemsPayload const& payload)
    {
        m_session->UpdateItems(payload);
    }

    void ViewerHost::Hide()
    {
        m_session->Hide();
    }

    bool ViewerHost::IsPreviewVisible() const
    {
        return m_session && m_session->IsVisible();
    }

    void ViewerHost::Execute(Command command)
    {
        auto const item = m_session->CurrentItem();
        HWND const hwnd = m_window ? winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(m_window)->Hwnd() : nullptr;

        switch (command)
        {
        case Command::Close:
            Hide();
            break;
        case Command::Next:
            m_session->Navigate(+1);
            break;
        case Command::Prev:
            m_session->Navigate(-1);
            break;
        case Command::ZoomIn:
            m_session->Zoom(ZoomCommand::In);
            break;
        case Command::ZoomOut:
            m_session->Zoom(ZoomCommand::Out);
            break;
        case Command::ZoomReset:
            m_session->Zoom(ZoomCommand::Reset);
            break;
        case Command::ToggleFullscreen:
            FL_LOG_INFO(L"ToggleFullscreen: not implemented");
            break;
        case Command::Pin:
            PinCurrent();
            break;
        case Command::OpenDefault:
            if (item)
            {
                actions::OpenDefault(*item);
            }
            break;
        case Command::OpenWith:
            if (item)
            {
                actions::OpenWith(hwnd, *item);
            }
            break;
        case Command::Reveal:
            if (item)
            {
                actions::Reveal(*item);
            }
            break;
        case Command::Copy:
            if (item)
            {
                actions::CopyFile(hwnd, *item);
            }
            break;
        }
    }

    bool ViewerHost::ExecutePreviewerCommand(std::wstring_view command)
    {
        return m_session->ExecutePreviewerCommand(command);
    }

    bool ViewerHost::HandleKey(uint32_t, bool, bool, bool)
    {
        return false;
    }

    void ViewerHost::PinCurrent()
    {
    }

    void ViewerHost::PrewarmHidden()
    {
        core::perf::Mark("prewarm_done", "fgChanged=0");
        if (m_callbacks.prewarmDone)
        {
            m_callbacks.prewarmDone();
        }
    }

    void ViewerHost::OpenSettingsWindow(std::wstring_view page)
    {
        if (m_callbacks.settingsWindowOpen)
        {
            m_callbacks.settingsWindowOpen(true);
        }
        ShowSettingsWindow(page, [this] {
            if (m_callbacks.settingsWindowOpen)
            {
                m_callbacks.settingsWindowOpen(false);
            }
        });
    }
}
