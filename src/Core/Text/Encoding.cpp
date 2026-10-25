#include "Text/Encoding.h"

#include <Windows.h>

#include <algorithm>
#include <limits>

#include "Text/Utf.h"

namespace fastlook::core
{
    namespace
    {
        bool StartsWith(std::span<const uint8_t> s, std::initializer_list<uint8_t> prefix)
        {
            return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
        }

        bool IsValidUtf8(std::span<const uint8_t> s, bool allowTruncatedTail)
        {
            size_t i = 0;
            while (i < s.size())
            {
                uint8_t const lead = s[i];
                if (lead < 0x80)
                {
                    ++i;
                    continue;
                }

                size_t len = 0;
                uint8_t lo = 0x80, hi = 0xBF; // allowed range of the first continuation byte
                if (lead >= 0xC2 && lead <= 0xDF)
                {
                    len = 2;
                }
                else if (lead >= 0xE0 && lead <= 0xEF)
                {
                    len = 3;
                    if (lead == 0xE0) lo = 0xA0;      // overlong
                    else if (lead == 0xED) hi = 0x9F; // surrogates
                }
                else if (lead >= 0xF0 && lead <= 0xF4)
                {
                    len = 4;
                    if (lead == 0xF0) lo = 0x90;      // overlong
                    else if (lead == 0xF4) hi = 0x8F; // > U+10FFFF
                }
                else
                {
                    return false; // C0, C1, F5..FF and stray continuation bytes
                }

                for (size_t k = 1; k < len; ++k)
                {
                    if (i + k >= s.size())
                    {
                        return allowTruncatedTail; // cut by the end of the sample; every byte so far was valid
                    }
                    uint8_t const b = s[i + k];
                    if (b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xBF))
                    {
                        return false;
                    }
                }
                i += len;
            }
            return true;
        }

        std::wstring DecodeUtf16(std::span<const uint8_t> bytes, bool bigEndian)
        {
            std::wstring out;
            out.resize(bytes.size() / 2);
            for (size_t i = 0; i < out.size(); ++i)
            {
                uint8_t const a = bytes[2 * i], b = bytes[2 * i + 1];
                out[i] = static_cast<wchar_t>(bigEndian ? (a << 8 | b) : (b << 8 | a));
            }
            if (bytes.size() % 2 != 0)
            {
                out.push_back(L'\uFFFD'); // dangling byte
            }
            return out;
        }

        std::wstring DecodeAnsi(std::span<const uint8_t> bytes, uint32_t codePage)
        {
            std::wstring out;
            size_t offset = 0;
            constexpr size_t kMaxPiece = static_cast<size_t>((std::numeric_limits<int>::max)());
            while (offset < bytes.size())
            {
                int const piece = static_cast<int>((std::min)(bytes.size() - offset, kMaxPiece));
                auto const* src = reinterpret_cast<const char*>(bytes.data() + offset);
                int const n = MultiByteToWideChar(codePage, 0, src, piece, nullptr, 0);
                if (n <= 0)
                {
                    out.append(static_cast<size_t>(piece), L'\uFFFD');
                }
                else
                {
                    size_t const at = out.size();
                    out.resize(at + static_cast<size_t>(n));
                    MultiByteToWideChar(codePage, 0, src, piece, out.data() + at, n);
                }
                offset += static_cast<size_t>(piece);
            }
            return out;
        }
    }

    EncodingInfo DetectEncoding(std::span<const uint8_t> s, uint32_t ansiCp, bool sampleIsPrefix)
    {
        if (StartsWith(s, {0xFF, 0xFE, 0x00, 0x00}) || StartsWith(s, {0x00, 0x00, 0xFE, 0xFF}))
            return {TextEncoding::Unsupported, 0, 4};
        if (StartsWith(s, {0xEF, 0xBB, 0xBF})) return {TextEncoding::Utf8, 65001, 3};
        if (StartsWith(s, {0xFF, 0xFE})) return {TextEncoding::Utf16LE, 1200, 2};
        if (StartsWith(s, {0xFE, 0xFF})) return {TextEncoding::Utf16BE, 1201, 2};
        if (IsValidUtf8(s, sampleIsPrefix)) return {TextEncoding::Utf8, 65001, 0};
        return {TextEncoding::Ansi, ansiCp, 0};
    }

    std::wstring DecodeToUtf16(std::span<const uint8_t> bytes, EncodingInfo const& info)
    {
        switch (info.encoding)
        {
        case TextEncoding::Utf8:
            return FromUtf8({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        case TextEncoding::Utf16LE:
            return DecodeUtf16(bytes, false);
        case TextEncoding::Utf16BE:
            return DecodeUtf16(bytes, true);
        case TextEncoding::Ansi:
            return DecodeAnsi(bytes, info.codePage);
        case TextEncoding::Unsupported:
            break;
        }
        return {};
    }
}
