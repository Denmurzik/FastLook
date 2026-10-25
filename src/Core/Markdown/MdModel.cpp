#include "Markdown/MdModel.h"

#include <md4c.h>

#include <string_view>
#include <utility>

#include "Text/Utf.h"

namespace fastlook::core::md
{
    namespace
    {
        constexpr size_t kMaxBlockDepth = 64;

        struct NamedEntity
        {
            std::string_view name;
            wchar_t ch;
        };

        // md4c keeps its entity table private; this covers the entities that occur in real documents.
        constexpr NamedEntity kEntities[] = {
            {"amp", L'&'},        {"lt", L'<'},         {"gt", L'>'},         {"quot", L'"'},
            {"apos", L'\''},      {"nbsp", L' '},  {"copy", L'©'},  {"reg", L'®'},
            {"trade", L'™'}, {"mdash", L'—'}, {"ndash", L'–'}, {"hellip", L'…'},
            {"laquo", L'«'}, {"raquo", L'»'}, {"lsquo", L'‘'}, {"rsquo", L'’'},
            {"ldquo", L'“'}, {"rdquo", L'”'}, {"bull", L'•'},  {"middot", L'·'},
            {"deg", L'°'},   {"plusmn", L'±'}, {"times", L'×'}, {"divide", L'÷'},
            {"euro", L'€'},  {"pound", L'£'},  {"yen", L'¥'},   {"cent", L'¢'},
            {"sect", L'§'},  {"para", L'¶'},   {"larr", L'←'},  {"rarr", L'→'},
            {"uarr", L'↑'},  {"darr", L'↓'},   {"harr", L'↔'},  {"ne", L'≠'},
            {"le", L'≤'},    {"ge", L'≥'},     {"infin", L'∞'}, {"check", L'✓'},
        };

        void AppendCodePoint(std::wstring& out, unsigned long cp)
        {
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                cp = 0xFFFD;
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            }
            else
                out.push_back(static_cast<wchar_t>(cp));
        }

        // `raw` is the entity as written, e.g. "&amp;", "&#35;", "&#x44F;". Unknown names stay literal.
        void AppendEntity(std::wstring& out, std::string_view raw)
        {
            if (raw.size() >= 4 && raw.front() == '&' && raw.back() == ';' && raw[1] == '#')
            {
                bool hex = raw[2] == 'x' || raw[2] == 'X';
                unsigned long cp = 0;
                for (char c : raw.substr(hex ? 3 : 2, raw.size() - (hex ? 4 : 3)))
                {
                    unsigned digit;
                    if (c >= '0' && c <= '9')
                        digit = static_cast<unsigned>(c - '0');
                    else if (hex && c >= 'a' && c <= 'f')
                        digit = static_cast<unsigned>(c - 'a' + 10);
                    else if (hex && c >= 'A' && c <= 'F')
                        digit = static_cast<unsigned>(c - 'A' + 10);
                    else
                    {
                        cp = 0xFFFD;
                        break;
                    }
                    cp = cp * (hex ? 16 : 10) + digit;
                    if (cp > 0x10FFFF)
                    {
                        cp = 0xFFFD;
                        break;
                    }
                }
                AppendCodePoint(out, cp);
                return;
            }
            if (raw.size() >= 3 && raw.front() == '&' && raw.back() == ';')
            {
                auto name = raw.substr(1, raw.size() - 2);
                for (auto const& e : kEntities)
                    if (e.name == name)
                    {
                        out.push_back(e.ch);
                        return;
                    }
            }
            out += fastlook::core::FromUtf8(raw);
        }

        void AppendText(std::wstring& out, MD_TEXTTYPE type, std::string_view text)
        {
            switch (type)
            {
            case MD_TEXT_NULLCHAR:
                out.push_back(L'�');
                break;
            case MD_TEXT_BR:
                out.push_back(L'\n');
                break;
            case MD_TEXT_SOFTBR:
                out.push_back(L' ');
                break;
            case MD_TEXT_ENTITY:
                AppendEntity(out, text);
                break;
            default:
                out += fastlook::core::FromUtf8(text);
                break;
            }
        }

