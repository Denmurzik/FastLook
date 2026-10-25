#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fastlook::core::ipc
{
    // 4-byte little-endian length + payload.
    std::string Frame(std::string_view payload);

    // Reassembles frames from an arbitrary byte stream. A length above kMaxFrameBytes is a permanent failure.
    class FrameReader
    {
    public:
        void Feed(std::span<const char> bytes);
        std::optional<std::string> Next();
        bool Failed() const { return m_failed; }

    private:
        void CheckHeader();

        std::string m_buf;
        size_t m_pos = 0;
        bool m_failed = false;
    };
}
