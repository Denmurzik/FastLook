#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

#include "Text/Keywords.h"
#include "Text/TokenCategory.h"

namespace fastlook::core
{
    // First match of: "comment", "literal string"/"string", "literal numeric"/"number", "keyword", "preprocessor", "operator",
    // "identifier", "error" among the space-separated Lexilla style tags; Default when none applies.
    TokenCategory CategoryFromTags(std::string_view tags);

    struct StyleRun
    {
        uint32_t start = 0, length = 0; // byte offsets
        TokenCategory category = TokenCategory::Default;
    };

    // Non-Default runs of the whole buffer, ascending and non-overlapping. Empty when the lexer does not exist.
    std::vector<StyleRun> HighlightBuffer(std::string_view bytes, std::string_view lexerName, uint32_t codePage = 65001);

    // Lexes a buffer front to back in steps, keeping the lexer's per-line state between them. `bytes` (at most 20 MiB) must
    // outlive the object. LexThrough runs on one thread at a time; LexedThrough and CategoryAt may be read from another thread.
    class IncrementalLexer
    {
    public:
        IncrementalLexer(std::span<const uint8_t> bytes, std::string_view lexerName, uint32_t codePage);
        ~IncrementalLexer();
        IncrementalLexer(IncrementalLexer const&) = delete;
        IncrementalLexer& operator=(IncrementalLexer const&) = delete;

        bool Valid() const; // the lexer exists
        // Lexes [LexedThrough(), byteEnd) rounded up to a line end, in steps; stops between steps on a stop request.
        void LexThrough(uint64_t byteEnd, std::stop_token stop);
        uint64_t LexedThrough() const;
        // Default for bytes not lexed yet.
        TokenCategory CategoryAt(uint64_t byte) const;
        // Start of the line containing `byte` and the end of that line including its EOL (clamped to the buffer).
        uint64_t LineEndAfter(uint64_t byte) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_;
    };
}
