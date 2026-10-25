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
        Image = 32
    };

    struct Span
    {
        std::wstring text;
        uint8_t flags = 0;
        std::wstring href; // link target or image src
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

    // GitHub dialect (tables, strikethrough, task lists, permissive autolinks). Soft breaks become a space,
    // hard breaks L"\n"; entities are decoded. Nesting deeper than 64 blocks is flattened into the 64th.
    Document Parse(std::string_view utf8);
}
