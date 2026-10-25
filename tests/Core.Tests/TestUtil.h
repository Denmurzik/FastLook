#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <span>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>

#include "Text/Utf.h"

// Removes every directory handed out by MakeTempDir when the test process exits.
struct TempDirCleanup
{
    std::mutex mutex;
    std::vector<std::filesystem::path> dirs;
    ~TempDirCleanup()
    {
        for (auto const& dir : dirs)
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    }
};

inline TempDirCleanup& TempDirs()
{
    static TempDirCleanup cleanup;
    return cleanup;
}

inline std::filesystem::path MakeTempDir()
{
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() /
               (L"FastLookTests-" + std::to_wstring(rd()) + L"-" + std::to_wstring(counter++));
    std::filesystem::create_directories(dir);
    {
        auto& cleanup = TempDirs();
        std::lock_guard lock(cleanup.mutex);
        cleanup.dirs.push_back(dir);
    }
    return dir;
}

inline void WriteFile(std::filesystem::path const& file, std::string_view content)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

inline std::string ReadFile(std::filesystem::path const& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::filesystem::path FixturePath(std::string_view rel)
{
    return std::filesystem::path(FASTLOOK_FIXTURES_DIR) / fastlook::core::FromUtf8(rel);
}

// File content, "" if the fixture is missing.
inline std::string Fixture(std::string_view rel)
{
    return ReadFile(FixturePath(rel));
}

inline std::vector<uint8_t> ReadBytes(std::string_view rel)
{
    auto s = Fixture(rel);
    return std::vector<uint8_t>(s.begin(), s.end());
}

inline bool Exists(std::string_view rel)
{
    return std::filesystem::exists(FixturePath(rel));
}

inline std::span<const uint8_t> B(std::string_view s)
{
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
