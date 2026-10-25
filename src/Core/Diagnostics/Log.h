#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace fastlook::core
{
    enum class LogLevel
    {
        Info,
        Debug
    };

    namespace log
    {
        // Opens <logsDir>/<component>.log for appending. Before a write that would exceed maxBytes the
        // current file becomes <component>.1.log (the previous .1.log is deleted).
        void Init(std::filesystem::path const& logsDir, std::wstring_view component, LogLevel level, std::uint64_t maxBytes = 1u << 20);
        void SetLevel(LogLevel level);
        // Thread-safe. Debug messages are dropped while the level is Info; a no-op before Init.
        void Write(LogLevel level, std::wstring_view message);
        // "<path>" at Info, the full path at Debug.
        std::wstring RedactPath(std::filesystem::path const& path);
    }
}

#define FL_LOG_INFO(...) ::fastlook::core::log::Write(::fastlook::core::LogLevel::Info, std::format(__VA_ARGS__))
#define FL_LOG_DEBUG(...) ::fastlook::core::log::Write(::fastlook::core::LogLevel::Debug, std::format(__VA_ARGS__))
