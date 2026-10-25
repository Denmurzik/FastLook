#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <ILexer.h>

namespace fastlook::core
{
    // Scintilla::IDocument over a read-only byte span: what a Lexilla lexer needs to style it. Keeps one style byte per
    // text byte plus per-line state (fold levels are not kept: nothing folds). Lines end at CR, LF or CRLF. Single-threaded, except that style bytes
    // below a position the owner has published may be read by another thread while later bytes are being written.
    class LexDocument final : public Scintilla::IDocument
    {
    public:
        LexDocument(std::span<const uint8_t> bytes, int codePage);

        uint8_t Style(uint64_t position) const { return position < styles_.size() ? styles_[static_cast<size_t>(position)] : 0; }
        uint64_t Size() const { return bytes_.size(); }
        uint64_t LineCount() const { return lineStarts_.size(); }
        uint64_t LineStartOffset(uint64_t line) const;
        uint64_t LineOfOffset(uint64_t position) const;

        int SCI_METHOD Version() const override { return Scintilla::dvRelease4; }
        void SCI_METHOD SetErrorStatus(int) override {}
        Sci_Position SCI_METHOD Length() const override { return static_cast<Sci_Position>(bytes_.size()); }
        void SCI_METHOD GetCharRange(char* buffer, Sci_Position position, Sci_Position lengthRetrieve) const override;
        char SCI_METHOD StyleAt(Sci_Position position) const override { return static_cast<char>(Style(static_cast<uint64_t>(position))); }
        Sci_Position SCI_METHOD LineFromPosition(Sci_Position position) const override;
        Sci_Position SCI_METHOD LineStart(Sci_Position line) const override;
        int SCI_METHOD GetLevel(Sci_Position line) const override;
        int SCI_METHOD SetLevel(Sci_Position line, int level) override;
        int SCI_METHOD GetLineState(Sci_Position line) const override;
        int SCI_METHOD SetLineState(Sci_Position line, int state) override;
        void SCI_METHOD StartStyling(Sci_Position position) override { stylePosition_ = position; }
        bool SCI_METHOD SetStyleFor(Sci_Position length, char style) override;
        bool SCI_METHOD SetStyles(Sci_Position length, const char* styles) override;
        void SCI_METHOD DecorationSetCurrentIndicator(int) override {}
        void SCI_METHOD DecorationFillRange(Sci_Position, int, Sci_Position) override {}
        void SCI_METHOD ChangeLexerState(Sci_Position, Sci_Position) override {}
        int SCI_METHOD CodePage() const override { return codePage_; }
        bool SCI_METHOD IsDBCSLeadByte(char ch) const override;
        const char* SCI_METHOD BufferPointer() override { return reinterpret_cast<const char*>(bytes_.data()); }
        int SCI_METHOD GetLineIndentation(Sci_Position line) override;
        Sci_Position SCI_METHOD LineEnd(Sci_Position line) const override;
        Sci_Position SCI_METHOD GetRelativePosition(Sci_Position positionStart, Sci_Position characterOffset) const override;
        int SCI_METHOD GetCharacterAndWidth(Sci_Position position, Sci_Position* pWidth) const override;

    private:
        bool InRange(Sci_Position line) const { return line >= 0 && static_cast<uint64_t>(line) < lineStarts_.size(); }
        int CharWidth(Sci_Position position) const; // bytes of the character starting at `position`

        std::span<const uint8_t> bytes_;
        int codePage_;
        std::vector<uint8_t> styles_;
        std::vector<uint32_t> lineStarts_; // always has at least one entry; a buffer is below 4 GiB
        std::vector<int> lineStates_;
        Sci_Position stylePosition_ = 0;
    };
}
