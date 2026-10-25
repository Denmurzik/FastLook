#include "Diagnostics/Log.h"

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <system_error>

#include "Text/Utf.h"

namespace fastlook::core::log
{
    namespace
    {
        std::mutex g_mutex;
        std::filesystem::path g_current;  // <logsDir>/<component>.log
        std::filesystem::path g_previous; // <logsDir>/<component>.1.log
        std::uint64_t g_maxBytes = 0;
        std::uint64_t g_size = 0;
        bool g_ready = false;
        std::atomic<LogLevel> g_level{LogLevel::Info};

        std::string Timestamp()
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                               t.wSecond, t.wMilliseconds);
        }

        bool AppendBytes(std::filesystem::path const& file, std::string const& bytes)
        {
            HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return false;
            DWORD written = 0;
            bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
            CloseHandle(h);
            return ok;
        }
    }

    void Init(std::filesystem::path const& logsDir, std::wstring_view component, LogLevel level, std::uint64_t maxBytes)
    {
        std::lock_guard lock(g_mutex);
        std::error_code ec;
        std::filesystem::create_directories(logsDir, ec);
        std::wstring name(component);
        g_current = logsDir / (name + L".log");
        g_previous = logsDir / (name + L".1.log");
        g_maxBytes = maxBytes;
        auto size = std::filesystem::file_size(g_current, ec);
        g_size = ec ? 0 : size;
        g_level = level;
        g_ready = true;
    }

    void SetLevel(LogLevel level)
    {
        g_level = level;
    }

    void Write(LogLevel level, std::wstring_view message)
    {
        if (level == LogLevel::Debug && g_level.load() != LogLevel::Debug)
            return;

        std::string line = std::format("{} [{}] [{}] ", Timestamp(), level == LogLevel::Debug ? 'D' : 'I', GetCurrentThreadId());
        line += ToUtf8(message);
        line += "\r\n";

        std::lock_guard lock(g_mutex);
        if (!g_ready)
            return;
        if (g_size > 0 && g_size + line.size() > g_maxBytes)
        {
            std::error_code ec;
            std::filesystem::remove(g_previous, ec);
            std::filesystem::rename(g_current, g_previous, ec);
            g_size = 0;
        }
        if (AppendBytes(g_current, line))
            g_size += line.size();
    }

    std::wstring RedactPath(std::filesystem::path const& path)
    {
        return g_level.load() == LogLevel::Debug ? path.wstring() : L"<path>";
    }
}
