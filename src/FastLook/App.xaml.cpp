#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#include <memory>

using namespace winrt;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Microsoft::UI::Xaml;
using namespace std::chrono_literals;

namespace
{
    // Spike-only switches, see scripts/measure-ram.ps1.
    struct Options
    {
        bool hidden{ false };        // --hidden: activate once, then AppWindow.Hide()
        bool trim{ false };          // --hidden-trim: --hidden + trim working set after hide
        int reshowMs{ 0 };           // --reshow-ms N: AppWindow.Show() N ms after launch, log time to first frame
        std::wstring logPath;        // --log <file>
    };

    Options ParseOptions()
    {
        Options o;
        int argc = 0;
        PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i)
        {
            std::wstring_view a{ argv[i] };
            if (a == L"--hidden") { o.hidden = true; }
            else if (a == L"--hidden-trim") { o.hidden = o.trim = true; }
            else if (a == L"--reshow-ms" && i + 1 < argc) { o.reshowMs = _wtoi(argv[++i]); }
            else if (a == L"--log" && i + 1 < argc) { o.logPath = argv[++i]; }
        }
        LocalFree(argv);
        return o;
    }

    void Log(Options const& o, wchar_t const* fmt, double v)
    {
        if (o.logPath.empty()) return;
        FILE* f{};
        if (_wfopen_s(&f, o.logPath.c_str(), L"a") == 0 && f)
        {
            fwprintf(f, fmt, v);
            fclose(f);
        }
    }

    double NowMs()
    {
        LARGE_INTEGER c{}, fr{};
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&fr);
        return 1000.0 * static_cast<double>(c.QuadPart) / static_cast<double>(fr.QuadPart);
    }

    Options g_opts;
}

namespace winrt::FastLook::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        g_opts = ParseOptions();

        m_window = make<MainWindow>();
        m_window.Activate();

        if (!g_opts.hidden) return;

        auto queue = DispatcherQueue::GetForCurrentThread();
        // Hide after the first layout/render pass has had a chance to run.
        queue.TryEnqueue(DispatcherQueuePriority::Low, [this, queue]
        {
            m_window.AppWindow().Hide();

            if (g_opts.trim)
            {
                // Trim a moment after hide so the hide's own layout work is not paged straight back in.
                m_trimTimer = queue.CreateTimer();
                m_trimTimer.IsRepeating(false);
                m_trimTimer.Interval(1s);
                m_trimTimer.Tick([this](auto&&, auto&&)
                {
                    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
                });
                m_trimTimer.Start();
            }

            if (g_opts.reshowMs > 0)
            {
                m_showTimer = queue.CreateTimer();
                m_showTimer.IsRepeating(false);
                m_showTimer.Interval(std::chrono::milliseconds(g_opts.reshowMs));
                m_showTimer.Tick([this](auto&&, auto&&)
                {
                    auto t0 = std::make_shared<double>(NowMs());
                    auto token = std::make_shared<event_token>();
                    auto armed = std::make_shared<bool>(false);
                    *token = Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(
                        [t0, token, armed](auto&&, auto&&)
                    {
                        if (!*armed) return;
                        Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(*token);
                        Log(g_opts, L"reshow_first_frame_ms=%.2f\n", NowMs() - *t0);
                    });
                    m_window.AppWindow().Show();
                    *armed = true;
                    Log(g_opts, L"reshow_show_call_ms=%.2f\n", NowMs() - *t0);
                });
                m_showTimer.Start();
            }
        });
    }
}
