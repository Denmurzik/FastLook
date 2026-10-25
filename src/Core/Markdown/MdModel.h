#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastlook::core::md
{
    enum class SpanFlag : uint8_t
    {
        None = 0,
        Bold = 1,
        Italic = 2,
        Code = 4,
        Strike = 8,
        Link = 16,
        Image = 32,
        RawHtml = 64 // inline HTML kept as literal text; render monospace, never interpret
    };

    struct Span
    {
        std::wstring text;
        uint8_t flags = 0;
        // Link span: href is the link target. Image span: href is the image src and text the alt text.
        std::wstring href;
        // Image nested in a link ([![alt](src)](target)): flags are Image|Link, href is src, linkHref is the target.
        // Empty for every other span.
        std::wstring linkHref;
        bool operator==(Span const&) const = default;
    };

    enum class BlockType
    {
        Heading,
        Paragraph,
        List,
        ListItem,
        Quote,
        Table,
        TableRow,
        TableCell,
        Code,
        Html,
        HRule
    };

    enum class Align
    {
        Default,
        Left,
        Center,
        Right
    };

    // ListItem shapes: a tight item holds its text directly in `spans` (nested lists/blocks in `children`, after the
    // spans); a loose item (blank lines between items) has no spans and holds Paragraph blocks in `children`.
    // Consumers must handle both.
    struct Block
    {
        BlockType type = BlockType::Paragraph;
        int level = 0; // heading 1..6
        bool ordered = false;
        unsigned start = 1;
        std::optional<bool> checked; // task list item
        bool header = false;         // table header cell/row
        Align align = Align::Default;
        std::wstring lang;
        std::wstring text; // code/html literal
        std::vector<Span> spans;
        std::vector<Block> children;
    };

    struct Document
    {
        std::vector<Block> blocks;
    };

    // Returns an empty Document on input over 4 GiB or if md4c fails.
    // GitHub dialect (tables, strikethrough, task lists, permissive autolinks). Soft breaks become a space,
    // hard breaks L"\n"; entities are decoded. Nesting deeper than 64 blocks is flattened into the 64th.
    Document Parse(std::string_view utf8);
}
