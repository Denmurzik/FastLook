#include "HookThread.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <cstdlib>
#include <string>

#include "../DaemonApp.h"
#include "Diagnostics/Log.h"
#include "FocusProbe.h"
#include "../SharedState.h"
#include "Text/Utf.h"

namespace fastlook::daemon
{
    // Fixed size, so recording never allocates: 1 us buckets, the last one collects everything >= kBuckets - 1; the exact
    // maximum is kept separately.
    struct Histogram
    {
        static constexpr uint32_t kBuckets = 256;
        uint32_t bucket[kBuckets]{};
        uint32_t count = 0;
        uint32_t maxUs = 0;

        void Record(uint32_t us) noexcept
        {
            ++bucket[us < kBuckets ? us : kBuckets - 1];
            ++count;
            maxUs = us > maxUs ? us : maxUs;
        }

        // Smallest bucket (in us) holding the given fraction of the samples; 0 without samples. With fewer than 1000 samples
        // p99 is the maximum bucket: read it together with `count`.
        uint32_t Percentile(uint32_t permille) const noexcept
        {
            if (count == 0)
            {
                return 0;
            }
            uint64_t const need = (static_cast<uint64_t>(count) * permille + 999) / 1000;
            uint64_t seen = 0;
            for (uint32_t i = 0; i < kBuckets; ++i)
            {
                seen += bucket[i];
                if (seen >= need)
                {
                    return i;
                }
            }
            return kBuckets - 1;
        }

        void Reset() noexcept
        {
            *this = Histogram{};
        }
    };

    struct HookThread::Context
    {
        ~Context()
        {
            if (ready)
            {
                CloseHandle(ready);
            }
        }

        core::hook::HookFilter filter; // hook thread only
        std::atomic<uint32_t> config{0};
        std::atomic<HHOOK> hook{nullptr};
        std::atomic<uint64_t> heartbeatMs{0};
        // Callback duration histograms, hook thread only: `all` for every callback, `space` for the fresh Space keydown path
        // (ProbeFocus + Decide), the one the 50 us budget is really about.
        Histogram all;
        Histogram space;
        Histogram trigger; // the subset of `space` that ended in a Trigger (adds the PostMessage)
        std::atomic<bool> abandoned{false};
        std::atomic<DWORD> threadId{0};
        HWND notify = nullptr;
        HANDLE ready = nullptr;
        // Callback-only state: the filter's own held flag is private, and ProbeFocus runs only for a fresh Space keydown.
        bool spaceHeld = false;
        uint32_t spaceDownMs = 0;
        bool installFailLogged = false; // hook thread only
        // Modifier keys as seen by the hook, one bit per physical key (hook thread only): saves five GetAsyncKeyState syscalls per Space. Re-synced from the
        // real keyboard state on install, reset and every heartbeat, so a lost event is healed within a second.
        uint8_t mods = 0;
    };

    namespace
    {
        using Context = HookThread::Context;

        enum : UINT
        {
            WM_HOOK_REHOOK = WM_APP + 1,
            WM_HOOK_APPLY = WM_APP + 2,
            WM_HOOK_TRIGGER = WM_APP + 4 // wParam: foreground HWND, lParam: Alt was held
        };

        constexpr UINT kHeartbeatMs = 1000;
        constexpr UINT kRehookMs = 60000;
        constexpr UINT kStallDelayMs = 3000;
        constexpr uint64_t kStaleMs = 5000;
        constexpr DWORD kReadyWaitMs = 2000;
        constexpr DWORD kStopWaitMs = 2000;
        // The filter heals a lost key-up after 1500 ms; ours must never be the longer one.
        constexpr uint32_t kSpaceStaleMs = 1000;
        constexpr WORD kAltFixVk = 0xE8; // unassigned: cancels the menu-bar activation on Alt release

        constexpr uint32_t kPausedBit = 0x200;
        constexpr uint32_t kDesktopBit = 0x100;

