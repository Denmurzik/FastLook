#include "Text/TextDocument.h"

#include <Windows.h>

#include <algorithm>
#include <vector>

#include "Common/SafeMem.h"

namespace fastlook::core
{
    namespace
    {
        constexpr uint64_t kSniffBytes = 64 * 1024;
        constexpr uint64_t kChunkBytes = 4 * 1024 * 1024;
        constexpr uint64_t kFirstChunkBytes = 64 * 1024; // so the first screen is ready (and reported) early

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

        // The code page of legacy "ANSI" files. With the "Beta: Use Unicode UTF-8 for worldwide language support" setting the
        // process code page is UTF-8, which would turn every cp1251/cp1252 file into U+FFFD: use the system locale's own page.
        uint32_t LegacyAnsiCodePage()
        {
            uint32_t const acp = GetACP();
            if (acp != CP_UTF8) return acp;
            DWORD page = 0;
            if (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&page),
                                sizeof(page) / sizeof(wchar_t)) != 0 &&
                page != 0 && page != CP_UTF8)
                return page;
            return 1252;
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

    TextDocument::TextDocument(void* mapping, const uint8_t* view, std::span<const uint8_t> raw, EncodingInfo encoding)
        : mapping_(mapping),
          view_(view),
          raw_(raw),
          size_(raw.size()),
          encoding_(encoding),
          content_(raw.subspan((std::min)(static_cast<size_t>(encoding.bomLength), raw.size()))),
          index_(encoding)
    {
    }

    TextDocument::~TextDocument()
    {
        if (view_ != nullptr) UnmapViewOfFile(view_);
        if (mapping_ != nullptr) CloseHandle(mapping_);
    }

    // Takes ownership of mapping/view; releases them if the first bytes are unreadable.
    std::unique_ptr<TextDocument> TextDocument::Create(void* mapping, const uint8_t* view, std::span<const uint8_t> raw)
    {
        EncodingInfo encoding;
        if (!raw.empty())
        {
            auto const sniff = static_cast<size_t>((std::min<uint64_t>)(raw.size(), kSniffBytes));
            std::vector<uint8_t> head(sniff);
            if (!SafeCopy(head.data(), raw.data(), sniff))
            {
                if (view != nullptr) UnmapViewOfFile(view);
                if (mapping != nullptr) CloseHandle(mapping);
                return nullptr;
            }
            encoding = DetectEncoding(head, LegacyAnsiCodePage(), raw.size() > kSniffBytes);
        }
        return std::unique_ptr<TextDocument>(new TextDocument(mapping, view, raw, encoding));
    }

    std::unique_ptr<TextDocument> TextDocument::Open(std::filesystem::path const& path)
    {
        Handle file{CreateFileW(ExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.h == INVALID_HANDLE_VALUE) return nullptr;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.h, &size) || size.QuadPart < 0) return nullptr;
        auto const bytes = static_cast<uint64_t>(size.QuadPart);
        if (bytes == 0) return Create(nullptr, nullptr, {});

        void* mapping = CreateFileMappingW(file.h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) return nullptr;
        auto const* view = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (view == nullptr)
        {
            CloseHandle(mapping);
            return nullptr;
        }
        return Create(mapping, view, {view, static_cast<size_t>(bytes)});
    }

    std::unique_ptr<TextDocument> TextDocument::OpenView(std::span<const uint8_t> bytes)
    {
        return Create(nullptr, nullptr, bytes);
    }

    void TextDocument::BuildIndex(std::stop_token stop, std::function<void(uint64_t lines, bool complete)> progress)
    {
        if (index_.Complete())
        {
            if (progress) progress(index_.LineCount(), true);
            return;
        }
        std::vector<uint8_t> buffer;
        uint64_t done = indexed_.load();
        for (;;)
        {
            if (stop.stop_requested() || faulted_) return;
            auto const n = static_cast<size_t>((std::min)(done == 0 ? kFirstChunkBytes : kChunkBytes, content_.size() - done));
            bool const last = done + n >= content_.size();
            buffer.resize(n);
            if (n != 0 && !SafeCopy(buffer.data(), content_.data() + done, n))
            {
                faulted_ = true;
                return;
            }
            index_.Append(buffer, last);
            done += n;
            indexed_ = done;
            if (progress) progress(index_.LineCount(), last);
            if (last) return;
        }
    }

    bool TextDocument::ReadLine(uint64_t line, std::wstring& text, bool& continuation) const
    {
        text.clear();
        continuation = false;
        if (faulted_) return false;
        if (line >= index_.LineCount()) return true;

        LineIndex::LineSpan span;
        std::vector<uint8_t> bytes;
        auto scan = [&] { span = index_.Line(line, content_); }; // scans raw mapped bytes
        if (!SafeRun(raw_.data(), raw_.size(), scan))
        {
            faulted_ = true;
            return false;
        }
        bytes.resize(static_cast<size_t>(span.end - span.begin));
        if (!bytes.empty() && !SafeCopy(bytes.data(), content_.data() + span.begin, bytes.size()))
        {
            faulted_ = true;
            return false;
        }
        continuation = span.continuation;
        text = DecodeToUtf16(bytes, encoding_);
        return true;
    }

    std::wstring TextDocument::LineText(uint64_t line) const
    {
        std::wstring text;
        bool continuation;
        ReadLine(line, text, continuation);
        return text;
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

        std::wstring out, text;
        for (uint64_t l = from.line; l <= to.line; ++l)
        {
            bool continuation;
            if (!ReadLine(l, text, continuation)) return {};
            if (l != from.line && !continuation) out += L"\r\n";
            size_t const begin = l == from.line ? (std::min<size_t>)(from.column, text.size()) : 0;
            size_t const end = l == to.line ? (std::min<size_t>)(to.column, text.size()) : text.size();
            if (end > begin) out.append(text, begin, end - begin);
        }
        return out;
    }
}
