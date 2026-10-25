#include "Text/Highlight.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>

#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>

#include "Text/LexDocument.h"

namespace fastlook::core
{
    namespace
    {
        constexpr uint64_t kStepBytes = 512 * 1024; // granularity at which LexThrough notices a stop request

        bool HasWord(std::string_view tags, std::string_view word)
        {
            size_t pos = 0;
            while (pos < tags.size())
            {
                size_t const end = std::min(tags.find(' ', pos), tags.size());
                if (tags.substr(pos, end - pos) == word) return true;
                pos = end + 1;
            }
            return false;
        }

        struct StyleCategory
        {
            int style;
            TokenCategory category;
        };

        using enum TokenCategory;

        // Most Lexilla lexers publish no style tags (only cpp, python, bash, ruby, perl, lua, rust and a few others do); these cover
        // the mapped lexers that do not.
        constexpr StyleCategory kJson[] = {
            {SCE_JSON_NUMBER, Number},         {SCE_JSON_STRING, String},          {SCE_JSON_STRINGEOL, Error},   {SCE_JSON_PROPERTYNAME, Identifier},
            {SCE_JSON_ESCAPESEQUENCE, String}, {SCE_JSON_LINECOMMENT, Comment},    {SCE_JSON_BLOCKCOMMENT, Comment}, {SCE_JSON_OPERATOR, Operator},
            {SCE_JSON_URI, String},            {SCE_JSON_COMPACTIRI, String},      {SCE_JSON_KEYWORD, Keyword},   {SCE_JSON_LDKEYWORD, Keyword},
            {SCE_JSON_ERROR, Error}};
        constexpr StyleCategory kPowerShell[] = {
            {SCE_POWERSHELL_COMMENT, Comment},    {SCE_POWERSHELL_STRING, String},       {SCE_POWERSHELL_CHARACTER, String},
            {SCE_POWERSHELL_NUMBER, Number},      {SCE_POWERSHELL_VARIABLE, Identifier}, {SCE_POWERSHELL_OPERATOR, Operator},
            {SCE_POWERSHELL_KEYWORD, Keyword},    {SCE_POWERSHELL_CMDLET, Keyword},      {SCE_POWERSHELL_ALIAS, Keyword},
            {SCE_POWERSHELL_FUNCTION, Keyword},   {SCE_POWERSHELL_USER1, Keyword},       {SCE_POWERSHELL_COMMENTSTREAM, Comment},
            {SCE_POWERSHELL_HERE_STRING, String}, {SCE_POWERSHELL_HERE_CHARACTER, String}, {SCE_POWERSHELL_COMMENTDOCKEYWORD, Comment}};
        constexpr StyleCategory kSql[] = {
            {SCE_SQL_COMMENT, Comment},        {SCE_SQL_COMMENTLINE, Comment},     {SCE_SQL_COMMENTDOC, Comment},          {SCE_SQL_NUMBER, Number},
            {SCE_SQL_WORD, Keyword},           {SCE_SQL_STRING, String},           {SCE_SQL_CHARACTER, String},            {SCE_SQL_SQLPLUS, Keyword},
            {SCE_SQL_SQLPLUS_PROMPT, Comment}, {SCE_SQL_OPERATOR, Operator},       {SCE_SQL_SQLPLUS_COMMENT, Comment},     {SCE_SQL_COMMENTLINEDOC, Comment},
            {SCE_SQL_WORD2, Keyword},          {SCE_SQL_COMMENTDOCKEYWORD, Comment}, {SCE_SQL_COMMENTDOCKEYWORDERROR, Comment},
            {SCE_SQL_USER1, Keyword},          {SCE_SQL_USER2, Keyword},           {SCE_SQL_USER3, Keyword},               {SCE_SQL_USER4, Keyword},
            {SCE_SQL_QUOTEDIDENTIFIER, Identifier}, {SCE_SQL_QOPERATOR, String}};
        constexpr StyleCategory kBatch[] = {
            {SCE_BAT_COMMENT, Comment}, {SCE_BAT_WORD, Keyword},          {SCE_BAT_LABEL, Preprocessor},
            {SCE_BAT_COMMAND, Keyword}, {SCE_BAT_IDENTIFIER, Identifier}, {SCE_BAT_OPERATOR, Operator}};
        constexpr StyleCategory kCss[] = {
            {SCE_CSS_TAG, Keyword},            {SCE_CSS_CLASS, Identifier},     {SCE_CSS_PSEUDOCLASS, Identifier}, {SCE_CSS_OPERATOR, Operator},
            {SCE_CSS_IDENTIFIER, Identifier},  {SCE_CSS_VALUE, Number},         {SCE_CSS_COMMENT, Comment},        {SCE_CSS_ID, Identifier},
            {SCE_CSS_IMPORTANT, Keyword},      {SCE_CSS_DIRECTIVE, Preprocessor}, {SCE_CSS_DOUBLESTRING, String},  {SCE_CSS_SINGLESTRING, String},
            {SCE_CSS_IDENTIFIER2, Identifier}, {SCE_CSS_ATTRIBUTE, Identifier}, {SCE_CSS_PSEUDOELEMENT, Identifier},
            {SCE_CSS_GROUP_RULE, Preprocessor}, {SCE_CSS_VARIABLE, Identifier}};
        constexpr StyleCategory kYaml[] = {
            {SCE_YAML_COMMENT, Comment},        {SCE_YAML_IDENTIFIER, Identifier}, {SCE_YAML_KEYWORD, Keyword}, {SCE_YAML_NUMBER, Number},
            {SCE_YAML_REFERENCE, Preprocessor}, {SCE_YAML_DOCUMENT, Preprocessor}, {SCE_YAML_TEXT, String},     {SCE_YAML_ERROR, Error},
            {SCE_YAML_OPERATOR, Operator}};
        constexpr StyleCategory kToml[] = {
            {SCE_TOML_COMMENT, Comment},           {SCE_TOML_KEYWORD, Keyword},           {SCE_TOML_NUMBER, Number},
            {SCE_TOML_TABLE, Preprocessor},        {SCE_TOML_KEY, Identifier},            {SCE_TOML_ERROR, Error},
            {SCE_TOML_OPERATOR, Operator},         {SCE_TOML_STRING_SQ, String},          {SCE_TOML_STRING_DQ, String},
            {SCE_TOML_TRIPLE_STRING_SQ, String},   {SCE_TOML_TRIPLE_STRING_DQ, String},   {SCE_TOML_ESCAPECHAR, String},
            {SCE_TOML_DATETIME, Number},           {SCE_TOML_STRINGEOL, Error}};
        constexpr StyleCategory kProps[] = {
            {SCE_PROPS_COMMENT, Comment}, {SCE_PROPS_SECTION, Preprocessor}, {SCE_PROPS_ASSIGNMENT, Operator},
            {SCE_PROPS_DEFVAL, String},   {SCE_PROPS_KEY, Identifier}};
        constexpr StyleCategory kDiff[] = {
            {SCE_DIFF_COMMENT, Comment},       {SCE_DIFF_COMMAND, Keyword},       {SCE_DIFF_HEADER, Preprocessor},
            {SCE_DIFF_POSITION, Number},       {SCE_DIFF_DELETED, Error},         {SCE_DIFF_ADDED, String},
            {SCE_DIFF_CHANGED, Keyword},       {SCE_DIFF_PATCH_ADD, String},      {SCE_DIFF_PATCH_DELETE, Error},
            {SCE_DIFF_REMOVED_PATCH_ADD, Error}, {SCE_DIFF_REMOVED_PATCH_DELETE, Error}};
        constexpr StyleCategory kCmake[] = {
            {SCE_CMAKE_COMMENT, Comment},       {SCE_CMAKE_STRINGDQ, String},      {SCE_CMAKE_STRINGLQ, String},
            {SCE_CMAKE_STRINGRQ, String},       {SCE_CMAKE_COMMANDS, Keyword},     {SCE_CMAKE_PARAMETERS, Identifier},
            {SCE_CMAKE_VARIABLE, Identifier},   {SCE_CMAKE_USERDEFINED, Keyword},  {SCE_CMAKE_WHILEDEF, Keyword},
            {SCE_CMAKE_FOREACHDEF, Keyword},    {SCE_CMAKE_IFDEFINEDEF, Keyword},  {SCE_CMAKE_MACRODEF, Keyword},
            {SCE_CMAKE_STRINGVAR, Identifier},  {SCE_CMAKE_NUMBER, Number}};

