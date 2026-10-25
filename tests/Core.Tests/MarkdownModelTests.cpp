#include <gtest/gtest.h>

#include <algorithm>

#include "Markdown/MdModel.h"

using namespace fastlook::core::md;

namespace
{
    constexpr uint8_t F(SpanFlag f) { return static_cast<uint8_t>(f); }
}

TEST(MdModel, HeadingsAndInlineSpans)
{
    auto d = Parse("# Title\n\nHello **bold** _it_ `code` [link](https://x.y) ~~no~~\n");
    ASSERT_EQ(d.blocks.size(), 2u);
    EXPECT_EQ(d.blocks[0].type, BlockType::Heading);
    EXPECT_EQ(d.blocks[0].level, 1);
    ASSERT_EQ(d.blocks[0].spans.size(), 1u);
    EXPECT_EQ(d.blocks[0].spans[0].text, L"Title");
    auto& s = d.blocks[1].spans;
    ASSERT_GE(s.size(), 9u);
    EXPECT_EQ(s[0].text, L"Hello ");
    EXPECT_EQ(s[1], (Span{L"bold", F(SpanFlag::Bold)}));
    EXPECT_EQ(s[3], (Span{L"it", F(SpanFlag::Italic)}));
    EXPECT_EQ(s[5].text, L"code");
    EXPECT_EQ(s[5].flags, F(SpanFlag::Code));
    auto link = std::ranges::find_if(s, [](auto& x) { return x.flags & F(SpanFlag::Link); });
    ASSERT_NE(link, s.end());
    EXPECT_EQ(link->text, L"link");
    EXPECT_EQ(link->href, L"https://x.y");
    auto strike = std::ranges::find_if(s, [](auto& x) { return x.flags & F(SpanFlag::Strike); });
    ASSERT_NE(strike, s.end());
    EXPECT_EQ(strike->text, L"no");
}

TEST(MdModel, NestedSpanFlagsCombine)
{
    auto d = Parse("**a _b_ c**\n");
    auto& s = d.blocks[0].spans;
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0], (Span{L"a ", F(SpanFlag::Bold)}));
    EXPECT_EQ(s[1], (Span{L"b", static_cast<uint8_t>(F(SpanFlag::Bold) | F(SpanFlag::Italic))}));
    EXPECT_EQ(s[2], (Span{L" c", F(SpanFlag::Bold)}));
}

TEST(MdModel, SoftAndHardBreaksAndEntities)
{
    auto soft = Parse("a\nb\n");
    ASSERT_EQ(soft.blocks[0].spans.size(), 1u);
    EXPECT_EQ(soft.blocks[0].spans[0].text, L"a b");

    auto hard = Parse("a  \nb\n");
    ASSERT_EQ(hard.blocks[0].spans.size(), 1u);
    EXPECT_EQ(hard.blocks[0].spans[0].text, L"a\nb");

    auto ent = Parse("x &amp; y &lt; &#35; &#x44F; &copy;\n");
    ASSERT_EQ(ent.blocks[0].spans.size(), 1u);
    EXPECT_EQ(ent.blocks[0].spans[0].text, L"x & y < # \u044F \u00A9");
}

TEST(MdModel, NestedOrderedListAndTasks)
{
    auto d = Parse("3. a\n   - [x] b\n   - [ ] c\n");
    ASSERT_EQ(d.blocks.size(), 1u);
    auto& list = d.blocks[0];
    EXPECT_EQ(list.type, BlockType::List);
    EXPECT_TRUE(list.ordered);
    EXPECT_EQ(list.start, 3u);
    ASSERT_EQ(list.children.size(), 1u);
    auto& item = list.children[0];
    EXPECT_EQ(item.type, BlockType::ListItem);
    EXPECT_FALSE(item.checked.has_value());
    ASSERT_EQ(item.spans.size(), 1u);
    EXPECT_EQ(item.spans[0].text, L"a");
    ASSERT_EQ(item.children.size(), 1u);
    auto& inner = item.children[0];
    EXPECT_EQ(inner.type, BlockType::List);
    EXPECT_FALSE(inner.ordered);
    ASSERT_EQ(inner.children.size(), 2u);
    EXPECT_EQ(inner.children[0].type, BlockType::ListItem);
    ASSERT_TRUE(inner.children[0].checked.has_value());
    EXPECT_TRUE(*inner.children[0].checked);
    EXPECT_EQ(inner.children[0].spans[0].text, L"b");
    ASSERT_TRUE(inner.children[1].checked.has_value());
    EXPECT_FALSE(*inner.children[1].checked);
}

