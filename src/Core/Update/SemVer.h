#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fastlook::core
{
    struct SemVer
    {
        uint32_t major = 0, minor = 0, patch = 0;
        std::string prerelease;
        bool operator==(SemVer const&) const = default;
    };

    // Accepts a leading 'v'; ignores "+build"; rejects leading zeros and missing parts.
    std::optional<SemVer> ParseSemVer(std::string_view);
    // <0, 0, >0 per semver.org precedence.
    int CompareSemVer(SemVer const&, SemVer const&);
}