        std::wstring AttrToWide(MD_ATTRIBUTE const& attr)
        {
            std::wstring out;
            if (attr.text == nullptr)
                return out;
            for (int i = 0; attr.substr_offsets[i] < attr.size; ++i)
                AppendText(out, attr.substr_types[i],
                           std::string_view(attr.text + attr.substr_offsets[i], attr.substr_offsets[i + 1] - attr.substr_offsets[i]));
            return out;
        }

        Align ToAlign(MD_ALIGN a)
        {
            switch (a)
            {
            case MD_ALIGN_LEFT:
                return Align::Left;
            case MD_ALIGN_CENTER:
                return Align::Center;
            case MD_ALIGN_RIGHT:
                return Align::Right;
            default:
                return Align::Default;
            }
        }

        class Builder
        {
        public:
            Document Take()
            {
                // An aborted parse can leave open blocks; fold them so the partial tree is still usable.
                while (stack_.size() > 1)
                    Pop();
                if (!stack_.empty() && doc_.blocks.empty())
                    doc_.blocks = std::move(stack_.front().children);
                return std::move(doc_);
            }

            int EnterBlock(MD_BLOCKTYPE type, void* detail)
            {
                switch (type)
                {
                case MD_BLOCK_DOC:
                    stack_.clear();
                    stack_.emplace_back();
                    return 0;
                case MD_BLOCK_THEAD:
                    inHead_ = true;
                    return 0;
                case MD_BLOCK_TBODY:
                    return 0;
                default:
                    break;
                }

                if (stack_.size() > kMaxBlockDepth)
                {
                    ++flattened_;
                    return 0;
                }

                Block b;
                switch (type)
                {
                case MD_BLOCK_QUOTE:
                    b.type = BlockType::Quote;
                    break;
                case MD_BLOCK_UL:
                    b.type = BlockType::List;
                    break;
                case MD_BLOCK_OL:
                    b.type = BlockType::List;
                    b.ordered = true;
                    b.start = static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start;
                    break;
                case MD_BLOCK_LI:
                {
                    b.type = BlockType::ListItem;
                    auto* d = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
                    if (d->is_task)
                        b.checked = d->task_mark != ' ';
                    break;
                }
                case MD_BLOCK_HR:
                    b.type = BlockType::HRule;
                    break;
                case MD_BLOCK_H:
                    b.type = BlockType::Heading;
                    b.level = static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level);
                    break;
                case MD_BLOCK_CODE:
                    b.type = BlockType::Code;
                    b.lang = AttrToWide(static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->lang);
                    break;
                case MD_BLOCK_HTML:
                    b.type = BlockType::Html;
                    break;
                case MD_BLOCK_P:
                    b.type = BlockType::Paragraph;
                    break;
                case MD_BLOCK_TABLE:
                    b.type = BlockType::Table;
                    break;
                case MD_BLOCK_TR:
                    b.type = BlockType::TableRow;
                    b.header = inHead_;
                    break;
                case MD_BLOCK_TH:
                case MD_BLOCK_TD:
                    b.type = BlockType::TableCell;
                    b.header = type == MD_BLOCK_TH;
                    b.align = ToAlign(static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align);
                    break;
                default:
                    break;
                }
                stack_.push_back(std::move(b));
                canMerge_ = false;
                return 0;
            }

            int LeaveBlock(MD_BLOCKTYPE type)
            {
                switch (type)
                {
                case MD_BLOCK_DOC:
                    doc_.blocks = std::move(stack_.front().children);
                    return 0;
                case MD_BLOCK_THEAD:
                    inHead_ = false;
                    return 0;
                case MD_BLOCK_TBODY:
                    return 0;
                default:
                    break;
                }
                if (flattened_ > 0)
                {
                    --flattened_;
                    return 0;
                }
                Pop();
                return 0;
            }

