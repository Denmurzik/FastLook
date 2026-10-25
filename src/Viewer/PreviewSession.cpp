#include "pch.h"
#include "PreviewSession.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <shellscalingapi.h>

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "FileTypes/ClassifyFile.h"
#include "FileTypes/Extensions.h"
#include "FileTypes/FileName.h"
#include "Nav/NavList.h"
#include "PreviewWindow.xaml.h"
#include "Previewers/Placeholder.h"
#include "Previewers/PreviewerRegistry.h"
#include "Ui/Placement.h"
#include "Util/Async.h"
#include "Util/Foreground.h"
#include "Util/PreviewErrorBridge.h"
#include "Util/Str.h"
#include "ViewerHost.h"
#include "WindowAnimator.h"

#pragma comment(lib, "shcore.lib")

namespace fastlook::viewer
{
    namespace
    {
        using core::PreviewerKind;
        using winrt::Windows::Foundation::TimeSpan;

        constexpr TimeSpan kPrepareTimeout = std::chrono::seconds(10);

        std::string_view KindName(PreviewerKind kind)
        {
            switch (kind)
            {
            case PreviewerKind::Image: return "image";
            case PreviewerKind::Text: return "text";
            case PreviewerKind::Markdown: return "markdown";
            case PreviewerKind::Media: return "media";
            case PreviewerKind::Pdf: return "pdf";
            case PreviewerKind::Archive: return "archive";
            case PreviewerKind::Folder: return "folder";
            case PreviewerKind::Shell: return "shell";
            case PreviewerKind::Fallback: return "fallback";
            }
            return "fallback";
        }

        bool SamePath(std::wstring_view a, std::wstring_view b)
        {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }

        core::SizeD DefaultSizeFor(std::wstring_view path)
        {
            auto const kind = core::KindForExtension(core::BaseName(path));
            return kind == PreviewerKind::Text ? core::kDefaultTextSize : core::kDefaultOtherSize;
        }

        struct Classified
        {
            core::FileClassification classification;
            std::optional<FallbackInfo> missing;
        };

        struct ComScope
        {
            bool initialised;
            ComScope() : initialised(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
            ~ComScope()
            {
                if (initialised)
                {
                    CoUninitialize();
                }
            }
        };

