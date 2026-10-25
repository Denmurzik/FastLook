#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace fastlook::daemon
{
    // Written by the ViewerManager (Task 29), read by the keyboard hook (Task 27).
    extern std::atomic<DWORD> g_viewerPid;
    extern std::atomic<uint64_t> g_lastTriggerQpc;
    // The daemon's --perf-log path (empty when off). Set once at startup, before any Viewer is launched; the Viewer gets it with a ".viewer" suffix.
    extern std::wstring g_perfLogPath;
}
