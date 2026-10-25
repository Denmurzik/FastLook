#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Diagnostics/Log.h"
#include "HandlerHost.h"
#include "Ipc/HostProtocol.h"
#include "System/AppPaths.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    namespace proto = fastlook::core::host;
    using fastlook::core::LogLevel;

    constexpr UINT kMsgRect = WM_APP + 1;
    constexpr UINT kMsgQuit = WM_APP + 2;
    constexpr wchar_t kControlClass[] = L"FastLookPreviewHostControl";

    std::mutex g_writeMutex;

    void WriteReply(proto::HostReply const& reply)
    {
        std::string const line = proto::FormatHostReply(reply);
        HANDLE const out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE)
        {
            return;
        }
        std::lock_guard lock{g_writeMutex};
        DWORD written = 0;
        WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    void SendKey(uint32_t vk, uint32_t mods)
    {
        WriteReply({.status = proto::HostStatus::Key, .vk = vk, .mods = mods});
    }

    [[noreturn]] void Fail(HRESULT hr, std::string text)
    {
        FL_LOG_INFO(L"host failed: {:#010x} {}", static_cast<uint32_t>(hr), std::wstring(text.begin(), text.end()));
        WriteReply({.status = proto::HostStatus::Error, .hr = static_cast<uint32_t>(hr), .text = std::move(text)});
        ExitProcess(1);
    }

    RECT ToRect(fastlook::core::RectI const& r)
    {
        return RECT{r.x, r.y, r.x + r.w, r.y + r.h};
    }

    // Reads command lines from stdin and posts them to the control window; EOF (the Viewer is gone) posts quit.
    void StdinReader(HWND control)
    {
        HANDLE const in = GetStdHandle(STD_INPUT_HANDLE);
        std::string pending;
        char buffer[512];
        bool open = in != nullptr && in != INVALID_HANDLE_VALUE;
        while (open)
        {
            DWORD read = 0;
            if (!ReadFile(in, buffer, sizeof(buffer), &read, nullptr) || read == 0)
            {
                break;
            }
            pending.append(buffer, read);
            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos)
            {
                auto command = proto::ParseHostCommand(std::string_view(pending).substr(0, newline));
                pending.erase(0, newline + 1);
                if (!command)
                {
                    continue;
                }
                if (command->cmd == proto::HostCmd::Quit)
                {
                    open = false;
                    break;
                }
                auto* rect = new RECT(ToRect(command->rect));
                if (!PostMessageW(control, kMsgRect, 0, reinterpret_cast<LPARAM>(rect)))
                {
                    delete rect;
                }
            }
        }
        PostMessageW(control, kMsgQuit, 0, 0);
    }

    struct HostState
    {
        fastlook::host::HandlerHost* handler = nullptr;
        bool quitting = false;
    };

    LRESULT CALLBACK ControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<HostState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message)
        {
        case kMsgRect:
        {
            std::unique_ptr<RECT> rect{reinterpret_cast<RECT*>(lParam)};
            if (state && state->handler && !state->quitting)
            {
                state->handler->SetRect(*rect);
            }
            return 0;
        }
        case kMsgQuit:
            if (state)
            {
                state->quitting = true;
                if (state->handler)
                {
                    state->handler->Unload();
                }
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    HWND CreateControlWindow(HostState* state)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ControlProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kControlClass;
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(0, kControlClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (hwnd)
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        return hwnd;
    }

    [[noreturn]] void SleepForever()
    {
        for (;;)
        {
            Sleep(INFINITE);
        }
    }

    std::optional<proto::HostArgs> ReadArgs()
    {
        int count = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!argv)
        {
            return std::nullopt;
        }
        std::vector<std::wstring_view> args(argv + 1, argv + count);
        auto parsed = proto::ParseHostArgs(args);
        LocalFree(argv);
        return parsed;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    fastlook::core::log::Init(fastlook::core::CurrentAppPaths().logsDir, L"host", LogLevel::Info);

    auto const args = ReadArgs();
    if (!args)
    {
        Fail(E_INVALIDARG, "bad command line");
    }
    if (HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); FAILED(hr))
    {
        Fail(hr, "CoInitializeEx failed");
    }

    HostState state;
    HWND const control = CreateControlWindow(&state);
    if (!control)
    {
        Fail(HRESULT_FROM_WIN32(GetLastError()), "control window failed");
    }

    fastlook::host::HandlerHost handler(&SendKey);
    state.handler = &handler;

    if (HRESULT hr = handler.Create(args->clsid, args->file); FAILED(hr))
    {
        Fail(hr, "handler could not be created or initialised");
    }
    FL_LOG_INFO(L"handler created");
    HWND const parent = reinterpret_cast<HWND>(static_cast<uintptr_t>(args->parentHwnd));
    if (!IsWindow(parent))
    {
        Fail(HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE), "parent window is gone");
    }
    if (HRESULT hr = handler.Attach(parent, ToRect(args->rect), args->dark); FAILED(hr))
    {
        Fail(hr, "SetWindow failed");
    }

    FL_LOG_INFO(L"handler attached");

    // Only now read stdin: the first command can be a rect for the already attached handler.
    std::thread(StdinReader, control).detach();

    if (args->debugHang)
    {
        SleepForever();
    }
    if (HRESULT hr = handler.DoPreview(); FAILED(hr))
    {
        Fail(hr, "DoPreview failed");
    }
    WriteReply({.status = proto::HostStatus::Ready});
    FL_LOG_INFO(L"preview ready");
    if (args->debugHangAfterReady)
    {
        SleepForever();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        // In-process handler windows are focused in this thread: forwarded keys never reach their window procedures.
        if (fastlook::host::ForwardKeyMessage(msg, &SendKey))
        {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    handler.Unload();
    state.handler = nullptr;
    return 0;
}
