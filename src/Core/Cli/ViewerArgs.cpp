#include "Cli/ViewerArgs.h"

namespace fastlook::core
{
    namespace
    {
        bool ParseInt(std::wstring_view text, int32_t& out)
        {
            bool negative = false;
            if (!text.empty() && text[0] == L'-')
            {
                negative = true;
                text.remove_prefix(1);
            }
            if (text.empty())
            {
                return false;
            }
            int64_t value = 0;
            for (wchar_t c : text)
            {
                if (c < L'0' || c > L'9')
                {
                    return false;
                }
                value = value * 10 + (c - L'0');
                if (value > INT32_MAX)
                {
                    return false;
                }
            }
            out = static_cast<int32_t>(negative ? -value : value);
            return true;
        }

        // "x,y,w,h"; anything else yields nullopt.
        std::optional<RectI> ParseRect(std::wstring_view text)
        {
            int32_t parts[4]{};
            for (int n = 0; n < 4; ++n)
            {
                auto const comma = text.find(L',');
                if ((n < 3) != (comma != std::wstring_view::npos) || !ParseInt(text.substr(0, comma), parts[n]))
                {
                    return std::nullopt;
                }
                if (n < 3)
                {
                    text.remove_prefix(comma + 1);
                }
            }
            return RectI{parts[0], parts[1], parts[2], parts[3]};
        }
    }

    ViewerArgs ParseViewerArgs(std::span<const std::wstring_view> args)
    {
        ViewerArgs r;
        bool bench = false, preview = false, pipe = false;

        for (size_t i = 0; i < args.size(); ++i)
        {
            std::wstring_view const a = args[i];
            // Consumes the option's value; nullopt when the option is the last argument.
            auto value = [&]() -> std::optional<std::wstring_view> {
                if (i + 1 < args.size())
                {
                    return args[++i];
                }
                return std::nullopt;
            };

            if (a == L"--preview")
            {
                preview = true;
                while (i + 1 < args.size() && !args[i + 1].starts_with(L"--"))
                {
                    r.files.emplace_back(args[++i]);
                }
            }
            else if (a == L"--icon-rect")
            {
                if (auto v = value())
                {
                    r.iconRect = ParseRect(*v);
                }
            }
            else if (a == L"--background")
            {
                r.background = true;
            }
            else if (a == L"--pipe")
            {
                if (auto v = value())
                {
                    r.pipeName = *v;
                    pipe = true;
                }
            }
            else if (a == L"--settings")
            {
                // Settings is the fallback mode; the flag only makes it explicit.
            }
            else if (a == L"--settings-page")
            {
                if (auto v = value())
                {
                    r.settingsPage = *v;
                }
            }
            else if (a == L"--bench")
            {
                if (auto v = value())
                {
                    r.benchDir = *v;
                    bench = true;
                }
            }
            else if (a == L"--iterations")
            {
                int32_t n = 0;
                if (auto v = value(); v && ParseInt(*v, n) && n > 0)
                {
                    r.iterations = static_cast<uint32_t>(n);
                }
            }
            else if (a == L"--out")
            {
                if (auto v = value())
                {
                    r.benchOut = *v;
                }
            }
            else if (a == L"--perf-log")
            {
                if (auto v = value())
                {
                    r.perfLog = *v;
                }
            }
            else if (a == L"--script")
            {
                if (auto v = value())
                {
                    r.script = *v;
                }
            }
            else if (a == L"--theme")
            {
                if (auto v = value())
                {
                    if (*v == L"light")
                    {
                        r.theme = ThemePref::Light;
                    }
                    else if (*v == L"dark")
                    {
                        r.theme = ThemePref::Dark;
                    }
                }
            }
            else if (a == L"--lang")
            {
                if (auto v = value())
                {
                    if (*v == L"en")
                    {
                        r.lang = LanguagePref::English;
                    }
                    else if (*v == L"ru")
                    {
                        r.lang = LanguagePref::Russian;
                    }
                }
            }
            else
            {
                r.unknown.emplace_back(a);
            }
        }

        r.mode = bench ? ViewerMode::Bench : preview ? ViewerMode::Preview : pipe ? ViewerMode::Server : ViewerMode::Settings;
        return r;
    }
}
