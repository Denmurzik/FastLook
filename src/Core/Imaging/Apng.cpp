#include "Imaging/Apng.h"

#include <array>
#include <cstring>

#include "Imaging/DecodeMath.h"

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

        // Chunks a frame needs to decode with the right colours; everything else before IDAT (text, EXIF, private data) is dropped.
        bool IsSharedColourChunk(Chunk const& c)
        {
            return Is(c, "PLTE") || Is(c, "tRNS") || Is(c, "gAMA") || Is(c, "cHRM") || Is(c, "sRGB") || Is(c, "sBIT") || Is(c, "iCCP");
        }

        struct PendingFrame
        {
            FrameInfo info;
            std::vector<uint8_t> idat; // concatenated zlib data
            bool open = false;
        };

        bool HasSignature(std::span<const uint8_t> file)
        {
            return file.size() >= 8 + 12 && std::memcmp(file.data(), kSignature, 8) == 0;
        }

        // Walks the chunks after the signature until `visit` returns false. False when a chunk is truncated.
        template <typename Visit>
        bool ForEachChunk(std::span<const uint8_t> file, Visit&& visit)
        {
            size_t pos = 8;
            while (pos + 12 <= file.size())
            {
                Chunk chunk;
                chunk.size = ReadU32(file.data() + pos);
                if (chunk.size > file.size() - pos - 12)
                {
                    return false;
                }
                std::memcpy(chunk.type, file.data() + pos + 4, 4);
                chunk.data = file.data() + pos + 8;
                pos += 12 + static_cast<size_t>(chunk.size);
                if (!visit(chunk))
                {
                    break;
                }
            }
            return true;
        }

        // The canvas of an IHDR, when it may be animated: checked before any frame work.
        std::optional<SizeU> AnimatableCanvas(Chunk const& ihdr)
        {
            if (ihdr.size != 13)
            {
                return std::nullopt;
            }
            SizeU const canvas{ReadU32(ihdr.data), ReadU32(ihdr.data + 4)};
            return CanAnimateCanvas(canvas) ? std::optional{canvas} : std::nullopt;
        }
    }

    std::optional<ApngFile> ParseApng(std::span<const uint8_t> file)
    {
        if (!HasSignature(file))
        {
            return std::nullopt;
        }

        ApngFile result;
        bool haveIhdr = false, haveActl = false, sawIdat = false, invalid = false;
        PendingFrame current;

        auto finish = [&] {
            if (current.open && !current.idat.empty())
            {
                result.frames.push_back({current.info, std::move(current.idat)});
            }
            current = {};
        };
        auto reject = [&] {
            invalid = true;
            return false;
        };

        uint32_t sequence = 0;
        bool const complete = ForEachChunk(file, [&](Chunk const& chunk) {
            if (Is(chunk, "IHDR"))
            {
                auto const canvas = AnimatableCanvas(chunk);
                if (!canvas)
                {
                    return reject(); // the still path shows the first frame; no frame data is collected for it
                }
                std::memcpy(result.ihdr.data(), chunk.data, 13);
                result.canvas = *canvas;
                haveIhdr = true;
            }
            else if (Is(chunk, "acTL"))
            {
                if (chunk.size != 8 || sawIdat)
                {
                    return reject();
                }
                uint32_t const declared = ReadU32(chunk.data);
                result.loops = ReadU32(chunk.data + 4);
                haveActl = declared > 0 && declared <= kMaxFrames;
            }
            else if (Is(chunk, "fcTL"))
            {
                if (!haveActl || !haveIhdr || chunk.size != 26 || ReadU32(chunk.data) != sequence++ || result.frames.size() >= kMaxFrames)
                {
                    return reject();
                }
                finish();
                uint32_t const w = ReadU32(chunk.data + 4), h = ReadU32(chunk.data + 8);
                uint32_t const x = ReadU32(chunk.data + 12), y = ReadU32(chunk.data + 16);
                if (w == 0 || h == 0 || uint64_t{x} + w > result.canvas.w || uint64_t{y} + h > result.canvas.h)
                {
                    return reject();
                }
                uint16_t const num = ReadU16(chunk.data + 20), den = ReadU16(chunk.data + 22);
                uint8_t const dispose = chunk.data[24], blend = chunk.data[25];
                if (dispose > 2 || blend > 1)
                {
                    return reject();
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
                    return reject();
                }
                current.idat.insert(current.idat.end(), chunk.data + 4, chunk.data + chunk.size);
            }
            else if (Is(chunk, "IEND"))
            {
                return false;
            }
            else if (!sawIdat && IsSharedColourChunk(chunk))
            {
                if (result.shared.size() + 12 + chunk.size > kMaxApngSharedBytes)
                {
                    return reject();
                }
                AppendChunk(result.shared, chunk.type, chunk.data, chunk.size);
            }
            return true;
        });
        if (!complete || invalid)
        {
            return std::nullopt; // truncated or invalid
        }
        finish();

        if (!haveIhdr || !haveActl || result.frames.empty())
        {
            return std::nullopt;
        }
        return result;
    }

    std::vector<uint8_t> BuildApngFramePng(ApngFile const& file, size_t index)
    {
        if (index >= file.frames.size())
        {
            return {};
        }
        auto const& frame = file.frames[index];
        std::vector<uint8_t> png;
        png.reserve(8 + 25 + file.shared.size() + 12 + frame.idat.size() + 12);
        png.insert(png.end(), kSignature, kSignature + 8);
        std::array<uint8_t, 13> header = file.ihdr;
        WriteU32(header.data(), static_cast<uint32_t>(frame.info.rect.w));
        WriteU32(header.data() + 4, static_cast<uint32_t>(frame.info.rect.h));
        AppendChunk(png, "IHDR", header.data(), header.size());
        png.insert(png.end(), file.shared.begin(), file.shared.end());
        AppendChunk(png, "IDAT", frame.idat.data(), frame.idat.size());
        AppendChunk(png, "IEND", nullptr, 0);
        return png;
    }

    bool DeclaresApngAnimation(std::span<const uint8_t> file)
    {
        if (!HasSignature(file))
        {
            return false;
        }
        bool animatable = false, declared = false;
        ForEachChunk(file, [&](Chunk const& chunk) {
            if (Is(chunk, "IHDR"))
            {
                animatable = AnimatableCanvas(chunk).has_value();
            }
            else if (Is(chunk, "acTL"))
            {
                declared = chunk.size == 8 && ReadU32(chunk.data) > 1;
            }
            return !Is(chunk, "IDAT") && !Is(chunk, "IEND");
        });
        return animatable && declared;
    }
}