        thread_local Context* t_ctx = nullptr;
        std::atomic<bool> g_stallConsumed{false};
        uint64_t g_qpcFreq = 1;

        static_assert(sizeof(core::hook::HookConfig) == 8, "HookConfig changed: update Pack/Unpack");
        static_assert(static_cast<unsigned>(core::ActivationKey::ShiftSpace) <= 0xFF);

        // Excludes time spent suspended, so a resume never looks like a stale heartbeat.
        uint64_t NowMs() noexcept
        {
            ULONGLONG t100ns = 0;
            QueryUnbiasedInterruptTime(&t100ns);
            return t100ns / 10000;
        }

        uint32_t Pack(core::hook::HookConfig c) noexcept
        {
            return static_cast<uint32_t>(c.key) | (c.desktopEnabled ? kDesktopBit : 0) | (c.paused ? kPausedBit : 0);
        }

        core::hook::HookConfig Unpack(uint32_t v) noexcept
        {
            core::hook::HookConfig c;
            c.key = static_cast<core::ActivationKey>(v & 0xFF);
            c.desktopEnabled = (v & kDesktopBit) != 0;
            c.paused = (v & kPausedBit) != 0;
            return c;
        }

        uint64_t Qpc() noexcept
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return static_cast<uint64_t>(t.QuadPart);
        }

        // Touches the code paths and pages the callback uses, so the first real key does not pay for them.
        void WarmUp() noexcept
        {
            HWND const fg = GetForegroundWindow();
            GetAsyncKeyState(VK_SHIFT);
            Qpc();
            core::hook::FocusSnapshot snap;
            ProbeFocus(fg, snap);
            ProbeFocus(fg, snap);
        }

        bool KeyDown(int vk) noexcept
        {
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
        }

        // One bit per physical modifier key, in the order of kModKeys.
        constexpr int kModKeys[] = {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN};
        constexpr uint8_t kCtrlBits = 0x03, kAltBits = 0x0C, kShiftBits = 0x30, kWinBits = 0xC0;

        uint8_t ReadModifiers() noexcept
        {
            uint8_t m = 0;
            for (size_t i = 0; i < std::size(kModKeys); ++i)
            {
                m |= KeyDown(kModKeys[i]) ? static_cast<uint8_t>(1u << i) : 0;
            }
            return m;
        }

        void TrackModifier(Context& ctx, uint32_t vk, bool down) noexcept
        {
            for (size_t i = 0; i < std::size(kModKeys); ++i)
            {
                if (static_cast<uint32_t>(kModKeys[i]) == vk)
                {
                    uint8_t const bit = static_cast<uint8_t>(1u << i);
                    ctx.mods = down ? static_cast<uint8_t>(ctx.mods | bit) : static_cast<uint8_t>(ctx.mods & ~bit);
                    return;
                }
            }
        }

        LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam)
        {
            Context* const ctx = t_ctx;
            if (code != HC_ACTION || !ctx)
            {
                return CallNextHookEx(nullptr, code, wParam, lParam);
            }
            auto const& ll = *reinterpret_cast<KBDLLHOOKSTRUCT const*>(lParam);
            if (ll.dwExtraInfo == core::hook::kInjectedMarker)
            {
                return CallNextHookEx(nullptr, code, wParam, lParam);
            }
            uint64_t const t0 = Qpc();

            bool const down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            TrackModifier(*ctx, ll.vkCode, down);

            // Decide() only looks at Space and at type-ahead keydowns: everything else passes without any further syscall.
            if (ll.vkCode != VK_SPACE)
            {
                if (!down || !core::hook::IsTypeAheadKey(ll.vkCode))
                {
                    ctx->all.Record(static_cast<uint32_t>((Qpc() - t0) * 1000000 / g_qpcFreq));
                    return CallNextHookEx(nullptr, code, wParam, lParam);
                }
            }

            core::hook::KeySnapshot k;
            k.vk = ll.vkCode;
            k.keyDown = down;
            k.extraInfo = ll.dwExtraInfo;
            k.ctrl = (ctx->mods & kCtrlBits) != 0;
            k.alt = (ctx->mods & kAltBits) != 0;
            k.shift = (ctx->mods & kShiftBits) != 0;
            k.win = (ctx->mods & kWinBits) != 0;
            k.timeMs = ll.time;
            HWND const fg = GetForegroundWindow();
            k.fgThreadId = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;

            core::hook::FocusSnapshot focus;
            core::hook::FocusSnapshot const* focusPtr = nullptr;
            if (k.vk == VK_SPACE)
            {
                if (k.keyDown)
                {
                    bool const held = ctx->spaceHeld && k.timeMs - ctx->spaceDownMs <= kSpaceStaleMs;
                    ctx->spaceHeld = true;
                    ctx->spaceDownMs = k.timeMs;
                    if (!held)
                    {
                        ProbeFocus(fg, focus);
                        focusPtr = &focus;
                    }
                }
                else
                {
                    ctx->spaceHeld = false;
                }
            }

            auto const verdict = ctx->filter.Decide(k, focusPtr, Unpack(ctx->config.load(std::memory_order_relaxed)));

            if (verdict == core::hook::HookVerdict::Trigger)
            {
                g_lastTriggerQpc.store(t0, std::memory_order_relaxed);
                // Posting to our own queue is far cheaper than waking the daemon's thread inside the callback; the loop
                // forwards it as soon as the callback returns.
                PostThreadMessageW(ctx->threadId.load(std::memory_order_relaxed), WM_HOOK_TRIGGER, reinterpret_cast<WPARAM>(fg), k.alt ? 1 : 0);
            }

            uint64_t const t1 = Qpc();
            auto const us = static_cast<uint32_t>((t1 - t0) * 1000000 / g_qpcFreq);
            ctx->all.Record(us);
            if (focusPtr)
            {
                ctx->space.Record(us);
                if (verdict == core::hook::HookVerdict::Trigger)
                {
                    ctx->trigger.Record(us);
                }
            }

            if (verdict == core::hook::HookVerdict::Pass)
            {
                return CallNextHookEx(nullptr, code, wParam, lParam);
            }
            return 1;
        }

        void RemoveHook(Context& ctx)
        {
            if (HHOOK const h = ctx.hook.exchange(nullptr))
            {
                UnhookWindowsHookEx(h);
            }
        }

        void ResetState(Context& ctx)
        {
            ctx.filter.Reset();
            ctx.spaceHeld = false;
            ctx.mods = ReadModifiers();
        }

        // Installs the new hook before removing the old one, so there is no gap. While paused, only removes.
        // resetFilter: forget held/swallow/type-ahead state; false for the periodic rehook, which has no gap and
        // so cannot have lost a key-up (a reset there would drop a live swallow or type-ahead window).
        void InstallHook(Context& ctx, bool resetFilter)
        {
            if (ctx.abandoned.load())
            {
                return;
            }
            if (Unpack(ctx.config.load()).paused)
            {
                if (ctx.hook.load())
                {
                    RemoveHook(ctx);
                    FL_LOG_INFO(L"hook removed (paused)");
                }
                ResetState(ctx);
                return;
            }
            HHOOK const fresh = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyboardProc, GetModuleHandleW(nullptr), 0);
            if (!fresh)
            {
                if (!ctx.installFailLogged)
                {
                    ctx.installFailLogged = true;
                    FL_LOG_INFO(L"hook install failed {}", GetLastError());
                }
                return; // keep the previous hook, if any; the heartbeat tick retries when there is none
            }
            if (HHOOK const old = ctx.hook.exchange(fresh))
            {
                UnhookWindowsHookEx(old);
            }
            ctx.installFailLogged = false;
            if (resetFilter)
            {
                ResetState(ctx);
            }
            if (ctx.abandoned.load())
            {
                RemoveHook(ctx); // the watchdog gave up on this thread while it was installing
                return;
            }
            FL_LOG_INFO(L"hook installed hook_cb_max_us={} hook_cb_p50_us={} hook_cb_p99_us={} hook_cb_count={} hook_space_max_us={} "
                        L"hook_space_p50_us={} hook_space_p99_us={} hook_space_count={} hook_trigger_max_us={} hook_trigger_p50_us={} hook_trigger_count={}",
                        ctx.all.maxUs, ctx.all.Percentile(500), ctx.all.Percentile(990), ctx.all.count, ctx.space.maxUs,
                        ctx.space.Percentile(500), ctx.space.Percentile(990), ctx.space.count, ctx.trigger.maxUs, ctx.trigger.Percentile(500),
                        ctx.trigger.count);
            ctx.all.Reset();
            ctx.space.Reset();
            ctx.trigger.Reset();
        }

        // Alt+Space was swallowed: a tagged unassigned key between Alt down and up stops Explorer showing its menu bar on Alt release.
        void InjectAltFix()
        {
            INPUT in[2]{};
            for (auto& i : in)
            {
                i.type = INPUT_KEYBOARD;
                i.ki.wVk = kAltFixVk;
                i.ki.dwExtraInfo = core::hook::kInjectedMarker;
            }
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
        }

        DWORD WINAPI ThreadMain(void* param)
        {
            std::unique_ptr<std::shared_ptr<Context>> holder(static_cast<std::shared_ptr<Context>*>(param));
            Context& ctx = **holder;
            t_ctx = &ctx;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

            MSG msg;
            PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // creates the thread's message queue
            ctx.threadId = GetCurrentThreadId();
            ctx.heartbeatMs = NowMs();

            UINT_PTR const heartbeatTimer = SetTimer(nullptr, 0, kHeartbeatMs, nullptr);
            UINT_PTR const rehookTimer = SetTimer(nullptr, 0, kRehookMs, nullptr);
            UINT_PTR stallTimer = 0;
            DWORD stallMs = 0;
            wchar_t env[16];
            if (DWORD const n = GetEnvironmentVariableW(L"FASTLOOK_TEST_STALL_HOOK_MS", env, 16); n > 0 && n < 16)
            {
                stallMs = static_cast<DWORD>(wcstoul(env, nullptr, 10));
                if (stallMs > 0 && !g_stallConsumed.load())
                {
                    stallTimer = SetTimer(nullptr, 0, kStallDelayMs, nullptr);
                }
            }

            WarmUp();
            InstallHook(ctx, true);
            SetEvent(ctx.ready);

            while (!ctx.abandoned.load() && GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                switch (msg.message)
                {
                case WM_TIMER:
                    if (msg.wParam == heartbeatTimer)
                    {
                        ctx.heartbeatMs = NowMs();
                        ctx.mods = ReadModifiers();
                        if (!ctx.hook.load() && !Unpack(ctx.config.load()).paused)
                        {
                            InstallHook(ctx, true); // an earlier install failed: do not wait for the 60 s timer
                        }
                    }
                    else if (msg.wParam == rehookTimer)
                    {
                        FL_LOG_INFO(L"rehook reason=timer");
                        InstallHook(ctx, false);
                    }
                    else if (stallTimer != 0 && msg.wParam == stallTimer)
                    {
                        KillTimer(nullptr, stallTimer);
                        stallTimer = 0;
                        if (!g_stallConsumed.exchange(true))
                        {
                            FL_LOG_INFO(L"test: stalling hook thread for {} ms", stallMs);
                            Sleep(stallMs);
                        }
                    }
                    break;
                case WM_HOOK_REHOOK:
                    InstallHook(ctx, true);
                    break;
                case WM_HOOK_APPLY:
                    // Install after unpause, remove on pause; otherwise nothing changed.
                    if (Unpack(ctx.config.load()).paused == (ctx.hook.load() != nullptr))
                    {
                        InstallHook(ctx, true);
                    }
                    break;
                case WM_HOOK_TRIGGER:
                    PostMessageW(ctx.notify, WM_FL_TRIGGER, msg.wParam, 0);
                    if (msg.lParam)
                    {
                        InjectAltFix();
                    }
                    break;
                default:
                    break;
                }
            }

            KillTimer(nullptr, heartbeatTimer);
            KillTimer(nullptr, rehookTimer);
            if (ctx.abandoned.load())
            {
                FL_LOG_INFO(L"abandoned hook thread exited");
            }
            else
            {
                RemoveHook(ctx);
            }
            t_ctx = nullptr;
            return 0;
        }
    }

    HookThread::~HookThread()
    {
        Stop();
    }

    bool HookThread::Launch()
    {
        auto ctx = std::make_shared<Context>();
        ctx->config = Pack(m_config);
        ctx->notify = m_notify;
        ctx->heartbeatMs = NowMs();
        ctx->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ctx->ready)
        {
            return false;
        }
        auto* param = new std::shared_ptr<Context>(ctx);
        HANDLE const thread = CreateThread(nullptr, 0, &ThreadMain, param, 0, nullptr);
        if (!thread)
        {
            delete param;
            return false;
        }
        m_ctx = std::move(ctx);
        m_thread = thread;
        WaitForSingleObject(m_ctx->ready, kReadyWaitMs);
        return true;
    }

    bool HookThread::Start(HWND notify, core::hook::HookConfig config)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = static_cast<uint64_t>(f.QuadPart);
        m_notify = notify;
        m_config = config;
        m_stopped = false;
        bool const ok = Launch();
        if (!ok)
        {
            FL_LOG_INFO(L"hook thread start failed {}", GetLastError());
        }
        return ok;
    }

    void HookThread::Stop()
    {
        m_stopped = true;
        if (!m_ctx)
        {
            return;
        }
        PostThreadMessageW(m_ctx->threadId, WM_QUIT, 0, 0);
        if (WaitForSingleObject(m_thread, kStopWaitMs) != WAIT_OBJECT_0)
        {
            m_ctx->abandoned = true;
            RemoveHook(*m_ctx);
        }
        CloseHandle(m_thread);
        m_thread = nullptr;
        m_ctx.reset();
        FL_LOG_INFO(L"hook stopped");
    }

    void HookThread::SetConfig(core::hook::HookConfig config)
    {
        m_config = config;
        FL_LOG_INFO(L"hook config paused={}", config.paused);
        if (m_ctx)
        {
            m_ctx->config = Pack(config);
            PostThreadMessageW(m_ctx->threadId, WM_HOOK_APPLY, 0, 0);
        }
    }

    void HookThread::Rehook(std::string_view reason)
    {
        FL_LOG_INFO(L"rehook reason={}", core::FromUtf8(reason));
        if (m_ctx)
        {
            PostThreadMessageW(m_ctx->threadId, WM_HOOK_REHOOK, 0, 0);
        }
    }

    void HookThread::CheckWatchdog()
    {
        if (m_stopped)
        {
            return;
        }
        if (!m_ctx) // an earlier relaunch failed: keep trying on every tick
        {
            if (!Launch())
            {
                FL_LOG_INFO(L"hook thread relaunch failed {}", GetLastError());
            }
            return;
        }
        if (NowMs() - m_ctx->heartbeatMs.load() <= kStaleMs)
        {
            return;
        }
        FL_LOG_INFO(L"hook watchdog restart");
        // Unhook from here: the stalled thread cannot, and it exits on its own once it wakes.
        m_ctx->abandoned = true;
        RemoveHook(*m_ctx);
        CloseHandle(m_thread);
        m_thread = nullptr;
        m_ctx.reset();
        if (!Launch())
        {
            FL_LOG_INFO(L"hook thread relaunch failed {}", GetLastError());
        }
    }
}
