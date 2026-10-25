#include "Ipc/Framing.h"

#include <cstring>

#include "Ipc/Protocol.h"

namespace fastlook::core::ipc
{
    namespace
    {
        constexpr size_t kHeaderBytes = 4;

        uint32_t ReadLength(const char* p)
        {
            auto u = [&](int i) { return static_cast<uint32_t>(static_cast<unsigned char>(p[i])); };
            return u(0) | (u(1) << 8) | (u(2) << 16) | (u(3) << 24);
        }
    }

    std::string Frame(std::string_view payload)
    {
        auto n = static_cast<uint32_t>(payload.size());
        std::string out;
        out.reserve(kHeaderBytes + payload.size());
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
        }
        out.append(payload);
        return out;
    }

    void FrameReader::Feed(std::span<const char> bytes)
    {
        if (m_failed)
        {
            return;
        }
        m_buf.append(bytes.data(), bytes.size());
        CheckHeader();
    }

    std::optional<std::string> FrameReader::Next()
    {
        if (m_failed || m_buf.size() - m_pos < kHeaderBytes)
        {
            return std::nullopt;
        }
        uint32_t len = ReadLength(m_buf.data() + m_pos);
        if (m_buf.size() - m_pos - kHeaderBytes < len)
        {
            return std::nullopt;
        }
        std::string payload = m_buf.substr(m_pos + kHeaderBytes, len);
        m_pos += kHeaderBytes + len;
        if (m_pos == m_buf.size())
        {
            m_buf.clear();
            m_pos = 0;
        }
        else if (m_pos >= (1u << 16) && m_pos * 2 >= m_buf.size())
        {
            m_buf.erase(0, m_pos);
            m_pos = 0;
        }
        CheckHeader();
        return payload;
    }

    void FrameReader::CheckHeader()
    {
        if (m_buf.size() - m_pos >= kHeaderBytes && ReadLength(m_buf.data() + m_pos) > kMaxFrameBytes)
        {
            m_failed = true;
        }
    }
}
