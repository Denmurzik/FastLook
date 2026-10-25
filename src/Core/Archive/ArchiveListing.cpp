#include "Archive/ArchiveListing.h"

#include <Windows.h>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string_view>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Text/Utf.h"

namespace fastlook::core
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        constexpr size_t kMaxPathChars = 1024;
        constexpr size_t kMaxTotalPathChars = 16u * 1024 * 1024; // 32 MB of names, whatever the entry count
        constexpr size_t kMaxFolderHashes = 500000;              // bookkeeping for the folder count (8 bytes each)
        constexpr size_t kReadBlock = 64 * 1024;
        constexpr int kMaxRetries = 3;
        // libarchive's seekable ZIP reader loads the whole central directory; a bigger one is read as a stream instead.
        constexpr uint32_t kMaxSeekableZipEntries = 60000;
        constexpr DWORD kEocdSearchBytes = 22 + 65535;

        // The reading side: a file handle behind libarchive's callbacks. Every callback checks the deadline and the stop token,
        // so a solid archive that decompresses for minutes while a header is parsed is cut off.
        struct Source
        {
            HANDLE file = INVALID_HANDLE_VALUE;
            std::stop_token stop;
            clock::time_point deadline;
            std::vector<char> buffer = std::vector<char>(kReadBlock);
            bool aborted = false;

            bool Expired()
            {
                if (!aborted && (stop.stop_requested() || clock::now() >= deadline))
                {
                    aborted = true;
                }
                return aborted;
            }
        };

        la_ssize_t ReadCallback(archive* a, void* data, void const** out)
        {
            auto& source = *static_cast<Source*>(data);
            if (source.Expired())
            {
                archive_set_error(a, EINTR, "Interrupted");
                return -1;
            }
            DWORD got = 0;
            if (!ReadFile(source.file, source.buffer.data(), static_cast<DWORD>(source.buffer.size()), &got, nullptr))
            {
                archive_set_error(a, EIO, "Read error");
                return -1;
            }
            *out = source.buffer.data();
            return static_cast<la_ssize_t>(got);
        }

        la_int64_t SkipCallback(archive*, void* data, la_int64_t request)
        {
            auto& source = *static_cast<Source*>(data);
            LARGE_INTEGER size{}, here{}, zero{};
            if (request <= 0 || source.Expired() || !GetFileSizeEx(source.file, &size) ||
                !SetFilePointerEx(source.file, zero, &here, FILE_CURRENT))
            {
                return 0;
            }
            LARGE_INTEGER step;
            step.QuadPart = std::min<la_int64_t>(request, std::max<LONGLONG>(0, size.QuadPart - here.QuadPart));
            if (!SetFilePointerEx(source.file, step, nullptr, FILE_CURRENT))
            {
                return 0;
            }
            return step.QuadPart;
        }

        la_int64_t SeekCallback(archive*, void* data, la_int64_t offset, int whence)
        {
            auto& source = *static_cast<Source*>(data);
            DWORD method = FILE_BEGIN;
            if (whence == SEEK_CUR)
            {
                method = FILE_CURRENT;
            }
            else if (whence == SEEK_END)
            {
                method = FILE_END;
            }
            LARGE_INTEGER to, now{};
            to.QuadPart = offset;
            if (!SetFilePointerEx(source.file, to, &now, method))
            {
                return ARCHIVE_FATAL;
            }
            return now.QuadPart;
        }

        struct HandleCloser
        {
            HANDLE h;
            ~HandleCloser()
            {
                if (h != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(h);
                }
            }
        };

        // True when the file ends in a ZIP end-of-central-directory record announcing more entries than the seekable reader
        // should load (or a ZIP64 count, 0xFFFF). The file position is back at 0 afterwards.
        bool IsHugeZip(HANDLE file)
        {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size) || size.QuadPart < 22)
            {
                return false;
            }
            DWORD const want = static_cast<DWORD>(std::min<LONGLONG>(size.QuadPart, kEocdSearchBytes));
            LARGE_INTEGER from;
            from.QuadPart = size.QuadPart - want;
            std::vector<uint8_t> tail(want);
            DWORD got = 0;
            bool const read = SetFilePointerEx(file, from, nullptr, FILE_BEGIN) && ReadFile(file, tail.data(), want, &got, nullptr);
            LARGE_INTEGER zero{};
            SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
            if (!read || got < 22)
            {
                return false;
            }
            for (size_t i = got - 22 + 1; i-- > 0;)
            {
                if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6)
                {
                    uint32_t const total = tail[i + 10] | (static_cast<uint32_t>(tail[i + 11]) << 8);
                    return total > kMaxSeekableZipEntries;
                }
            }
            return false;
        }

        // Empty when the name has no wide form: with hdrcharset set, libarchive drops names that are not valid in that charset.
        std::optional<std::wstring> EntryName(archive_entry* entry)
        {
            if (wchar_t const* wide = archive_entry_pathname_w(entry); wide != nullptr)
            {
                return std::wstring(wide, wcsnlen(wide, kMaxPathChars * 4));
            }
            return std::nullopt;
        }

        // '/'-separated, no leading "./", no trailing '/', cut at kMaxPathChars.
        std::wstring NormalizePath(std::wstring path)
        {
            std::replace(path.begin(), path.end(), L'\\', L'/');
            size_t skip = 0;
            while (path.compare(skip, 2, L"./") == 0)
            {
                skip += 2;
                while (skip < path.size() && path[skip] == L'/')
                {
                    ++skip;
                }
            }
            path.erase(0, skip);
            while (!path.empty() && path.back() == L'/')
            {
                path.pop_back();
            }
            if (path == L".")
            {
                path.clear();
            }
            if (path.size() > kMaxPathChars)
            {
                path.resize(kMaxPathChars);
                if (IS_HIGH_SURROGATE(path.back()))
                {
                    path.pop_back();
                }
            }
            return path;
        }

        bool ContainsNoCase(char const* text, char const* needle)
        {
            if (text == nullptr)
            {
                return false;
            }
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower.find(needle) != std::string::npos;
        }

        bool IsRawCandidate(std::filesystem::path const& file)
        {
            std::wstring ext = file.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return ext == L".gz" || ext == L".bz2" || ext == L".xz" || ext == L".zst" || ext == L".lz4" || ext == L".lzma";
        }

        struct Collector
        {
            std::unordered_set<uint64_t> folders; // hashes of folder paths, explicit and implied
            size_t pathChars = 0;
            bool isZip = false;
            std::optional<int64_t> previousPosition; // header position of the entry stored last (ZIP)

            // Registers every folder on `path` (the parents, and the path itself when it is a folder).
            void NoteFolders(std::wstring const& path, bool pathIsFolder)
            {
                uint64_t hash = 1469598103934665603ull;
                size_t i = 0;
                while (i < path.size())
                {
                    size_t const slash = path.find(L'/', i);
                    size_t const end = slash == std::wstring::npos ? path.size() : slash;
                    bool const last = slash == std::wstring::npos;
                    if (end > i && !(end - i == 1 && path[i] == L'.'))
                    {
                        hash = (hash ^ '/') * 1099511628211ull;
                        for (size_t k = i; k < end; ++k)
                        {
                            hash = (hash ^ path[k]) * 1099511628211ull;
                        }
                        if ((!last || pathIsFolder) && folders.size() < kMaxFolderHashes)
                        {
                            folders.insert(hash);
                        }
                    }
                    i = end + 1;
                }
            }
        };

        enum class Outcome
        {
            Done,
            Unrecognized
        };

        struct ScanRequest
        {
            ArchiveLimits limits;
            std::stop_token stop;
            clock::time_point deadline;
            bool raw = false;              // a single compressed stream instead of an archive
            std::string headerCharset;     // for names without a charset flag
        };

        // `unreadableNames` is set when a name was not valid in the header charset (the entry is listed as "?").
        Outcome Scan(std::filesystem::path const& file, ScanRequest const& request, ArchiveListing& listing, bool& unreadableNames)
        {
            auto const& limits = request.limits;
            bool const raw = request.raw;
            Source source;
            source.stop = request.stop;
            source.deadline = request.deadline;
            HandleCloser closer{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            source.file = closer.h;
            if (source.file == INVALID_HANDLE_VALUE)
            {
                listing.error = L"The file could not be opened";
                return Outcome::Done;
            }

            archive* a = archive_read_new();
            if (a == nullptr)
            {
                listing.error = L"Out of memory";
                return Outcome::Done;
            }
            archive_entry* entry = archive_entry_new();
            struct Cleanup
            {
                archive* a;
                archive_entry* e;
                ~Cleanup()
                {
                    if (e != nullptr)
                    {
                        archive_entry_free(e);
                    }
                    archive_read_free(a);
                }
            } cleanup{a, entry};

            archive_read_support_filter_all(a);
            if (raw)
            {
                archive_read_support_format_raw(a);
            }
            else
            {
                archive_read_support_format_all(a);
            }
            archive_read_set_options(a, ("hdrcharset=" + request.headerCharset).c_str());
            archive_read_set_callback_data(a, &source);
            archive_read_set_read_callback(a, ReadCallback);
            archive_read_set_skip_callback(a, SkipCallback);
            if (!IsHugeZip(source.file))
            {
                archive_read_set_seek_callback(a, SeekCallback);
            }
            if (archive_read_open1(a) != ARCHIVE_OK)
            {
                char const* message = archive_error_string(a);
                listing.error = FromUtf8(message != nullptr ? message : "The archive could not be opened");
                listing.encryptedHeaders = ContainsNoCase(message, "passphrase");
                return source.aborted ? Outcome::Done : Outcome::Unrecognized;
            }

            Collector c;
            std::wstring const rawName = file.stem().wstring();
            int retries = 0;
            for (;;)
            {
                if (source.Expired())
                {
                    listing.truncated = true;
                    break;
                }
                int const status = archive_read_next_header2(a, entry);
                if (status == ARCHIVE_EOF)
                {
                    break;
                }
                if (status == ARCHIVE_RETRY)
                {
                    if (++retries > kMaxRetries)
                    {
                        listing.truncated = !listing.entries.empty();
                        break;
                    }
                    continue;
                }
                if (status == ARCHIVE_FATAL)
                {
                    char const* message = archive_error_string(a);
                    if (source.aborted)
                    {
                        listing.truncated = true;
                    }
                    else if (ContainsNoCase(message, "passphrase") || ContainsNoCase(message, "encrypted"))
                    {
                        listing.encryptedHeaders = true;
                        listing.error = FromUtf8(message);
                    }
                    else if (listing.entries.empty())
                    {
                        listing.error = FromUtf8(message != nullptr ? message : "The archive is damaged");
                        if (message != nullptr && std::strstr(message, "Unrecognized archive format") != nullptr)
                        {
                            return Outcome::Unrecognized;
                        }
                    }
                    else
                    {
                        listing.truncated = true; // a damaged tail: what was read stays
                    }
                    break;
                }
                // ARCHIVE_OK and ARCHIVE_WARN (a recoverable oddity): use the entry.
                retries = 0;
                if (listing.format.empty())
                {
                    char const* name = archive_format_name(a);
                    listing.format = FromUtf8(name != nullptr ? name : "");
                    c.isZip = (archive_format(a) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP;
                }

                // ZIP: the distance between two local headers is the entry's packed size (header and data descriptor included).
                int64_t const position = archive_read_header_position(a);
                if (c.isZip && c.previousPosition && position > *c.previousPosition && !listing.entries.empty())
                {
                    listing.entries.back().compressedSize = static_cast<uint64_t>(position - *c.previousPosition);
                }

                if (listing.entries.size() >= limits.maxEntries)
                {
                    listing.truncated = true;
                    break;
                }

                ArchiveEntry item;
                if (raw)
                {
                    item.path = NormalizePath(rawName);
                    item.sizeKnown = archive_entry_size_is_set(entry) != 0;
                }
                else if (auto name = EntryName(entry))
                {
                    item.path = NormalizePath(std::move(*name));
                }
                else
                {
                    unreadableNames = true;
                    item.path = L"?";
                }
                if (item.path.empty())
                {
                    archive_read_data_skip(a);
                    continue;
                }
                c.pathChars += item.path.size();
                if (c.pathChars > kMaxTotalPathChars)
                {
                    listing.truncated = true;
                    break;
                }
                item.isDir = archive_entry_filetype(entry) == AE_IFDIR;
                if (archive_entry_size_is_set(entry) && !item.isDir)
                {
                    item.size = static_cast<uint64_t>(std::max<la_int64_t>(0, archive_entry_size(entry)));
                }
                if (archive_entry_mtime_is_set(entry))
                {
                    item.mtime = static_cast<int64_t>(archive_entry_mtime(entry));
                }
                item.encrypted = archive_entry_is_encrypted(entry) != 0;
                if (archive_entry_is_metadata_encrypted(entry) != 0)
                {
                    listing.encryptedHeaders = true;
                }

                if (!item.isDir)
                {
                    ++listing.fileCount;
                    listing.totalSize = item.size > UINT64_MAX - listing.totalSize ? UINT64_MAX : listing.totalSize + item.size;
                }
                c.NoteFolders(item.path, item.isDir);
                c.previousPosition = position;
                listing.entries.push_back(std::move(item));
                if (raw)
                {
                    break; // one entry; skipping its data would decompress the whole stream
                }
                archive_read_data_skip(a);
            }

            // Folders are counted through the set so that an explicit folder and the parents implied by files are not counted twice.
            listing.dirCount = c.folders.size();
            return Outcome::Done;
        }
    }

    ArchiveListing ListArchive(std::filesystem::path const& file, ArchiveLimits limits, std::stop_token stop)
    {
        ArchiveListing listing;
        try
        {
            ScanRequest request{limits, std::move(stop), clock::now() + limits.maxTime, false, "UTF-8"};
            bool unreadableNames = false;
            auto const outcome = Scan(file, request, listing, unreadableNames);
            if (outcome == Outcome::Unrecognized && listing.entries.empty() && IsRawCandidate(file))
            {
                // A single compressed file (foo.txt.gz): one entry named after the file without its last extension.
                ArchiveListing rawListing;
                request.raw = true;
                bool ignored = false;
                if (Scan(file, request, rawListing, ignored) == Outcome::Done && !rawListing.entries.empty() && rawListing.error.empty())
                {
                    return rawListing;
                }
            }
            else if (unreadableNames && !request.stop.stop_requested() && clock::now() < request.deadline)
            {
                // Names that are not UTF-8 come from a DOS-era or locale-bound tool: read them again in the OEM code page.
                ArchiveListing oemListing;
                request.headerCharset = "CP" + std::to_string(GetOEMCP());
                bool stillUnreadable = false;
                if (Scan(file, request, oemListing, stillUnreadable) == Outcome::Done && oemListing.entries.size() >= listing.entries.size())
                {
                    return oemListing;
                }
            }
        }
        catch (...)
        {
            listing = ArchiveListing{};
            listing.error = L"The archive could not be read";
        }
        return listing;
    }
}
