#include "Text/LineIndex.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace fastlook::core
{
    namespace
    {
        struct Checkpoint
        {
            uint64_t offset = 0;
            bool continuation = false;
        };

        enum class Unit
        {
            Byte,
            Utf8,
            Utf16LE,
            Utf16BE
        };

        Unit UnitOf(EncodingInfo const& info)
        {
            switch (info.encoding)
            {
            case TextEncoding::Utf8: return Unit::Utf8;
            case TextEncoding::Utf16LE: return Unit::Utf16LE;
            case TextEncoding::Utf16BE: return Unit::Utf16BE;
            default: return Unit::Byte;
            }
        }

        // Splits a byte stream into lines. The same state machine drives the streaming build and the forward
        // scans of Line()/LineFromOffset(), so they always agree on the boundaries.
        class Scanner
        {
        public:
            Scanner(Unit unit, uint64_t lineStart)
                : unit_(unit), size_(unit >= Unit::Utf16LE ? 2 : 1), pos_(lineStart), lineStart_(lineStart), lastLead_(lineStart)
            {
            }

            // onLine(lineEnd, nextBegin, nextIsContinuation) runs once per finished line and returns false to
            // stop the scan; Feed/Finish then return false too.
            template <class F>
            bool Feed(std::span<const uint8_t> chunk, F&& onLine)
            {
                for (uint8_t const b : chunk)
                {
                    uint64_t const at = pos_++;
                    uint32_t u;
                    if (size_ == 1)
                    {
                        u = b;
                        unitStart_ = at;
                    }
                    else if (!haveFirst_)
                    {
                        first_ = b;
                        haveFirst_ = true;
                        unitStart_ = at;
                        continue;
                    }
                    else
                    {
                        haveFirst_ = false;
                        u = unit_ == Unit::Utf16LE ? (static_cast<uint32_t>(b) << 8 | first_) : (first_ << 8 | b);
                    }
                    if (!Process(u, onLine)) return false;
                }
                return true;
            }

            // End of data: resolves a CR that was waiting for a possible LF.
            template <class F>
            bool Finish(F&& onLine)
            {
                if (pendingCr_)
                {
                    pendingCr_ = false;
                    return onLine(crPos_, crPos_ + size_, false);
                }
                return true;
            }

        private:
            template <class F>
            bool Process(uint32_t u, F& onLine)
            {
                uint64_t const next = unitStart_ + size_;
                if (pendingCr_)
                {
                    pendingCr_ = false;
                    if (u == 0x0A)
                    {
                        Start(next, 0);
                        return onLine(crPos_, next, false);
                    }
                    Start(unitStart_, 0);
                    if (!onLine(crPos_, unitStart_, false)) return false;
                }
                if (u == 0x0A)
                {
                    Start(next, 0);
                    return onLine(unitStart_, next, false);
                }
                if (u == 0x0D)
                {
                    pendingCr_ = true;
                    crPos_ = unitStart_;
                    return true;
                }
                if (run_ == LineIndex::kMaxLineUnits)
                {
                    uint64_t split = unitStart_;
                    if (unit_ == Unit::Utf8 && (u & 0xC0) == 0x80 && lastLead_ > lineStart_)
                    {
                        split = lastLead_; // do not cut a UTF-8 sequence
                    }
                    else if (size_ == 2 && (u & 0xFC00) == 0xDC00 && (prev_ & 0xFC00) == 0xD800)
                    {
                        split = prevStart_; // do not cut a surrogate pair
                    }
                    Start(split, static_cast<uint32_t>((unitStart_ - split) / size_));
                    if (!onLine(split, split, true)) return false;
                }
                if (unit_ != Unit::Utf8 || (u & 0xC0) != 0x80) lastLead_ = unitStart_;
                prev_ = u;
                prevStart_ = unitStart_;
                ++run_;
                return true;
            }

            void Start(uint64_t at, uint32_t run)
            {
                lineStart_ = at;
                lastLead_ = at;
                run_ = run;
                prev_ = 0;
            }

            Unit unit_;
            uint32_t size_;
            uint64_t pos_;
            uint64_t unitStart_ = 0;
            uint64_t lineStart_;
            uint64_t lastLead_;
            uint64_t prevStart_ = 0;
            uint64_t crPos_ = 0;
            uint32_t run_ = 0;
            uint32_t prev_ = 0;
            uint32_t first_ = 0;
            bool haveFirst_ = false;
            bool pendingCr_ = false;
        };
    }

    struct LineIndex::Impl
    {
        explicit Impl(EncodingInfo i) : unit(UnitOf(i)), scanner(unit, 0) { checkpoints.push_back({0, false}); }

        Unit unit;
        mutable std::shared_mutex mutex;
        Scanner scanner;
        std::vector<Checkpoint> checkpoints; // line k * kCheckpointEvery starts at checkpoints[k]
        uint64_t finished = 0;               // lines already terminated
        bool complete = false;
    };

    LineIndex::LineIndex(EncodingInfo info) : m_(std::make_unique<Impl>(info)) {}
    LineIndex::~LineIndex() = default;

    void LineIndex::Append(std::span<const uint8_t> chunk, bool isLast)
    {
        std::unique_lock lock(m_->mutex);
        if (m_->complete) return;
        auto onLine = [this](uint64_t, uint64_t nextBegin, bool nextContinuation) {
            if (++m_->finished % kCheckpointEvery == 0)
            {
                m_->checkpoints.push_back({nextBegin, nextContinuation});
            }
            return true;
        };
        m_->scanner.Feed(chunk, onLine);
        if (isLast)
        {
            m_->scanner.Finish(onLine);
            m_->complete = true;
        }
    }

    uint64_t LineIndex::LineCount() const
    {
        std::shared_lock lock(m_->mutex);
        return m_->finished + 1;
    }

    bool LineIndex::Complete() const
    {
        std::shared_lock lock(m_->mutex);
        return m_->complete;
    }

    LineIndex::LineSpan LineIndex::Line(uint64_t n, std::span<const uint8_t> content) const
    {
        uint64_t const size = content.size();
        Checkpoint cp;
        {
            std::shared_lock lock(m_->mutex);
            if (n > m_->finished) return {size, size, false};
            cp = m_->checkpoints[static_cast<size_t>(n / kCheckpointEvery)];
        }
        if (cp.offset > size) return {size, size, false};

        LineSpan span{cp.offset, size, cp.continuation};
        uint64_t remaining = n % kCheckpointEvery;
        bool found = false;
        auto onLine = [&](uint64_t end, uint64_t nextBegin, bool nextContinuation) {
            if (remaining == 0)
            {
                span.end = end;
                found = true;
                return false;
            }
            --remaining;
            span.begin = nextBegin;
            span.continuation = nextContinuation;
            return true;
        };
        Scanner scanner(m_->unit, cp.offset);
        if (scanner.Feed(content.subspan(static_cast<size_t>(cp.offset)), onLine) && scanner.Finish(onLine) && !found)
        {
            // The open last line runs to the end of the content (an odd trailing UTF-16 byte is dropped).
            uint64_t const tail = size - (std::min)(span.begin, size);
            span.end = m_->unit >= Unit::Utf16LE ? span.begin + (tail & ~uint64_t{1}) : size;
        }
        span.begin = (std::min)(span.begin, size);
        span.end = (std::max)(span.begin, (std::min)(span.end, size));
        return span;
    }

    uint64_t LineIndex::LineFromOffset(uint64_t byteOffset, std::span<const uint8_t> content) const
    {
        uint64_t line, last;
        Checkpoint cp;
        {
            std::shared_lock lock(m_->mutex);
            last = m_->finished;
            if (byteOffset >= content.size()) return last;
            auto it = std::upper_bound(m_->checkpoints.begin(), m_->checkpoints.end(), byteOffset,
                                       [](uint64_t off, Checkpoint const& c) { return off < c.offset; });
            size_t const k = static_cast<size_t>(it - m_->checkpoints.begin()) - 1;
            cp = m_->checkpoints[k];
            line = k * kCheckpointEvery;
        }
        if (cp.offset > content.size()) return last;

        auto onLine = [&](uint64_t, uint64_t nextBegin, bool) {
            if (nextBegin > byteOffset) return false;
            ++line;
            return true;
        };
        Scanner scanner(m_->unit, cp.offset);
        if (scanner.Feed(content.subspan(static_cast<size_t>(cp.offset)), onLine)) scanner.Finish(onLine);
        return (std::min)(line, last);
    }
}
