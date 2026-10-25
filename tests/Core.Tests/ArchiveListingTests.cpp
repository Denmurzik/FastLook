#include <Windows.h>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "Archive/ArchiveListing.h"
#include "TestUtil.h"

using namespace fastlook::core;
using namespace std::chrono;

namespace
{
    bool HasPathEnding(ArchiveListing const& listing, std::wstring_view tail)
    {
        return std::any_of(listing.entries.begin(), listing.entries.end(), [&](ArchiveEntry const& e) {
            return e.path.size() >= tail.size() && e.path.compare(e.path.size() - tail.size(), tail.size(), tail) == 0;
        });
    }

    void Put16(std::string& out, uint16_t v)
    {
        out.push_back(static_cast<char>(v & 0xFF));
        out.push_back(static_cast<char>(v >> 8));
    }

    void Put32(std::string& out, uint32_t v)
    {
        Put16(out, static_cast<uint16_t>(v & 0xFFFF));
        Put16(out, static_cast<uint16_t>(v >> 16));
    }

    // A ZIP of empty stored entries with the given raw names. Local headers first, then the central directory.
    // utf8Flag: general purpose flag bit 11 ("the names are UTF-8").
    std::string BuildZip(std::vector<std::string> const& names, bool utf8Flag = true)
    {
        uint16_t const flags = utf8Flag ? 0x0800 : 0;
        std::string body, central;
        for (auto const& name : names)
        {
            uint32_t const offset = static_cast<uint32_t>(body.size());
            Put32(body, 0x04034b50);
            Put16(body, 20);
            Put16(body, flags);
            Put16(body, 0);
            Put32(body, 0);
            Put32(body, 0);
            Put32(body, 0);
            Put32(body, 0);
            Put16(body, static_cast<uint16_t>(name.size()));
            Put16(body, 0);
            body += name;

            Put32(central, 0x02014b50);
            Put16(central, 20);
            Put16(central, 20);
            Put16(central, flags);
            Put16(central, 0);
            Put32(central, 0);
            Put32(central, 0);
            Put32(central, 0);
            Put32(central, 0);
            Put16(central, static_cast<uint16_t>(name.size()));
            Put16(central, 0);
            Put16(central, 0);
            Put16(central, 0);
            Put16(central, 0);
            Put32(central, 0);
            Put32(central, offset);
            central += name;
        }
        std::string zip = body;
        uint32_t const centralOffset = static_cast<uint32_t>(zip.size());
        zip += central;
        Put32(zip, 0x06054b50);
        Put16(zip, 0);
        Put16(zip, 0);
        Put16(zip, static_cast<uint16_t>(std::min<size_t>(names.size(), 0xFFFF)));
        Put16(zip, static_cast<uint16_t>(std::min<size_t>(names.size(), 0xFFFF)));
        Put32(zip, static_cast<uint32_t>(central.size()));
        Put32(zip, centralOffset);
        Put16(zip, 0);
        return zip;
    }

    std::filesystem::path WriteTemp(std::wstring const& name, std::string_view content)
    {
        auto const file = MakeTempDir() / name;
        WriteFile(file, content);
        return file;
    }
}

TEST(ArchiveListing, ZipCountsAndImpliedDirs)
{
    auto const l = ListArchive(FixturePath("archive/sample.zip"), {}, {});
    EXPECT_TRUE(l.error.empty());
    EXPECT_EQ(l.fileCount, 3u);
    EXPECT_EQ(l.dirCount, 2u);
    EXPECT_EQ(l.totalSize, 3u * 100u);
    ASSERT_FALSE(l.entries.empty());
    EXPECT_TRUE(l.entries[0].compressedSize.has_value());
    EXPECT_FALSE(l.truncated);
}

TEST(ArchiveListing, ZipPackedSizesAreFilledForAllButTheLastEntry)
{
    auto const l = ListArchive(FixturePath("archive/sample.zip"), {}, {});
    ASSERT_GE(l.entries.size(), 2u);
    for (size_t i = 0; i + 1 < l.entries.size(); ++i)
    {
        EXPECT_TRUE(l.entries[i].compressedSize.has_value()) << i;
    }
    EXPECT_FALSE(l.entries.back().compressedSize.has_value());
}

TEST(ArchiveListing, TarGzAnd7z)
{
    for (char const* name : {"archive/sample.tar.gz", "archive/sample.7z"})
    {
        auto const l = ListArchive(FixturePath(name), {}, {});
        EXPECT_TRUE(l.error.empty()) << name;
        EXPECT_EQ(l.fileCount, 3u) << name;
        EXPECT_EQ(l.dirCount, 2u) << name;
        EXPECT_EQ(l.totalSize, 300u) << name;
        EXPECT_FALSE(l.format.empty()) << name;
    }
}

