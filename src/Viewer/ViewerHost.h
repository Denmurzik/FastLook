#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <winrt/FastLook.h>
#include <winrt/Microsoft.UI.Dispatching.h>

#include "Cli/ViewerArgs.h"
#include "Ipc/Protocol.h"
#include "Previewers/PreviewContext.h"
#include "Settings/Settings.h"
#include "System/AppPaths.h"
#include "Ui/Commands.h"

namespace winrt::FastLook::implementation
{
    struct PreviewWindow;
}

namespace fastlook::viewer
{
    using core::Command;

    class PreviewSession;

    enum class CloseBehavior
    {
        ExitProcess,
        HideWarm
    };

    struct HostCallbacks
    {
        std::function<void()> previewShown, previewHidden, settingsSaved, prewarmDone;
        std::function<void(int)> pinnedCountChanged;
        std::function<void(bool)> settingsWindowOpen;
        std::function<void(PreviewItem const&, core::ipc::NavMode, HWND explorer)> navigated;
    };

    // The UI-thread facade of the Viewer process: settings, the preview window and the session. All members are called on
    // the UI thread. Show/Toggle/UpdateItems and navigation delegate 1:1 to PreviewSession.
    class ViewerHost
    {
    public:
        static ViewerHost& Get();

        void Initialize(core::AppPaths const& paths, CloseBehavior behavior, core::ViewerArgs const& args);

        // Effective settings: the saved ones with the command-line theme override applied.
        core::Settings const& Settings() const { return m_settings; }
        void SaveSettings(core::Settings const& settings);
        void ReloadSettings(); // applies the theme

        void SetCallbacks(HostCallbacks callbacks) { m_callbacks = std::move(callbacks); }

        // Both return once the window is visible (decision 27).
        void Show(core::ipc::ShowPayload const& payload);
        bool Toggle(core::ipc::ShowPayload const& payload); // returns shown
        void UpdateItems(core::ipc::ItemsPayload const& payload);

        void Hide();
        bool IsPreviewVisible() const;
        void Execute(Command command);
        bool ExecutePreviewerCommand(std::wstring_view command);
        // IKeyTarget first, then the key map, then Execute. Stub until the chrome task.
        bool HandleKey(uint32_t vk, bool ctrl, bool shift, bool alt);
        void PinCurrent(); // stub until the chrome task
        void PrewarmHidden(); // stub until the chrome task: only reports prewarm_done

        void OpenSettingsWindow(std::wstring_view page);

        winrt::FastLook::PreviewWindow CurrentWindow() const { return m_window; }

        // Fired after every content_ready (bench, script runner).
        std::function<void()> contentReady;

        // Session-facing members.
        HostCallbacks const& Callbacks() const { return m_callbacks; }
        core::ViewerArgs const& Args() const { return m_args; }
        CloseBehavior Behavior() const { return m_behavior; }
        winrt::Microsoft::UI::Dispatching::DispatcherQueue const& Ui() const { return m_ui; }
        bool Dark() const;
        // Creates the window on first use.
        winrt::FastLook::implementation::PreviewWindow* Window();
        void ExitApp();

    private:
        ViewerHost() = default;

        void ApplySettings();
        void ApplyTheme();

        core::AppPaths m_paths;
        CloseBehavior m_behavior = CloseBehavior::ExitProcess;
        core::ViewerArgs m_args;
        core::SettingsDocument m_doc;
        core::Settings m_settings;
        HostCallbacks m_callbacks;
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_ui{nullptr};
        winrt::FastLook::PreviewWindow m_window{nullptr};
        std::shared_ptr<PreviewSession> m_session;
        bool m_exiting = false;
    };
}
