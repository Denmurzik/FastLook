#include "Crypto/Sha256.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <fstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fastlook::core
{
    namespace
    {
        constexpr size_t kChunk = 1024 * 1024;

        // BCrypt SHA-256 state; every call fails once setup failed.
        class Hasher
        {
        public:
            Hasher()
            {
                if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                {
                    return;
                }
                if (BCryptCreateHash(m_alg, &m_hash, nullptr, 0, nullptr, 0, 0) < 0)
                {
                    return;
                }
                m_ok = true;
            }
            ~Hasher()
            {
                if (m_hash)
                {
                    BCryptDestroyHash(m_hash);
                }
                if (m_alg)
                {
                    BCryptCloseAlgorithmProvider(m_alg, 0);
                }
            }
            Hasher(Hasher const&) = delete;
            Hasher& operator=(Hasher const&) = delete;

            bool Update(std::span<const uint8_t> data)
            {
                return m_ok && BCryptHashData(m_hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) >= 0;
            }
            std::optional<Sha256Digest> Finish()
            {
                Sha256Digest digest{};
                if (!m_ok || BCryptFinishHash(m_hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
                {
                    return std::nullopt;
                }
                return digest;
            }

        private:
            BCRYPT_ALG_HANDLE m_alg = nullptr;
            BCRYPT_HASH_HANDLE m_hash = nullptr;
            bool m_ok = false;
        };

        int HexValue(char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        }
    }

    Sha256Digest Sha256(std::span<const uint8_t> data)
    {
        Hasher hasher;
        // BCrypt takes ULONG sizes: feed big buffers in chunks.
        for (size_t at = 0; at < data.size(); at += kChunk)
        {
            hasher.Update(data.subspan(at, std::min(kChunk, data.size() - at)));
        }
        return hasher.Finish().value_or(Sha256Digest{});
    }

    std::optional<Sha256Digest> Sha256File(std::filesystem::path const& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return std::nullopt;
        }
        Hasher hasher;
        std::vector<uint8_t> buffer(kChunk);
        while (in)
        {
            in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            auto const got = static_cast<size_t>(in.gcount());
            if (got > 0 && !hasher.Update(std::span<const uint8_t>(buffer.data(), got)))
            {
                return std::nullopt;
            }
        }
        if (!in.eof())
        {
            return std::nullopt;
        }
        return hasher.Finish();
    }

    std::optional<Sha256Digest> ParseSha256Line(std::string_view text, std::string_view expectedFileName)
    {
        auto const line = text.substr(0, text.find_first_of("\r\n"));
        if (line.size() < 64)
        {
            return std::nullopt;
        }
        Sha256Digest digest{};
        for (size_t i = 0; i < 32; ++i)
        {
            int const hi = HexValue(line[2 * i]);
            int const lo = HexValue(line[2 * i + 1]);
            if (hi < 0 || lo < 0)
            {
                return std::nullopt;
            }
            digest[i] = static_cast<uint8_t>(hi * 16 + lo);
        }
        auto rest = line.substr(64);
        while (!rest.empty() && rest.back() == ' ')
        {
            rest.remove_suffix(1);
        }
        if (rest.empty())
        {
            return digest;
        }
        if (rest.front() != ' ')
        {
            return std::nullopt;
        }
        rest.remove_prefix(1);
        if (!rest.empty() && (rest.front() == ' ' || rest.front() == '*'))
        {
            rest.remove_prefix(1);
        }
        if (rest != expectedFileName)
        {
            return std::nullopt;
        }
        return digest;
    }

    std::string ToHex(Sha256Digest const& digest)
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (auto b : digest)
        {
            out.push_back(kDigits[b >> 4]);
            out.push_back(kDigits[b & 15]);
        }
        return out;
    }
}
