#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace fastlook::core
{
    enum class TextEncoding
    {
        Utf8,
        Utf16LE,
        Utf16BE,
        Ansi,
        Unsupported
    };

    struct EncodingInfo
    {
        TextEncoding encoding = TextEncoding::Utf8;
        uint32_t codePage = 65001;
        uint32_t bomLength = 0;

        bool operator==(EncodingInfo const&) const = default;
    };

    // BOM first, then strict UTF-8 validation (ASCII included), otherwise ANSI with `ansiCodePage`.
    // `sampleIsPrefix`: the sample is only the start of a longer file, so a sequence cut at its end is accepted.
    EncodingInfo DetectEncoding(std::span<const uint8_t> sample, uint32_t ansiCodePage, bool sampleIsPrefix);

    // `bytes` carries no BOM. Invalid input decodes to U+FFFD; Unsupported decodes to an empty string.
    std::wstring DecodeToUtf16(std::span<const uint8_t> bytes, EncodingInfo const& info);
}