        template <size_t N>
        void Fill(std::array<TokenCategory, 256>& categories, StyleCategory const (&table)[N])
        {
            for (auto const& entry : table)
            {
                if (entry.style >= 0 && entry.style < 256 && categories[static_cast<size_t>(entry.style)] == Default)
                {
                    categories[static_cast<size_t>(entry.style)] = entry.category;
                }
            }
        }

        void ApplyStyleFallback(std::array<TokenCategory, 256>& categories, std::string_view lexerName)
        {
            if (lexerName == "json") Fill(categories, kJson);
            else if (lexerName == "powershell") Fill(categories, kPowerShell);
            else if (lexerName == "sql") Fill(categories, kSql);
            else if (lexerName == "batch") Fill(categories, kBatch);
            else if (lexerName == "css") Fill(categories, kCss);
            else if (lexerName == "yaml") Fill(categories, kYaml);
            else if (lexerName == "toml") Fill(categories, kToml);
            else if (lexerName == "props") Fill(categories, kProps);
            else if (lexerName == "diff") Fill(categories, kDiff);
            else if (lexerName == "cmake") Fill(categories, kCmake);
        }

        struct LexerRelease
        {
            void operator()(Scintilla::ILexer5* lexer) const
            {
                if (lexer) lexer->Release();
            }
        };
    }

