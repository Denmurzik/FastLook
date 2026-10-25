#include <gtest/gtest.h>

#include <vector>

#include "FileTypes/Sniff.h"

using namespace fastlook::core;

TEST(Sniff, Rules)
{
    EXPECT_TRUE(LooksLikeText(std::vector<uint8_t>{0xFF, 0xFE, 'a', 0x00}));    // UTF-16 LE BOM
    EXPECT_FALSE(LooksLikeText(std::vector<uint8_t>{0xFF, 0xFE, 0x00, 0x00}));  // UTF-32 LE BOM
    EXPECT_FALSE(LooksLikeText(std::vector<uint8_t>{0x00, 0x00, 0xFE, 0xFF}));  // UTF-32 BE BOM
    EXPECT_TRUE(LooksLikeText(std::vector<uint8_t>{0xFE, 0xFF, 0x00, 'a'}));    // UTF-16 BE BOM
    EXPECT_TRUE(LooksLikeText(std::vector<uint8_t>{0xEF, 0xBB, 0xBF, 0x00}));   // UTF-8 BOM wins over NUL
    EXPECT_TRUE(LooksLikeText(std::span<const uint8_t>{}));
    EXPECT_FALSE(LooksLikeText(std::vector<uint8_t>{'a', 0, 'b'}));
}

TEST(Sniff, ControlByteRatio)
{
    std::vector<uint8_t> five(100, 'a');
    for (int i = 0; i < 5; ++i)
        five[i * 20] = 0x01;
    EXPECT_FALSE(LooksLikeText(five));

    std::vector<uint8_t> half(200, 'a'); // 0.5 % control bytes
    half[10] = 0x02;
    EXPECT_TRUE(LooksLikeText(half));

    std::vector<uint8_t> exactlyOnePercent(100, 'a');
    exactlyOnePercent[7] = 0x1F; // 1 % is not "more than" 1 %
    EXPECT_TRUE(LooksLikeText(exactlyOnePercent));

    std::vector<uint8_t> twoPercent(100, 'a');
    twoPercent[7] = 0x1F;
    twoPercent[9] = 0x0E;
    EXPECT_FALSE(LooksLikeText(twoPercent));

    std::vector<uint8_t> allowed(100, '\t'); // tab, LF, VT, FF, CR, ESC are not counted
    allowed[1] = '\n';
    allowed[2] = '\r';
    allowed[3] = 0x0C;
    allowed[4] = 0x1B;
    allowed[5] = 0x0B;
    EXPECT_TRUE(LooksLikeText(allowed));
}

TEST(Sniff, Cp1251BytesAreText)
{
    std::vector<uint8_t> cp1251{0xCF, 0xF0, 0xE8, 0xE2, 0xE5, 0xF2, ' ', 0xEC, 0xE8, 0xF0, '\r', '\n'};
    EXPECT_TRUE(LooksLikeText(cp1251));
}
