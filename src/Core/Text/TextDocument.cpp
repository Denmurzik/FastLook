#include "Text/TextDocument.h"

#include <Windows.h>

#include <algorithm>

namespace fastlook::core
{
    namespace
    {
        constexpr uint64_t kSniffBytes = 64 * 1024;
        constexpr uint64_t kChunkBytes = 4 * 1024 * 1024;

        // Paths over MAX_PATH only open with the extended-length prefix.
        std::wstring ExtendedPath(std::filesystem::path const& path)
        {
            std::wstring s = path.native();
            if (s.size() < 248 || s.starts_with(LR"(\\?\)") || s.starts_with(LR"(\\.\)")) return s;
            std::error_code ec;
            auto abs = std::filesystem::absolute(path, ec);
            if (ec) return s;
            s = abs.native();
            if (s.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + s.substr(2);
            return LR"(\\?\)" + s;
        }

        struct Handle
        {
            HANDLE h;
            ~Handle()
            {
                if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
        };
    }

    TextDocument::TextDocument(void* mapping, const uint8_t* view, uint64_t size, EncodingInfo encoding)
        : mapping_(mapping),
          view_(view),
          size_(size),
          encoding_(encoding),
          content_(view, static_cast<size_t>(size)),
          index_(encoding)
    {
        content_ = content_.subspan((std::min)(static_cast<size_t>(encoding.bomLength), content_.size()));
    }

    TextDocument::~TextDocument()
    {
        if (view_ != nullptr) UnmapViewOfFile(view_);
        if (mapping_ != nullptr) CloseHandle(mapping_);
    }

    std::unique_ptr<TextDocument> TextDocument::Open(std::filesystem::path const& path)
    {
        Handle file{CreateFileW(ExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.h == INVALID_HANDLE_VALUE) return nullptr;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.h, &size) || size.QuadPart < 0) return nullptr;
        auto const bytes = static_cast<uint64_t>(size.QuadPart);

        void* mapping = nullptr;
        const uint8_t* view = nullptr;
        EncodingInfo encoding;
        if (bytes > 0)
        {
            mapping = CreateFileMappingW(file.h, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping == nullptr) return nullptr;
            view = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
            if (view == nullptr)
            {
                CloseHandle(mapping);
                return nullptr;
            }
            auto const sniff = (std::min)(bytes, kSniffBytes);
            encoding = DetectEncoding({view, static_cast<size_t>(sniff)}, GetACP(), bytes > kSniffBytes);
        }
        return std::unique_ptr<TextDocument>(new TextDocument(mapping, view, bytes, encoding));
    }

    void TextDocument::BuildIndex(std::stop_token stop, std::function<void(uint64_t lines, bool complete)> progress)
    {
        if (index_.Complete())
        {
            if (progress) progress(index_.LineCount(), true);
            return;
        }
        uint64_t done = indexed_.load();
        for (;;)
        {
            if (stop.stop_requested()) return;
            auto const n = (std::min)(kChunkBytes, content_.size() - done);
            bool const last = done + n >= content_.size();
            index_.Append(content_.subspan(static_cast<size_t>(done), static_cast<size_t>(n)), last);
            done += n;
            indexed_ = done;
            if (progress) progress(index_.LineCount(), last);
            if (last) return;
        }
    }

    std::wstring TextDocument::LineText(uint64_t line) const
    {
        if (line >= index_.LineCount()) return {};
        auto const span = index_.Line(line, content_);
        return DecodeToUtf16(content_.subspan(static_cast<size_t>(span.begin), static_cast<size_t>(span.end - span.begin)), encoding_);
    }

    std::wstring TextDocument::ExtractRange(TextPos from, TextPos to) const
    {
        if (to < from) return {};
        uint64_t const lines = index_.LineCount();
        if (from.line >= lines) return {};
        if (to.line >= lines)
        {
            to.line = lines - 1;
            to.column = (std::numeric_limits<uint32_t>::max)();
        }

        std::wstring out;
        for (uint64_t l = from.line; l <= to.line; ++l)
        {
            auto const span = index_.Line(l, content_);
            if (l != from.line && !span.continuation) out += L"\r\n";
            std::wstring text = LineText(l);
            size_t const begin = l == from.line ? (std::min<size_t>)(from.column, text.size()) : 0;
            size_t const end = l == to.line ? (std::min<size_t>)(to.column, text.size()) : text.size();
            if (end > begin) out.append(text, begin, end - begin);
        }
        return out;
    }
}
