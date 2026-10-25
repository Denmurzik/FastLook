#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace fastlook::daemon
{
    // Written by the ViewerManager (Task 29), read by the keyboard hook (Task 27).
    extern std::atomic<DWORD> g_viewerPid;
    extern std::atomic<uint64_t> g_lastTriggerQpc;
}
