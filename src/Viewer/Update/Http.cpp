#include "pch.h"
#include "Update/Http.h"

#include <winhttp.h>

#include <fstream>
#include <memory>

#include "Common/Version.h"
#include "Diagnostics/Log.h"
#include "Text/Utf.h"
#include "Update/ReleaseInfo.h"

#pragma comment(lib, "winhttp.lib")

namespace fastlook::viewer
{
    namespace
    {
        constexpr int kMaxHops = 5;
        constexpr size_t kMaxGetBody = 8u * 1024 * 1024;

        struct HandleCloser
        {
            void operator()(void* handle) const
            {
                WinHttpCloseHandle(handle);
            }
        };
        using Handle = std::unique_ptr<void, HandleCloser>;

        std::wstring QueryHeader(HINTERNET request, DWORD info)
        {
            DWORD bytes = 0;
            WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
            {
                return {};
            }
            std::wstring value(bytes / sizeof(wchar_t), L'\0');
            if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
            {
                return {};
            }
            value.resize(bytes / sizeof(wchar_t));
            return value;
        }

        // Resolves a Location header against the URL it answered. Only absolute https URLs and "/path" forms are handled;
        // anything else fails the allowlist later.
        std::wstring ResolveLocation(std::wstring const& base, std::wstring const& location)
        {
            if (!location.empty() && location.front() == L'/' && base.starts_with(L"https://"))
            {
                auto const hostEnd = base.find_first_of(L"/?#", 8);
                return base.substr(0, hostEnd) + location;
            }
            return location;
        }

        // Sends one request. On success `request` is open with its headers received and `status` is set.
        bool Send(std::wstring const& url, std::chrono::seconds timeout, wchar_t const* headers, Handle& session,
                  Handle& connection, Handle& request, uint32_t& status)
        {
            // Children first: closing a parent handle already closes its children.
            request.reset();
            connection.reset();
            URL_COMPONENTSW parts{};
            parts.dwStructSize = sizeof(parts);
            parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
            if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) || !parts.lpszHostName)
            {
                return false;
            }
            bool const secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
            if (!secure && parts.nScheme != INTERNET_SCHEME_HTTP)
            {
                return false;
            }
            std::wstring const host(parts.lpszHostName, parts.dwHostNameLength);
            std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
            if (path.empty())
            {
                path = L"/";
            }

            if (!session)
            {
                static std::wstring const agent = L"FastLook/" FASTLOOK_VERSION_WSTR;
                session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0));
                if (!session)
                {
                    return false;
                }
                int const ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
                WinHttpSetTimeouts(session.get(), ms, ms, ms, ms);
                DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000 /* TLS 1.3 */;
                WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
            }
            connection.reset(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
            if (!connection)
            {
                return false;
            }
            request.reset(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
            if (!request)
            {
                return false;
            }
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

            static constexpr wchar_t kHeaders[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
            if (!WinHttpSendRequest(request.get(), kHeaders, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.get(), nullptr))
            {
                return false;
            }
            DWORD code = 0;
            DWORD size = sizeof(code);
            if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX))
            {
                return false;
            }
            status = code;
            return true;
        }

        constexpr wchar_t kApiHeaders[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        constexpr wchar_t kFileHeaders[] = L"Accept: application/octet-stream\r\n";

        // Opens the final (non-redirect) response of `url`. The handles stay alive in the out parameters.
        bool OpenFollowing(std::wstring url, std::chrono::seconds timeout, wchar_t const* headers, bool allowAnyFirst,
                           Handle& session, Handle& connection, Handle& request, uint32_t& status)
        {
            for (int hop = 0; hop <= kMaxHops; ++hop)
            {
                if (!(hop == 0 && allowAnyFirst) && !core::IsAllowedUpdateUrl(core::ToUtf8(url)))
                {
                    FL_LOG_INFO(L"Update request to a disallowed URL refused");
                    return false;
                }
                if (!Send(url, timeout, headers, session, connection, request, status))
                {
                    return false;
                }
                if (status < 300 || status > 399 || status == 304)
                {
                    return true;
                }
                auto const location = QueryHeader(request.get(), WINHTTP_QUERY_LOCATION);
                if (location.empty())
                {
                    return false;
                }
                url = ResolveLocation(url, location);
            }
            return false; // too many redirects
        }
    }

    std::optional<HttpResult> HttpGet(std::wstring const& url, std::chrono::seconds timeout, bool allowAnyUrl)
    {
        Handle session, connection, request;
        HttpResult result;
        if (!OpenFollowing(url, timeout, kApiHeaders, allowAnyUrl, session, connection, request, result.status))
        {
            return std::nullopt;
        }
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available))
            {
                return std::nullopt;
            }
            if (available == 0)
            {
                return result;
            }
            if (result.body.size() + available > kMaxGetBody)
            {
                return std::nullopt;
            }
            auto const at = result.body.size();
            result.body.resize(at + available);
            DWORD got = 0;
            if (!WinHttpReadData(request.get(), result.body.data() + at, available, &got))
            {
                return std::nullopt;
            }
            result.body.resize(at + got);
        }
    }

    bool HttpDownload(std::wstring const& url, std::filesystem::path const& to,
                      std::function<void(uint64_t got, uint64_t total)> progress, std::stop_token stop)
    {
        Handle session, connection, request;
        uint32_t status = 0;
        // A download has no latency budget like a metadata call; the per-operation timeout still ends a stalled one.
        if (!OpenFollowing(url, std::chrono::seconds(30), kFileHeaders, false, session, connection, request, status) || status != 200)
        {
            return false;
        }
        uint64_t total = 0;
        {
            auto const length = QueryHeader(request.get(), WINHTTP_QUERY_CONTENT_LENGTH);
            if (!length.empty())
            {
                total = _wcstoui64(length.c_str(), nullptr, 10);
            }
        }

        bool ok = false;
        {
            std::ofstream out(to, std::ios::binary | std::ios::trunc);
            std::vector<char> buffer(64 * 1024);
            uint64_t received = 0;
            ok = static_cast<bool>(out);
            while (ok)
            {
                if (stop.stop_requested())
                {
                    ok = false;
                    break;
                }
                DWORD got = 0;
                if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got))
                {
                    ok = false;
                    break;
                }
                if (got == 0)
                {
                    break;
                }
                out.write(buffer.data(), got);
                received += got;
                if (!out)
                {
                    ok = false;
                    break;
                }
                if (progress)
                {
                    progress(received, total);
                }
            }
            out.flush();
            ok = ok && static_cast<bool>(out) && (total == 0 || received == total);
        }
        if (!ok)
        {
            std::error_code ec;
            std::filesystem::remove(to, ec);
        }
        return ok;
    }
}
