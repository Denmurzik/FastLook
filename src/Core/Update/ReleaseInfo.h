#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Update/SemVer.h"

namespace fastlook::core
{
    // Env FASTLOOK_UPDATE_API overrides it with a local JSON file (tests); URLs are refused there.
    inline constexpr char kReleasesApi[] = "https://api.github.com/repos/Denmurzik/FastLook/releases/latest";

    struct ReleaseInfo
    {
        SemVer version;
        std::string tag, htmlUrl, setupName, setupUrl, shaUrl;
    };

    // Reads a GitHub "latest release" document. Needs a SemVer tag_name and both FastLook-Setup-<ver>.exe and
    // FastLook-Setup-<ver>.exe.sha256 assets whose URLs lie under https://github.com/Denmurzik/FastLook/releases/download/<tag>/;
    // anything else yields nullopt.
    std::optional<ReleaseInfo> ParseLatestRelease(std::string_view json);

    // https:// and a host of the GitHub release infrastructure (api.github.com, github.com, objects.githubusercontent.com,
    // release-assets.githubusercontent.com). Applied to the first request and to every redirect target.
    bool IsAllowedUpdateUrl(std::string_view url);
}
