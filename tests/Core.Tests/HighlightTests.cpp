#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Text/Highlight.h"
#include "Text/Utf.h"

using namespace fastlook::core;

namespace
{
    TokenCategory At(std::vector<StyleRun> const& runs, size_t byte)
    {
        for (auto const& run : runs)
        {
            if (byte >= run.start && byte < run.start + run.length) return run.category;
        }
        return TokenCategory::Default;
    }

    TokenCategory AtOf(std::string const& text, std::string_view lexer, std::string_view needle)
    {
        return At(HighlightBuffer(text, lexer), text.find(needle));
    }
}

TEST(Highlight, CategoryFromTags)
{
    EXPECT_EQ(CategoryFromTags("comment line"), TokenCategory::Comment);
    EXPECT_EQ(CategoryFromTags("literal string"), TokenCategory::String);
    EXPECT_EQ(CategoryFromTags("literal numeric"), TokenCategory::Number);
    EXPECT_EQ(CategoryFromTags("keyword"), TokenCategory::Keyword);
    EXPECT_EQ(CategoryFromTags("preprocessor"), TokenCategory::Preprocessor);
    EXPECT_EQ(CategoryFromTags("operator"), TokenCategory::Operator);
    EXPECT_EQ(CategoryFromTags("identifier"), TokenCategory::Identifier);
    EXPECT_EQ(CategoryFromTags("error"), TokenCategory::Error);
    EXPECT_EQ(CategoryFromTags("preprocessor comment"), TokenCategory::Comment);
    EXPECT_EQ(CategoryFromTags(""), TokenCategory::Default);
    EXPECT_EQ(CategoryFromTags("whitespace"), TokenCategory::Default);
}

TEST(Highlight, UnknownLexerGivesNoRuns)
{
    EXPECT_TRUE(HighlightBuffer("int x;", "no-such-lexer").empty());
}

TEST(Highlight, Cpp)
{
    std::string const s = "int main() { return 0; } // hi";
    auto const r = HighlightBuffer(s, "cpp");
    EXPECT_EQ(At(r, 0), TokenCategory::Keyword);
    EXPECT_EQ(At(r, s.find('0')), TokenCategory::Number);
    EXPECT_EQ(At(r, s.find("//")), TokenCategory::Comment);
    EXPECT_EQ(At(r, s.find("main")), TokenCategory::Identifier);
}

TEST(Highlight, CppPreprocessorAndString)
{
    std::string const s = "#include <a>\nconst char* p = \"x\";\n";
    auto const r = HighlightBuffer(s, "cpp");
    EXPECT_EQ(At(r, 0), TokenCategory::Preprocessor);
    EXPECT_EQ(At(r, s.find("\"x\"")), TokenCategory::String);
    EXPECT_EQ(At(r, s.find("const")), TokenCategory::Keyword);
}

TEST(Highlight, PythonStringAndComment)
{
    std::string const s = "x = 'a'  # c\ndef f(): pass\n";
    auto const r = HighlightBuffer(s, "python");
    EXPECT_EQ(At(r, s.find("'a'")), TokenCategory::String);
    EXPECT_EQ(At(r, s.find('#')), TokenCategory::Comment);
    EXPECT_EQ(At(r, s.find("def")), TokenCategory::Keyword);
}

TEST(Highlight, JsonAndPowershellAndSql)
{
    std::string const json = "{\"k\": [1, true, \"v\"]}";
    EXPECT_EQ(AtOf(json, "json", "\"v\""), TokenCategory::String);
    EXPECT_EQ(AtOf(json, "json", "1"), TokenCategory::Number);
    EXPECT_EQ(AtOf(json, "json", "true"), TokenCategory::Keyword);

    std::string const ps = "function F { 'a' } # c\n";
    EXPECT_EQ(AtOf(ps, "powershell", "function"), TokenCategory::Keyword);
    EXPECT_EQ(AtOf(ps, "powershell", "'a'"), TokenCategory::String);
    EXPECT_EQ(AtOf(ps, "powershell", "# c"), TokenCategory::Comment);

    std::string const sql = "SELECT 'a' FROM t -- c\n";
    EXPECT_EQ(AtOf(sql, "sql", "SELECT"), TokenCategory::Keyword);
    EXPECT_EQ(AtOf(sql, "sql", "'a'"), TokenCategory::String);
    EXPECT_EQ(AtOf(sql, "sql", "-- c"), TokenCategory::Comment);
}

