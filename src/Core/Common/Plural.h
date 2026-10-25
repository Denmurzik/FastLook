#pragma once

#include <cstdint>

namespace fastlook::core
{
    enum class PluralForm
    {
        One,
        Few,
        Many,
        Other
    };

    // Russian: One (1, 21, 101, not 11), Few (2-4, 22-24, not 12-14), Many (everything else). Never Other.
    PluralForm PluralRu(uint64_t n);
    // English: One for 1, Other otherwise.
    PluralForm PluralEn(uint64_t n);
}