    TokenCategory CategoryFromTags(std::string_view tags)
    {
        if (HasWord(tags, "comment")) return TokenCategory::Comment;
        if (HasWord(tags, "string")) return TokenCategory::String; // "literal string ..." and plain "string"
        if (HasWord(tags, "numeric") || HasWord(tags, "number")) return TokenCategory::Number;
        if (HasWord(tags, "keyword")) return TokenCategory::Keyword;
        if (HasWord(tags, "preprocessor")) return TokenCategory::Preprocessor;
        if (HasWord(tags, "operator")) return TokenCategory::Operator;
        if (HasWord(tags, "identifier")) return TokenCategory::Identifier;
        if (HasWord(tags, "error")) return TokenCategory::Error;
        return TokenCategory::Default;
    }

    struct IncrementalLexer::Impl
    {
        Impl(std::span<const uint8_t> bytes, std::string_view lexerName, uint32_t codePage)
            : doc(bytes, static_cast<int>(codePage))
        {
            std::string const name{lexerName};
            lexer.reset(CreateLexer(name.c_str()));
            if (!lexer) return;
            lexer->PropertySet("lexer.cpp.track.preprocessor", "0"); // no greyed-out inactive code in a previewer
            lexer->PropertySet("lexer.cpp.update.preprocessor", "0");
            ApplyKeywords(lexer.get(), lexerName);
            categories.fill(TokenCategory::Default);
            for (int style = 0; style < static_cast<int>(categories.size()); ++style)
            {
                char const* const tags = lexer->TagsOfStyle(style);
                categories[static_cast<size_t>(style)] = tags ? CategoryFromTags(tags) : TokenCategory::Default;
            }
            ApplyStyleFallback(categories, lexerName);
        }

        LexDocument doc;
        std::unique_ptr<Scintilla::ILexer5, LexerRelease> lexer;
        std::array<TokenCategory, 256> categories{};
        std::atomic<uint64_t> lexed{0};
    };

    IncrementalLexer::IncrementalLexer(std::span<const uint8_t> bytes, std::string_view lexerName, uint32_t codePage)
        : m_(std::make_unique<Impl>(bytes, lexerName, codePage))
    {
    }

    IncrementalLexer::~IncrementalLexer() = default;

    bool IncrementalLexer::Valid() const
    {
        return m_->lexer != nullptr;
    }

    uint64_t IncrementalLexer::LexedThrough() const
    {
        return m_->lexed.load(std::memory_order_acquire);
    }

    uint64_t IncrementalLexer::LineEndAfter(uint64_t byte) const
    {
        auto const& doc = m_->doc;
        if (byte >= doc.Size()) return doc.Size();
        return doc.LineStartOffset(doc.LineOfOffset(byte) + 1);
    }

    void IncrementalLexer::LexThrough(uint64_t byteEnd, std::stop_token stop)
    {
        auto& m = *m_;
        if (!m.lexer || byteEnd == 0) return;
        uint64_t const size = m.doc.Size();
        uint64_t const end = LineEndAfter(std::min(byteEnd, size) - 1);
        uint64_t from = m.lexed.load(std::memory_order_relaxed);
        while (from < end && !stop.stop_requested())
        {
            uint64_t const step = from + kStepBytes >= end ? end : LineEndAfter(from + kStepBytes - 1);
            int const initStyle = from == 0 ? 0 : m.doc.Style(from - 1);
            m.lexer->Lex(static_cast<Sci_PositionU>(from), static_cast<Sci_Position>(step - from), initStyle, &m.doc);
            from = step;
            m.lexed.store(from, std::memory_order_release);
        }
    }

    TokenCategory IncrementalLexer::CategoryAt(uint64_t byte) const
    {
        if (!m_->lexer || byte >= LexedThrough()) return TokenCategory::Default;
        return m_->categories[m_->doc.Style(byte)];
    }

    std::vector<StyleRun> HighlightBuffer(std::string_view bytes, std::string_view lexerName, uint32_t codePage)
    {
        std::vector<StyleRun> runs;
        std::span<const uint8_t> const span{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
        IncrementalLexer lexer{span, lexerName, codePage};
        if (!lexer.Valid()) return runs;
        lexer.LexThrough(bytes.size(), {});
        for (uint64_t i = 0; i < bytes.size(); ++i)
        {
            TokenCategory const category = lexer.CategoryAt(i);
            if (category == TokenCategory::Default) continue;
            if (!runs.empty() && runs.back().category == category && runs.back().start + runs.back().length == i)
            {
                ++runs.back().length;
            }
            else
            {
                runs.push_back({static_cast<uint32_t>(i), 1, category});
            }
        }
        return runs;
    }
}
