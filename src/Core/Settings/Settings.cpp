#include "Settings/Settings.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <system_error>
#include <type_traits>
#include <utility>

namespace fastlook::core
{
    namespace
    {
        using json = nlohmann::json;

        template <typename E>
        struct EnumName
        {
            E value;
            char const* name;
        };

        constexpr std::array kActivationKeys{
            EnumName<ActivationKey>{ActivationKey::Space, "space"},
            EnumName<ActivationKey>{ActivationKey::CtrlSpace, "ctrl+space"},
            EnumName<ActivationKey>{ActivationKey::AltSpace, "alt+space"},
            EnumName<ActivationKey>{ActivationKey::ShiftSpace, "shift+space"},
        };
        constexpr std::array kThemes{
            EnumName<ThemePref>{ThemePref::System, "system"},
            EnumName<ThemePref>{ThemePref::Light, "light"},
            EnumName<ThemePref>{ThemePref::Dark, "dark"},
        };
        constexpr std::array kLanguages{
            EnumName<LanguagePref>{LanguagePref::System, "system"},
            EnumName<LanguagePref>{LanguagePref::English, "en"},
            EnumName<LanguagePref>{LanguagePref::Russian, "ru"},
        };
        constexpr std::array kMarkdownViews{
            EnumName<MarkdownView>{MarkdownView::Rendered, "rendered"},
            EnumName<MarkdownView>{MarkdownView::Source, "source"},
        };
        constexpr std::array kLogLevels{
            EnumName<LogLevel>{LogLevel::Info, "info"},
            EnumName<LogLevel>{LogLevel::Debug, "debug"},
        };
        constexpr std::array<uint32_t, 5> kIdleChoices{0, 5, 15, 30, 60};

        template <typename E, size_t N>
        E EnumFrom(json const& obj, char const* key, std::array<EnumName<E>, N> const& table, E fallback)
        {
            auto it = obj.find(key);
            if (it == obj.end() || !it->is_string())
                return fallback;
            auto const& s = it->template get_ref<std::string const&>();
            for (auto const& entry : table)
                if (s == entry.name)
                    return entry.value;
            return fallback;
        }

        template <typename E, size_t N>
        char const* NameOf(std::array<EnumName<E>, N> const& table, E value)
        {
            for (auto const& entry : table)
                if (entry.value == value)
                    return entry.name;
            return table[0].name;
        }

        bool BoolFrom(json const& obj, char const* key, bool fallback)
        {
            auto it = obj.find(key);
            return it != obj.end() && it->is_boolean() ? it->get<bool>() : fallback;
        }

        // Integer-valued numbers only; anything else (string, bool, float, negative) is "absent".
        bool UintFrom(json const& obj, char const* key, uint64_t& out)
        {
            auto it = obj.find(key);
            if (it == obj.end())
                return false;
            if (it->is_number_unsigned())
            {
                out = it->get<uint64_t>();
                return true;
            }
            if (it->is_number_integer())
            {
                out = 0; // negative integers clamp to the lower bound
                return true;
            }
            return false;
        }

        json const& ObjectOrEmpty(json const& root, char const* key)
        {
            static json const empty = json::object();
            auto it = root.find(key);
            return it != root.end() && it->is_object() ? *it : empty;
        }

