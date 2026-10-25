#include "FileTypes/Sniff.h"

#include <cstddef>
#include <initializer_list>

namespace fastlook::core
{
    namespace
    {
        bool StartsWith(std::span<const uint8_t> head, std::initializer_list<uint8_t> prefix)
        {
            if (head.size() < prefix.size())
                return false;
            size_t i = 0;
            for (uint8_t b : prefix)
                if (head[i++] != b)
                    return false;
            return true;
        }

        bool IsSuspiciousControl(uint8_t b)
        {
            return (b >= 0x01 && b <= 0x08) || (b >= 0x0E && b <= 0x1A) || (b >= 0x1C && b <= 0x1F);
        }
    }

    bool LooksLikeText(std::span<const uint8_t> head)
    {
        if (StartsWith(head, {0xFF, 0xFE, 0x00, 0x00}) || StartsWith(head, {0x00, 0x00, 0xFE, 0xFF}))
            return false; // UTF-32 is not rendered as text
        if (StartsWith(head, {0xEF, 0xBB, 0xBF}) || StartsWith(head, {0xFF, 0xFE}) || StartsWith(head, {0xFE, 0xFF}))
            return true;
        if (head.empty())
            return true;

        size_t control = 0;
        for (uint8_t b : head)
        {
            if (b == 0)
                return false;
            if (IsSuspiciousControl(b))
                ++control;
        }
        return control * 100 <= head.size(); // more than 1 % is binary
    }
}
