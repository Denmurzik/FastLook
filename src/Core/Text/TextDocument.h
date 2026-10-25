#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <string>

#include "Text/Encoding.h"
#include "Text/LineIndex.h"

namespace fastlook::core
{
    struct TextPos
    {
        uint64_t line = 0;
        uint32_t column = 0; // UTF-16 units
        auto operator<=>(TextPos const&) const = default;
    };

    // Read-only memory-mapped text file with a lazily built sparse line index.
    class TextDocument
    {
    public:
        // nullptr on failure (missing, locked, access denied). Shares read/write/delete; a 0-byte file is an empty document.
        static std::unique_ptr<TextDocument> Open(std::filesystem::path const& path);
        ~TextDocument();
        TextDocument(TextDocument const&) = delete;
        TextDocument& operator=(TextDocument const&) = delete;

        EncodingInfo const& Encoding() const { return encoding_; }
        uint64_t SizeBytes() const { return size_; }
        std::span<const uint8_t> Content() const { return content_; } // after the BOM

        // Indexes in 4 MiB chunks; stops early (resumable) on stop request. `progress` runs after every chunk.
        void BuildIndex(std::stop_token stop, std::function<void(uint64_t lines, bool complete)> progress);
        uint64_t LineCount() const { return index_.LineCount(); }
        bool IndexComplete() const { return index_.Complete(); }

        std::wstring LineText(uint64_t line) const;            // decoded, no EOL
        std::wstring ExtractRange(TextPos from, TextPos to) const; // lines joined with L"\r\n", continuation segments joined directly
        LineIndex const& Index() const { return index_; }

    private:
        TextDocument(void* mapping, const uint8_t* view, uint64_t size, EncodingInfo encoding);

        void* mapping_;
        const uint8_t* view_;
        uint64_t size_;
        EncodingInfo encoding_;
        std::span<const uint8_t> content_;
        LineIndex index_;
        std::atomic<uint64_t> indexed_{0}; // bytes of content_ already fed to the index
    };
}
