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
    //
    // The file may be truncated by another process while it is mapped (shares read/write/delete), which turns
    // reads of the mapping into an in-page error. BuildIndex, LineText and ExtractRange are guarded: on such a
    // fault they return empty/stop and Faulted() becomes true. Content() and Index() hand out raw access and are
    // NOT guarded; prefer the guarded accessors.
    class TextDocument
    {
    public:
        // nullptr on failure (missing, locked, access denied). Shares read/write/delete; a 0-byte file is an empty document.
        static std::unique_ptr<TextDocument> Open(std::filesystem::path const& path);
        // Non-owning view over caller memory that must outlive the document (tests, in-memory text).
        static std::unique_ptr<TextDocument> OpenView(std::span<const uint8_t> bytes);
        ~TextDocument();
        TextDocument(TextDocument const&) = delete;
        TextDocument& operator=(TextDocument const&) = delete;

        EncodingInfo const& Encoding() const { return encoding_; }
        uint64_t SizeBytes() const { return size_; }
        std::span<const uint8_t> Content() const { return content_; } // after the BOM; raw, unguarded

        // Indexes in 4 MiB chunks (a 64 KiB first one); stops early (resumable) on stop request or fault. `progress` runs after every
        // chunk. Single caller only: never run BuildIndex concurrently with itself.
        void BuildIndex(std::stop_token stop, std::function<void(uint64_t lines, bool complete)> progress);
        uint64_t LineCount() const { return index_.LineCount(); }
        bool IndexComplete() const { return index_.Complete(); }
        // The backing file became unreadable (see class comment). Sticky.
        bool Faulted() const { return faulted_; }

        std::wstring LineText(uint64_t line) const;                // decoded, no EOL
        // Same, and reports whether the row is a continuation segment of a longer line. False when the file faulted.
        bool LineText(uint64_t line, std::wstring& text, bool& continuation) const { return ReadLine(line, text, continuation); }
        // Number of the logical line (continuation segments share their line's number) that row `row` belongs to. False when
        // the file faulted.
        bool LogicalLine(uint64_t row, uint64_t& logical) const;
        std::wstring ExtractRange(TextPos from, TextPos to) const; // lines joined with L"\r\n", continuation segments joined directly
        // ExtractRange in row batches, stopping once `maxUnits` UTF-16 units are collected (`truncated` tells whether text was cut
        // off). Every batch is sized so the transient memory stays near the remaining budget, not near the batch limit.
        // Empty when the file faulted.
        std::wstring ExtractRangeCapped(TextPos from, TextPos to, size_t maxUnits, bool& truncated, uint64_t maxBatchRows = 2048) const;
        LineIndex const& Index() const { return index_; }          // Line()/LineFromOffset() on it scan Content() unguarded

    private:
        TextDocument(void* mapping, const uint8_t* view, std::span<const uint8_t> raw, EncodingInfo encoding);
        static std::unique_ptr<TextDocument> Create(void* mapping, const uint8_t* view, std::span<const uint8_t> raw);

        // Decoded text of one line; false = faulted (or `line` out of range with text left empty).
        bool ReadLine(uint64_t line, std::wstring& text, bool& continuation) const;

        void* mapping_;
        const uint8_t* view_; // owned mapping view, null for OpenView
        std::span<const uint8_t> raw_;
        uint64_t size_;
        EncodingInfo encoding_;
        std::span<const uint8_t> content_;
        LineIndex index_;
        std::atomic<uint64_t> indexed_{0}; // bytes of content_ already fed to the index
        mutable std::atomic<bool> faulted_{false};
    };
}