TEST(ArchiveListing, SingleGzViaRaw)
{
    auto const l = ListArchive(FixturePath("archive/single.txt.gz"), {}, {});
    ASSERT_EQ(l.entries.size(), 1u);
    EXPECT_EQ(l.entries[0].path, L"single.txt");
    EXPECT_FALSE(l.entries[0].sizeKnown);
    EXPECT_EQ(l.fileCount, 1u);
    EXPECT_EQ(l.dirCount, 0u);
}

TEST(ArchiveListing, CyrillicNames)
{
    auto const l = ListArchive(FixturePath("archive/cyrillic.zip"), {}, {});
    EXPECT_TRUE(HasPathEnding(l, L"файл.txt"));
    EXPECT_EQ(l.dirCount, 1u);
}

TEST(ArchiveListing, UnflaggedUtf8NamesAreRead)
{
    // Zips from Linux tools carry UTF-8 names without the flag.
    auto const l = ListArchive(WriteTemp(L"linux.zip", BuildZip({"\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82/a.txt"}, false)), {}, {});
    ASSERT_EQ(l.entries.size(), 1u);
    EXPECT_EQ(l.entries[0].path, L"Привет/a.txt");
}

TEST(ArchiveListing, NonUtf8NamesFallBackToTheOemCodePage)
{
    // A ZIP written by a DOS-era tool: the names are in the OEM code page, without the UTF-8 flag.
    UINT const oem = GetOEMCP();
    for (std::wstring const wanted : {std::wstring(L"файл.txt"), std::wstring(L"café.txt")})
    {
        std::string raw(64, '\0');
        BOOL usedDefault = FALSE;
        int const len = WideCharToMultiByte(oem, WC_NO_BEST_FIT_CHARS, wanted.c_str(), static_cast<int>(wanted.size()), raw.data(), 64, nullptr, &usedDefault);
        if (len <= 0 || usedDefault)
        {
            continue;
        }
        raw.resize(static_cast<size_t>(len));
        // Not valid UTF-8 (a lone high byte), so the first pass cannot read it.
        auto const l = ListArchive(WriteTemp(L"oem.zip", BuildZip({raw}, false)), {}, {});
        ASSERT_EQ(l.entries.size(), 1u);
        EXPECT_EQ(l.entries[0].path, wanted);
        return;
    }
    GTEST_SKIP() << "OEM code page " << oem << " cannot encode the sample names";
}

TEST(ArchiveListing, EntryLimitTruncates)
{
    ArchiveLimits limits;
    limits.maxEntries = 1;
    auto const l = ListArchive(FixturePath("archive/sample.zip"), limits, {});
    EXPECT_TRUE(l.truncated);
    EXPECT_EQ(l.entries.size(), 1u);
}

TEST(ArchiveListing, ExactlyAtTheLimitIsNotTruncated)
{
    auto const all = ListArchive(FixturePath("archive/sample.tar.gz"), {}, {});
    ArchiveLimits limits;
    limits.maxEntries = all.entries.size();
    auto const l = ListArchive(FixturePath("archive/sample.tar.gz"), limits, {});
    EXPECT_FALSE(l.truncated);
    EXPECT_EQ(l.entries.size(), all.entries.size());
}

TEST(ArchiveListing, NotAnArchive)
{
    auto const l = ListArchive(FixturePath("fallback/unknown.bin2"), {}, {});
    EXPECT_FALSE(l.error.empty());
    EXPECT_TRUE(l.entries.empty());
}

TEST(ArchiveListing, MissingFile)
{
    auto const l = ListArchive(MakeTempDir() / L"nope.zip", {}, {});
    EXPECT_FALSE(l.error.empty());
    EXPECT_TRUE(l.entries.empty());
}

TEST(ArchiveListing, EmptyFile)
{
    auto const l = ListArchive(WriteTemp(L"empty.zip", ""), {}, {});
    EXPECT_TRUE(l.entries.empty());
}

TEST(ArchiveListing, EncryptedZip)
{
    if (!Exists("archive/encrypted.zip"))
    {
        GTEST_SKIP();
    }
    auto const l = ListArchive(FixturePath("archive/encrypted.zip"), {}, {});
    ASSERT_FALSE(l.entries.empty());
    EXPECT_TRUE(l.encryptedHeaders || std::any_of(l.entries.begin(), l.entries.end(), [](ArchiveEntry const& e) { return e.encrypted; }));
}

TEST(ArchiveListing, TruncatedArchiveDoesNotThrow)
{
    auto const bytes = Fixture("archive/sample.zip");
    ASSERT_GT(bytes.size(), 100u);
    for (size_t keep : {size_t{0}, size_t{10}, bytes.size() / 2, bytes.size() - 5})
    {
        auto const file = WriteTemp(L"cut.zip", std::string_view(bytes).substr(0, keep));
        EXPECT_NO_THROW({ (void)ListArchive(file, {}, {}); }) << keep;
    }
    auto const gz = Fixture("archive/sample.tar.gz");
    EXPECT_NO_THROW({ (void)ListArchive(WriteTemp(L"cut.tar.gz", std::string_view(gz).substr(0, gz.size() / 2)), {}, {}); });
}

