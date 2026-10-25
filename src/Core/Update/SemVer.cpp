#include "Update/SemVer.h"

#include <algorithm>
#include <charconv>
#include <vector>

namespace fastlook::core
{
    namespace
    {
        bool IsDigit(char c) { return c >= '0' && c <= '9'; }
        bool IsIdentChar(char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-'; }

        bool AllDigits(std::string_view s)
        {
            return !s.empty() && std::ranges::all_of(s, IsDigit);
        }

        bool ParseNumber(std::string_view s, uint32_t& out)
        {
            if (!AllDigits(s) || (s.size() > 1 && s[0] == '0'))
            {
                return false;
            }
            auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
            return ec == std::errc{} && end == s.data() + s.size();
        }

        std::vector<std::string_view> Split(std::string_view s, char sep)
        {
            std::vector<std::string_view> parts;
            size_t start = 0;
            for (;;)
            {
                size_t pos = s.find(sep, start);
                if (pos == std::string_view::npos)
                {
                    parts.push_back(s.substr(start));
                    return parts;
                }
                parts.push_back(s.substr(start, pos - start));
                start = pos + 1;
            }
        }

        bool ValidIdentifiers(std::string_view s, bool rejectNumericLeadingZero)
        {
            for (auto id : Split(s, '.'))
            {
                if (id.empty() || !std::ranges::all_of(id, IsIdentChar))
                {
                    return false;
                }
                if (rejectNumericLeadingZero && AllDigits(id) && id.size() > 1 && id[0] == '0')
                {
                    return false;
                }
            }
            return true;
        }

        int ComparePrerelease(std::string_view a, std::string_view b)
        {
            // A version without a prerelease has higher precedence.
            if (a.empty() || b.empty())
            {
                return a.empty() == b.empty() ? 0 : (a.empty() ? 1 : -1);
            }
            auto ia = Split(a, '.');
            auto ib = Split(b, '.');
            for (size_t i = 0; i < std::min(ia.size(), ib.size()); ++i)
            {
                bool na = AllDigits(ia[i]), nb = AllDigits(ib[i]);
                int c;
                if (na && nb)
                {
                    // No leading zeros, so a longer numeral is larger; avoids overflow.
                    c = ia[i].size() != ib[i].size() ? (ia[i].size() < ib[i].size() ? -1 : 1) : ia[i].compare(ib[i]);
                }
                else if (na != nb)
                {
                    c = na ? -1 : 1; // numeric identifiers sort below alphanumeric ones
                }
                else
                {
                    c = ia[i].compare(ib[i]);
                }
                if (c != 0)
                {
                    return c < 0 ? -1 : 1;
                }
            }
            if (ia.size() == ib.size())
            {
                return 0;
            }
            return ia.size() < ib.size() ? -1 : 1;
        }
    }

    std::optional<SemVer> ParseSemVer(std::string_view text)
    {
        if (text.starts_with('v'))
        {
            text.remove_prefix(1);
        }

        if (size_t plus = text.find('+'); plus != std::string_view::npos)
        {
            if (!ValidIdentifiers(text.substr(plus + 1), false))
            {
                return std::nullopt;
            }
            text = text.substr(0, plus);
        }

        SemVer v;
        if (size_t dash = text.find('-'); dash != std::string_view::npos)
        {
            auto pre = text.substr(dash + 1);
            if (!ValidIdentifiers(pre, true))
            {
                return std::nullopt;
            }
            v.prerelease = std::string(pre);
            text = text.substr(0, dash);
        }

        auto core = Split(text, '.');
        if (core.size() != 3 || !ParseNumber(core[0], v.major) || !ParseNumber(core[1], v.minor) ||
            !ParseNumber(core[2], v.patch))
        {
            return std::nullopt;
        }
        return v;
    }

    int CompareSemVer(SemVer const& a, SemVer const& b)
    {
        if (a.major != b.major) return a.major < b.major ? -1 : 1;
        if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
        if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
        return ComparePrerelease(a.prerelease, b.prerelease);
    }
}
