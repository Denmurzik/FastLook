#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace fastlook::core
{
    enum class PreviewerKind { Image, Text, Markdown, Media, Pdf, Archive, Folder, Shell, Fallback };
    enum class PerceivedKind { Unknown, Image, Text, Audio, Video, Compressed, Document, Other };

    struct ClassifyInput
    {
        std::wstring_view fileName;
        bool isDirectory = false;
        bool isVirtual = false;
        bool isCloudPlaceholder = false;
        PerceivedKind perceived = PerceivedKind::Unknown;
        std::span<const uint8_t> head; // at most 4096 bytes
        bool hasWicDecoder = false;
        bool hasShellHandler = false; // non-blocked
        bool useShellHandlers = true;
    };

    // Pure classification, spec 5.2 order.
    PreviewerKind ClassifyPreview(ClassifyInput const& in);
}
