#include "Imaging/Apng.h"

#include <array>
#include <cstring>

namespace fastlook::core
{
    namespace
    {
        constexpr uint8_t kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        constexpr uint32_t kMaxFrames = 100000;

        uint32_t ReadU32(uint8_t const* p)
        {
            return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
        }

        uint16_t ReadU16(uint8_t const* p)
        {
            return static_cast<uint16_t>((p[0] << 8) | p[1]);
        }

        void WriteU32(uint8_t* out, uint32_t v)
        {
            out[0] = static_cast<uint8_t>(v >> 24);
            out[1] = static_cast<uint8_t>(v >> 16);
            out[2] = static_cast<uint8_t>(v >> 8);
            out[3] = static_cast<uint8_t>(v);
        }

        void AppendU32(std::vector<uint8_t>& out, uint32_t v)
        {
            uint8_t bytes[4];
            WriteU32(bytes, v);
            out.insert(out.end(), bytes, bytes + 4);
        }

        uint32_t Crc32(uint8_t const* data, size_t size)
        {
            static auto const table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t n = 0; n < 256; ++n)
                {
                    uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                    {
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    }
                    t[n] = c;
                }
                return t;
            }();
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < size; ++i)
            {
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            }
            return ~crc;
        }

        void AppendChunk(std::vector<uint8_t>& out, char const* type, uint8_t const* data, size_t size)
        {
            AppendU32(out, static_cast<uint32_t>(size));
            size_t const start = out.size();
            out.insert(out.end(), type, type + 4);
            if (size > 0)
            {
                out.insert(out.end(), data, data + size);
            }
            AppendU32(out, Crc32(out.data() + start, out.size() - start));
        }

        struct Chunk
        {
            char type[5]{};
            uint8_t const* data = nullptr;
            uint32_t size = 0;
        };

        bool Is(Chunk const& c, char const* type)
        {
            return std::memcmp(c.type, type, 4) == 0;
        }

        struct PendingFrame
        {
            FrameInfo info;
            std::vector<uint8_t> idat; // concatenated zlib data
            bool open = false;
        };
    }

    std::optional<ApngFile> ParseApng(std::span<const uint8_t> file)
    {
        if (file.size() < 8 + 12 || std::memcmp(file.data(), kSignature, 8) != 0)
        {
            return std::nullopt;
        }

        ApngFile result;
        std::array<uint8_t, 13> ihdr{};
        std::vector<uint8_t> shared; // PLTE, tRNS and other ancillary chunks that precede the image data
        bool haveIhdr = false, haveActl = false, sawIdat = false, ended = false;
        PendingFrame current;

        auto finish = [&] {
            if (current.open && !current.idat.empty())
            {
                ApngFrame frame;
                frame.info = current.info;
                frame.png.insert(frame.png.end(), kSignature, kSignature + 8);
                std::array<uint8_t, 13> header = ihdr;
                WriteU32(header.data(), static_cast<uint32_t>(frame.info.rect.w));
                WriteU32(header.data() + 4, static_cast<uint32_t>(frame.info.rect.h));
                AppendChunk(frame.png, "IHDR", header.data(), header.size());
                frame.png.insert(frame.png.end(), shared.begin(), shared.end());
                AppendChunk(frame.png, "IDAT", current.idat.data(), current.idat.size());
                AppendChunk(frame.png, "IEND", nullptr, 0);
                result.frames.push_back(std::move(frame));
            }
            current = {};
        };

        size_t pos = 8;
        uint32_t sequence = 0;
        while (pos + 12 <= file.size() && !ended)
        {
            Chunk chunk;
            chunk.size = ReadU32(file.data() + pos);
            if (chunk.size > file.size() - pos - 12)
            {
                return std::nullopt; // truncated
            }
            std::memcpy(chunk.type, file.data() + pos + 4, 4);
            chunk.data = file.data() + pos + 8;
            pos += 12 + static_cast<size_t>(chunk.size);

            if (Is(chunk, "IHDR"))
            {
                if (chunk.size != 13)
                {
                    return std::nullopt;
                }
                std::memcpy(ihdr.data(), chunk.data, 13);
                result.canvas = {ReadU32(chunk.data), ReadU32(chunk.data + 4)};
                haveIhdr = result.canvas.w > 0 && result.canvas.h > 0;
            }
            else if (Is(chunk, "acTL"))
            {
                if (chunk.size != 8 || sawIdat)
                {
                    return std::nullopt;
                }
                uint32_t const declared = ReadU32(chunk.data);
                result.loops = ReadU32(chunk.data + 4);
                haveActl = declared > 0 && declared <= kMaxFrames;
            }
            else if (Is(chunk, "fcTL"))
            {
                if (!haveActl || !haveIhdr || chunk.size != 26 || ReadU32(chunk.data) != sequence++ || result.frames.size() >= kMaxFrames)
                {
                    return std::nullopt;
                }
                finish();
                uint32_t const w = ReadU32(chunk.data + 4), h = ReadU32(chunk.data + 8);
                uint32_t const x = ReadU32(chunk.data + 12), y = ReadU32(chunk.data + 16);
                if (w == 0 || h == 0 || uint64_t{x} + w > result.canvas.w || uint64_t{y} + h > result.canvas.h)
                {
                    return std::nullopt;
                }
                uint16_t const num = ReadU16(chunk.data + 20), den = ReadU16(chunk.data + 22);
                uint8_t const dispose = chunk.data[24], blend = chunk.data[25];
                if (dispose > 2 || blend > 1)
                {
                    return std::nullopt;
                }
                current.open = true;
                current.info.rect = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(w), static_cast<int32_t>(h)};
                current.info.delayMs = static_cast<uint32_t>(uint64_t{num} * 1000 / (den == 0 ? 100 : den));
                current.info.dispose = static_cast<Dispose>(dispose);
                current.info.blend = blend == 0 ? Blend::Source : Blend::Over;
            }
            else if (Is(chunk, "IDAT"))
            {
                sawIdat = true;
                if (current.open) // the default image doubles as the first frame only when an fcTL precedes it
                {
                    current.idat.insert(current.idat.end(), chunk.data, chunk.data + chunk.size);
                }
            }
            else if (Is(chunk, "fdAT"))
            {
                if (!current.open || chunk.size < 4 || ReadU32(chunk.data) != sequence++)
                {
                    return std::nullopt;
                }
                current.idat.insert(current.idat.end(), chunk.data + 4, chunk.data + chunk.size);
            }
            else if (Is(chunk, "IEND"))
            {
                ended = true;
            }
            else if (!sawIdat)
            {
                AppendChunk(shared, chunk.type, chunk.data, chunk.size); // PLTE, tRNS, gAMA, sRGB, ...
            }
        }
        finish();

        if (!haveIhdr || !haveActl || result.frames.empty())
        {
            return std::nullopt;
        }
        return result;
    }
}
