#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Geometry.h"

namespace fastlook::core::host
{
    // Command line of FastLook.PreviewHost.exe. `rect` is in client pixels of the parent window (left, top, width, height).
    struct HostArgs
    {
        uint64_t parentHwnd = 0;
        std::wstring clsid;
        std::wstring file;
        RectI rect;
        bool dark = false;
        bool debugHang = false;
        bool debugHangAfterReady = false;
    };

    // --parent <hex> --clsid {..} --file "<p>" --rect l,t,w,h --theme dark|light [--debug-hang] [--debug-hang-after-ready]
    // The result parses back with CommandLineToArgvW (quotes and backslashes in `file` are escaped).
    std::wstring FormatHostArgs(HostArgs const&);
    // `args` excludes the executable name. Nullopt on a missing or malformed required argument.
    std::optional<HostArgs> ParseHostArgs(std::span<const std::wstring_view> args);

    // Viewer -> host, one line each on stdin.
    enum class HostCmd
    {
        Rect,
        Quit
    };

    struct HostCommand
    {
        HostCmd cmd = HostCmd::Quit;
        RectI rect;
    };

    // "rect l t w h" | "quit"
    std::optional<HostCommand> ParseHostCommand(std::string_view line);
    std::string FormatHostCommand(HostCommand const&);

    inline constexpr uint32_t kModCtrl = 1, kModShift = 2, kModAlt = 4;

    // Host -> Viewer, one line each on stdout.
    enum class HostStatus
    {
        Ready,
        Error,
        Key
    };

    struct HostReply
    {
        HostStatus status = HostStatus::Ready;
        uint32_t hr = 0;
        std::string text;
        uint32_t vk = 0, mods = 0;
    };

    // "ready\n" | "error 0x80004005 <text>\n" | "key <vk> <mods>\n"
    std::string FormatHostReply(HostReply const&);
    // `line` without the line terminator (a trailing CR/LF is tolerated).
    std::optional<HostReply> ParseHostReply(std::string_view line);

    // Keys a handler window must hand back to the Viewer: Space, Esc, arrows, Enter, F11 (no modifiers) and Ctrl+W.
    bool IsForwardedHostKey(uint32_t vk, uint32_t mods);
}
