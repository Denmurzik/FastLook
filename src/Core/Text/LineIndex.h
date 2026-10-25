#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "Text/Encoding.h"

namespace fastlook::core
{
    // Sparse line index: only every kCheckpointEvery-th line start is stored, Line(n) scans forward from the
    // nearest checkpoint. CR, LF and CRLF end a line; a run of kMaxLineUnits without EOL ends a segment and the
    // next segment is flagged `continuation`.
    // One writer (Append); readers are safe concurrently with it.
    class LineIndex
    {
    public:
        static constexpr uint32_t kCheckpointEvery = 64, kMaxLineUnits = 16384;

        explicit LineIndex(EncodingInfo info);
        ~LineIndex();
        LineIndex(LineIndex const&) = delete;
        LineIndex& operator=(LineIndex const&) = delete;

        // Chunks are consecutive bytes after the BOM. Calls after the last chunk are ignored.
        void Append(std::span<const uint8_t> chunk, bool isLast);

        // Lines seen so far, counting the one still open; never decreases.
        uint64_t LineCount() const;
        bool Complete() const;

        struct LineSpan
        {
            uint64_t begin = 0, end = 0; // byte offsets after BOM, EOL excluded
            bool continuation = false;
        };

        // `content` = the whole text after the BOM (at least up to the bytes appended so far).
        LineSpan Line(uint64_t n, std::span<const uint8_t> content) const;
        uint64_t LineFromOffset(uint64_t byteOffset, std::span<const uint8_t> content) const;

        // Row `n` -> number of the logical line it belongs to: continuation rows (segments of a line longer than
        // kMaxLineUnits) share the number of the row that started them. Scans like Line(); rows past the end map
        // to the last logical line.
        uint64_t LogicalLine(uint64_t n, std::span<const uint8_t> content) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_;
    };
}
