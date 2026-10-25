#include "Diagnostics/PerfLog.h"

#include <Windows.h>

#include <format>
#include <mutex>
#include <string>
#include <system_error>

namespace fastlook::core::perf
{
    namespace
    {
        std::mutex g_mutex;
        HANDLE g_file = INVALID_HANDLE_VALUE;
    }

    void Open(std::filesystem::path const& file)
    {
        std::lock_guard lock(g_mutex);
        if (g_file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_file);
            g_file = INVALID_HANDLE_VALUE;
        }
        std::error_code ec;
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), ec);
        g_file = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    bool Enabled()
    {
        std::lock_guard lock(g_mutex);
        return g_file != INVALID_HANDLE_VALUE;
    }

    void Mark(std::string_view name, std::string_view detail)
    {
        std::lock_guard lock(g_mutex);
        if (g_file == INVALID_HANDLE_VALUE)
            return;

        LARGE_INTEGER qpc, freq;
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&freq);
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned long long ft64 = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

        std::string line = std::format("{} qpc={} freq={} ft={} {}\n", name, qpc.QuadPart, freq.QuadPart, ft64, detail);
        DWORD written = 0;
        WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
}
