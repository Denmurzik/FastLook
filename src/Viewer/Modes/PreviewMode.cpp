#include "pch.h"
#include "Modes/Modes.h"

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "Modes/ScriptRunner.h"
#include "System/AppPaths.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    namespace
    {
        bool IsVirtualPath(std::wstring_view path)
        {
            return path.starts_with(L"::") || path.starts_with(L"shell:");
        }

        std::wstring AbsolutePath(std::wstring const& path)
        {
            DWORD const length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
            if (length == 0)
            {
                return path;
            }
            std::wstring full(length, L'\0');
            DWORD const written = GetFullPathNameW(path.c_str(), length, full.data(), nullptr);
            if (written == 0 || written >= length)
            {
                return path;
            }
            full.resize(written);
            return full;
        }
    }

    core::ipc::ShowPayload PayloadFromArgs(core::ViewerArgs const& args)
    {
        core::ipc::ShowPayload payload;
        for (auto const& file : args.files)
        {
            bool const isVirtual = IsVirtualPath(file);
            payload.items.push_back({isVirtual ? file : AbsolutePath(file), isVirtual});
        }
        payload.navMode = core::ipc::NavMode::Selection;
        payload.iconRect = args.iconRect;
        return payload;
    }

    void RunPreviewMode(core::ViewerArgs const& args)
    {
        auto& host = ViewerHost::Get();
        host.Initialize(core::CurrentAppPaths(), CloseBehavior::ExitProcess, args);

        if (args.files.empty())
        {
            FL_LOG_INFO(L"--preview without files");
            host.ExitApp();
            return;
        }

        // The window frame is built before the first show request, as the server mode does at prewarm: the 50 ms
        // show-to-visible budget covers showing it, not XAML construction.
        core::perf::Mark("startup_host");
        host.Window();
        core::perf::Mark("startup_window");

        if (args.background)
        {
            host.PrewarmHidden(); // nothing is shown until the script's `show`
        }
        else
        {
            host.Show(PayloadFromArgs(args));
        }

        if (!args.script.empty())
        {
            RunScript(args.script);
        }
    }
}
