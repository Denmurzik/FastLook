#include "Peek/PeekDetect.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

namespace fastlook::core
{
    namespace
    {
        using nlohmann::json;

        struct Shortcut
        {
            bool win = false, ctrl = false, alt = false, shift = false;
            int code = 0;
            bool operator==(Shortcut const&) const = default;
        };

        bool BoolOf(json const& o, char const* key)
        {
            auto it = o.find(key);
            return it != o.end() && it->is_boolean() && it->get<bool>();
        }

        bool ParseShortcut(json const& j, Shortcut& out)
        {
            if (!j.is_object()) return false;
            auto code = j.find("code");
            if (code == j.end() || !code->is_number_integer()) return false;
            out = {BoolOf(j, "win"), BoolOf(j, "ctrl"), BoolOf(j, "alt"), BoolOf(j, "shift"), code->get<int>()};
            return true;
        }

        Shortcut OurShortcut(ActivationKey k)
        {
            return {false, k == ActivationKey::CtrlSpace, k == ActivationKey::AltSpace, k == ActivationKey::ShiftSpace, VK_SPACE};
        }

        json TryParse(std::string_view s)
        {
            auto j = json::parse(s.begin(), s.end(), nullptr, false);
            return j.is_discarded() ? json() : j;
        }

        std::string ReadWhole(std::filesystem::path const& p)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in) return {};
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }

    PeekStatus DetectPeekConflict(std::string_view globalSettingsJson, std::string_view peekSettingsJson, ActivationKey ours)
    {
        try
        {
            auto g = TryParse(globalSettingsJson);
            if (!g.is_object()) return {};
            auto en = g.find("enabled");
            if (en == g.end() || !en->is_object()) return {};
            auto pk = en->find("Peek");
            if (pk == en->end() || !pk->is_boolean() || !pk->get<bool>()) return {};

            Shortcut shortcut{false, true, false, false, VK_SPACE}; // Peek's default: Ctrl+Space
            bool spaceToActivate = false;
            auto p = TryParse(peekSettingsJson);
            if (p.is_object())
            {
                auto props = p.find("properties");
                if (props != p.end() && props->is_object())
                {
                    Shortcut s;
                    auto act = props->find("ActivationShortcut");
                    auto def = props->find("DefaultActivationShortcut");
                    if (act != props->end() && ParseShortcut(*act, s)) shortcut = s;
                    else if (def != props->end() && ParseShortcut(*def, s)) shortcut = s;

                    auto sp = props->find("EnableSpaceToActivate");
                    if (sp != props->end() && sp->is_object()) spaceToActivate = BoolOf(*sp, "value");
                }
            }
            bool conflict = shortcut == OurShortcut(ours) || (spaceToActivate && ours == ActivationKey::Space);
            return {true, conflict};
        }
        catch (...)
        {
            return {};
        }
    }

    PeekStatus DetectPeekConflictOnDisk(ActivationKey ours)
    {
        try
        {
            std::filesystem::path dir;
            wchar_t buf[MAX_PATH * 4];
            DWORD n = GetEnvironmentVariableW(L"FASTLOOK_POWERTOYS_DIR", buf, static_cast<DWORD>(std::size(buf)));
            if (n > 0 && n < std::size(buf))
            {
                dir = buf;
            }
            else
            {
                n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
                if (n == 0 || n >= std::size(buf)) return {};
                dir = std::filesystem::path(buf) / L"Microsoft" / L"PowerToys";
            }
            return DetectPeekConflict(ReadWhole(dir / L"settings.json"), ReadWhole(dir / L"Peek" / L"settings.json"), ours);
        }
        catch (...)
        {
            return {};
        }
    }
}
