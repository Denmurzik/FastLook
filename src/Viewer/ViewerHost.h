#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

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
        // Server mode: runs its exit sequence (IPC first, then ExitAfterUnload). Unset: RequestExit goes to ExitAfterUnload.
        std::function<void()> exitRequested;
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
        // False when nothing was written (the file on disk could not be read, or the write failed).
        bool SaveSettings(core::Settings const& settings);
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
        // The current previewer's IKeyTarget first, then the key map, then Execute. True when the key was consumed.
        bool HandleKey(uint32_t vk, bool ctrl, bool shift, bool alt);
        // A key forwarded from a child window (a shell handler's site): handled by the window that holds the child, the warm
        // preview window or a pinned one. Dropped when the child is in neither.
        bool HandleKeyFrom(HWND child, uint32_t vk, bool ctrl, bool shift, bool alt);
        // Moves the shown preview into its own window (min/max, switchers) and warms a fresh hidden window.
        void PinCurrent();
        // Shows the window off-screen without activating it until its first frame, then hides it (decision 26).
        void PrewarmHidden();
        int PinnedCount() const { return static_cast<int>(m_pinned.size()); }

        void OpenSettingsWindow(std::wstring_view page);
        // Re-resolves the caption-button colours when the theme follows the system.
        void RefreshChrome();

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
        // Closes the preview window and every pinned window for good (no hide-instead-of-close); the exit sequence uses it.
        void CloseAllWindows();
        // Unloads every previewer (the preview's and the pinned ones), then closes all windows and exits once a media player's
        // ordered close (after a frame, on its own thread) has had time to run. Show requests are ignored from here on.
        void ExitAfterUnload();
        // An exit asked for from inside the app (the updater): the mode's exit sequence when it has one, else ExitAfterUnload.
        void RequestExit();

    private:
        struct PinnedWindow;

        ViewerHost() = default;

        void PrewarmWindow(bool notify);
        void ExecutePinned(std::shared_ptr<PinnedWindow> const& pinned, Command command);
        bool HandlePinnedKey(std::shared_ptr<PinnedWindow> const& pinned, uint32_t vk, bool ctrl, bool shift, bool alt);
        void OnPinnedClosing(std::shared_ptr<PinnedWindow> const& pinned);

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
        winrt::event_token m_closingToken{};
        std::vector<std::shared_ptr<PinnedWindow>> m_pinned;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_exitTimer{nullptr};
        bool m_exiting = false;
    };
}
