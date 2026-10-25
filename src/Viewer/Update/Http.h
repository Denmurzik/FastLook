#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace fastlook::viewer
{
    struct HttpResult
    {
        uint32_t status = 0;
        std::vector<uint8_t> body;
    };

    // Blocking GET over WinHTTP with TLS validation on; call it off the UI thread. UA "FastLook/<ver>", Accept
    // application/vnd.github+json. Redirects are followed by hand (at most 5 hops) and every URL, the first included, must
    // pass core::IsAllowedUpdateUrl, otherwise the call fails. `allowAnyUrl` skips that check for the first URL only (the
    // FASTLOOK_UPDATE_API test override). The body is capped at 8 MiB. nullopt on any transport failure.
    std::optional<HttpResult> HttpGet(std::wstring const& url, std::chrono::seconds timeout = std::chrono::seconds(15),
                                      bool allowAnyUrl = false);

    // Streams a 200 response to `to` (replaced). `progress` gets (received, total or 0 when unknown) from the calling
    // thread. False on a non-200 answer, a transport failure, a stop request or a write failure; the partial file is removed.
    bool HttpDownload(std::wstring const& url, std::filesystem::path const& to,
                      std::function<void(uint64_t got, uint64_t total)> progress, std::stop_token stop);
}
