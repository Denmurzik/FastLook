#pragma once

#include <string_view>

namespace fastlook::viewer
{
    // Runs `--script "<cmd>;<cmd>"` on the UI thread without blocking it. Commands: show next prev zoomin zoomout zoomreset
    // fullscreen pin close open copy wait:<ms> cmd:<name>. Each command writes a `script_<cmd>` perf mark first.
    // `show` shows the --preview files again (it is how a --background process gets its first window).
    void RunScript(std::wstring_view script);
}
