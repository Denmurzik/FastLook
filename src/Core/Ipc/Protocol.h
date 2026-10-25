#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Geometry.h"

namespace fastlook::core::ipc
{
    inline constexpr uint32_t kProtocolVersion = 1;
    inline constexpr uint32_t kMaxFrameBytes = 32u << 20;

    enum class NavMode
    {
        Selection,
        Folder
    };

    struct Item
    {
        std::wstring path;
        bool isVirtual = false;
        bool operator==(Item const&) const = default;
    };

    struct ShowPayload
    {
        uint64_t explorerHwnd = 0;
        bool isDesktop = false;
        uint64_t monitor = 0;
        std::vector<Item> items;
        uint32_t index = 0;
        bool listComplete = true;
        NavMode navMode = NavMode::Selection;
        std::optional<RectI> iconRect;
        bool operator==(ShowPayload const&) const = default;
    };

    struct ItemsPayload
    {
        std::vector<Item> items;
        uint32_t index = 0;
        NavMode navMode = NavMode::Folder;
        bool operator==(ItemsPayload const&) const = default;
    };

    enum class MsgType
    {
        Show,
        Toggle,
        Items,
        Ping,
        Pong,
        SettingsChanged,
        OpenSettings,
        Shutdown,
        Ack,
        Event
    };

    struct Message
    {
        MsgType type = MsgType::Ping;
        uint32_t id = 0;
        uint32_t replyTo = 0;
        ShowPayload show;      // Show, Toggle
        ItemsPayload items;    // Items
        std::string page;      // OpenSettings
        std::string eventName; // Event
        bool shown = false;    // Ack
        bool operator==(Message const&) const = default;
    };

    std::string Serialize(Message const&);
    // nullopt: malformed JSON, missing/unknown "type", "v" missing or != kProtocolVersion, wrongly typed field.
    std::optional<Message> Parse(std::string_view json);

    // L"\\\\.\\pipe\\FastLook-<user SID string>-v1"
    std::wstring PipeNameForCurrentUser();
}
