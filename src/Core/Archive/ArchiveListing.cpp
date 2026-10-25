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
#include <span>
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
        constexpr size_t kReadBlock = 16 * 1024; // the limits are checked once per block: small enough for fast-expanding xz / zstd
        constexpr int kMaxRetries = 3;
        // libarchive's seekable ZIP reader loads the whole central directory; a bigger one is read as a stream instead.
        constexpr uint32_t kMaxSeekableZipEntries = 60000;
        constexpr uint64_t kMaxSeekableZipDirectoryBytes = 8u * 1024 * 1024; // measured on the file, not taken from the end record
        constexpr uint64_t kMaxSevenZipHeaderBytes = 16u * 1024 * 1024;     // encoded, and decoded size of an encoded header
        // libarchive sizes per-file arrays by the declared file count, which only the header bytes bound (>= 4 bytes per file):
        // a plain header is capped lower than a decoded one, which real archives need for 100 000+ entries.
        constexpr uint64_t kMaxPlainSevenZipHeaderBytes = 4u * 1024 * 1024;
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
                    uint32_t const directoryOffset = tail[i + 16] | (static_cast<uint32_t>(tail[i + 17]) << 8) |
                                                     (static_cast<uint32_t>(tail[i + 18]) << 16) | (static_cast<uint32_t>(tail[i + 19]) << 24);
                    uint64_t const endRecord = static_cast<uint64_t>(from.QuadPart) + i;
                    // libarchive walks the directory records it finds, not the count the end record declares: bound the span itself.
                    return total > kMaxSeekableZipEntries || directoryOffset > endRecord ||
                           endRecord - directoryOffset > kMaxSeekableZipDirectoryBytes;
                }
            }
            return false;
        }

        // 7-Zip numbers: the leading 1 bits of the first byte count the extra bytes.
        struct SevenZipReader
        {
            std::span<uint8_t const> data;
            size_t at = 0;
            bool failed = false;

            uint8_t Byte()
            {
                if (at >= data.size())
                {
                    failed = true;
                    return 0;
                }
                return data[at++];
            }

            uint64_t Number()
            {
                uint8_t const first = Byte();
                uint8_t mask = 0x80;
                uint64_t value = 0;
                for (int i = 0; i < 8; ++i)
                {
                    if ((first & mask) == 0)
                    {
                        return value | (static_cast<uint64_t>(first & (mask - 1)) << (8 * i));
                    }
                    value |= static_cast<uint64_t>(Byte()) << (8 * i);
                    mask >>= 1;
                }
                return value;
            }

            void Skip(uint64_t n)
            {
                if (n > data.size() - at)
                {
                    failed = true;
                    at = data.size();
                    return;
                }
                at += static_cast<size_t>(n);
            }
        };

        // Largest decoded size declared by the StreamsInfo of an encoded 7z header; nullopt when it cannot be parsed.
        std::optional<uint64_t> LargestEncodedHeaderSize(std::span<uint8_t const> bytes)
        {
            constexpr uint64_t kMaxFolders = 64;
            constexpr uint64_t kMaxCoders = 32;
            constexpr uint64_t kMaxStreams = 1024;
            SevenZipReader r{bytes};
            if (r.Byte() != 0x17)
            {
                return std::nullopt;
            }
            uint8_t id = r.Byte();
            if (id == 0x06) // PackInfo
            {
                r.Number(); // pack position
                uint64_t const packStreams = r.Number();
                if (packStreams > kMaxFolders)
                {
                    return std::nullopt;
                }
                for (id = r.Byte(); id != 0x00 && !r.failed; id = r.Byte())
                {
                    if (id == 0x09) // sizes
                    {
                        for (uint64_t i = 0; i < packStreams && !r.failed; ++i)
                        {
                            r.Number();
                        }
                    }
                    else if (id == 0x0A) // digests: only the "all defined" form
                    {
                        if (r.Byte() == 0)
                        {
                            return std::nullopt;
                        }
                        r.Skip(packStreams * 4);
                    }
                    else
                    {
                        return std::nullopt;
                    }
                }
                id = r.Byte();
            }
            if (id != 0x07 || r.Byte() != 0x0B) // UnPackInfo, Folder
            {
                return std::nullopt;
            }
            uint64_t const folders = r.Number();
            if (folders == 0 || folders > kMaxFolders || r.Byte() != 0) // external data is not supported
            {
                return std::nullopt;
            }
            uint64_t totalOut = 0;
            for (uint64_t f = 0; f < folders && !r.failed; ++f)
            {
                uint64_t const coders = r.Number();
                if (coders == 0 || coders > kMaxCoders)
                {
                    return std::nullopt;
                }
                uint64_t in = 0, out = 0;
                for (uint64_t c = 0; c < coders && !r.failed; ++c)
                {
                    uint8_t const flags = r.Byte();
                    r.Skip(flags & 0x0F);
                    if (flags & 0x10)
                    {
                        in += r.Number();
                        out += r.Number();
                        if (in > kMaxStreams || out > kMaxStreams)
                        {
                            return std::nullopt; // also stops a declared 2^62 streams before any loop runs on it
                        }
                    }
                    else
                    {
                        ++in;
                        ++out;
                    }
                    if (flags & 0x20)
                    {
                        r.Skip(r.Number());
                    }
                    if (flags & 0x80)
                    {
                        return std::nullopt; // alternative methods: not produced by any known writer
                    }
                }
                if (r.failed || out == 0 || in < out - 1 || in > kMaxStreams || out > kMaxStreams)
                {
                    return std::nullopt;
                }
                uint64_t const bindPairs = out - 1;
                for (uint64_t b = 0; b < bindPairs * 2 && !r.failed; ++b)
                {
                    r.Number();
                }
                uint64_t const packed = in - bindPairs;
                if (packed > 1)
                {
                    for (uint64_t k = 0; k < packed && !r.failed; ++k)
                    {
                        r.Number();
                    }
                }
                totalOut += out;
            }
            if (r.Byte() != 0x0C) // CodersUnPackSize
            {
                return std::nullopt;
            }
            uint64_t largest = 0;
            for (uint64_t i = 0; i < totalOut && !r.failed; ++i)
            {
                largest = std::max(largest, r.Number());
            }
            if (r.failed)
            {
                return std::nullopt;
            }
            return largest;
        }

        // A 7z reader holds the whole decoded header (names and properties of every file) in memory before the first entry is
        // returned, and its size is whatever the file declares. True when the header is, or may be, bigger than the limit
        // (a header that cannot be parsed counts as too large). The file position is back at 0 afterwards. Not a 7z file: false.
        bool SevenZipHeaderTooLarge(HANDLE file)
        {
            auto readAt = [&](uint64_t offset, void* out, DWORD n) {
                LARGE_INTEGER to;
                to.QuadPart = static_cast<LONGLONG>(offset);
                DWORD got = 0;
                return SetFilePointerEx(file, to, nullptr, FILE_BEGIN) && ReadFile(file, out, n, &got, nullptr) && got == n;
            };
            struct Rewind
            {
                HANDLE h;
                ~Rewind()
                {
                    LARGE_INTEGER zero{};
                    SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
                }
            } rewind{file};

            uint8_t start[32];
            if (!readAt(0, start, sizeof start) || std::memcmp(start, "7z\xBC\xAF\x27\x1C", 6) != 0)
            {
                return false;
            }
            auto le64 = [&](int at) {
                uint64_t v = 0;
                for (int i = 7; i >= 0; --i)
                {
                    v = (v << 8) | start[at + i];
                }
                return v;
            };
            uint64_t const offset = le64(12);
            uint64_t const headerSize = le64(20);
            if (headerSize == 0)
            {
                return false;
            }
            if (headerSize > kMaxSevenZipHeaderBytes)
            {
                return true;
            }
            LARGE_INTEGER fileSize{};
            if (!GetFileSizeEx(file, &fileSize) || offset > static_cast<uint64_t>(fileSize.QuadPart) ||
                headerSize > static_cast<uint64_t>(fileSize.QuadPart) - std::min<uint64_t>(32 + offset, static_cast<uint64_t>(fileSize.QuadPart)))
            {
                return false; // points outside the file: libarchive fails on it without allocating
            }
            std::vector<uint8_t> head(static_cast<size_t>(std::min<uint64_t>(headerSize, 4096)));
            if (!readAt(32 + offset, head.data(), static_cast<DWORD>(head.size())))
            {
                return false;
            }
            if (head[0] != 0x17)
            {
                return headerSize > kMaxPlainSevenZipHeaderBytes; // a plain header
            }
            auto const largest = LargestEncodedHeaderSize(head);
            return !largest || *largest > kMaxSevenZipHeaderBytes;
        }

        // Empty when the name has no wide form: with hdrcharset set, libarchive drops names that are not valid in that charset.
        std::optional<std::wstring> EntryName(archive_entry* entry)
        {
            if (wchar_t const* wide = archive_entry_pathname_w(entry); wide != nullptr)
            {
                return std::wstring(wide, wcsnlen(wide, kMaxPathChars + 16)); // a cut name never holds more than this
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
            path.shrink_to_fit(); // the kept names are what the memory budget counts
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
            if (!raw && SevenZipHeaderTooLarge(source.file))
            {
                listing.headerTooLarge = true;
                listing.error = L"The archive header is too large";
                return Outcome::Done;
            }
            if (archive_read_open1(a) != ARCHIVE_OK)
            {
                char const* message = archive_error_string(a);
                listing.error = FromUtf8(message != nullptr ? message : "The archive could not be opened");
                listing.encryptedHeaders = ContainsNoCase(message, "passphrase");
                if (source.aborted)
                {
                    listing.error.clear(); // cut by the time limit or stop, not damaged
                    listing.truncated = true;
                    return Outcome::Done;
                }
                return Outcome::Unrecognized;
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
