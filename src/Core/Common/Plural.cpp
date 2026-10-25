#include "Common/Plural.h"

namespace fastlook::core
{
    PluralForm PluralRu(uint64_t n)
    {
        uint64_t const mod10 = n % 10;
        uint64_t const mod100 = n % 100;
        if (mod10 == 1 && mod100 != 11)
        {
            return PluralForm::One;
        }
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14))
        {
            return PluralForm::Few;
        }
        return PluralForm::Many;
    }

    PluralForm PluralEn(uint64_t n)
    {
        return n == 1 ? PluralForm::One : PluralForm::Other;
    }
}
