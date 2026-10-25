#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

#include "Update/ReleaseInfo.h"

namespace fastlook::core
{
    inline constexpr uint64_t kMaxInstallerBytes = 200ull * 1024 * 1024;
    inline constexpr uint64_t kMaxChecksumBytes = 1024;
    inline constexpr wchar_t kInstallerArgs[] = L"/SILENT /SUPPRESSMSGBOXES /NORESTART";

    enum class DownloadOutcome
    {
        Verified,
        Cancelled,
        Failed,
        Mismatch
    };

    using ProgressFn = std::function<void(uint64_t got, uint64_t total)>;
    // Fetches `url` into `to` (at most `maxBytes`); false on any failure or stop. The viewer plugs WinHTTP in here.
    using FetchFile = std::function<bool(std::string const& url, std::filesystem::path const& to, uint64_t maxBytes,
                                         ProgressFn const& progress, std::stop_token stop)>;

    // Downloads the checksum and the installer of `release` into `dir` (recreated) and compares SHA-256. Anything but
    // Verified leaves neither file behind; a stop request, also one that arrives while hashing, yields Cancelled.
    DownloadOutcome DownloadAndVerifyInstaller(ReleaseInfo const& release, std::filesystem::path const& dir, FetchFile const& fetch,
                                               ProgressFn const& progress, std::stop_token stop);

    // Starts `setup` with kInstallerArgs through the shell (so UAC applies). False when it could not be started.
    bool LaunchInstaller(std::filesystem::path const& setup);
}