TEST(Highlight, IncrementalMatchesWhole)
{
    std::string text;
    for (int i = 0; i < 2000; ++i)
    {
        switch (i % 10)
        {
        case 0: text += "/* block comment\n"; break;
        case 1: text += "   still inside\n"; break;
        case 2: text += "*/ int a" + std::to_string(i) + " = 42;\n"; break;
        case 3: text += "const char* s = \"text " + std::to_string(i) + "\";\n"; break;
        default: text += "for (int j = 0; j < " + std::to_string(i) + "; ++j) { /* x */ }\n"; break;
        }
    }
    auto const whole = HighlightBuffer(text, "cpp");
    std::span<const uint8_t> const bytes{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    IncrementalLexer lexer{bytes, "cpp", 65001};
    ASSERT_TRUE(lexer.Valid());

    // Steps that end inside a block comment: line 0 opens one, line 1 is its body.
    size_t const insideComment = text.find("still inside") + 3;
    std::vector<uint64_t> const steps{insideComment, text.size() / 4, text.size() / 2, text.size()};
    uint64_t previous = 0;
    for (uint64_t step : steps)
    {
        lexer.LexThrough(step, {});
        EXPECT_GE(lexer.LexedThrough(), step);
        EXPECT_GE(lexer.LexedThrough(), previous);
        EXPECT_TRUE(lexer.LexedThrough() == text.size() || text[lexer.LexedThrough() - 1] == '\n'); // line aligned
        previous = lexer.LexedThrough();
    }
    EXPECT_EQ(lexer.LexedThrough(), text.size());
    for (int k = 0; k < 50; ++k)
    {
        size_t const byte = text.size() * static_cast<size_t>(k) / 50 + static_cast<size_t>(k);
        EXPECT_EQ(lexer.CategoryAt(byte), At(whole, byte)) << "byte " << byte;
    }
    EXPECT_EQ(lexer.CategoryAt(text.find("still inside") + 2), TokenCategory::Comment);
    EXPECT_EQ(lexer.CategoryAt(text.size() + 10), TokenCategory::Default);
}

TEST(Highlight, UnlexedBytesAreDefault)
{
    std::string const text = "int a;\nint b;\nint c;\n";
    std::span<const uint8_t> const bytes{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    IncrementalLexer lexer{bytes, "cpp", 65001};
    EXPECT_EQ(lexer.CategoryAt(0), TokenCategory::Default);
    lexer.LexThrough(1, {});
    EXPECT_EQ(lexer.LexedThrough(), 7u); // rounded to the end of line 0
    EXPECT_EQ(lexer.CategoryAt(0), TokenCategory::Keyword);
    EXPECT_EQ(lexer.CategoryAt(7), TokenCategory::Default);
}

TEST(Highlight, StopRequestHaltsBetweenSteps)
{
    std::string text;
    for (int i = 0; i < 100000; ++i) text += "int a = 1;\n";
    std::span<const uint8_t> const bytes{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    IncrementalLexer lexer{bytes, "cpp", 65001};
    std::stop_source source;
    source.request_stop();
    lexer.LexThrough(text.size(), source.get_token());
    EXPECT_EQ(lexer.LexedThrough(), 0u);
}

TEST(Highlight, Utf8StepMatchesMultiByteToWideChar)
{
    // Valid, stray/truncated/overlong/surrogate/out-of-range sequences, and 4-byte characters.
    std::vector<std::string> const samples{
        "plain", "cafÃ©", "â¬", "ðx", "âx", "âx", "ðx", "ðx",
        "À", "Á¿", "à", "í ", "ô", "õ", "a",
        "ÿþ", "abâ", "abð", "â¬â", "ðð"};
    for (auto const& s : samples)
    {
        std::span<const uint8_t> const bytes{reinterpret_cast<const uint8_t*>(s.data()), s.size()};
        size_t units = 0;
        for (size_t i = 0; i < bytes.size();)
        {
            auto const step = NextUtf8Step(bytes, i);
            ASSERT_GE(step.length, 1u);
            i += step.length;
            units += step.units;
        }
        EXPECT_EQ(units, FromUtf8(s).size()) << ::testing::PrintToString(s);
    }
}
