#include "Ipc/Protocol.h"

#include <windows.h>
#include <sddl.h>

#include <limits>
#include <vector>

#include <nlohmann/json.hpp>

#include "Text/Utf.h"

namespace fastlook::core::ipc
{
    namespace
    {
        using Json = nlohmann::ordered_json;

        struct TypeName
        {
            MsgType type;
            const char* name;
        };

        constexpr TypeName kTypeNames[] = {
            {MsgType::Show, "show"},
            {MsgType::Toggle, "toggle"},
            {MsgType::Items, "items"},
            {MsgType::Ping, "ping"},
            {MsgType::Pong, "pong"},
            {MsgType::SettingsChanged, "settingsChanged"},
            {MsgType::OpenSettings, "openSettings"},
            {MsgType::Shutdown, "shutdown"},
            {MsgType::Ack, "ack"},
            {MsgType::Event, "event"},
        };

        const char* NavModeName(NavMode m) { return m == NavMode::Folder ? "folder" : "selection"; }

        Json ItemsToJson(std::vector<Item> const& items)
        {
            Json arr = Json::array();
            for (auto const& it : items)
            {
                arr.push_back(Json{{"path", ToUtf8(it.path)}, {"virtual", it.isVirtual}});
            }
            return arr;
        }

        bool ItemsFromJson(Json const& j, std::vector<Item>& out)
        {
            if (!j.is_array())
            {
                return false;
            }
            out.clear();
            out.reserve(j.size());
            for (auto const& e : j)
            {
                if (!e.is_object())
                {
                    return false;
                }
                auto path = e.find("path");
                if (path == e.end() || !path->is_string())
                {
                    return false;
                }
                Item item;
                item.path = FromUtf8(path->get_ref<std::string const&>());
                if (auto v = e.find("virtual"); v != e.end())
                {
                    if (!v->is_boolean())
                    {
                        return false;
                    }
                    item.isVirtual = v->get<bool>();
                }
                out.push_back(std::move(item));
            }
            return true;
        }

        // Each reader leaves `out` untouched when the key is absent; returns false when present with the wrong type.
        template <typename T>
        bool ReadUInt(Json const& j, const char* key, T& out)
        {
            auto it = j.find(key);
            if (it == j.end())
            {
                return true;
            }
            if (!it->is_number_unsigned())
            {
                return false;
            }
            auto v = it->get<uint64_t>();
            if (v > std::numeric_limits<T>::max())
            {
                return false;
            }
            out = static_cast<T>(v);
            return true;
        }

        bool ReadBool(Json const& j, const char* key, bool& out)
        {
            auto it = j.find(key);
            if (it == j.end())
            {
                return true;
            }
            if (!it->is_boolean())
            {
                return false;
            }
            out = it->get<bool>();
            return true;
        }

        bool ReadString(Json const& j, const char* key, std::string& out)
        {
            auto it = j.find(key);
            if (it == j.end())
            {
                return true;
            }
            if (!it->is_string())
            {
                return false;
            }
            out = it->get<std::string>();
            return true;
        }

        bool ReadNavMode(Json const& j, NavMode& out)
        {
            auto it = j.find("navMode");
            if (it == j.end())
            {
                return true;
            }
            if (!it->is_string())
            {
                return false;
            }
            auto const& s = it->get_ref<std::string const&>();
            if (s == "folder") { out = NavMode::Folder; return true; }
            if (s == "selection") { out = NavMode::Selection; return true; }
            return false;
        }

        bool ReadRect(Json const& j, std::optional<RectI>& out)
        {
            auto it = j.find("iconRect");
            if (it == j.end())
            {
                return true;
            }
            if (!it->is_array() || it->size() != 4)
            {
                return false;
            }
            int32_t v[4];
            for (size_t i = 0; i < 4; ++i)
            {
                auto const& e = (*it)[i];
                if (!e.is_number_integer())
                {
                    return false;
                }
                auto n = e.get<int64_t>();
                if (n < std::numeric_limits<int32_t>::min() || n > std::numeric_limits<int32_t>::max())
                {
                    return false;
                }
                v[i] = static_cast<int32_t>(n);
            }
            out = RectI{v[0], v[1], v[2], v[3]};
            return true;
        }

