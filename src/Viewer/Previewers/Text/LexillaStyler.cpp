#include "pch.h"
#include "Previewers/Text/LexillaStyler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "Common/SafeMem.h"
#include "Diagnostics/Log.h"
#include "Text/Highlight.h"

namespace fastlook::viewer
{
    namespace
    {
        using core::TokenCategory;

        constexpr uint64_t kMaxStyledBytes = 8ull * 1024 * 1024;
        constexpr uint64_t kMaxStyledLines = 500'000; // per-line lexer state is allocated for every line
        constexpr uint64_t kLookAheadRows = 200;
        constexpr size_t kCopyChunk = 4 * 1024 * 1024;

        // The text and the lexer state built over it; published to the UI thread once the lexer exists.
        struct Lexing
        {
            Lexing(std::vector<uint8_t> text, std::string_view lexerName, uint32_t codePage)
                : bytes(std::move(text)), lexer(bytes, lexerName, codePage)
            {
            }
            std::vector<uint8_t> bytes; // a private copy: the mapping may fault, this cannot
            core::IncrementalLexer lexer;
        };

        class LexillaStyler final : public ILineStyler
        {
        public:
            LexillaStyler(std::shared_ptr<core::TextDocument> document, std::string lexerName) : m_doc(std::move(document)), m_lexerName(std::move(lexerName)) {}

            ~LexillaStyler() override
            {
                if (m_worker.joinable())
                {
                    m_worker.request_stop();
                    m_wake.notify_all();
                    m_worker.join();
                }
            }

            void StyleLine(uint64_t line, std::wstring_view text, std::vector<StyledRun>& out) override
            {
                std::shared_ptr<Lexing> const lexing = m_lexing.load();
                if (!lexing || line >= m_doc->LineCount())
                {
                    return;
                }
                auto const span = LineSpan(line);
                uint64_t const lexed = lexing->lexer.LexedThrough();
                if (!span || span->begin >= lexed)
                {
                    return;
                }
                uint64_t const end = std::min(span->end, lexed);
                auto const& encoding = m_doc->Encoding();
                bool const utf8 = encoding.encoding == core::TextEncoding::Utf8;
                std::span<const uint8_t> const bytes{lexing->bytes};

                uint32_t unit = 0;
                for (uint64_t i = span->begin; i < end && unit < text.size();)
                {
                    int length = 1;
                    uint32_t units = 1;
                    if (utf8)
                    {
                        auto const step = core::NextUtf8Step(bytes, static_cast<size_t>(i));
                        length = static_cast<int>(step.length);
                        units = step.units;
                    }
                    else if (IsDBCSLeadByteEx(encoding.codePage, bytes[static_cast<size_t>(i)]) && i + 1 < end)
                    {
                        length = 2;
                    }
                    TokenCategory const category = lexing->lexer.CategoryAt(i);
                    units = std::min<uint32_t>(units, static_cast<uint32_t>(text.size()) - unit);
                    if (category != TokenCategory::Default)
                    {
                        if (!out.empty() && out.back().category == category && out.back().start + out.back().length == unit)
                        {
                            out.back().length += units;
                        }
                        else
                        {
                            out.push_back({unit, units, category});
                        }
                    }
                    unit += units;
                    i += static_cast<uint64_t>(length);
                }
            }

            void EnsureStyledThrough(uint64_t lastVisibleLine) override
            {
                uint64_t const target = lastVisibleLine + kLookAheadRows;
                if (m_ended || (target <= m_requested.load() && m_satisfied.load()))
                {
                    return;
                }
                {
                    std::lock_guard lock{m_mutex};
                    m_requested = std::max(m_requested.load(), target);
                    m_satisfied = false;
                    m_pending = true;
                    if (!m_worker.joinable())
                    {
                        m_worker = std::jthread([this](std::stop_token stop) { Run(stop); });
                    }
                }
                m_wake.notify_all();
            }

