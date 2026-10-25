#include <gtest/gtest.h>

#include "Common/Plural.h"

using namespace fastlook::core;

TEST(Plural, Russian)
{
    struct Case
    {
        uint64_t n;
        PluralForm form;
    };
    Case const cases[] = {
        {0, PluralForm::Many},   {1, PluralForm::One},    {2, PluralForm::Few},    {4, PluralForm::Few},
        {5, PluralForm::Many},   {11, PluralForm::Many},  {12, PluralForm::Many},  {14, PluralForm::Many},
        {21, PluralForm::One},   {22, PluralForm::Few},   {25, PluralForm::Many},  {101, PluralForm::One},
        {111, PluralForm::Many}, {112, PluralForm::Many}, {24388, PluralForm::Many},
    };
    for (auto const& c : cases)
    {
        EXPECT_EQ(PluralRu(c.n), c.form) << c.n;
    }
}

TEST(Plural, English)
{
    for (uint64_t n : {0u, 2u, 4u, 5u, 11u, 12u, 14u, 21u, 22u, 25u, 101u, 111u})
    {
        EXPECT_EQ(PluralEn(n), PluralForm::Other) << n;
    }
    EXPECT_EQ(PluralEn(1), PluralForm::One);
}
