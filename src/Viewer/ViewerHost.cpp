#include "pch.h"
#include "ViewerHost.h"

#include <winrt/Windows.UI.ViewManagement.h>

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "PreviewSession.h"
#include "PreviewWindow.xaml.h"
#include "SettingsLauncher.h"
#include "Ui/KeyMap.h"
#include "Ui/Placement.h"
#include "WindowActions.h"

namespace fastlook::viewer
{
    // A preview that was pinned: its own window, the previewer that fills it and the item it shows.
    struct ViewerHost::PinnedWindow
    {
        winrt::FastLook::PreviewWindow window{nullptr};
        std::shared_ptr<IPreviewer> previewer;
        PreviewItem item;
        winrt::event_token closingToken{};
        bool closed = false;
    };

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
        if (m_session)
        {
            m_session->InvalidatePreload(); // neighbours were prepared with the previous settings and theme
        }
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
        auto* window = winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(m_window);
        bool const dark = Dark();
        window->SetTheme(theme);
        window->ApplyCaptionColors(dark);
        for (auto const& pinned : m_pinned)
        {
            auto* pinnedWindow = winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(pinned->window);
            pinnedWindow->SetTheme(theme);
            pinnedWindow->ApplyCaptionColors(dark);
        }
    }

    void ViewerHost::RefreshChrome()
    {
        if (m_window && m_settings.general.theme == core::ThemePref::System)
        {
            winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(m_window)->ApplyCaptionColors(Dark());
        }
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

    bool ViewerHost::SaveSettings(core::Settings const& settings)
    {
        m_doc.values = settings;
        if (m_args.theme && settings.general.theme == *m_args.theme)
        {
            // The command-line theme is a session override, not a user choice: keep what is on disk.
            m_doc.values.general.theme = core::LoadSettings(m_paths.settingsFile).values.general.theme;
        }
        // readFailed: the file exists but was unreadable, so writing would destroy the user's settings.
        bool const saved = core::SaveSettings(m_paths.settingsFile, m_doc);
        if (!saved)
        {
            FL_LOG_INFO(L"Saving settings failed{}", m_doc.readFailed ? L" (the file could not be read)" : L"");
        }
        ApplySettings();
        if (saved && m_callbacks.settingsSaved)
        {
            m_callbacks.settingsSaved();
        }
        return saved;
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
            auto* impl = winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(m_window);
            impl->SetCommandHandler([this](Command command) { Execute(command); });
            impl->SetKeyHandler([this](uint32_t vk, bool ctrl, bool shift, bool alt) { return HandleKey(vk, ctrl, shift, alt); });
            m_closingToken = m_window.AppWindow().Closing([this](auto const&, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
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

    void ViewerHost::CloseAllWindows()
    {
        m_exiting = true;
        auto const close = [](winrt::FastLook::PreviewWindow const& window) {
            try
            {
                if (window)
                {
                    window.Close();
                }
            }
            catch (...)
            {
                FL_LOG_INFO(L"Closing a window failed");
            }
        };
        close(m_window);
        for (auto const& pinned : m_pinned)
        {
            close(pinned->window);
        }
        CloseSettingsWindow();
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
            if (m_window && IsPreviewVisible())
            {
                winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(m_window)->ToggleFullscreen();
            }
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

    namespace
    {
        // The previewer's own key handling first, then the key map. True when the key was consumed.
        bool DispatchKey(std::shared_ptr<IPreviewer> const& previewer, uint32_t vk, bool ctrl, bool shift, bool alt, bool pinned,
                         std::function<void(Command)> const& execute)
        {
            if (auto* target = dynamic_cast<IKeyTarget*>(previewer.get()))
            {
                if (target->OnKey(static_cast<winrt::Windows::System::VirtualKey>(vk), ctrl, shift))
                {
                    return true;
                }
            }
            if (auto const command = core::MapPreviewKey(vk, ctrl, shift, alt, pinned))
            {
                execute(*command);
                return true;
            }
            return false;
        }
    }

    bool ViewerHost::HandleKey(uint32_t vk, bool ctrl, bool shift, bool alt)
    {
        if (!m_session)
        {
            return false;
        }
        return DispatchKey(m_session->AttachedPreviewer(), vk, ctrl, shift, alt, false, [this, vk](Command command) {
            auto* window = m_window ? winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(m_window) : nullptr;
            // The first Esc leaves full screen, the second closes.
            if (command == Command::Close && vk == VK_ESCAPE && window && window->IsFullscreen())
            {
                window->ToggleFullscreen();
                return;
            }
            Execute(command);
        });
    }

    void ViewerHost::PinCurrent()
    {
        if (!m_session || !m_window)
        {
            return;
        }
        auto view = m_session->DetachForPin();
        if (!view)
        {
            if (!m_session->DeferPin())
            {
                FL_LOG_INFO(L"Pin ignored: nothing is shown");
            }
            return;
        }

        auto pinned = std::make_shared<PinnedWindow>();
        pinned->window = std::exchange(m_window, nullptr);
        pinned->previewer = std::move(view->previewer);
        pinned->item = std::move(view->item);
        pinned->window.AppWindow().Closing(std::exchange(m_closingToken, {}));

        auto* impl = winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(pinned->window);
        impl->SetPinned(true);
        std::weak_ptr<PinnedWindow> const weak = pinned;
        impl->SetCommandHandler([this, weak](Command command) {
            if (auto strong = weak.lock())
            {
                ExecutePinned(strong, command);
            }
        });
        impl->SetKeyHandler([this, weak](uint32_t vk, bool ctrl, bool shift, bool alt) {
            auto strong = weak.lock();
            return strong && DispatchKey(strong->previewer, vk, ctrl, shift, alt, true, [&](Command command) {
                auto* window = winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(strong->window);
                if (command == Command::Close && vk == VK_ESCAPE && window->IsFullscreen())
                {
                    window->ToggleFullscreen();
                    return;
                }
                ExecutePinned(strong, command);
            });
        });
        pinned->closingToken = pinned->window.AppWindow().Closing(
            [this, weak](auto const&, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const&) {
                if (auto strong = weak.lock())
                {
                    OnPinnedClosing(strong);
                }
            });
        m_pinned.push_back(pinned);

        int const count = PinnedCount();
        core::perf::Mark("pinned", "count=" + std::to_string(count));
        FL_LOG_INFO(L"pinned count={}", count);
        if (m_callbacks.pinnedCountChanged)
        {
            m_callbacks.pinnedCountChanged(count);
        }
        if (m_callbacks.previewHidden)
        {
            m_callbacks.previewHidden();
        }

        // The next preview shows in a fresh warm window.
        try
        {
            PrewarmWindow(false);
        }
        catch (...)
        {
            FL_LOG_INFO(L"Warming the window after a pin failed");
        }
    }

    void ViewerHost::ExecutePinned(std::shared_ptr<PinnedWindow> const& pinned, Command command)
    {
        HWND const hwnd = winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(pinned->window)->Hwnd();
        switch (command)
        {
        case Command::Close:
            // Through WM_CLOSE: Window.Close() would skip AppWindow.Closing, which is where a pinned window is released.
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case Command::ZoomIn:
        case Command::ZoomOut:
        case Command::ZoomReset:
            if (auto* zoomable = dynamic_cast<IZoomable*>(pinned->previewer.get()))
            {
                zoomable->Zoom(command == Command::ZoomIn ? ZoomCommand::In : command == Command::ZoomOut ? ZoomCommand::Out : ZoomCommand::Reset);
            }
            break;
        case Command::OpenDefault:
            actions::OpenDefault(pinned->item);
            break;
        case Command::OpenWith:
            actions::OpenWith(hwnd, pinned->item);
            break;
        case Command::Reveal:
            actions::Reveal(pinned->item);
            break;
        case Command::Copy:
            actions::CopyFile(hwnd, pinned->item);
            break;
        case Command::ToggleFullscreen:
            winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(pinned->window)->ToggleFullscreen();
            break;
        case Command::Next:
        case Command::Prev:
        case Command::Pin:
            break; // a pinned window shows one fixed item
        }
    }

    void ViewerHost::OnPinnedClosing(std::shared_ptr<PinnedWindow> const& pinned)
    {
        if (pinned->closed)
        {
            return;
        }
        pinned->closed = true;
        try
        {
            pinned->previewer->Unload();
        }
        catch (...)
        {
        }
        // The window is still closing: its last reference goes a turn later.
        m_ui.TryEnqueue([pinned] {});
        if (auto const it = std::ranges::find(m_pinned, pinned); it != m_pinned.end())
        {
            m_pinned.erase(it);
        }

        if (m_exiting)
        {
            return;
        }
        int const count = PinnedCount();
        core::perf::Mark("pinned", "count=" + std::to_string(count));
        FL_LOG_INFO(L"pinned count={}", count);
        if (m_callbacks.pinnedCountChanged)
        {
            m_callbacks.pinnedCountChanged(count);
        }
        if (m_behavior == CloseBehavior::ExitProcess && count == 0 && !IsPreviewVisible())
        {
            ExitApp();
        }
    }

    void ViewerHost::PrewarmHidden()
    {
        PrewarmWindow(true);
    }

    void ViewerHost::PrewarmWindow(bool notify)
    {
        using winrt::Microsoft::UI::Xaml::Media::CompositionTarget;

        HWND const foreground = GetForegroundWindow();
        Window();
        auto const window = m_window;
        auto appWindow = window.AppWindow();
        appWindow.MoveAndResize({-32000, -32000, static_cast<int32_t>(core::kMinWindowW), static_cast<int32_t>(core::kMinWindowH)});
        appWindow.Show(false); // not activated: the foreground stays where it is

        struct State
        {
            bool done = false;
            winrt::event_token rendering{};
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
        };
        auto state = std::make_shared<State>();
        auto finish = [this, state, window, foreground, notify] {
            if (std::exchange(state->done, true))
            {
                return;
            }
            if (state->rendering)
            {
                CompositionTarget::Rendering(state->rendering);
            }
            if (state->timer)
            {
                state->timer.Stop();
                state->timer = nullptr; // drops the Tick handler that holds this state
            }
            // A show request that arrived meanwhile owns the window now.
            if (m_window == window && !IsPreviewVisible())
            {
                window.AppWindow().Hide();
            }
            bool const changed = GetForegroundWindow() != foreground;
            core::perf::Mark("prewarm_done", changed ? "fgChanged=1" : "fgChanged=0");
            if (notify && m_callbacks.prewarmDone)
            {
                m_callbacks.prewarmDone();
            }
        };
        state->rendering = CompositionTarget::Rendering([finish](auto const&, auto const&) { finish(); });
        // An off-screen window may never render a frame: do not wait for one forever.
        state->timer = m_ui.CreateTimer();
        state->timer.Interval(std::chrono::milliseconds(800));
        state->timer.IsRepeating(false);
        state->timer.Tick([finish](auto const&, auto const&) { finish(); });
        state->timer.Start();
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