        // Background thread.
        Classified Classify(core::ipc::Item const& ref, bool useShellHandlers)
        {
            Classified result;
            if (!ref.isVirtual && GetFileAttributesW(ref.path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                DWORD const error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                {
                    result.missing = FallbackInfo{std::wstring{Str(L"Common", L"NotFound")}, std::nullopt};
                    return result;
                }
            }
            try
            {
                ComScope com;
                result.classification = core::ClassifyFile(ref.path, ref.isVirtual, useShellHandlers);
            }
            catch (...)
            {
                result.classification = {};
            }
            return result;
        }

        // Background thread: the name Explorer shows for a shell-namespace item; the parsing name when it cannot be resolved.
        std::wstring ShellDisplayName(std::wstring const& path)
        {
            try
            {
                ComScope com;
                winrt::com_ptr<IShellItem> item;
                if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()))))
                {
                    wchar_t* name = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name != nullptr)
                    {
                        std::wstring result{name};
                        CoTaskMemFree(name);
                        if (!result.empty())
                        {
                            return result;
                        }
                    }
                }
            }
            catch (...)
            {
            }
            return std::wstring{core::BaseName(path)};
        }

        FallbackInfo DescribeFailure(winrt::hresult_error const& e)
        {
            if (auto error = TryDecodePreviewError(e))
            {
                if (error->message.empty())
                {
                    error->message = Str(L"Common", L"PreviewFailed");
                }
                return FallbackInfo{std::move(error->message), std::move(error->storeProductId)};
            }
            FL_LOG_INFO(L"Prepare failed: 0x{:08X}", static_cast<uint32_t>(e.code().value));
            if (e.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || e.code() == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            {
                return FallbackInfo{std::wstring{Str(L"Common", L"NotFound")}, std::nullopt};
            }
            return FallbackInfo{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
        }

        // An already failed IAsyncAction for a PrepareAsync that threw before returning one.
        winrt::Windows::Foundation::IAsyncAction Rethrow(std::exception_ptr error)
        {
            if (error)
            {
                std::rethrow_exception(error);
            }
            co_return;
        }

        enum class Stage
        {
            Classified, // the previewer chosen by classification
            Fallback,
            Placeholder // last resort, cannot fail in practice
        };
    }

    // Awaitable: suspends until the run completes or the timeout expires (run->timedOut tells which).
    struct PreviewSession::WaitForPrepare
    {
        PreviewSession* session;
        std::shared_ptr<PrepareRun> run;

        bool await_ready() const noexcept { return run->completed; }

        void await_suspend(std::coroutine_handle<> handle) const
        {
            run->waiter = handle;
            auto timer = session->m_ui.CreateTimer();
            timer.Interval(kPrepareTimeout);
            timer.IsRepeating(false);
            timer.Tick([weakRun = std::weak_ptr<PrepareRun>{run}, ui = session->m_ui](auto const& sender, auto const&) {
                sender.Stop();
                // Resume outside the timer's own callback: the continuation drops the run, and with it this timer.
                ui.TryEnqueue([weakRun] {
                    auto run = weakRun.lock();
                    if (run && !run->completed && run->waiter)
                    {
                        run->timedOut = true;
                        std::exchange(run->waiter, {}).resume();
                    }
                });
            });
            run->timer = timer;
            timer.Start();
        }

        void await_resume() const noexcept {}
    };

    PreviewSession::PreviewSession() : m_ui(ViewerHost::Get().Ui()) {}

    void PreviewSession::Show(core::ipc::ShowPayload const& payload)
    {
        core::perf::Mark("show_request");
        if (payload.items.empty())
        {
            FL_LOG_INFO(L"Show without items ignored");
            return;
        }

        m_items = payload.items;
        m_index = std::min<size_t>(payload.index, m_items.size() - 1);
        m_navMode = payload.navMode;
        m_listComplete = payload.listComplete;
        m_explorer = reinterpret_cast<HWND>(static_cast<uintptr_t>(payload.explorerHwnd));
        m_isDesktop = payload.isDesktop;
        m_monitorHint = payload.monitor;
        m_iconRect = payload.iconRect;
        ++m_hideSerial; // a hide still animating out is abandoned
        m_pinPending = false;
        StartRequest(false);
    }

    bool PreviewSession::Toggle(core::ipc::ShowPayload const& payload)
    {
        if (m_visible && !payload.items.empty() && !m_items.empty())
        {
            auto const& target = payload.items[std::min<size_t>(payload.index, payload.items.size() - 1)];
            if (SamePath(target.path, m_items[m_index].path))
            {
                Hide();
                return false;
            }
        }
        Show(payload);
        return true;
    }

    void PreviewSession::UpdateItems(core::ipc::ItemsPayload const& payload)
    {
        if (!m_visible || m_items.empty() || payload.items.empty())
        {
            return;
        }
        // The user may already have navigated: the list is taken over only when it still contains what is displayed.
        auto const located = core::nav::LocateItem(payload.items, m_items[m_index]);
        if (!located)
        {
            return;
        }
        m_items = payload.items;
        m_index = *located;
        m_navMode = payload.navMode;
        m_listComplete = true;
        ViewerHost::Get().Window()->SetCounter(m_index, m_items.size());
        SchedulePrefetch();
    }

    void PreviewSession::Navigate(int delta)
    {
        if (!m_visible || m_items.empty() || (!m_listComplete && m_items.size() <= 1))
        {
            return; // the list is still being collected and the current item is all there is
        }
        auto const next = static_cast<int64_t>(m_index) + delta;
        if (next < 0 || next >= static_cast<int64_t>(m_items.size()))
        {
            return; // decision 29: no wrap-around
        }
        core::perf::Mark("nav_request");
        m_index = static_cast<size_t>(next);
        m_lastDelta = delta < 0 ? -1 : +1;
        if (m_navMode == core::ipc::NavMode::Folder && m_explorer != nullptr)
        {
            m_selectionSync.Sync(m_explorer, m_isDesktop, m_items[m_index]); // returns at once; never in Selection mode
        }
        StartRequest(true);
    }

    void PreviewSession::Zoom(ZoomCommand command)
    {
        if (m_attached)
        {
            if (auto* zoomable = dynamic_cast<IZoomable*>(m_attached->previewer.get()))
            {
                zoomable->Zoom(command);
            }
        }
    }

    bool PreviewSession::ExecutePreviewerCommand(std::wstring_view command)
    {
        if (m_attached)
        {
            if (auto* target = dynamic_cast<ICommandTarget*>(m_attached->previewer.get()))
            {
                return target->Execute(command);
            }
        }
        return false;
    }

    std::optional<PreviewItem> PreviewSession::CurrentItem() const
    {
        if (m_items.empty())
        {
            return std::nullopt;
        }
        if (m_current)
        {
            return m_current->item;
        }
        if (SamePath(m_shownItem.ref.path, m_items[m_index].path))
        {
            return m_shownItem;
        }
        PreviewItem item;
        item.ref = m_items[m_index];
        return item;
    }

    void PreviewSession::StartRequest(bool fromNavigation)
    {
        CancelCurrent();

        auto& host = ViewerHost::Get();
        auto* window = host.Window();

        auto req = std::make_shared<Request>();
        req->id = ++m_nextRequestId;
        req->ref = m_items[m_index];
        req->fromNavigation = fromNavigation;
        req->item.ref = req->ref;
        m_current = req;

        window->SetItem(req->ref.path, req->ref.isVirtual ? winrt::hstring{} : winrt::hstring{core::BaseName(req->ref.path)}, m_index, m_items.size());
        if (req->ref.isVirtual)
        {
            ResolveTitleAsync(req->ref);
        }

        if (!m_visible)
        {
            // Decision 27: visible at once in the Loading state, sized by the file type; no waiting for classification.
            auto const monitor = ResolveMonitor(false);
            window->ShowLoading();
            host.RefreshChrome();
            auto const rect = core::FitPreviewWindow(DefaultSizeFor(req->ref.path), false, monitor.work, monitor.scale);
            m_visible = true;
            window->AppWindow().IsShownInSwitchers(true);
            anim::In(host.CurrentWindow(), m_iconRect, rect);
            ForceForeground(window->Hwnd());
            WatchFirstFrame();
            if (host.Callbacks().previewShown)
            {
                host.Callbacks().previewShown();
            }
        }

        RunRequestAsync(req);
    }

    winrt::fire_and_forget PreviewSession::RunRequestAsync(std::shared_ptr<Request> req)
    {
        auto self = shared_from_this();
        auto const ui = m_ui;
        bool const useShellHandlers = ViewerHost::Get().Settings().preview.useShellHandlers;

        // A fire_and_forget coroutine terminates the process on an escaping exception: nothing may leave this try block.
        try
        {
            auto const preloaded = TryAttachPreloaded(req);
            if (preloaded == Preloaded::Attached)
            {
                co_return;
            }
            if (preloaded == Preloaded::Pending)
            {
                auto const run = req->run;
                co_await WaitForPrepare{this, run};
                if (!IsCurrent(req))
                {
                    co_return;
                }
                bool attached = false;
                if (run->timedOut)
                {
                    Retire(run);
                }
                else
                {
                    try
                    {
                        run->action.GetResults();
                        Attach(req, run, run->previewer->View(), KindName(req->item.kind));
                        attached = true;
                    }
                    catch (...)
                    {
                        try
                        {
                            run->previewer->Unload();
                        }
                        catch (...)
                        {
                        }
                    }
                }
                req->run = nullptr;
                if (attached)
                {
                    co_return;
                }
                // The preloaded preparation failed: take the normal path (which shows the failure ladder).
            }
            co_await winrt::resume_background();
            Classified classified;
            if (!req->cancelled)
            {
                classified = Classify(req->ref, useShellHandlers);
            }
            if (!co_await ResumeOn(ui) || !IsCurrent(req))
            {
                co_return;
            }

            auto const& cls = classified.classification;
            req->item.kind = cls.kind;
            req->item.shellHandlerClsid = cls.shellHandlerClsid;
            req->item.cloudPlaceholder = cls.cloudPlaceholder;
            m_shownItem = req->item;

            std::optional<FallbackInfo> fallback = classified.missing;
            Stage stage = (fallback || cls.kind == PreviewerKind::Fallback) ? Stage::Fallback : Stage::Classified;
            auto const monitor = ResolveMonitor(true);

            std::shared_ptr<PrepareRun> good;
            winrt::Microsoft::UI::Xaml::UIElement view{nullptr};
            PreviewerKind shownKind = PreviewerKind::Fallback;

            while (!good)
            {
                std::shared_ptr<IPreviewer> previewer;
                try
                {
                    switch (stage)
                    {
                    case Stage::Classified:
                        previewer = CreatePreviewer(cls.kind);
                        shownKind = cls.kind;
                        break;
                    case Stage::Fallback:
                        previewer = CreatePreviewer(PreviewerKind::Fallback);
                        shownKind = PreviewerKind::Fallback;
                        break;
                    case Stage::Placeholder:
                        previewer = MakePlaceholderPreviewer(L"Fallback");
                        shownKind = PreviewerKind::Fallback;
                        break;
                    }
                }
                catch (...)
                {
                    previewer = nullptr;
                }

                std::optional<FallbackInfo> failure;
                if (!previewer)
                {
                    failure = FallbackInfo{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
                }
                else if (std::shared_ptr<PreviewContext> ctx = MakeContextSafe(monitor, fallback); !ctx)
                {
                    failure = FallbackInfo{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
                }
                else
                {
                    auto run = std::make_shared<PrepareRun>();
                    run->previewer = previewer;
                    req->run = run;
                    StartPrepare(run, req->item, std::move(ctx));

                    co_await WaitForPrepare{this, run};

                    if (!IsCurrent(req))
                    {
                        co_return; // superseded: Retire() already took over the run
                    }
                    if (run->timedOut)
                    {
                        Retire(run); // Unload happens once PrepareAsync really completes
                        failure = FallbackInfo{std::wstring{Str(L"Common", L"TimedOut")}, std::nullopt};
                    }
                    else
                    {
                        try
                        {
                            run->action.GetResults();
                            view = previewer->View();
                            Attach(req, run, view, KindName(shownKind)); // may throw if the window rejects the view
                            good = run;
                        }
                        catch (winrt::hresult_error const& e)
                        {
                            failure = DescribeFailure(e);
                        }
                        catch (...)
                        {
                            failure = FallbackInfo{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
                        }
                        if (!good)
                        {
                            try
                            {
                                previewer->Unload();
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                    req->run = nullptr;
                }

                if (good)
                {
                    break;
                }
                if (stage == Stage::Placeholder)
                {
                    FL_LOG_INFO(L"No previewer could be shown");
                    co_return;
                }
                fallback = std::move(failure);
                stage = stage == Stage::Classified ? Stage::Fallback : Stage::Placeholder;
            }
        }
        catch (...)
        {
            FL_LOG_INFO(L"Preview request failed with an exception");
            ShowFailure(req);
        }
    }

    PreviewSession::Preloaded PreviewSession::TryAttachPreloaded(std::shared_ptr<Request> const& req)
    {
        if (CurrentPreloadKey() != m_preloadKey)
        {
            m_preloader.Clear(); // monitor, DPI or theme changed since the neighbours were prepared
            return Preloaded::None;
        }
        auto taken = m_preloader.Take(req->ref);
        if (!taken)
        {
            return Preloaded::None;
        }
        auto const& previewer = taken->previewer;
        try
        {
            req->item = taken->item;
            m_shownItem = taken->item;
            auto run = std::make_shared<PrepareRun>();
            run->previewer = previewer;
            if (taken->pending)
            {
                run->action = taken->pending;
                req->run = run;
                WatchRun(run);
                return Preloaded::Pending;
            }
            run->completed = true; // PrepareAsync finished in the preloader
            Attach(req, run, previewer->View(), KindName(taken->item.kind));
            return Preloaded::Attached;
        }
        catch (...)
        {
            FL_LOG_INFO(L"Attaching a preloaded previewer failed");
            req->run = nullptr;
            if (taken->pending)
            {
                try
                {
                    taken->pending.Cancel();
                }
                catch (...)
                {
                }
            }
            try
            {
                previewer->Unload();
            }
            catch (...)
            {
            }
            return Preloaded::None;
        }
    }

    PreviewSession::PreloadKey PreviewSession::CurrentPreloadKey() const
    {
        auto const monitor = ResolveMonitor(true);
        return {monitor.work, monitor.scale, ViewerHost::Get().Dark()};
    }

    void PreviewSession::InvalidatePreload()
    {
        m_preloader.Clear();
        SchedulePrefetch();
    }

    void PreviewSession::SchedulePrefetch()
    {
        if (!m_visible || m_current || m_items.empty())
        {
            return;
        }
        if (auto const key = CurrentPreloadKey(); key != m_preloadKey)
        {
            m_preloader.Clear();
            m_preloadKey = key;
        }
        std::vector<core::ipc::Item> neighbours;
        for (int const delta : {m_lastDelta, -m_lastDelta})
        {
            auto const index = static_cast<int64_t>(m_index) + delta;
            if (index >= 0 && index < static_cast<int64_t>(m_items.size()))
            {
                neighbours.push_back(m_items[static_cast<size_t>(index)]);
            }
        }
        auto ctx = MakeContextSafe(ResolveMonitor(true), std::nullopt);
        if (ctx)
        {
            m_preloader.Prefetch(std::move(neighbours), std::move(ctx));
        }
    }

    winrt::fire_and_forget PreviewSession::ResolveTitleAsync(core::ipc::Item ref)
    {
        auto self = shared_from_this();
        auto const ui = m_ui;
        try
        {
            co_await winrt::resume_background();
            std::wstring name = ShellDisplayName(ref.path);
            if (!co_await ResumeOn(ui))
            {
                co_return;
            }
            if (m_visible && !m_items.empty() && SamePath(m_items[m_index].path, ref.path))
            {
                ViewerHost::Get().Window()->SetTitle(winrt::hstring{name});
            }
        }
        catch (...)
        {
            FL_LOG_INFO(L"Resolving the display name failed");
        }
    }

    void PreviewSession::ShowFailure(std::shared_ptr<Request> const& req)
    {
        // Never leave the window on Loading: drop the request and show the PreviewFailed text in place of the content.
        try
        {
            if (!IsCurrent(req))
            {
                return;
            }
            CancelCurrent();
            m_pinPending = false; // a pin requested during a failing load must not pin the next item
            winrt::Microsoft::UI::Xaml::Controls::TextBlock text;
            text.Text(Str(L"Common", L"PreviewFailed"));
            text.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
            text.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
            ViewerHost::Get().Window()->SetContent(text);
            if (auto previous = std::exchange(m_attached, nullptr))
            {
                try
                {
                    previous->previewer->Unload();
                }
                catch (...)
                {
                }
            }
            SchedulePrefetch(); // the neighbours of a failed item are still worth preparing
        }
        catch (...)
        {
            FL_LOG_INFO(L"Showing the failure text failed");
        }
    }

    bool PreviewSession::IsCurrent(std::shared_ptr<Request> const& req) const
    {
        return m_current == req && !req->cancelled;
    }

    void PreviewSession::CancelCurrent()
    {
        if (!m_current)
        {
            return;
        }
        m_current->cancelled = true;
        Retire(m_current->run);
        m_current.reset();
    }

    void PreviewSession::StartPrepare(std::shared_ptr<PrepareRun> const& run, PreviewItem item, std::shared_ptr<PreviewContext const> ctx)
    {
        try
        {
            run->action = run->previewer->PrepareAsync(std::move(item), std::move(ctx));
        }
        catch (...)
        {
            run->action = Rethrow(std::current_exception());
        }

        WatchRun(run);
    }

    void PreviewSession::WatchRun(std::shared_ptr<PrepareRun> const& run)
    {
        // Weak run: whoever still cares holds it (request, m_retiring); a nobody-cares run needs no completion handling.
        run->action.Completed([weakSelf = weak_from_this(), weakRun = std::weak_ptr<PrepareRun>{run}, ui = m_ui](auto const&, auto) {
            ui.TryEnqueue([weakSelf, weakRun] {
                auto session = weakSelf.lock();
                auto run = weakRun.lock();
                if (session && run)
                {
                    session->OnPrepareCompleted(run);
                }
            });
        });
    }

    void PreviewSession::OnPrepareCompleted(std::shared_ptr<PrepareRun> const& run)
    {
        run->completed = true;
        if (run->timer)
        {
            run->timer.Stop();
            run->timer = nullptr;
        }
        if (run->retired)
        {
            std::erase(m_retiring, run);
            try
            {
                run->previewer->Unload();
            }
            catch (...)
            {
            }
        }
        if (auto waiter = std::exchange(run->waiter, {}))
        {
            waiter.resume();
        }
    }

    void PreviewSession::Retire(std::shared_ptr<PrepareRun> const& run)
    {
        if (!run || run->retired)
        {
            return;
        }
        run->retired = true;
        if (run->action)
        {
            try
            {
                run->action.Cancel();
            }
            catch (...)
            {
            }
        }
        if (run->completed)
        {
            try
            {
                run->previewer->Unload();
            }
            catch (...)
            {
            }
        }
        else
        {
            m_retiring.push_back(run);
        }
    }

    std::shared_ptr<PreviewContext> PreviewSession::MakeContextSafe(Monitor const& monitor, std::optional<FallbackInfo> fallback)
    {
        try
        {
            return MakeContext(monitor, std::move(fallback));
        }
        catch (...)
        {
            FL_LOG_INFO(L"Creating the preview context failed");
            return nullptr;
        }
    }

    std::shared_ptr<PreviewContext> PreviewSession::MakeContext(Monitor const& monitor, std::optional<FallbackInfo> fallback)
    {
        auto& host = ViewerHost::Get();
        auto ctx = std::make_shared<PreviewContext>();
        ctx->hwnd = host.Window()->Hwnd();
        ctx->ui = m_ui;
        ctx->scale = monitor.scale;
        ctx->maxContentDip = {static_cast<float>(monitor.work.w * core::kMaxWorkAreaFraction / monitor.scale),
                              static_cast<float>(monitor.work.h * core::kMaxWorkAreaFraction / monitor.scale - core::kTitleBarDip)};
        ctx->dark = host.Dark();
        ctx->settings = host.Settings();
        ctx->fallback = std::move(fallback);
        ctx->requestResize = [weakSelf = weak_from_this(), ui = m_ui] {
            ui.TryEnqueue([weakSelf] {
                if (auto session = weakSelf.lock())
                {
                    session->FitWindow(true);
                }
            });
        };
        return ctx;
    }

    void PreviewSession::Attach(std::shared_ptr<Request> const& req, std::shared_ptr<PrepareRun> const& run,
                                winrt::Microsoft::UI::Xaml::UIElement const& view, std::string_view kindName)
    {
        auto& host = ViewerHost::Get();
        auto* window = host.Window();

        // The only step allowed to throw: if it does, the window keeps its previous content and the caller falls back.
        window->SetContent(view);

        auto previous = std::move(m_attached);
        m_attached = run;
        m_current.reset();
        if (previous)
        {
            try
            {
                previous->previewer->Unload();
            }
            catch (...)
            {
            }
        }

        try
        {
            if (auto* provider = dynamic_cast<IToolbarProvider*>(run->previewer.get()))
            {
                window->SetToolbar(provider->ToolbarItems());
            }
        }
        catch (...)
        {
            FL_LOG_INFO(L"Collecting the toolbar failed");
        }

        FitWindow(false);

        core::perf::Mark("content_ready", std::string{"kind="}.append(kindName));
        try
        {
            if (req->fromNavigation && host.Callbacks().navigated)
            {
                host.Callbacks().navigated(m_shownItem, m_navMode, m_explorer);
            }
            if (host.contentReady)
            {
                host.contentReady();
            }
        }
        catch (...)
        {
            FL_LOG_INFO(L"A content-ready callback threw");
        }

        if (std::exchange(m_pinPending, false))
        {
            host.PinCurrent();
        }
        else
        {
            SchedulePrefetch();
        }
    }

    PreviewSession::Monitor PreviewSession::ResolveMonitor(bool preferWindow) const
    {
        HMONITOR monitor = nullptr;
        MONITORINFO info{sizeof(info)};
        if (preferWindow)
        {
            if (HWND const hwnd = ViewerHost::Get().Window()->Hwnd())
            {
                monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            }
        }
        else if (m_monitorHint != 0)
        {
            monitor = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(m_monitorHint));
        }
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            GetMonitorInfoW(monitor, &info);
        }

        UINT dpiX = USER_DEFAULT_SCREEN_DPI, dpiY = USER_DEFAULT_SCREEN_DPI;
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);

        Monitor result;
        result.work = {info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top};
        result.scale = dpiX / static_cast<double>(USER_DEFAULT_SCREEN_DPI);
        return result;
    }

    void PreviewSession::FitWindow(bool force)
    {
        try
        {
            FitWindowImpl(force);
        }
        catch (...)
        {
            FL_LOG_INFO(L"Fitting the window failed");
        }
    }

    void PreviewSession::FitWindowImpl(bool force)
    {
        if (!m_visible || !m_attached)
        {
            return;
        }
        if (ViewerHost::Get().Window()->IsFullscreen())
        {
            return; // the content just re-lays out inside the full-screen window
        }
        auto const& previewer = *m_attached->previewer;
        auto const preferred = previewer.PreferredSize();
        if (preferred.Width <= 0 || preferred.Height <= 0)
        {
            return; // no opinion: keep the size the window has
        }

        auto& host = ViewerHost::Get();
        auto const window = host.CurrentWindow();
        auto const monitor = ResolveMonitor(true);
        core::RectI const target = core::FitPreviewWindow({preferred.Width, preferred.Height}, previewer.KeepAspect(), monitor.work, monitor.scale);

        auto const position = window.AppWindow().Position();
        auto const size = window.AppWindow().Size();
        core::RectI const current{position.X, position.Y, size.Width, size.Height};

        bool const changed = force ? target != current
                                   : std::abs(target.w - current.w) > current.w * 0.1 || std::abs(target.h - current.h) > current.h * 0.1;
        if (changed)
        {
            anim::Resize(window, current, target);
        }
    }

    void PreviewSession::WatchFirstFrame()
    {
        using winrt::Microsoft::UI::Xaml::Media::CompositionTarget;
        if (m_renderToken)
        {
            CompositionTarget::Rendering(m_renderToken);
            m_renderToken = {};
        }
        m_renderToken = CompositionTarget::Rendering([weakSelf = weak_from_this()](auto const&, auto const&) {
            if (auto session = weakSelf.lock(); session && session->m_renderToken)
            {
                CompositionTarget::Rendering(session->m_renderToken);
                session->m_renderToken = {};
                core::perf::Mark("window_visible");
            }
        });
    }

    void PreviewSession::Hide()
    {
        if (!m_visible)
        {
            return;
        }
        m_visible = false;
        m_pinPending = false;
        CancelCurrent();
        m_preloader.Clear();
        HideAsync(++m_hideSerial);
    }

    std::optional<PreviewSession::PinnedView> PreviewSession::DetachForPin()
    {
        if (!m_visible || !m_attached || m_current)
        {
            return std::nullopt;
        }
        PinnedView view{m_attached->previewer, m_shownItem};
        m_attached.reset();
        m_visible = false;
        m_preloader.Clear();
        ++m_hideSerial;
        if (m_renderToken)
        {
            winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(m_renderToken);
            m_renderToken = {};
        }
        return view;
    }

    bool PreviewSession::DeferPin()
    {
        if (!m_visible || !m_current)
        {
            return false;
        }
        m_pinPending = true;
        return true;
    }

    winrt::fire_and_forget PreviewSession::HideAsync(uint64_t serial)
    {
        auto self = shared_from_this();
        // A fire_and_forget coroutine terminates the process on an escaping exception: nothing may leave this try block.
        try
        {
            auto& host = ViewerHost::Get();
            auto const window = host.CurrentWindow();
            auto const iconRect = m_iconRect;
            HWND const explorer = m_explorer;
            HWND const hwnd = host.Window()->Hwnd();

            try
            {
                co_await anim::Out(window, iconRect);
            }
            catch (...)
            {
            }
            if (serial != m_hideSerial)
            {
                co_return; // shown again while the window was animating out
            }

            // Give the foreground back to the Explorer window the preview was opened from while our window still holds it:
            // after Hide() Windows would pick some other window (decision 25).
            if (explorer != nullptr && IsWindow(explorer))
            {
                HWND const foreground = GetForegroundWindow();
                if (foreground == nullptr || foreground == hwnd)
                {
                    SetForegroundWindow(explorer);
                }
            }

            window.AppWindow().Hide();
            window.AppWindow().IsShownInSwitchers(false);
            if (auto* impl = winrt::get_self<winrt::FastLook::implementation::PreviewWindow>(window); impl->IsFullscreen())
            {
                impl->ToggleFullscreen(); // hidden by now: the next show starts windowed without a flash at the old bounds
            }
            if (m_renderToken)
            {
                winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(m_renderToken);
                m_renderToken = {};
            }

            // Unload only once the view is out of the window; an attached previewer's PrepareAsync has completed by definition.
            host.Window()->ClearContent();
            if (auto attached = std::exchange(m_attached, nullptr))
            {
                try
                {
                    attached->previewer->Unload();
                }
                catch (...)
                {
                }
            }
            core::perf::Mark("hidden");

            if (host.Callbacks().previewHidden)
            {
                host.Callbacks().previewHidden();
            }
            if (host.Behavior() == CloseBehavior::ExitProcess && host.PinnedCount() == 0)
            {
                host.ExitApp();
            }
        }
        catch (...)
        {
            FL_LOG_INFO(L"Hiding the preview failed with an exception");
        }
    }
}
