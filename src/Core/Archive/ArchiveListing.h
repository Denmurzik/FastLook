#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace fastlook::core
{
    struct ArchiveEntry
    {
        // '/'-separated, as stored (never resolved or extracted); cut at 1024 characters. Bidi controls and other control
        // characters are replaced with U+FFFD, so a name always reads as what it is.
        std::wstring path;
        uint64_t size = 0;
        std::optional<uint64_t> compressedSize; // ZIP only; empty for the last entry and for other formats
        int64_t mtime = 0;                      // unix seconds
        bool isDir = false;
        bool encrypted = false;
        bool sizeKnown = true; // false for a single-file compressed stream (.gz, .xz, ...): the size is not in the header
    };

    struct ArchiveLimits
    {
        size_t maxEntries = 100000;
        std::chrono::milliseconds maxTime{2000};
    };

    struct ArchiveListing
    {
        std::vector<ArchiveEntry> entries;
        uint64_t fileCount = 0, dirCount = 0, totalSize = 0; // dirCount: explicit folders plus implied parents
        bool truncated = false;                              // the entry limit, the time limit, stop or a damaged tail cut the list
        bool encryptedHeaders = false;
        bool headerTooLarge = false; // a 7z header that would take more than 16 MB to hold: refused before it is read
        std::wstring format;
        std::wstring error; // diagnostic text (English); empty when the archive could be read
    };

    // Reads headers only (data is skipped, never decoded to disk). Never throws; a file that is not an archive yields a
    // non-empty `error`. Memory is bounded: names are cut at 1024 characters and the folder bookkeeping is capped.
    ArchiveListing ListArchive(std::filesystem::path const& file, ArchiveLimits limits, std::stop_token stop);
}
