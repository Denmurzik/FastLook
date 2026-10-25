#include "pch.h"
#include "Previewers/Shell/ShellPreviewer.h"

#include <atomic>
#include <cmath>
#include <thread>

#include "Diagnostics/Log.h"
#include "Ipc/HostProtocol.h"
#include "Previewers/Factories.h"
#include "Shell/HandlerLookup.h"
#include "System/AppPaths.h"
#include "Util/Async.h"
#include "Util/Str.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    namespace proto = core::host;

    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;

        constexpr wchar_t kSiteClass[] = L"FastLookShellSite";
        constexpr auto kReadyTimeout = std::chrono::milliseconds(8000);
        constexpr DWORD kQuitWaitMs = 300;
        constexpr DWORD kKillWaitMs = 500;
        constexpr UINT kWatchdogProbeMs = 200;
        constexpr int kWatchdogFailures = 2;
        constexpr int kTitleBarDip = 48;
        constexpr auto kParkInterval = std::chrono::seconds(1);

        LRESULT CALLBACK SiteProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_ERASEBKGND)
            {
                return 1; // the handler paints its own area
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        void RegisterSiteClass()
        {
            static bool const registered = [] {
                WNDCLASSW wc{};
                wc.lpfnWndProc = SiteProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = kSiteClass;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                return RegisterClassW(&wc) != 0;
            }();
            (void)registered;
        }

        // True when `window`'s thread answers a message within `ms`. Background threads only: this is a blocking send.
        bool ProbeResponsive(HWND window, UINT ms)
        {
            DWORD_PTR result = 0;
            return SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, ms, &result) != 0;
        }

        struct ProbeState
        {
            std::atomic<bool> inFlight{false};
            std::atomic<ULONGLONG> okAt{0};
        };

        // Sites that still had a foreign child when their previewer unloaded. They are hidden and stay so until a fresh background
        // probe shows the child's thread answering messages (DestroyWindow sends to the child): a hidden HWND costs nothing.
        struct ParkedSite
        {
            HWND site = nullptr;
            std::shared_ptr<ProbeState> probe = std::make_shared<ProbeState>();
        };

        struct Parked
        {
            std::vector<ParkedSite> sites;
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
        };

        Parked& ParkedSites()
        {
            static Parked* parked = new Parked; // intentionally leaked: WinRT objects must not die in static destruction
            return *parked;
        }

        constexpr ULONGLONG kProbeFreshMs = 1500;

        void SweepParked()
        {
            auto& parked = ParkedSites();
            std::erase_if(parked.sites, [](ParkedSite& entry) {
                if (!IsWindow(entry.site))
                {
                    return true;
                }
                HWND const child = GetWindow(entry.site, GW_CHILD);
                if (!child)
                {
                    DestroyWindow(entry.site);
                    return true;
                }
                ULONGLONG const okAt = entry.probe->okAt.load();
                if (okAt != 0 && GetTickCount64() - okAt < kProbeFreshMs)
                {
                    DestroyWindow(entry.site);
                    return true;
                }
                if (!entry.probe->inFlight.exchange(true))
                {
                    std::thread([probe = entry.probe, child] {
                        if (ProbeResponsive(child, 200))
                        {
                            probe->okAt = GetTickCount64();
                        }
                        probe->inFlight = false;
                    }).detach();
                }
                return false;
            });
            if (parked.sites.empty() && parked.timer)
            {
                parked.timer.Stop();
            }
        }

        void Park(HWND site, winrt::Microsoft::UI::Dispatching::DispatcherQueue const& ui)
        {
            auto& parked = ParkedSites();
            parked.sites.push_back(ParkedSite{site});
            if (!parked.timer)
            {
                parked.timer = ui.CreateTimer();
                parked.timer.Interval(kParkInterval);
                parked.timer.IsRepeating(true);
                parked.timer.Tick([](auto const&, auto const&) { SweepParked(); });
            }
            parked.timer.Start();
        }

        // Last step of an unload, on the UI thread, once our host is gone: destroy the site, or park it while a foreign
        // process (WINWORD/EXCEL) still owns a window under it.
        void FinishSite(HWND site, winrt::Microsoft::UI::Dispatching::DispatcherQueue const& ui)
        {
            if (!IsWindow(site))
            {
                return;
            }
            if (GetWindow(site, GW_CHILD))
            {
                Park(site, ui);
            }
            else
            {
                DestroyWindow(site);
            }
        }

        // Keyboard focus belongs to the XAML island's input site, not to the top-level window: only there do key presses reach
        // XAML (PreviewKeyDown -> Esc/Space/arrows). The handler's window takes the focus when it is created or shown.
        void FocusInputSite(HWND top)
        {
            HWND inputSite = nullptr;
            EnumChildWindows(
                top,
                [](HWND child, LPARAM param) -> BOOL {
                    wchar_t name[64]{};
                    GetClassNameW(child, name, static_cast<int>(std::size(name)));
                    if (std::wstring_view{name} == L"InputSiteWindowClass")
                    {
                        *reinterpret_cast<HWND*>(param) = child;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&inputSite));
            SetFocus(inputSite ? inputSite : top);
        }

        // SetFocus sends WM_KILLFOCUS to the window that has it; with the queues attached that is a blocking send to the handler's
        // thread. So when the focus sits in a foreign thread, that thread is probed in the background first.
        void ReturnFocusToXaml(HWND top, winrt::Microsoft::UI::Dispatching::DispatcherQueue const& ui,
                               std::shared_ptr<std::atomic<bool>> const& alive)
        {
            HWND const focused = GetFocus();
            if (!focused || GetWindowThreadProcessId(focused, nullptr) == GetCurrentThreadId())
            {
                FocusInputSite(top);
                return;
            }
            std::thread([top, ui, alive, focused] {
                if (!ProbeResponsive(focused, 200))
                {
                    FL_LOG_INFO(L"focus not returned: the window holding it does not answer");
                    return;
                }
                ui.TryEnqueue([top, alive] {
                    if (alive->load())
                    {
                        FocusInputSite(top);
                    }
                });
            }).detach();
        }

        std::wstring FailedText()
        {
            return std::wstring{Str(L"Shell", L"Failed")};
        }

        // FASTLOOK_HOST_DEBUG_HANG: 1|before -> hang before ready; after-ready -> hang after it.
        void ReadDebugHang(proto::HostArgs& args)
        {
            wchar_t value[32]{};
            DWORD const n = GetEnvironmentVariableW(L"FASTLOOK_HOST_DEBUG_HANG", value, static_cast<DWORD>(std::size(value)));
            if (n == 0 || n >= std::size(value))
            {
                return;
            }
            std::wstring_view const text{value, n};
            if (text == L"1" || text == L"before")
            {
                args.debugHang = true;
            }
            else if (text == L"after-ready")
            {
                args.debugHangAfterReady = true;
            }
        }
    }

    winrt::Windows::Foundation::IAsyncAction ShellPreviewer::PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx)
    {
        auto self = std::static_pointer_cast<ShellPreviewer>(shared_from_this());
        auto cancel = co_await winrt::get_cancellation_token();
        m_ctx = ctx;
        auto const ui = ctx->ui;
        if (!co_await ResumeOn(ui))
        {
            co_return;
        }
        if (!item.shellHandlerClsid)
        {
            throw PreviewError{FailedText(), std::nullopt};
        }

        std::wstring const clsid = *item.shellHandlerClsid;
        CreateSite();
        if (!m_site)
        {
            throw PreviewError{FailedText(), std::nullopt};
        }
        RECT client{};
        GetClientRect(m_site, &client);

        proto::HostArgs args;
        args.parentHwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_site));
        args.clsid = clsid;
        args.file = item.ref.path;
        args.rect = core::RectI{0, 0, client.right, client.bottom};
        args.dark = ctx->dark;
        ReadDebugHang(args);

        auto const alive = m_alive;
        auto onKey = [ui, alive](uint32_t vk, uint32_t mods) {
            ui.TryEnqueue([alive, vk, mods] {
                if (alive->load())
                {
                    ViewerHost::Get().HandleKey(vk, (mods & proto::kModCtrl) != 0, (mods & proto::kModShift) != 0, (mods & proto::kModAlt) != 0);
                }
            });
        };

        co_await winrt::resume_background();
        HostProcess::Result result{HostProcess::Outcome::Error};
        std::shared_ptr<HostProcess> host;
        // The blocklist is decided at classification; checked again where the handler is actually launched.
        if (core::IsBlockedPreviewHandler(clsid, core::HandlerServerPath(clsid)))
        {
            FL_LOG_INFO(L"shell handler blocked");
        }
        else
        {
            std::wstring launchError;
            host = HostProcess::Launch(core::CurrentAppPaths().exeDir / L"FastLook.PreviewHost.exe", args, std::move(onKey), launchError);
            if (!host)
            {
                FL_LOG_INFO(L"PreviewHost launch failed: {}", launchError);
            }
            else
            {
                result = host->WaitReady(kReadyTimeout, [&] { return cancel(); });
            }
        }

        if (!co_await ResumeOn(ui))
        {
            if (host)
            {
                host->Terminate();
            }
            co_return;
        }
        m_host = std::move(host);
        if (result.outcome == HostProcess::Outcome::Cancelled)
        {
            co_return; // navigation: the session unloads this previewer, which kills the host
        }
        if (result.outcome != HostProcess::Outcome::Ready)
        {
            FL_LOG_INFO(L"shell preview failed: outcome={} hr={:#010x}", static_cast<int>(result.outcome), result.hr);
            Unload();
            throw PreviewError{FailedText(), std::nullopt};
        }

        m_ready = true;
        // The handler window took the focus when it was created: give it back so Esc/Space reach the Viewer.
        ReturnFocusToXaml(ctx->hwnd, ui, m_alive);
        StartWatchdog();
        FL_LOG_INFO(L"shell preview ready");
    }

    void ShellPreviewer::CreateSite()
    {
        RegisterSiteClass();
        RECT client{};
        GetClientRect(m_ctx->hwnd, &client);
        int const top = static_cast<int>(std::lround(kTitleBarDip * m_ctx->scale));
        int const width = std::max<int>(1, client.right);
        int const height = std::max<int>(1, client.bottom - top);
        // Not visible until the placeholder is laid out: the handler's window is created into an invisible parent.
        // WS_EX_LAYERED gives the site its own DWM surface: without it the XAML island (a DirectComposition child) is composed over
        // every non-layered sibling, whatever the z-order, and the handler's window stays invisible.
        m_site = CreateWindowExW(WS_EX_LAYERED, kSiteClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, top, width, height, m_ctx->hwnd,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!m_site)
        {
            FL_LOG_INFO(L"site window failed: {}", GetLastError());
        }
        else
        {
            SetLayeredWindowAttributes(m_site, 0, 255, LWA_ALPHA);
        }
        m_lastRect = core::RectI{0, top, width, height};
    }

    winrt::Microsoft::UI::Xaml::UIElement ShellPreviewer::View()
    {
        if (!m_placeholder)
        {
            m_placeholder = Grid{};
            m_placeholder.HorizontalAlignment(HorizontalAlignment::Stretch);
            m_placeholder.VerticalAlignment(VerticalAlignment::Stretch);
            std::weak_ptr<ShellPreviewer> const weak = std::static_pointer_cast<ShellPreviewer>(shared_from_this());
            m_sizeRevoker = m_placeholder.SizeChanged(winrt::auto_revoke, [weak](auto const&, auto const&) {
                if (auto self = weak.lock())
                {
                    self->UpdateBounds();
                }
            });
            m_layoutRevoker = m_placeholder.LayoutUpdated(winrt::auto_revoke, [weak](auto const&, auto const&) {
                if (auto self = weak.lock())
                {
                    self->UpdateBounds();
                }
            });
            m_loadedRevoker = m_placeholder.Loaded(winrt::auto_revoke, [weak](auto const&, auto const&) {
                if (auto self = weak.lock())
                {
                    self->UpdateBounds();
                    self->FocusRoot();
                }
            });
        }
        return m_placeholder;
    }

    void ShellPreviewer::FocusRoot()
    {
        if (m_unloaded || !m_placeholder)
        {
            return;
        }
        ReturnFocusToXaml(m_ctx->hwnd, m_ctx->ui, m_alive);
        try
        {
            if (auto root = m_placeholder.XamlRoot())
            {
                if (auto content = root.Content())
                {
                    content.Focus(FocusState::Programmatic);
                }
            }
        }
        catch (...)
        {
        }
    }

    void ShellPreviewer::UpdateBounds()
    {
        if (m_unloaded || !m_ready || !m_site || !m_placeholder)
        {
            return;
        }
        try
        {
            auto const root = m_placeholder.XamlRoot();
            if (!root)
            {
                return;
            }
            auto const transform = m_placeholder.TransformToVisual(root.Content());
            auto const bounds = transform.TransformBounds({0.0f, 0.0f, static_cast<float>(m_placeholder.ActualWidth()),
                                                          static_cast<float>(m_placeholder.ActualHeight())});
            double const scale = root.RasterizationScale();
            core::RectI const rect{static_cast<int32_t>(std::lround(bounds.X * scale)), static_cast<int32_t>(std::lround(bounds.Y * scale)),
                                   static_cast<int32_t>(std::lround(bounds.Width * scale)), static_cast<int32_t>(std::lround(bounds.Height * scale))};
            if (rect.w <= 0 || rect.h <= 0 || rect == m_lastRect)
            {
                return;
            }
            // The site belongs to this thread, so SWP_ASYNCWINDOWPOS only keeps the call from sending to windows of other threads.
            SetWindowPos(m_site, HWND_TOP, rect.x, rect.y, rect.w, rect.h, SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (m_host && (rect.w != m_lastRect.w || rect.h != m_lastRect.h))
            {
                m_host->SendRect(core::RectI{0, 0, rect.w, rect.h});
            }
            bool const firstShow = !m_shown;
            m_shown = true;
            m_lastRect = rect;
            if (firstShow)
            {
                FocusRoot(); // the handler's window may take the focus when it becomes visible
            }
        }
        catch (...)
        {
            FL_LOG_INFO(L"shell placeholder bounds failed");
        }
    }

    void ShellPreviewer::StartWatchdog()
    {
        std::weak_ptr<ShellPreviewer> const weak = std::static_pointer_cast<ShellPreviewer>(shared_from_this());
        auto const alive = m_alive;
        auto const ui = m_ctx->ui;
        HWND const site = m_site;
        // Probes the handler's window once a second. A handler that stops answering would stall the Viewer's attached input queue.
        std::thread([weak, alive, ui, site] {
            int failures = 0;
            while (alive->load())
            {
                for (int slice = 0; slice < 10 && alive->load(); ++slice)
                {
                    Sleep(100);
                }
                if (!alive->load())
                {
                    return;
                }
                HWND const child = GetWindow(site, GW_CHILD);
                if (!child)
                {
                    continue;
                }
                failures = ProbeResponsive(child, kWatchdogProbeMs) ? 0 : failures + 1;
                if (failures >= kWatchdogFailures)
                {
                    ui.TryEnqueue([weak] {
                        if (auto self = weak.lock())
                        {
                            self->OnHung();
                        }
                    });
                    return;
                }
            }
        }).detach();
    }

    void ShellPreviewer::OnHung()
    {
        if (m_unloaded || !m_ready)
        {
            return;
        }
        FL_LOG_INFO(L"shell handler window stopped answering: host terminated");
        m_ready = false;
        if (m_host)
        {
            m_host->DetachKeyHandler();
            m_host->Terminate(); // our host's death destroys an in-process handler window and detaches the input queues
        }
        if (m_site)
        {
            ShowWindow(m_site, SW_HIDE); // a foreign handler window stays parked under the hidden site until Unload
        }
        if (m_placeholder)
        {
            TextBlock text;
            text.Text(winrt::hstring{FailedText()});
            text.HorizontalAlignment(HorizontalAlignment::Center);
            text.VerticalAlignment(VerticalAlignment::Center);
            m_placeholder.Children().Append(text);
        }
        FocusRoot();
    }

    void ShellPreviewer::Unload()
    {
        if (m_unloaded)
        {
            return;
        }
        m_unloaded = true;
        m_alive->store(false);
        m_sizeRevoker.revoke();
        m_layoutRevoker.revoke();
        m_loadedRevoker.revoke();

        HWND const site = std::exchange(m_site, nullptr);
        if (site)
        {
            ShowWindow(site, SW_HIDE);
        }
        auto const ui = m_ctx->ui;
        if (auto host = std::move(m_host))
        {
            host->DetachKeyHandler();
            host->RequestQuit();
            // The host's own teardown needs this thread to answer sent messages (child destruction notifies the site), so
            // nothing here waits: the exit wait and the kill happen on a worker, the site is finished back on the UI thread.
            std::thread([host, site, ui] {
                bool const graceful = host->WaitExit(kQuitWaitMs);
                if (!graceful)
                {
                    host->Terminate(); // our PreviewHost only
                    host->WaitExit(kKillWaitMs);
                }
                FL_LOG_INFO(L"shell host {}", graceful ? L"exited on quit" : L"terminated after the quit timeout");
                if (site && !ui.TryEnqueue([site, ui] { FinishSite(site, ui); }))
                {
                    FL_LOG_INFO(L"shell site left to the window teardown");
                }
            }).detach();
        }
        else if (site)
        {
            FinishSite(site, ui);
        }
        m_placeholder = nullptr;
    }

    std::shared_ptr<IPreviewer> CreateShellPreviewer()
    {
        return std::make_shared<ShellPreviewer>();
    }
}
