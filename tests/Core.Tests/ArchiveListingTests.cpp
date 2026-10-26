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
    EXPECT_EQ(l.fileCount, 0u);
    EXPECT_EQ(l.dirCount, 0u);
    EXPECT_EQ(l.totalSize, 0u);
    EXPECT_FALSE(l.truncated);
    EXPECT_FALSE(l.headerTooLarge);
    EXPECT_FALSE(l.encryptedHeaders);
    EXPECT_TRUE(l.error.empty()); // an empty file is an empty listing, not a failure
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
    // The kept name holds no more memory than the budget counts for it.
    for (auto const& e : l.entries)
    {
        EXPECT_LE(e.path.capacity(), e.path.size() + 16) << e.path.size();
    }
    EXPECT_EQ(l.entries[1].path, L"../../evil.txt");
    EXPECT_EQ(l.entries[2].path, L"C:/Windows/x.dll");
    EXPECT_EQ(l.entries[3].path, L"ok/file.txt"); // "./" has no name and is dropped
}

TEST(ArchiveListing, BidiAndControlCharactersInNamesAreNeutralised)
{
    // Final review B2#8: "invoice<RLO>fdp.exe" must not read as "invoiceexe.pdf".
    auto const l = ListArchive(WriteTemp(L"bidi.zip", BuildZip({"invoice\xE2\x80\xAE" "fdp.exe", "a\x01" "b.txt",
                                                                "x\xE2\x81\xA6" "y\xE2\x80\x8F" "z.txt", "\xD0\x9F\xD1\x80\xD0\xB8.txt"})),
                               {}, {});
    ASSERT_EQ(l.entries.size(), 4u);
    EXPECT_EQ(l.entries[0].path, L"invoice\uFFFDfdp.exe");
    EXPECT_EQ(l.entries[1].path, L"a\uFFFDb.txt");
    EXPECT_EQ(l.entries[2].path, L"x\uFFFDy\uFFFDz.txt");
    EXPECT_EQ(l.entries[3].path, L"\u041F\u0440\u0438.txt"); // ordinary non-ASCII names are untouched
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

namespace
{
    // 7-Zip variable-length number.
    void Put7z(std::string& out, uint64_t v)
    {
        for (int extra = 0; extra < 8; ++extra)
        {
            if (v < (1ull << (7 * (extra + 1))))
            {
                uint8_t const mask = static_cast<uint8_t>(0xFF << (8 - extra));
                out.push_back(static_cast<char>(mask | static_cast<uint8_t>(v >> (8 * extra))));
                for (int i = 0; i < extra; ++i)
                {
                    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
                }
                return;
            }
        }
        out.push_back(static_cast<char>(0xFF));
        for (int i = 0; i < 8; ++i)
        {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    }

    // A 7z file made of a signature header plus `header` placed right after it (offset 0). Checksums are not valid.
    std::string BuildSevenZip(std::string const& header, uint64_t declaredHeaderSize)
    {
        std::string out("7z\xBC\xAF\x27\x1C", 6);
        out.push_back(0);
        out.push_back(4);
        Put32(out, 0);
        Put32(out, 0);
        Put32(out, 0); // next header offset (u64)
        Put32(out, static_cast<uint32_t>(declaredHeaderSize));
        Put32(out, static_cast<uint32_t>(declaredHeaderSize >> 32));
        Put32(out, 0);
        return out + header;
    }

    // An encoded header: one LZMA folder whose decoded size is `unpackedSize`.
    std::string EncodedHeader(uint64_t unpackedSize)
    {
        std::string h;
        h.push_back(0x17);
        h.push_back(0x06); // PackInfo
        Put7z(h, 0);
        Put7z(h, 1);
        h.push_back(0x09);
        Put7z(h, 10);
        h.push_back(0x00);
        h.push_back(0x07); // UnPackInfo
        h.push_back(0x0B); // Folder
        Put7z(h, 1);
        h.push_back(0);
        Put7z(h, 1); // one coder
        h.push_back(0x23); // id size 3, has properties
        h += std::string("\x03\x01\x01", 3);
        Put7z(h, 5);
        h += std::string(5, '\x5D');
        h.push_back(0x0C);
        Put7z(h, unpackedSize);
        h.push_back(0x00);
        h.push_back(0x00);
        return h;
    }
}

TEST(ArchiveListing, SevenZipHeaderBiggerThanTheLimitIsRefusedUnread)
{
    // The signature header declares a 1 GB next header; the file is 32 bytes. Refused before libarchive sees it.
    auto const l = ListArchive(WriteTemp(L"huge-header.7z", BuildSevenZip("", 1ull << 30)), {}, {});
    EXPECT_TRUE(l.headerTooLarge);
    EXPECT_FALSE(l.error.empty());
    EXPECT_TRUE(l.entries.empty());
}

TEST(ArchiveListing, SevenZipEncodedHeaderThatInflatesPastTheLimitIsRefused)
{
    // A few bytes on disk that declare a 4 GB decoded header (the "LZMA of zeros" shape).
    auto const header = EncodedHeader(4ull << 30);
    auto const l = ListArchive(WriteTemp(L"bomb-header.7z", BuildSevenZip(header, header.size())), {}, {});
    EXPECT_TRUE(l.headerTooLarge);
}

TEST(ArchiveListing, SevenZipEncodedHeaderWithinTheLimitIsNotRefusedByTheCheck)
{
    auto const header = EncodedHeader(100000);
    auto const l = ListArchive(WriteTemp(L"small-header.7z", BuildSevenZip(header, header.size())), {}, {});
    EXPECT_FALSE(l.headerTooLarge); // libarchive then fails on the missing packed stream, which is a plain error
}

TEST(ArchiveListing, SevenZipEncodedHeaderThatCannotBeParsedIsRefused)
{
    std::string header = EncodedHeader(100);
    header.resize(header.size() - 6); // cut inside the folder description
    auto const l = ListArchive(WriteTemp(L"cut-header.7z", BuildSevenZip(header, header.size())), {}, {});
    EXPECT_TRUE(l.headerTooLarge);
}

TEST(ArchiveListing, SevenZipHeaderDeclaringAstronomicalStreamCountReturnsAtOnce)
{
    std::string h;
    h.push_back(0x17);
    h.push_back(0x07);
    h.push_back(0x0B);
    Put7z(h, 1);
    h.push_back(0);
    Put7z(h, 1);       // one coder
    h.push_back(0x10); // complex: counts follow
    Put7z(h, 1ull << 62);
    Put7z(h, 1);
    auto const file = WriteTemp(L"streams.7z", BuildSevenZip(h, h.size()));
    auto const t0 = steady_clock::now();
    auto const l = ListArchive(file, {}, {});
    EXPECT_LT(steady_clock::now() - t0, seconds(1));
    EXPECT_TRUE(l.headerTooLarge);
}

TEST(ArchiveListing, SevenZipPlainHeaderBiggerThanFourMegabytesIsRefused)
{
    std::string header(5u * 1024 * 1024, '\0');
    header[0] = 1; // a plain Header
    auto const l = ListArchive(WriteTemp(L"plain-big.7z", BuildSevenZip(header, header.size())), {}, {});
    EXPECT_TRUE(l.headerTooLarge);
}

TEST(ArchiveListing, RealSevenZipIsNotRefused)
{
    EXPECT_FALSE(ListArchive(FixturePath("archive/sample.7z"), {}, {}).headerTooLarge);
}

TEST(ArchiveListing, ZipEndRecordThatUnderstatesTheDirectoryStillListsAndIsBounded)
{
    // The end record says 1 entry, the directory holds 200 000 records (about 10 MB): it must be read as a stream.
    std::vector<std::string> names;
    for (int i = 0; i < 200000; ++i)
    {
        names.push_back("f" + std::to_string(i));
    }
    auto zip = BuildZip(names);
    auto const eocd = zip.rfind(std::string("PK\x05\x06", 4));
    ASSERT_NE(eocd, std::string::npos);
    zip[eocd + 8] = 1;
    zip[eocd + 9] = 0;
    zip[eocd + 10] = 1;
    zip[eocd + 11] = 0;
    auto const l = ListArchive(WriteTemp(L"lying.zip", zip), {}, {});
    EXPECT_EQ(l.entries.size(), 100000u);
    EXPECT_TRUE(l.truncated);
}

TEST(ArchiveListing, TimeLimitWithoutAnyEntryIsTruncatedAndEmpty)
{
    ArchiveLimits limits;
    limits.maxTime = milliseconds(0);
    auto const l = ListArchive(FixturePath("archive/sample.tar.gz"), limits, {});
    EXPECT_TRUE(l.entries.empty());
    EXPECT_TRUE(l.truncated);
    EXPECT_TRUE(l.error.empty());
}
