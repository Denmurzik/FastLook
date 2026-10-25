#pragma once

#include <span>
#include <string>
#include <string_view>

namespace fastlook::core
{
    enum class DaemonCommand
    {
        Run,
        Shutdown,
        Show,
        Toggle
    };

    struct DaemonArgs
    {
        DaemonCommand command = DaemonCommand::Run;
        std::wstring showPath; // --show and --toggle; empty when the path is missing (malformed)
        bool warm = false;
        std::wstring perfLog;
    };

    // argv without argv[0]. The last of --shutdown / --show / --toggle wins.
    DaemonArgs ParseDaemonArgs(std::span<const std::wstring_view> args);
}