        private:
            // Bytes of one row, scanned from the mapping, so guarded like TextDocument's own reads.
            std::optional<core::LineIndex::LineSpan> LineSpan(uint64_t line) const
            {
                auto const content = m_doc->Content();
                core::LineIndex::LineSpan span;
                bool const ok = core::SafeRun(content.data(), content.size(), [&] { span = m_doc->Index().Line(line, content); });
                if (!ok)
                {
                    return std::nullopt;
                }
                return span;
            }

            std::shared_ptr<Lexing> Build(std::stop_token stop)
            {
                auto const content = m_doc->Content();
                std::vector<uint8_t> copy(content.size());
                for (size_t done = 0; done < copy.size(); done += kCopyChunk)
                {
                    if (stop.stop_requested())
                    {
                        return nullptr;
                    }
                    size_t const n = std::min(kCopyChunk, copy.size() - done);
                    if (!core::SafeCopy(copy.data() + done, content.data() + done, n))
                    {
                        FL_LOG_INFO(L"Highlighting disabled: the file became unreadable");
                        return nullptr;
                    }
                }
                uint64_t lines = 1;
                for (size_t i = 0; i < copy.size() && lines <= kMaxStyledLines; ++i)
                {
                    lines += copy[i] == 0x0A || (copy[i] == 0x0D && !(i + 1 < copy.size() && copy[i + 1] == 0x0A));
                }
                if (lines > kMaxStyledLines)
                {
                    return nullptr; // too many lines to keep lexer state for
                }
                auto lexing = std::make_shared<Lexing>(std::move(copy), m_lexerName, m_doc->Encoding().codePage);
                return lexing->lexer.Valid() ? lexing : nullptr;
            }

            void Run(std::stop_token stop)
            {
                struct Ended
                {
                    std::atomic<bool>& flag;
                    ~Ended() { flag = true; }
                } const ended{m_ended};
                try
                {
                    std::shared_ptr<Lexing> const lexing = Build(stop);
                    if (!lexing)
                    {
                        return;
                    }
                    m_lexing.store(lexing);
                    while (!stop.stop_requested())
                    {
                        uint64_t requested;
                        {
                            std::unique_lock lock{m_mutex};
                            m_wake.wait(lock, [&] { return m_pending || stop.stop_requested(); });
                            if (stop.stop_requested())
                            {
                                return;
                            }
                            m_pending = false;
                            requested = m_requested.load();
                        }
                        uint64_t const lines = m_doc->LineCount();
                        if (lines == 0 || lexing->bytes.empty())
                        {
                            m_satisfied = m_doc->IndexComplete();
                            continue;
                        }
                        uint64_t const row = std::min(requested, lines - 1);
                        auto const span = LineSpan(row);
                        if (!span)
                        {
                            return;
                        }
                        lexing->lexer.LexThrough(span->end + 1, stop); // +1: an empty row's EOL byte still names its line
                        m_satisfied = (row == requested || m_doc->IndexComplete()) && !stop.stop_requested();
                        if (invalidated)
                        {
                            invalidated();
                        }
                    }
                }
                catch (...)
                {
                    FL_LOG_INFO(L"Syntax highlighting failed");
                }
            }

            std::shared_ptr<core::TextDocument> m_doc;
            std::string m_lexerName;
            std::atomic<std::shared_ptr<Lexing>> m_lexing; // set once by the worker
            std::atomic<uint64_t> m_requested{0};
            std::atomic<bool> m_satisfied{false};
            std::atomic<bool> m_ended{false}; // the worker returned (highlighting off or stopped)
            std::mutex m_mutex;
            std::condition_variable_any m_wake;
            bool m_pending = false; // under m_mutex
            std::jthread m_worker;
        };
    }

    std::shared_ptr<ILineStyler> MakeLexillaStyler(std::shared_ptr<core::TextDocument> document, std::string_view lexer)
    {
        if (!document || lexer.empty() || document->SizeBytes() > kMaxStyledBytes)
        {
            return nullptr;
        }
        auto const encoding = document->Encoding().encoding;
        if (encoding != core::TextEncoding::Utf8 && encoding != core::TextEncoding::Ansi)
        {
            return nullptr;
        }
        return std::make_shared<LexillaStyler>(std::move(document), std::string{lexer});
    }
}
