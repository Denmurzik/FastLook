#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>

namespace fastlook::core
{
    struct FolderStats
    {
        uint64_t files = 0;
        uint64_t folders = 0;
        uint64_t bytes = 0;
        bool truncated = false;
    };

    // Recursive count below `root` (the root itself is not counted). Reparse points and cloud directories (recall attributes) are counted and never entered
    // (each counts as one folder). Unreadable subfolders are skipped. Stops with truncated == true
    // when `stop` is requested or `deadline` passes. `progress` runs on the calling thread, at most every 100 ms, and once
    // more with the final value.
    FolderStats CountFolder(std::filesystem::path const& root, std::stop_token stop, std::chrono::steady_clock::time_point deadline,
                            std::function<void(FolderStats const&)> progress = {});
}