TEST(ArchiveListing, HostileNamesAreCutAndTraversalIsOnlyListed)
{
    std::string const huge(60000, 'a');
    auto const l = ListArchive(WriteTemp(L"hostile.zip", BuildZip({huge, "../../evil.txt", "C:\\Windows\\x.dll", "./", "ok/file.txt"})), {}, {});
    ASSERT_GE(l.entries.size(), 4u);
    EXPECT_EQ(l.entries[0].path.size(), 1024u);
    EXPECT_EQ(l.entries[1].path, L"../../evil.txt");
    EXPECT_EQ(l.entries[2].path, L"C:/Windows/x.dll");
    EXPECT_EQ(l.entries[3].path, L"ok/file.txt"); // "./" has no name and is dropped
}

TEST(ArchiveListing, ManyEntriesStayWithinTheLimit)
{
    std::vector<std::string> names;
    for (int i = 0; i < 20000; ++i)
    {
        names.push_back("d" + std::to_string(i % 100) + "/f" + std::to_string(i) + ".txt");
    }
    auto const file = WriteTemp(L"many.zip", BuildZip(names));
    auto const all = ListArchive(file, {}, {});
    EXPECT_EQ(all.entries.size(), 20000u);
    EXPECT_EQ(all.fileCount, 20000u);
    EXPECT_EQ(all.dirCount, 100u);
    ArchiveLimits limits;
    limits.maxEntries = 500;
    auto const cut = ListArchive(file, limits, {});
    EXPECT_EQ(cut.entries.size(), 500u);
    EXPECT_TRUE(cut.truncated);
}

TEST(ArchiveListing, StopTokenEndsTheListing)
{
    std::stop_source source;
    source.request_stop();
    auto const t0 = steady_clock::now();
    auto const l = ListArchive(FixturePath("archive/sample.zip"), {}, source.get_token());
    EXPECT_LT(steady_clock::now() - t0, seconds(1));
    EXPECT_TRUE(l.entries.empty());
}

TEST(ArchiveListing, TimeLimitCutsADecompressionBomb)
{
    // A tar.gz whose single member is 1 GB of zeros: the header is read at once, skipping it means inflating everything.
    auto const file = MakeTempDir() / L"bomb.tar.gz";
    {
        archive* w = archive_write_new();
        ASSERT_EQ(archive_write_add_filter_gzip(w), ARCHIVE_OK);
        archive_write_set_filter_option(w, "gzip", "compression-level", "1");
        ASSERT_EQ(archive_write_set_format_pax_restricted(w), ARCHIVE_OK);
        ASSERT_EQ(archive_write_open_filename_w(w, file.c_str()), ARCHIVE_OK);
        for (char const* name : {"first.bin", "bomb.bin", "last.bin"})
        {
            archive_entry* e = archive_entry_new();
            archive_entry_set_pathname(e, name);
            bool const big = std::string_view(name) == "bomb.bin";
            archive_entry_set_size(e, big ? (1ll << 30) : 10);
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0644);
            ASSERT_EQ(archive_write_header(w, e), ARCHIVE_OK);
            std::vector<char> zeros(1 << 20);
            for (long long left = big ? (1ll << 30) : 10; left > 0;)
            {
                auto const n = static_cast<size_t>(std::min<long long>(left, static_cast<long long>(zeros.size())));
                ASSERT_EQ(archive_write_data(w, zeros.data(), n), static_cast<la_ssize_t>(n));
                left -= static_cast<long long>(n);
            }
            archive_entry_free(e);
        }
        ASSERT_EQ(archive_write_close(w), ARCHIVE_OK);
        archive_write_free(w);
    }
    ArchiveLimits limits;
    limits.maxTime = milliseconds(100);
    auto const t0 = steady_clock::now();
    auto const l = ListArchive(file, limits, {});
    auto const took = steady_clock::now() - t0;
    EXPECT_TRUE(l.truncated);
    EXPECT_LT(l.entries.size(), 3u);
    EXPECT_LT(took, seconds(5));
}

TEST(ArchiveListing, ZipWithManyEntriesIsReadAsAStream)
{
    // More entries than the seekable reader should load: the central directory is skipped, the local headers are walked.
    std::vector<std::string> names;
    for (int i = 0; i < 70000; ++i)
    {
        names.push_back("f" + std::to_string(i));
    }
    auto const l = ListArchive(WriteTemp(L"huge.zip", BuildZip(names)), {}, {});
    EXPECT_EQ(l.entries.size(), 70000u);
    EXPECT_EQ(l.fileCount, 70000u);
    ASSERT_GT(l.entries.size(), 2u);
    EXPECT_TRUE(l.entries[0].compressedSize.has_value());
}
