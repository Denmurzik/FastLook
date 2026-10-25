#include "Update/ReleaseInfo.h"

#include <array>
#include <nlohmann/json.hpp>

namespace fastlook::core
{
    namespace
    {
        using json = nlohmann::json;

        std::string StringField(json const& object, char const* key)
        {
            auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        bool EqualsLower(std::string_view text, std::string_view lowerCase)
        {
            if (text.size() != lowerCase.size())
            {
                return false;
            }
            for (size_t i = 0; i < text.size(); ++i)
            {
                char c = text[i];
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
                if (c != lowerCase[i])
                {
                    return false;
                }
            }
            return true;
        }
    }

    bool IsAllowedUpdateUrl(std::string_view url)
    {
        constexpr std::string_view kScheme = "https://";
        if (!url.starts_with(kScheme))
        {
            return false;
        }
        url.remove_prefix(kScheme.size());
        auto const authority = url.substr(0, url.find_first_of("/?#"));
        // Userinfo ("github.com@evil.io") and explicit ports are not part of the allowed shapes.
        if (authority.find_first_of("@:\\") != std::string_view::npos)
        {
            return false;
        }
        constexpr std::array kHosts{std::string_view{"api.github.com"}, std::string_view{"github.com"},
                                    std::string_view{"objects.githubusercontent.com"},
                                    std::string_view{"release-assets.githubusercontent.com"}};
        for (auto host : kHosts)
        {
            if (EqualsLower(authority, host))
            {
                return true;
            }
        }
        return false;
    }

    std::optional<ReleaseInfo> ParseLatestRelease(std::string_view text)
    {
        json const doc = json::parse(text, nullptr, false);
        if (!doc.is_object())
        {
            return std::nullopt;
        }
        ReleaseInfo info;
        info.tag = StringField(doc, "tag_name");
        auto const version = ParseSemVer(info.tag);
        if (!version)
        {
            return std::nullopt;
        }
        info.version = *version;
        info.htmlUrl = StringField(doc, "html_url");
        if (!IsAllowedUpdateUrl(info.htmlUrl))
        {
            info.htmlUrl.clear();
        }

        // The asset names come from the tag's version, not from the document: a release cannot offer some other file.
        std::string const bare = info.tag.starts_with('v') ? info.tag.substr(1) : info.tag;
        std::string const setupName = "FastLook-Setup-" + bare + ".exe";
        std::string const shaName = setupName + ".sha256";
        auto const assets = doc.find("assets");
        if (assets == doc.end() || !assets->is_array())
        {
            return std::nullopt;
        }
        for (auto const& asset : *assets)
        {
            if (!asset.is_object())
            {
                continue;
            }
            auto const name = StringField(asset, "name");
            auto const url = StringField(asset, "browser_download_url");
            if (!IsAllowedUpdateUrl(url))
            {
                continue;
            }
            if (name == setupName)
            {
                info.setupName = name;
                info.setupUrl = url;
            }
            else if (name == shaName)
            {
                info.shaUrl = url;
            }
        }
        if (info.setupUrl.empty() || info.shaUrl.empty())
        {
            return std::nullopt;
        }
        return info;
    }
}
