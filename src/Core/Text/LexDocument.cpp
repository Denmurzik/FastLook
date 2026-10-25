#include "Text/LexDocument.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

namespace fastlook::core
{
    namespace
    {
        constexpr int kUtf8 = 65001;
        constexpr int kSciLevelBase = 0x400; // SC_FOLDLEVELBASE

        int Utf8SequenceLength(uint8_t lead)
        {
            if (lead >= 0xF0 && lead < 0xF8) return 4;
            if (lead >= 0xE0) return lead < 0xF0 ? 3 : 1;
            if (lead >= 0xC2) return lead < 0xE0 ? 2 : 1;
            return 1;
        }
    }

    LexDocument::LexDocument(std::span<const uint8_t> bytes, int codePage) : bytes_(bytes), codePage_(codePage), styles_(bytes.size(), 0)
    {
        lineStarts_.push_back(0);
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            uint8_t const c = bytes[i];
            if (c == '\n' || (c == '\r' && !(i + 1 < bytes.size() && bytes[i + 1] == '\n')))
            {
                lineStarts_.push_back(static_cast<uint32_t>(i + 1));
            }
        }
        lineStates_.assign(lineStarts_.size(), 0);
    }

    uint64_t LexDocument::LineStartOffset(uint64_t line) const
    {
        return line < lineStarts_.size() ? lineStarts_[static_cast<size_t>(line)] : bytes_.size();
    }

    uint64_t LexDocument::LineOfOffset(uint64_t position) const
    {
        if (position >= bytes_.size()) return lineStarts_.size() - 1;
        auto const it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), position);
        return static_cast<uint64_t>(it - lineStarts_.begin()) - 1;
    }

    void LexDocument::GetCharRange(char* buffer, Sci_Position position, Sci_Position lengthRetrieve) const
    {
        if (position < 0 || lengthRetrieve <= 0 || static_cast<uint64_t>(position) >= bytes_.size()) return;
        size_t const count = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(lengthRetrieve), bytes_.size() - static_cast<uint64_t>(position)));
        memcpy(buffer, bytes_.data() + position, count);
    }

    Sci_Position LexDocument::LineFromPosition(Sci_Position position) const
    {
        return static_cast<Sci_Position>(LineOfOffset(position < 0 ? 0 : static_cast<uint64_t>(position)));
    }

    Sci_Position LexDocument::LineStart(Sci_Position line) const
    {
        return static_cast<Sci_Position>(LineStartOffset(line < 0 ? 0 : static_cast<uint64_t>(line)));
    }

    Sci_Position LexDocument::LineEnd(Sci_Position line) const
    {
        if (!InRange(line)) return static_cast<Sci_Position>(bytes_.size());
        uint64_t end = static_cast<size_t>(line) + 1 < lineStarts_.size() ? lineStarts_[static_cast<size_t>(line) + 1] : bytes_.size();
        uint64_t const start = lineStarts_[static_cast<size_t>(line)];
        while (end > start && (bytes_[static_cast<size_t>(end) - 1] == '\n' || bytes_[static_cast<size_t>(end) - 1] == '\r'))
        {
            --end;
        }
        return static_cast<Sci_Position>(end);
    }

    int LexDocument::GetLevel(Sci_Position) const
    {
        return kSciLevelBase;
    }

    int LexDocument::SetLevel(Sci_Position, int)
    {
        return kSciLevelBase;
    }

    int LexDocument::GetLineState(Sci_Position line) const
    {
        return InRange(line) ? lineStates_[static_cast<size_t>(line)] : 0;
    }

    int LexDocument::SetLineState(Sci_Position line, int state)
    {
        if (!InRange(line)) return 0;
        int const previous = lineStates_[static_cast<size_t>(line)];
        lineStates_[static_cast<size_t>(line)] = state;
        return previous;
    }

    bool LexDocument::SetStyleFor(Sci_Position length, char style)
    {
        if (length < 0 || stylePosition_ < 0 || static_cast<uint64_t>(stylePosition_) + static_cast<uint64_t>(length) > styles_.size()) return false;
        std::fill_n(styles_.begin() + stylePosition_, length, static_cast<uint8_t>(style));
        stylePosition_ += length;
        return true;
    }

    bool LexDocument::SetStyles(Sci_Position length, const char* styles)
    {
        if (length < 0 || stylePosition_ < 0 || static_cast<uint64_t>(stylePosition_) + static_cast<uint64_t>(length) > styles_.size()) return false;
        memcpy(styles_.data() + stylePosition_, styles, static_cast<size_t>(length));
        stylePosition_ += length;
        return true;
    }

    bool LexDocument::IsDBCSLeadByte(char ch) const
    {
        if (codePage_ == kUtf8 || codePage_ == 0) return false;
        return IsDBCSLeadByteEx(static_cast<UINT>(codePage_), static_cast<BYTE>(ch)) != FALSE;
    }

    int LexDocument::GetLineIndentation(Sci_Position line)
    {
        if (!InRange(line)) return 0;
        int width = 0;
        for (uint64_t i = lineStarts_[static_cast<size_t>(line)]; i < bytes_.size(); ++i)
        {
            if (bytes_[static_cast<size_t>(i)] == ' ') ++width;
            else if (bytes_[static_cast<size_t>(i)] == '\t') width = (width / 8 + 1) * 8;
            else break;
        }
        return width;
    }

    int LexDocument::CharWidth(Sci_Position position) const
    {
        if (position < 0 || static_cast<uint64_t>(position) >= bytes_.size()) return 1;
        uint8_t const lead = bytes_[static_cast<size_t>(position)];
        int width = 1;
        if (codePage_ == kUtf8) width = Utf8SequenceLength(lead);
        else if (IsDBCSLeadByte(static_cast<char>(lead))) width = 2;
        if (static_cast<uint64_t>(position) + static_cast<uint64_t>(width) > bytes_.size()) return 1;
        if (codePage_ == kUtf8)
        {
            for (int k = 1; k < width; ++k)
            {
                if ((bytes_[static_cast<size_t>(position) + k] & 0xC0) != 0x80) return 1;
            }
        }
        return width;
    }

    Sci_Position LexDocument::GetRelativePosition(Sci_Position positionStart, Sci_Position characterOffset) const
    {
        Sci_Position const length = static_cast<Sci_Position>(bytes_.size());
        Sci_Position position = positionStart;
        for (; characterOffset > 0; --characterOffset)
        {
            if (position >= length) return length;
            position += CharWidth(position);
        }
        for (; characterOffset < 0; ++characterOffset)
        {
            if (position <= 0) return 0;
            --position;
            // Step over continuation bytes (UTF-8) or a trail byte that follows a DBCS lead byte.
            if (codePage_ == kUtf8)
            {
                int back = 0;
                while (position > 0 && back < 3 && (bytes_[static_cast<size_t>(position)] & 0xC0) == 0x80)
                {
                    --position;
                    ++back;
                }
                if (CharWidth(position) != back + 1) position += back; // not one sequence: the byte itself is the character
            }
            else if (position > 0 && IsDBCSLeadByte(static_cast<char>(bytes_[static_cast<size_t>(position) - 1])))
            {
                --position;
            }
        }
        return position;
    }

    int LexDocument::GetCharacterAndWidth(Sci_Position position, Sci_Position* pWidth) const
    {
        if (position < 0 || static_cast<uint64_t>(position) >= bytes_.size())
        {
            if (pWidth) *pWidth = 1;
            return 0;
        }
        int const width = CharWidth(position);
        if (pWidth) *pWidth = width;
        uint8_t const* const p = bytes_.data() + position;
        if (width == 1) return codePage_ == kUtf8 && p[0] >= 0x80 ? 0xDC00 + p[0] : p[0]; // lone high byte: invalid UTF-8
        if (codePage_ != kUtf8) return (p[0] << 8) | p[1];
        uint32_t cp = width == 2 ? (p[0] & 0x1Fu) : width == 3 ? (p[0] & 0x0Fu) : (p[0] & 0x07u);
        for (int k = 1; k < width; ++k) cp = (cp << 6) | (p[k] & 0x3Fu);
        return static_cast<int>(cp);
    }
}
