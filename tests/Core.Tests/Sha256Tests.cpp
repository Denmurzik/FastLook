#include "TestUtil.h"

#include "Crypto/Sha256.h"

using namespace fastlook::core;

TEST(Sha256, KnownVectors)
{
    EXPECT_EQ(ToHex(Sha256(B("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(ToHex(Sha256({})), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, FileMatchesBuffer)
{
    std::vector<uint8_t> data(3 * 1024 * 1024 + 17);
    for (size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<uint8_t>(i * 31 + (i >> 8));
    }
    auto const path = MakeTempDir() / L"blob.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    auto const fromFile = Sha256File(path);
    ASSERT_TRUE(fromFile);
    EXPECT_EQ(*fromFile, Sha256(data));
    EXPECT_FALSE(Sha256File(path.parent_path() / L"missing.bin"));
}

TEST(Sha256, ParseLine)
{
    auto h = std::string(64, 'a');
    EXPECT_TRUE(ParseSha256Line(h + "  FastLook-Setup-1.2.0.exe\n", "FastLook-Setup-1.2.0.exe"));
    EXPECT_TRUE(ParseSha256Line(h + " *FastLook-Setup-1.2.0.exe", "FastLook-Setup-1.2.0.exe"));
    EXPECT_TRUE(ParseSha256Line(h + "\n", "x"));
    EXPECT_TRUE(ParseSha256Line(std::string(64, 'A') + "  x\r\n", "x"));
    EXPECT_FALSE(ParseSha256Line(h + "  other.exe", "FastLook-Setup-1.2.0.exe"));
    EXPECT_FALSE(ParseSha256Line("zz", "x"));
    EXPECT_FALSE(ParseSha256Line(std::string(63, 'a') + "g  x", "x"));
    EXPECT_FALSE(ParseSha256Line(h + "x", "x"));
}
