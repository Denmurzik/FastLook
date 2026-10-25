#include "Cli/DaemonArgs.h"

namespace fastlook::core
{
    DaemonArgs ParseDaemonArgs(std::span<const std::wstring_view> args)
    {
        DaemonArgs r;
        for (size_t i = 0; i < args.size(); ++i)
        {
            std::wstring_view const a = args[i];
            if (a == L"--shutdown")
            {
                r.command = DaemonCommand::Shutdown;
            }
            else if (a == L"--show" || a == L"--toggle")
            {
                if (i + 1 < args.size())
                {
                    r.command = a == L"--show" ? DaemonCommand::Show : DaemonCommand::Toggle;
                    r.showPath = args[++i];
                }
            }
            else if (a == L"--warm")
            {
                r.warm = true;
            }
            else if (a == L"--perf-log")
            {
                if (i + 1 < args.size())
                {
                    r.perfLog = args[++i];
                }
            }
        }
        return r;
    }
}