        Settings ValuesFrom(json const& root)
        {
            Settings s;
            uint64_t n = 0;
            if (UintFrom(root, "version", n) && n >= 1 && n <= UINT32_MAX)
                s.version = static_cast<uint32_t>(n);

            auto const& g = ObjectOrEmpty(root, "general");
            s.general.desktopEnabled = BoolFrom(g, "desktopEnabled", s.general.desktopEnabled);
            s.general.activationKey = EnumFrom(g, "activationKey", kActivationKeys, s.general.activationKey);
            s.general.prewarm = BoolFrom(g, "prewarm", s.general.prewarm);
            if (UintFrom(g, "idleUnloadMinutes", n))
            {
                if (std::ranges::find(kIdleChoices, n) != kIdleChoices.end())
                    s.general.idleUnloadMinutes = static_cast<uint32_t>(n);
            }
            s.general.theme = EnumFrom(g, "theme", kThemes, s.general.theme);
            s.general.language = EnumFrom(g, "language", kLanguages, s.general.language);
            s.general.peekWarning = BoolFrom(g, "peekWarning", s.general.peekWarning);

            auto const& p = ObjectOrEmpty(root, "preview");
            s.preview.wordWrap = BoolFrom(p, "wordWrap", s.preview.wordWrap);
            if (UintFrom(p, "codeFontSize", n))
                s.preview.codeFontSize = static_cast<uint32_t>(std::clamp<uint64_t>(n, 8, 32));
            s.preview.autoplayMedia = BoolFrom(p, "autoplayMedia", s.preview.autoplayMedia);
            s.preview.useShellHandlers = BoolFrom(p, "useShellHandlers", s.preview.useShellHandlers);
            s.preview.markdownView = EnumFrom(p, "markdownView", kMarkdownViews, s.preview.markdownView);
            if (auto it = p.find("mediaVolume"); it != p.end() && it->is_number())
                s.preview.mediaVolume = std::clamp(it->get<double>(), 0.0, 1.0);

            s.logLevel = EnumFrom(root, "logLevel", kLogLevels, s.logLevel);
            return s;
        }

        // Replaces the file's content atomically-ish: write <file>.tmp, then rename over <file>.
        bool WriteReplacing(std::filesystem::path const& file, std::string_view content)
        {
            auto tmp = file;
            tmp += L".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    return false;
                out.write(content.data(), static_cast<std::streamsize>(content.size()));
                out.flush();
                if (!out)
                    return false;
            }
            if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            return true;
        }
    }

    SettingsDocument ParseSettings(std::string_view text)
    {
        SettingsDocument doc;
        auto root = json::parse(text, nullptr, /*allow_exceptions*/ false);
        if (root.is_discarded() || !root.is_object())
        {
            doc.recoveredFromCorrupt = true;
            return doc;
        }
        doc.values = ValuesFrom(root);
        doc.preservedJson = root.dump();
        return doc;
    }

    std::string SerializeSettings(SettingsDocument const& doc)
    {
        auto root = json::parse(doc.preservedJson, nullptr, false);
        if (root.is_discarded() || !root.is_object())
            root = json::object();

        auto const& v = doc.values;
        root["version"] = v.version;

        auto& g = root["general"];
        if (!g.is_object())
            g = json::object();
        g["desktopEnabled"] = v.general.desktopEnabled;
        g["activationKey"] = NameOf(kActivationKeys, v.general.activationKey);
        g["prewarm"] = v.general.prewarm;
        g["idleUnloadMinutes"] = v.general.idleUnloadMinutes;
        g["theme"] = NameOf(kThemes, v.general.theme);
        g["language"] = NameOf(kLanguages, v.general.language);
        g["peekWarning"] = v.general.peekWarning;

        auto& p = root["preview"];
        if (!p.is_object())
            p = json::object();
        p["wordWrap"] = v.preview.wordWrap;
        p["codeFontSize"] = v.preview.codeFontSize;
        p["autoplayMedia"] = v.preview.autoplayMedia;
        p["useShellHandlers"] = v.preview.useShellHandlers;
        p["markdownView"] = NameOf(kMarkdownViews, v.preview.markdownView);
        p["mediaVolume"] = v.preview.mediaVolume;

        root["logLevel"] = NameOf(kLogLevels, v.logLevel);
        return root.dump(2);
    }

    SettingsDocument LoadSettings(std::filesystem::path const& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            SettingsDocument unreadable;
            std::error_code existsEc;
            unreadable.readFailed = std::filesystem::exists(file, existsEc);
            if (unreadable.readFailed)
                FL_LOG_INFO(L"settings file exists but cannot be read: {}", log::RedactPath(file));
            return unreadable;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        auto doc = ParseSettings(text);
        if (doc.recoveredFromCorrupt)
        {
            auto bak = file;
            bak += L".bak";
            std::error_code ec;
            std::filesystem::copy_file(file, bak, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
                FL_LOG_INFO(L"warning: cannot back up corrupt settings file ({}): {}", ec.value(), log::RedactPath(bak));
        }
        return doc;
    }

    bool SaveSettings(std::filesystem::path const& file, SettingsDocument const& doc)
    {
        if (doc.readFailed)
            return false;
        std::error_code ec;
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), ec);
        return WriteReplacing(file, SerializeSettings(doc));
    }
}
