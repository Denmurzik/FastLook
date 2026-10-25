#include "HookThread.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "../DaemonApp.h"
#include "Diagnostics/Log.h"
#include "FocusProbe.h"
#include "../SharedState.h"
#include "Text/Utf.h"

namespace fastlook::daemon
{
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
        std::atomic<uint32_t> cbMaxUs{0};
        std::atomic<bool> abandoned{false};
        std::atomic<DWORD> threadId{0};
        HWND notify = nullptr;
        HANDLE ready = nullptr;
        // Callback-only state: the filter's own held flag is private, and ProbeFocus runs only for a fresh Space keydown.
        bool spaceHeld = false;
        uint32_t spaceDownMs = 0;
    };

    namespace
    {
        using Context = HookThread::Context;

        enum : UINT
        {
            WM_HOOK_REHOOK = WM_APP + 1,
            WM_HOOK_APPLY = WM_APP + 2,
            WM_HOOK_ALT_FIX = WM_APP + 3
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

        bool KeyDown(int vk) noexcept
        {
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
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

            core::hook::KeySnapshot k;
            k.vk = ll.vkCode;
            k.keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            k.extraInfo = ll.dwExtraInfo;
            k.ctrl = KeyDown(VK_CONTROL);
            k.alt = KeyDown(VK_MENU);
            k.shift = KeyDown(VK_SHIFT);
            k.win = KeyDown(VK_LWIN) || KeyDown(VK_RWIN);
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
                PostMessageW(ctx->notify, WM_FL_TRIGGER, reinterpret_cast<WPARAM>(fg), 0);
                if (k.alt)
                {
                    PostThreadMessageW(ctx->threadId.load(std::memory_order_relaxed), WM_HOOK_ALT_FIX, 0, 0);
                }
            }

            uint64_t const t1 = Qpc();
            auto const us = static_cast<uint32_t>((t1 - t0) * 1000000 / g_qpcFreq);
            uint32_t seen = ctx->cbMaxUs.load(std::memory_order_relaxed);
            while (us > seen && !ctx->cbMaxUs.compare_exchange_weak(seen, us, std::memory_order_relaxed))
            {
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

        // Installs the new hook before removing the old one, so there is no gap. While paused, only removes.
        void InstallHook(Context& ctx)
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
                ctx.filter.Reset();
                return;
            }
            HHOOK const fresh = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyboardProc, GetModuleHandleW(nullptr), 0);
            if (!fresh)
            {
                FL_LOG_INFO(L"hook install failed {}", GetLastError());
                return; // keep the previous hook, if any
            }
            if (HHOOK const old = ctx.hook.exchange(fresh))
            {
                UnhookWindowsHookEx(old);
            }
            ctx.filter.Reset();
            if (ctx.abandoned.load())
            {
                RemoveHook(ctx); // the watchdog gave up on this thread while it was installing
                return;
            }
            FL_LOG_INFO(L"hook installed hook_cb_max_us={}", ctx.cbMaxUs.exchange(0));
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
            ctx.heartbeatMs = GetTickCount64();

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

            InstallHook(ctx);
            SetEvent(ctx.ready);

            while (!ctx.abandoned.load() && GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                switch (msg.message)
                {
                case WM_TIMER:
                    if (msg.wParam == heartbeatTimer)
                    {
                        ctx.heartbeatMs = GetTickCount64();
                    }
                    else if (msg.wParam == rehookTimer)
                    {
                        FL_LOG_INFO(L"rehook reason=timer");
                        InstallHook(ctx);
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
                    InstallHook(ctx);
                    break;
                case WM_HOOK_APPLY:
                    // Install after unpause, remove on pause; otherwise nothing changed.
                    if (Unpack(ctx.config.load()).paused == (ctx.hook.load() != nullptr))
                    {
                        InstallHook(ctx);
                    }
                    break;
                case WM_HOOK_ALT_FIX:
                    InjectAltFix();
                    break;
                default:
                    break;
                }
            }

            KillTimer(nullptr, heartbeatTimer);
            KillTimer(nullptr, rehookTimer);
            if (!ctx.abandoned.load())
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
        ctx->heartbeatMs = GetTickCount64();
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
        if (m_stopped || !m_ctx || GetTickCount64() - m_ctx->heartbeatMs.load() <= kStaleMs)
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
        Launch();
    }
}
