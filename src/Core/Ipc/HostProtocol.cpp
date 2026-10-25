#include "Ipc/HostProtocol.h"

#include <charconv>
#include <cstdio>
#include <string>
#include <vector>

namespace fastlook::core::host
{
    namespace
    {
        constexpr uint32_t kVkSpace = 0x20, kVkEscape = 0x1B, kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28,
                           kVkReturn = 0x0D, kVkF11 = 0x7A, kVkW = 'W';

        // Quoting that CommandLineToArgvW reads back: backslashes double up in front of a quote and at the end.
        void AppendQuoted(std::wstring& out, std::wstring_view value)
        {
            out.push_back(L'"');
            size_t backslashes = 0;
            for (wchar_t c : value)
            {
                if (c == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == L'"')
                {
                    out.append(backslashes * 2 + 1, L'\\');
                }
                else
                {
                    out.append(backslashes, L'\\');
                }
                backslashes = 0;
                out.push_back(c);
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
        }

        template <typename T>
        bool ParseNumber(std::string_view text, T& value, int base = 10)
        {
            if (text.empty())
            {
                return false;
            }
            auto const [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
            return ec == std::errc{} && end == text.data() + text.size();
        }

        // ASCII digits only; anything else is not a number.
        bool Narrow(std::wstring_view text, std::string& out)
        {
            out.clear();
            for (wchar_t c : text)
            {
                if (c > 0x7F)
                {
                    return false;
                }
                out.push_back(static_cast<char>(c));
            }
            return true;
        }

        bool ParseHex(std::wstring_view text, uint64_t& value)
        {
            std::string narrow;
            return Narrow(text, narrow) && ParseNumber(narrow, value, 16);
        }

        bool ParseRect(std::wstring_view text, RectI& rect)
        {
            int32_t v[4]{};
            std::string narrow;
            for (int i = 0; i < 4; ++i)
            {
                size_t const comma = text.find(L',');
                if ((comma == std::wstring_view::npos) != (i == 3))
                {
                    return false;
                }
                if (!Narrow(text.substr(0, comma), narrow) || !ParseNumber(narrow, v[i]))
                {
                    return false;
                }
                if (i < 3)
                {
                    text.remove_prefix(comma + 1);
                }
            }
            rect = RectI{v[0], v[1], v[2], v[3]};
            return true;
        }

        // At most `maxParts` space-separated parts; the last one keeps the rest of the line.
        std::vector<std::string_view> Split(std::string_view line, size_t maxParts)
        {
            std::vector<std::string_view> parts;
            while (parts.size() + 1 < maxParts)
            {
                size_t const space = line.find(' ');
                if (space == std::string_view::npos)
                {
                    break;
                }
                parts.push_back(line.substr(0, space));
                line.remove_prefix(space + 1);
            }
            parts.push_back(line);
            return parts;
        }

        std::string_view TrimEol(std::string_view line)
        {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            {
                line.remove_suffix(1);
            }
            return line;
        }
    }

    std::wstring FormatHostArgs(HostArgs const& a)
    {
        wchar_t number[64];
        std::wstring out;
        swprintf_s(number, L"--parent %llx", static_cast<unsigned long long>(a.parentHwnd));
        out += number;
        out += L" --clsid ";
        AppendQuoted(out, a.clsid);
        out += L" --file ";
        AppendQuoted(out, a.file);
        out += L" --rect " + std::to_wstring(a.rect.x) + L"," + std::to_wstring(a.rect.y) + L"," + std::to_wstring(a.rect.w) + L"," +
               std::to_wstring(a.rect.h);
        out += a.dark ? L" --theme dark" : L" --theme light";
        if (a.debugHang)
        {
            out += L" --debug-hang";
        }
        if (a.debugHangAfterReady)
        {
            out += L" --debug-hang-after-ready";
        }
        return out;
    }

    std::optional<HostArgs> ParseHostArgs(std::span<const std::wstring_view> args)
    {
        HostArgs result;
        bool haveParent = false, haveClsid = false, haveFile = false, haveRect = false;
        for (size_t i = 0; i < args.size(); ++i)
        {
            auto const arg = args[i];
            auto const value = [&]() -> std::optional<std::wstring_view> {
                if (i + 1 >= args.size())
                {
                    return std::nullopt;
                }
                return args[++i];
            };
            if (arg == L"--parent")
            {
                auto v = value();
                haveParent = v && ParseHex(*v, result.parentHwnd);
                if (!haveParent)
                {
                    return std::nullopt;
                }
            }
            else if (arg == L"--clsid")
            {
                auto v = value();
                if (!v || v->empty())
                {
                    return std::nullopt;
                }
                result.clsid = *v;
                haveClsid = true;
            }
            else if (arg == L"--file")
            {
                auto v = value();
                if (!v || v->empty())
                {
                    return std::nullopt;
                }
                result.file = *v;
                haveFile = true;
            }
            else if (arg == L"--rect")
            {
                auto v = value();
                haveRect = v && ParseRect(*v, result.rect);
                if (!haveRect)
                {
                    return std::nullopt;
                }
            }
            else if (arg == L"--theme")
            {
                auto v = value();
                if (!v || (*v != L"dark" && *v != L"light"))
                {
                    return std::nullopt;
                }
                result.dark = *v == L"dark";
            }
            else if (arg == L"--debug-hang")
            {
                result.debugHang = true;
            }
            else if (arg == L"--debug-hang-after-ready")
            {
                result.debugHangAfterReady = true;
            }
            else
            {
                return std::nullopt;
            }
        }
        if (!haveParent || !haveClsid || !haveFile || !haveRect)
        {
            return std::nullopt;
        }
        return result;
    }

    std::optional<HostCommand> ParseHostCommand(std::string_view line)
    {
        line = TrimEol(line);
        if (line == "quit")
        {
            return HostCommand{HostCmd::Quit, {}};
        }
        auto const parts = Split(line, 5);
        if (parts.size() != 5 || parts[0] != "rect")
        {
            return std::nullopt;
        }
        int32_t v[4]{};
        for (int i = 0; i < 4; ++i)
        {
            if (!ParseNumber(parts[i + 1], v[i]))
            {
                return std::nullopt;
            }
        }
        return HostCommand{HostCmd::Rect, RectI{v[0], v[1], v[2], v[3]}};
    }

    std::string FormatHostCommand(HostCommand const& c)
    {
        if (c.cmd == HostCmd::Quit)
        {
            return "quit\n";
        }
        return "rect " + std::to_string(c.rect.x) + " " + std::to_string(c.rect.y) + " " + std::to_string(c.rect.w) + " " +
               std::to_string(c.rect.h) + "\n";
    }

    std::string FormatHostReply(HostReply const& r)
    {
        switch (r.status)
        {
        case HostStatus::Ready:
            return "ready\n";
        case HostStatus::Key:
            return "key " + std::to_string(r.vk) + " " + std::to_string(r.mods) + "\n";
        case HostStatus::Error:
        {
            char code[16];
            snprintf(code, sizeof(code), "0x%08X", r.hr);
            std::string text = r.text;
            for (char& c : text)
            {
                if (c == '\n' || c == '\r')
                {
                    c = ' ';
                }
            }
            return std::string("error ") + code + " " + text + "\n";
        }
        }
        return {};
    }

    std::optional<HostReply> ParseHostReply(std::string_view line)
    {
        line = TrimEol(line);
        if (line == "ready")
        {
            return HostReply{HostStatus::Ready};
        }
        if (line.starts_with("key "))
        {
            auto const parts = Split(line, 3);
            HostReply reply{HostStatus::Key};
            if (parts.size() != 3 || !ParseNumber(parts[1], reply.vk) || !ParseNumber(parts[2], reply.mods))
            {
                return std::nullopt;
            }
            return reply;
        }
        if (line.starts_with("error "))
        {
            auto const parts = Split(line, 3);
            HostReply reply{HostStatus::Error};
            std::string_view code = parts[1];
            if (!code.starts_with("0x") && !code.starts_with("0X"))
            {
                return std::nullopt;
            }
            code.remove_prefix(2);
            if (!ParseNumber(code, reply.hr, 16))
            {
                return std::nullopt;
            }
            if (parts.size() == 3)
            {
                reply.text = std::string(parts[2]);
            }
            return reply;
        }
        return std::nullopt;
    }

    bool IsForwardedHostKey(uint32_t vk, uint32_t mods)
    {
        if (mods == 0)
        {
            return vk == kVkSpace || vk == kVkEscape || vk == kVkLeft || vk == kVkRight || vk == kVkUp || vk == kVkDown ||
                   vk == kVkReturn || vk == kVkF11;
        }
        return mods == kModCtrl && vk == kVkW;
    }
}
