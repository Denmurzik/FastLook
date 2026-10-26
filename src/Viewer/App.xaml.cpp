#include "pch.h"
#include "App.xaml.h"

#include <string>
#include <vector>

#include "Cli/ViewerArgs.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "Modes/Modes.h"
#include "Util/PreviewErrorBridge.h"
#include "Settings/Settings.h"
#include "System/AppPaths.h"
#include "Util/Language.h"
#include "ViewerHost.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    std::vector<std::wstring> CommandLineArguments()
    {
        std::vector<std::wstring> args;
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            for (int i = 1; i < argc; ++i)
            {
                args.emplace_back(argv[i]);
            }
            LocalFree(argv);
        }
        return args;
    }

    void MarkProcessStart()
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        ULARGE_INTEGER value;
        value.LowPart = created.dwLowDateTime;
        value.HighPart = created.dwHighDateTime;
        fastlook::core::perf::Mark("process_start", "created_ft=" + std::to_string(value.QuadPart));
    }
}

namespace winrt::FastLook::implementation
{
    App::App()
    {
        fastlook::viewer::InstallPreviewErrorBridge();

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
        using namespace fastlook;

        auto const storage = CommandLineArguments();
        std::vector<std::wstring_view> views(storage.begin(), storage.end());
        auto const args = core::ParseViewerArgs(views);

        if (!args.perfLog.empty())
        {
            core::perf::Open(args.perfLog);
        }
        MarkProcessStart();

        auto const paths = core::CurrentAppPaths();
        auto const settings = core::LoadSettings(paths.settingsFile).values;
        core::log::Init(paths.logsDir, L"viewer", settings.logLevel);
        if (!args.unknown.empty())
        {
            FL_LOG_INFO(L"Ignored {} unknown argument(s)", args.unknown.size());
            for (auto const& unknown : args.unknown)
            {
                FL_LOG_DEBUG(L"Unknown argument: {}", unknown); // may be a path: Debug only
            }
        }
        core::perf::Mark("startup_settings");
        viewer::ApplyLanguage(settings.general.language, args.lang);
        core::perf::Mark("startup_language");

        try
        {
            switch (args.mode)
            {
            case core::ViewerMode::Preview:
                viewer::RunPreviewMode(args);
                break;
            case core::ViewerMode::Server:
                viewer::RunServerMode(args);
                break;
            case core::ViewerMode::Bench:
                viewer::RunBenchMode(args);
                break;
            case core::ViewerMode::Settings:
                viewer::RunSettingsMode(args);
                break;
            }
        }
        catch (...)
        {
            FL_LOG_INFO(L"Startup failed");
            throw;
        }
    }
}
