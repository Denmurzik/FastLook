#include "FileTypes/Extensions.h"

#include <unordered_map>

#include "FileTypes/FileName.h"

namespace fastlook::core
{
    namespace
    {
        struct LexerEntry
        {
            std::wstring_view ext;
            std::string_view lexer;
        };

        // Lexilla lexer names; "" = plain text.
        constexpr LexerEntry kLexers[] = {
            {L"c", "cpp"}, {L"cpp", "cpp"}, {L"cc", "cpp"}, {L"cxx", "cpp"}, {L"h", "cpp"}, {L"hpp", "cpp"},
            {L"hxx", "cpp"}, {L"cs", "cpp"}, {L"java", "cpp"}, {L"kt", "cpp"}, {L"kts", "cpp"}, {L"js", "cpp"},
            {L"mjs", "cpp"}, {L"cjs", "cpp"}, {L"ts", "cpp"}, {L"jsx", "cpp"}, {L"tsx", "cpp"}, {L"swift", "cpp"},
            {L"dart", "cpp"}, {L"go", "cpp"}, {L"rs", "rust"},
            {L"json", "json"},
            {L"xml", "xml"}, {L"xaml", "xml"}, {L"csproj", "xml"}, {L"vcxproj", "xml"}, {L"props", "xml"},
            {L"targets", "xml"}, {L"resw", "xml"}, {L"svg", "xml"},
            {L"html", "hypertext"}, {L"htm", "hypertext"},
            {L"php", "phpscript"},
            {L"css", "css"}, {L"scss", "css"}, {L"less", "css"},
            {L"py", "python"}, {L"pyw", "python"},
            {L"rb", "ruby"},
            {L"sql", "sql"},
            {L"sh", "bash"}, {L"bash", "bash"}, {L"zsh", "bash"},
            {L"ps1", "powershell"}, {L"psm1", "powershell"}, {L"psd1", "powershell"},
            {L"bat", "batch"}, {L"cmd", "batch"},
            {L"yaml", "yaml"}, {L"yml", "yaml"},
            {L"toml", "toml"},
            {L"ini", "props"}, {L"cfg", "props"}, {L"conf", "props"}, {L"reg", "props"},
            {L"md", "markdown"}, {L"markdown", "markdown"},
            {L"lua", "lua"},
            {L"r", "r"},
            {L"pl", "perl"}, {L"pm", "perl"},
            {L"mk", "makefile"},
            {L"cmake", "cmake"},
            {L"diff", "diff"}, {L"patch", "diff"},
            {L"tex", "latex"},
            {L"vb", "vb"}, {L"vbs", "vb"},
            {L"pas", "pascal"},
            {L"asm", "asm"},
            {L"f90", "fortran"},
            {L"hs", "haskell"},
            {L"log", ""}, {L"csv", ""}, {L"txt", ""},
        };

        constexpr std::wstring_view kImage[] = {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"tif", L"tiff", L"ico", L"webp",
                                                L"heic", L"heif", L"avif", L"jxr", L"dds", L"svg"};
        constexpr std::wstring_view kMarkdown[] = {L"md", L"markdown"};
        constexpr std::wstring_view kMedia[] = {L"mp4", L"m4v", L"mov", L"mkv", L"avi", L"wmv", L"webm", L"3gp", L"mpg",
                                                L"mpeg", L"ts", L"m2ts", L"mp3", L"m4a", L"aac", L"wav", L"flac", L"ogg",
                                                L"opus", L"wma"};
        constexpr std::wstring_view kPdf[] = {L"pdf"};
        constexpr std::wstring_view kArchive[] = {L"zip", L"7z", L"rar", L"tar", L"gz", L"tgz", L"bz2", L"xz", L"zst",
                                                  L"cab", L"iso"};
        constexpr std::wstring_view kExtraText[] = {L"tsv", L"nfo", L"env", L"gitignore", L"editorconfig"};

        struct Tables
        {
            std::unordered_map<std::wstring_view, PreviewerKind> kinds;
            std::unordered_map<std::wstring_view, std::string_view> lexers;
            std::unordered_map<std::wstring, std::string_view> specialNames; // lower-case base name -> lexer
            std::vector<std::wstring_view> mapped;

            Tables()
            {
                // emplace keeps the first entry: Image/Markdown/Media/Archive win over Text ("svg", "md", "ts").
                for (auto e : kImage) kinds.emplace(e, PreviewerKind::Image);
                for (auto e : kMarkdown) kinds.emplace(e, PreviewerKind::Markdown);
                for (auto e : kMedia) kinds.emplace(e, PreviewerKind::Media);
                for (auto e : kPdf) kinds.emplace(e, PreviewerKind::Pdf);
                for (auto e : kArchive) kinds.emplace(e, PreviewerKind::Archive);
                for (auto const& e : kLexers)
                {
                    kinds.emplace(e.ext, PreviewerKind::Text);
                    lexers.emplace(e.ext, e.lexer);
                    mapped.push_back(e.ext);
                }
                for (auto e : kExtraText) kinds.emplace(e, PreviewerKind::Text);

                specialNames = {{L"makefile", "makefile"}, {L"dockerfile", "bash"}, {L"cmakelists.txt", "cmake"}, {L".gitignore", ""}};
            }
        };

        Tables const& GetTables()
        {
            static Tables const tables;
            return tables;
        }
    }

    std::optional<PreviewerKind> KindForExtension(std::wstring_view fileName)
    {
        auto const& t = GetTables();
        if (t.specialNames.contains(LowerAscii(BaseName(fileName))))
            return PreviewerKind::Text;
        std::wstring ext = LowerExtension(fileName);
        if (ext.empty())
            return std::nullopt;
        auto it = t.kinds.find(ext);
        if (it == t.kinds.end())
            return std::nullopt;
        return it->second;
    }

    std::string_view LexerNameFor(std::wstring_view fileName)
    {
        auto const& t = GetTables();
        if (auto s = t.specialNames.find(LowerAscii(BaseName(fileName))); s != t.specialNames.end())
            return s->second;
        if (auto it = t.lexers.find(LowerExtension(fileName)); it != t.lexers.end())
            return it->second;
        return {};
    }

    std::vector<std::wstring_view> MappedTextExtensions()
    {
        return GetTables().mapped;
    }
}
