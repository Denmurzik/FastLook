#pragma once

#include <atomic>
#include <coroutine>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>

#include "Geometry.h"
#include "Ipc/Protocol.h"
#include "Previewers/IPreviewer.h"

namespace fastlook::viewer
{
    // The show flow and all list/navigation semantics (decisions 27 and 54). UI thread only.
    //
    // One Request per displayed item. Its coroutine classifies on a background thread, creates the previewer, runs
    // PrepareAsync (10 s timeout) and attaches the view if the request is still the current one. Every PrepareAsync is
    // tracked by a PrepareRun; a run that is cancelled or times out is "retired": the previewer is kept alive in
    // m_retiring and Unload() is called only once PrepareAsync has actually completed.
    class PreviewSession : public std::enable_shared_from_this<PreviewSession>
    {
    public:
        PreviewSession();

        void Show(core::ipc::ShowPayload const& payload);
        bool Toggle(core::ipc::ShowPayload const& payload); // returns shown
        void UpdateItems(core::ipc::ItemsPayload const& payload);
        void Navigate(int delta);
        void Hide();
        bool IsVisible() const { return m_visible; }

        void Zoom(ZoomCommand command);
        bool ExecutePreviewerCommand(std::wstring_view command);
        // Item of the current request (kind and handler known once it has been classified).
        std::optional<PreviewItem> CurrentItem() const;
        HWND ExplorerHwnd() const { return m_explorer; }

    private:
        struct Monitor
        {
            core::RectI work;
            double scale = 1.0;
        };

        // Tracks one PrepareAsync call. UI thread only, except that the Completed handler only enqueues to the UI thread.
        struct PrepareRun
        {
            std::shared_ptr<IPreviewer> previewer;
            winrt::Windows::Foundation::IAsyncAction action{nullptr};
            bool completed = false;
            bool retired = false;
            bool timedOut = false;
            std::coroutine_handle<> waiter;
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
        };

        struct Request
        {
            uint64_t id = 0;
            core::ipc::Item ref;
            bool fromNavigation = false;
            std::atomic<bool> cancelled{false};
            std::shared_ptr<PrepareRun> run;
            PreviewItem item; // filled after classification
        };

        struct WaitForPrepare;

        void StartRequest(bool fromNavigation);
        winrt::fire_and_forget RunRequestAsync(std::shared_ptr<Request> req);
        winrt::fire_and_forget HideAsync(uint64_t serial);

        void ShowFailure(std::shared_ptr<Request> const& req);
        bool IsCurrent(std::shared_ptr<Request> const& req) const;
        void CancelCurrent();
        void StartPrepare(std::shared_ptr<PrepareRun> const& run, PreviewItem item, std::shared_ptr<PreviewContext const> ctx);
        void OnPrepareCompleted(std::shared_ptr<PrepareRun> const& run);
        void Retire(std::shared_ptr<PrepareRun> const& run);
        void Attach(std::shared_ptr<Request> const& req, std::shared_ptr<PrepareRun> const& run, winrt::Microsoft::UI::Xaml::UIElement const& view,
                    std::string_view kindName);
        // MakeContext, or null when it threw (the caller then takes the failure ladder).
        std::shared_ptr<PreviewContext> MakeContextSafe(Monitor const& monitor, std::optional<FallbackInfo> fallback);
        std::shared_ptr<PreviewContext> MakeContext(Monitor const& monitor, std::optional<FallbackInfo> fallback);

        Monitor ResolveMonitor(bool preferWindow) const;
        void FitWindow(bool force); // never throws
        void FitWindowImpl(bool force);
        void WatchFirstFrame();

        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_ui{nullptr};

        std::vector<core::ipc::Item> m_items;
        size_t m_index = 0;
        core::ipc::NavMode m_navMode = core::ipc::NavMode::Selection;
        bool m_listComplete = true;
        HWND m_explorer = nullptr;
        uint64_t m_monitorHint = 0;
        std::optional<core::RectI> m_iconRect;

        bool m_visible = false;
        uint64_t m_nextRequestId = 0;
        uint64_t m_hideSerial = 0;
        std::shared_ptr<Request> m_current;
        PreviewItem m_shownItem; // last classified item (kind and handler)
        std::shared_ptr<PrepareRun> m_attached; // previewer whose view is in the window
        std::vector<std::shared_ptr<PrepareRun>> m_retiring;
        winrt::event_token m_renderToken{};
    };
}
