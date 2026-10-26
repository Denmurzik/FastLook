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

TEST(MdModel, ImageInsideLinkKeepsBothTargets)
{
    auto d = Parse("[![badge](img.svg)](https://target)\n");
    auto& s = d.blocks[0].spans;
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].flags, static_cast<uint8_t>(F(SpanFlag::Image) | F(SpanFlag::Link)));
    EXPECT_EQ(s[0].text, L"badge");
    EXPECT_EQ(s[0].href, L"img.svg");
    EXPECT_EQ(s[0].linkHref, L"https://target");
}

TEST(MdModel, LooseListItemsHoldParagraphs)
{
    auto d = Parse("- a\n\n- b\n");
    ASSERT_EQ(d.blocks.size(), 1u);
    auto& list = d.blocks[0];
    ASSERT_EQ(list.children.size(), 2u);
    for (auto& item : list.children)
    {
        EXPECT_EQ(item.type, BlockType::ListItem);
        EXPECT_TRUE(item.spans.empty());
        ASSERT_EQ(item.children.size(), 1u);
        EXPECT_EQ(item.children[0].type, BlockType::Paragraph);
    }
    EXPECT_EQ(list.children[1].children[0].spans[0].text, L"b");
}

TEST(MdModel, InlineRawHtmlIsFlaggedLiteral)
{
    auto d = Parse("a <b>x</b> c\n");
    auto& s = d.blocks[0].spans;
    auto raw = std::ranges::count_if(s, [](auto& x) { return x.flags & F(SpanFlag::RawHtml); });
    EXPECT_GE(raw, 1);
    std::wstring all;
    for (auto& x : s)
        all += x.text;
    EXPECT_EQ(all, L"a <b>x</b> c");
    EXPECT_EQ(s[0].flags, 0);
}

TEST(MdModel, UnknownEntityStaysLiteral)
{
    auto d = Parse("&notanentity; ok\n");
    EXPECT_EQ(d.blocks[0].spans[0].text, L"&notanentity; ok");
}

TEST(MdModel, InvalidUtf8BecomesReplacementCharacters)
{
    // A stray continuation byte, a truncated sequence, an overlong lead and a lone surrogate encoding; in text, code and a heading.
    auto d = Parse("a\x80z \xE2\x82 \xC0\xAF \xED\xA0\x80 end\n\n```\nc\xFFx\n```\n\n# h\xF8i\n");
    ASSERT_EQ(d.blocks.size(), 3u);
    std::wstring text;
    for (auto& s : d.blocks[0].spans)
        text += s.text;
    EXPECT_NE(text.find(L'\uFFFD'), std::wstring::npos);
    EXPECT_EQ(text.front(), L'a');
    EXPECT_NE(text.find(L"z "), std::wstring::npos);
    EXPECT_NE(text.find(L" end"), std::wstring::npos);
    EXPECT_NE(d.blocks[1].text.find(L'\uFFFD'), std::wstring::npos);
    EXPECT_NE(d.blocks[2].spans[0].text.find(L'\uFFFD'), std::wstring::npos);
}

TEST(MdModel, ShortenLiteralKeepsWholeLinesUpToTheCap)
{
    std::wstring const shortText = L"line 1\nline 2\n";
    auto const same = ShortenLiteral(shortText);
    EXPECT_EQ(same.text, shortText);
    EXPECT_FALSE(same.shortened);

    std::wstring lines;
    while (lines.size() <= kMaxLiteralChars)
    {
        lines += L"0123456789\n";
    }
    auto const cut = ShortenLiteral(lines);
    EXPECT_TRUE(cut.shortened);
    EXPECT_LE(cut.text.size(), kMaxLiteralChars);
    EXPECT_GT(cut.text.size(), kMaxLiteralChars - 22);
    EXPECT_EQ(cut.text.back(), L'\n');
    EXPECT_EQ(lines.compare(0, cut.text.size(), cut.text), 0);

    std::wstring fits;
    for (int i = 0; i < 100; ++i)
    {
        fits += L"0123456789\n";
    }
    auto const whole = ShortenLiteral(fits);
    EXPECT_EQ(whole.text, fits);
    EXPECT_FALSE(whole.shortened);
}

TEST(MdModel, ShortenLiteralCutsLongLines)
{
    std::wstring minified(kMaxLiteralLineChars - 1, L'x');
    minified += L"\U0001F600 and more\nnext line"; // a surrogate pair straddles the line cap
    auto const cut = ShortenLiteral(minified);
    EXPECT_TRUE(cut.shortened);
    EXPECT_EQ(cut.text, std::wstring(kMaxLiteralLineChars - 1, L'x') + L"\u2026\nnext line");

    auto const huge = ShortenLiteral(std::wstring(200000, L'y'));
    EXPECT_EQ(huge.text.size(), kMaxLiteralLineChars + 1);
}

TEST(MdModel, RenderWeightCountsCodeLinesAndHighlightRuns)
{
    // Final review B2#2: a fence is one block without spans, but renders a Run and a LineBreak per line.
    std::string log = "```\n";
    for (int i = 0; i < 500; ++i)
    {
        log += "2026-10-26 12:00:00 INFO something happened\n";
    }
    log += "```\n";
    auto const plain = EstimateRenderWeight(Parse(log));
    EXPECT_EQ(plain.blocks, 1u);
    EXPECT_GE(plain.inlines, 1000u);

    std::string tagged = "```json\n";
    for (int i = 0; i < 1000; ++i)
    {
        tagged += "{\"key\": [1, 2, 3], \"other\": true},\n";
    }
    tagged += "```\n";
    auto const highlighted = EstimateRenderWeight(Parse(tagged));
    EXPECT_GE(highlighted.inlines, 1800u + 30000u / 4); // shortened to kMaxLiteralChars first

    auto const html = EstimateRenderWeight(Parse("<div>\n" + std::string(3000, 'a') + "\n</div>\n"));
    EXPECT_GE(html.inlines, 4u);

    auto const prose = EstimateRenderWeight(Parse("# Title\n\nHello **bold** world\n"));
    EXPECT_EQ(prose.blocks, 2u);
    EXPECT_LE(prose.inlines, 5u);
}

TEST(MdModel, RenderWeightOfAHugeFenceCountsOnlyTheRenderedPart)
{
    std::string fence = "```\n" + std::string(200000, 'y') + "\n```\n"; // one minified line, cut to kMaxLiteralLineChars
    auto const weight = EstimateRenderWeight(Parse(fence));
    EXPECT_EQ(weight.inlines, 2u);
}