        bool ParseBody(Json const& j, Message& m)
        {
            switch (m.type)
            {
            case MsgType::Show:
            case MsgType::Toggle:
            {
                auto& s = m.show;
                if (!ReadUInt(j, "explorerHwnd", s.explorerHwnd) || !ReadBool(j, "isDesktop", s.isDesktop) ||
                    !ReadUInt(j, "monitor", s.monitor) || !ReadUInt(j, "index", s.index) ||
                    !ReadBool(j, "listComplete", s.listComplete) || !ReadNavMode(j, s.navMode) ||
                    !ReadRect(j, s.iconRect))
                {
                    return false;
                }
                if (auto it = j.find("items"); it != j.end() && !ItemsFromJson(*it, s.items))
                {
                    return false;
                }
                return true;
            }
            case MsgType::Items:
            {
                auto& s = m.items;
                if (!ReadUInt(j, "index", s.index) || !ReadNavMode(j, s.navMode))
                {
                    return false;
                }
                if (auto it = j.find("items"); it != j.end() && !ItemsFromJson(*it, s.items))
                {
                    return false;
                }
                return true;
            }
            case MsgType::OpenSettings:
                return ReadString(j, "page", m.page);
            case MsgType::Ack:
                return ReadBool(j, "shown", m.shown);
            case MsgType::Event:
                return ReadString(j, "name", m.eventName);
            default:
                return true;
            }
        }
    }

    std::string Serialize(Message const& m)
    {
        const char* typeName = "ping";
        for (auto const& t : kTypeNames)
        {
            if (t.type == m.type)
            {
                typeName = t.name;
            }
        }

        Json j = Json::object();
        j["v"] = kProtocolVersion;
        j["id"] = m.id;
        if (m.replyTo != 0)
        {
            j["re"] = m.replyTo;
        }
        j["type"] = typeName;

        switch (m.type)
        {
        case MsgType::Show:
        case MsgType::Toggle:
        {
            auto const& s = m.show;
            j["explorerHwnd"] = s.explorerHwnd;
            j["isDesktop"] = s.isDesktop;
            j["monitor"] = s.monitor;
            j["index"] = s.index;
            j["listComplete"] = s.listComplete;
            j["navMode"] = NavModeName(s.navMode);
            if (s.iconRect)
            {
                j["iconRect"] = Json::array({s.iconRect->x, s.iconRect->y, s.iconRect->w, s.iconRect->h});
            }
            j["items"] = ItemsToJson(s.items);
            break;
        }
        case MsgType::Items:
            j["index"] = m.items.index;
            j["navMode"] = NavModeName(m.items.navMode);
            j["items"] = ItemsToJson(m.items.items);
            break;
        case MsgType::OpenSettings:
            j["page"] = m.page;
            break;
        case MsgType::Ack:
            j["shown"] = m.shown;
            break;
        case MsgType::Event:
            j["name"] = m.eventName;
            break;
        default:
            break;
        }
        return j.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    std::optional<Message> Parse(std::string_view text)
    {
        Json j = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions*/ false);
        if (!j.is_object())
        {
            return std::nullopt;
        }

        auto v = j.find("v");
        if (v == j.end() || !v->is_number_unsigned() || v->get<uint64_t>() != kProtocolVersion)
        {
            return std::nullopt;
        }

        auto type = j.find("type");
        if (type == j.end() || !type->is_string())
        {
            return std::nullopt;
        }

        Message m;
        bool known = false;
        for (auto const& t : kTypeNames)
        {
            if (type->get_ref<std::string const&>() == t.name)
            {
                m.type = t.type;
                known = true;
            }
        }
        if (!known || !ReadUInt(j, "id", m.id) || !ReadUInt(j, "re", m.replyTo) || !ParseBody(j, m))
        {
            return std::nullopt;
        }
        return m;
    }

    std::wstring PipeNameForCurrentUser()
    {
        std::wstring sid = L"unknown";
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<char> buf(size);
            if (size != 0 && GetTokenInformation(token, TokenUser, buf.data(), size, &size))
            {
                LPWSTR str = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str))
                {
                    sid = str;
                    LocalFree(str);
                }
            }
            CloseHandle(token);
        }
        return L"\\\\.\\pipe\\FastLook-" + sid + L"-v1";
    }
}