            int EnterSpan(MD_SPANTYPE type, void* detail)
            {
                if (inImage_)
                    return 0; // alt text is flattened
                canMerge_ = false;
                uint8_t flag = 0;
                switch (type)
                {
                case MD_SPAN_EM:
                    flag = static_cast<uint8_t>(SpanFlag::Italic);
                    break;
                case MD_SPAN_STRONG:
                    flag = static_cast<uint8_t>(SpanFlag::Bold);
                    break;
                case MD_SPAN_DEL:
                    flag = static_cast<uint8_t>(SpanFlag::Strike);
                    break;
                case MD_SPAN_CODE:
                    flag = static_cast<uint8_t>(SpanFlag::Code);
                    break;
                case MD_SPAN_A:
                    flag = static_cast<uint8_t>(SpanFlag::Link);
                    break;
                default:
                    break; // images are handled below; wiki links, latex math and underline are not enabled
                }

                std::wstring href;
                if (type == MD_SPAN_A)
                    href = AttrToWide(static_cast<MD_SPAN_A_DETAIL*>(detail)->href);
                spanStack_.push_back({flag, std::move(href)});

                if (type == MD_SPAN_IMG)
                {
                    Span img;
                    img.flags = CurrentFlags() | static_cast<uint8_t>(SpanFlag::Image);
                    img.href = AttrToWide(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
                    stack_.back().spans.push_back(std::move(img));
                    inImage_ = true;
                }
                return 0;
            }

            int LeaveSpan(MD_SPANTYPE type)
            {
                if (inImage_)
                {
                    if (type != MD_SPAN_IMG)
                        return 0;
                    inImage_ = false;
                }
                if (!spanStack_.empty())
                    spanStack_.pop_back();
                canMerge_ = false;
                return 0;
            }

            int Text(MD_TEXTTYPE type, std::string_view text)
            {
                Block& cur = stack_.back();
                if (cur.type == BlockType::Code || cur.type == BlockType::Html)
                {
                    AppendText(cur.text, type, text);
                    return 0;
                }
                if (inImage_)
                {
                    AppendText(cur.spans.back().text, type, text);
                    return 0;
                }

                if (!canMerge_ || cur.spans.empty())
                {
                    Span s;
                    s.flags = CurrentFlags();
                    s.href = CurrentHref();
                    cur.spans.push_back(std::move(s));
                    canMerge_ = true;
                }
                AppendText(cur.spans.back().text, type, text);
                return 0;
            }

        private:
            struct SpanState
            {
                uint8_t flag;
                std::wstring href;
            };

            void Pop()
            {
                Block done = std::move(stack_.back());
                stack_.pop_back();
                stack_.back().children.push_back(std::move(done));
                canMerge_ = false;
            }

            uint8_t CurrentFlags() const
            {
                uint8_t f = 0;
                for (auto const& s : spanStack_)
                    f |= s.flag;
                return f;
            }

            std::wstring CurrentHref() const
            {
                for (auto it = spanStack_.rbegin(); it != spanStack_.rend(); ++it)
                    if (it->flag == static_cast<uint8_t>(SpanFlag::Link))
                        return it->href;
                return {};
            }

            Document doc_;
            std::vector<Block> stack_; // [0] is a synthetic root
            std::vector<SpanState> spanStack_;
            size_t flattened_ = 0;
            bool inHead_ = false;
            bool inImage_ = false;
            bool canMerge_ = false;
        };

        Builder& B(void* u)
        {
            return *static_cast<Builder*>(u);
        }
    }

    Document Parse(std::string_view utf8)
    {
        Builder builder;
        MD_PARSER parser{};
        parser.abi_version = 0;
        parser.flags = MD_DIALECT_GITHUB;
        parser.enter_block = [](MD_BLOCKTYPE t, void* d, void* u) { return B(u).EnterBlock(t, d); };
        parser.leave_block = [](MD_BLOCKTYPE t, void*, void* u) { return B(u).LeaveBlock(t); };
        parser.enter_span = [](MD_SPANTYPE t, void* d, void* u) { return B(u).EnterSpan(t, d); };
        parser.leave_span = [](MD_SPANTYPE t, void*, void* u) { return B(u).LeaveSpan(t); };
        parser.text = [](MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n, void* u) {
            return B(u).Text(t, std::string_view(s, n));
        };
        md_parse(utf8.data(), static_cast<MD_SIZE>(utf8.size()), &parser, &builder);
        return builder.Take();
    }
}