TEST(MdModel, QuoteHrAndCode)
{
    auto d = Parse("> q\n\n---\n\n```cpp\nint x;\n```\n");
    ASSERT_EQ(d.blocks.size(), 3u);
    EXPECT_EQ(d.blocks[0].type, BlockType::Quote);
    ASSERT_EQ(d.blocks[0].children.size(), 1u);
    EXPECT_EQ(d.blocks[0].children[0].type, BlockType::Paragraph);
    EXPECT_EQ(d.blocks[0].children[0].spans[0].text, L"q");
    EXPECT_EQ(d.blocks[1].type, BlockType::HRule);
    EXPECT_EQ(d.blocks[2].type, BlockType::Code);
    EXPECT_EQ(d.blocks[2].lang, L"cpp");
    EXPECT_EQ(d.blocks[2].text, L"int x;\n");
}

TEST(MdModel, TableWithAlignment)
{
    auto d = Parse("|a|b|\n|:-|-:|\n|1|2|\n");
    ASSERT_EQ(d.blocks.size(), 1u);
    auto& t = d.blocks[0];
    EXPECT_EQ(t.type, BlockType::Table);
    ASSERT_EQ(t.children.size(), 2u);
    auto& head = t.children[0];
    EXPECT_EQ(head.type, BlockType::TableRow);
    EXPECT_TRUE(head.header);
    ASSERT_EQ(head.children.size(), 2u);
    EXPECT_TRUE(head.children[0].header);
    EXPECT_EQ(head.children[0].type, BlockType::TableCell);
    EXPECT_EQ(head.children[0].align, Align::Left);
    EXPECT_EQ(head.children[1].align, Align::Right);
    EXPECT_EQ(head.children[0].spans[0].text, L"a");
    auto& row = t.children[1];
    EXPECT_FALSE(row.header);
    EXPECT_FALSE(row.children[0].header);
    EXPECT_EQ(row.children[0].align, Align::Left);
    EXPECT_EQ(row.children[1].align, Align::Right);
    EXPECT_EQ(row.children[1].spans[0].text, L"2");
}

TEST(MdModel, ImagesLocalAndRemote)
{
    auto d = Parse("![alt](pic.png) ![r](https://e/x.png)");
    auto& s = d.blocks[0].spans;
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0], (Span{L"alt", F(SpanFlag::Image), L"pic.png"}));
    EXPECT_EQ(s[1].text, L" ");
    EXPECT_EQ(s[2], (Span{L"r", F(SpanFlag::Image), L"https://e/x.png"}));
}

TEST(MdModel, HtmlBlockIsLiteral)
{
    auto d = Parse("<div>x</div>\n");
    ASSERT_EQ(d.blocks.size(), 1u);
    EXPECT_EQ(d.blocks[0].type, BlockType::Html);
    EXPECT_EQ(d.blocks[0].text, L"<div>x</div>\n");
}

TEST(MdModel, CyrillicText)
{
    auto d = Parse("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    ASSERT_EQ(d.blocks.size(), 1u);
    EXPECT_EQ(d.blocks[0].spans[0].text, L"\u041F\u0440\u0438\u0432\u0435\u0442");
}

TEST(MdModel, EmptyInputHasNoBlocks)
{
    EXPECT_TRUE(Parse("").blocks.empty());
}

TEST(MdModel, PathologicalNestingIsBounded)
{
    std::string deep(5000, '>');
    deep += " x\n";
    auto d = Parse(deep);
    ASSERT_EQ(d.blocks.size(), 1u);
    int depth = 0;
    for (const Block* b = &d.blocks[0]; !b->children.empty(); b = &b->children[0])
        ++depth;
    EXPECT_LE(depth, 64);
}
