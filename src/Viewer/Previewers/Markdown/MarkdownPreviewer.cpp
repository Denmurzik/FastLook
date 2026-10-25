#include "pch.h"
#include "Previewers/Markdown/MarkdownPreviewer.h"

#include <algorithm>
#include <limits>
#include <span>

#include <winrt/Windows.UI.h>

#include "Diagnostics/Log.h"
#include "Previewers/Common/ShellInfo.h"
#include "Previewers/Factories.h"
#include "Previewers/Text/LexillaStyler.h"
#include "Text/Utf.h"
#include "Ui/Placement.h"
#include "Util/Async.h"
#include "Util/Clipboard.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        constexpr uint64_t kMaxRenderedBytes = 2ull * 1024 * 1024; // larger files open as source
        constexpr size_t kMaxRenderedBlocks = 4000;                // XAML elements are heavy: bigger documents open as source
        constexpr size_t kMaxRenderedSpans = 40000;                // one Run/Hyperlink each; keeps the UI-thread build bounded

        struct Weight
        {
            size_t blocks = 0, spans = 0;
        };

        void Count(std::vector<core::md::Block> const& blocks, Weight& w)
        {
            w.blocks += blocks.size();
            for (auto const& block : blocks)
            {
                w.spans += block.spans.size();
                Count(block.children, w);
            }
        }

        bool ReadAll(std::filesystem::path const& path, std::vector<uint8_t>& out)
        {
            HANDLE const file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            LARGE_INTEGER size{};
            bool ok = GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) <= kMaxRenderedBytes;
            if (ok)
            {
                out.resize(static_cast<size_t>(size.QuadPart));
                DWORD read = 0;
                ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
            }
            CloseHandle(file);
            return ok;
        }

    }

    MarkdownPreviewer::~MarkdownPreviewer() = default;

    winrt::Windows::Foundation::IAsyncAction MarkdownPreviewer::PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx)
    {
        auto self = std::static_pointer_cast<MarkdownPreviewer>(shared_from_this());
        auto cancel = co_await winrt::get_cancellation_token();
        std::stop_source stop;
        cancel.callback([stop]() mutable { stop.request_stop(); });
        m_ctx = ctx;
        auto const ui = ctx->ui;
        std::filesystem::path const path{item.ref.path};
        m_baseDir = path.parent_path();

        co_await winrt::resume_background();
        std::shared_ptr<core::TextDocument> doc = core::TextDocument::Open(path);
        if (cancel())
        {
            co_return;
        }
        if (!doc)
        {
            throw PreviewError{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
        }
        if (doc->Encoding().encoding == core::TextEncoding::Unsupported)
        {
            throw PreviewError{std::wstring{Str(L"Markdown", L"UnsupportedEncoding")}, std::nullopt};
        }

        // The rendered path never needs the line index: the text is read straight from the file and the index is built when the
        // source view is first shown.
        core::md::Document parsed;
        bool sourceOnly = doc->SizeBytes() > kMaxRenderedBytes;
        if (!sourceOnly)
        {
            std::vector<uint8_t> bytes;
            if (!ReadAll(path, bytes))
            {
                throw PreviewError{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
            }
            auto const info = doc->Encoding();
            std::span<const uint8_t> body{bytes};
            body = body.subspan(std::min<size_t>(info.bomLength, body.size()));
            std::string utf8;
            if (info.encoding == core::TextEncoding::Utf8)
            {
                utf8.assign(reinterpret_cast<char const*>(body.data()), body.size());
            }
            else
            {
                utf8 = core::ToUtf8(core::DecodeToUtf16(body, info));
            }
            parsed = core::md::Parse(utf8);
            Weight weight;
            Count(parsed.blocks, weight);
            sourceOnly = weight.blocks > kMaxRenderedBlocks || weight.spans > kMaxRenderedSpans;
        }
        if (cancel())
        {
            co_return;
        }
        if (sourceOnly)
        {
            doc->BuildIndex(stop.get_token(), [](uint64_t, bool) {});
            if (cancel())
            {
                co_return;
            }
            if (doc->Faulted())
            {
                throw PreviewError{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
            }
        }

        m_doc = doc;
        m_styler = MakeLexillaStyler(doc, "markdown");
        m_markdown = sourceOnly ? core::md::Document{} : std::move(parsed);
        m_sourceOnly = sourceOnly;

        if (!co_await ResumeOn(ui))
        {
            co_return;
        }
    }

    winrt::Microsoft::UI::Xaml::UIElement MarkdownPreviewer::View()
    {
        m_root = winrt::Microsoft::UI::Xaml::Controls::Grid{};
        ShowMode(m_sourceOnly || m_ctx->settings.preview.markdownView == core::MarkdownView::Source);
        return m_root;
    }

    winrt::Windows::Foundation::Size MarkdownPreviewer::PreferredSize() const
    {
        return {static_cast<float>(core::kDefaultTextSize.w), static_cast<float>(core::kDefaultTextSize.h)};
    }

    void MarkdownPreviewer::Unload()
    {
        m_renderStop.request_stop();
        m_renderStop = {};
        if (m_images)
        {
            m_images->Close();
            m_images.reset();
        }
        m_toggle = nullptr;
        if (m_root)
        {
            m_root.Children().Clear();
        }
        m_rendered = nullptr;
        m_root = nullptr;
        m_code.reset();
        m_styler.reset(); // stops its worker
        m_doc.reset();
        m_markdown = {};
    }

    void MarkdownPreviewer::ShowMode(bool source)
    {
        if (!m_root)
        {
            return;
        }
        m_source = source;
        winrt::Microsoft::UI::Xaml::UIElement view{nullptr};
        if (source)
        {
            if (!m_code)
            {
                if (!m_doc->IndexComplete())
                {
                    m_doc->BuildIndex({}, [](uint64_t, bool) {}); // at most 2 MiB here (larger files were indexed in PrepareAsync)
                }
                m_code = std::make_unique<CodeView>(m_doc);
                m_code->SetStyler(m_styler);
                m_code->SetDark(m_ctx->dark);
                m_code->SetFontSize(static_cast<float>(m_ctx->settings.preview.codeFontSize));
                m_code->SetWrap(m_ctx->settings.preview.wordWrap);
                m_code->OnIndexProgress(m_doc->LineCount(), m_doc->IndexComplete());
            }
            view = m_code->Root();
        }
        else
        {
            if (!m_rendered)
            {
                m_rendered = RenderMarkdown(m_markdown, m_baseDir, m_ctx->dark, m_renderStop.get_token(), &m_images)
                                 .as<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>();
                m_markdown = {}; // the XAML tree holds everything now
            }
            view = m_rendered;
        }
        m_root.Children().Clear();
        m_root.Children().Append(view);
        if (m_toggle)
        {
            m_toggle.Content(winrt::box_value(Str(L"Markdown", source ? L"Rendered" : L"Source")));
        }
    }

    void MarkdownPreviewer::ToggleMode()
    {
        if (!m_sourceOnly)
        {
            ShowMode(!m_source);
        }
    }

    std::vector<winrt::Microsoft::UI::Xaml::UIElement> MarkdownPreviewer::ToolbarItems()
    {
        using namespace winrt::Microsoft::UI::Xaml;
        if (m_sourceOnly)
        {
            return {};
        }
        Controls::Button button;
        button.Content(winrt::box_value(Str(L"Markdown", m_source ? L"Rendered" : L"Source")));
        button.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
        button.BorderThickness({0, 0, 0, 0});
        button.Padding({10, 6, 10, 6});
        button.VerticalAlignment(VerticalAlignment::Center);
        button.IsTabStop(false);
        std::weak_ptr<IPreviewer> const weak = shared_from_this();
        button.Click([weak](auto const&, auto const&) {
            if (auto const previewer = weak.lock())
            {
                static_cast<MarkdownPreviewer&>(*previewer).ToggleMode();
            }
        });
        m_toggle = button;
        return {button};
    }

    bool MarkdownPreviewer::CopySelection()
    {
        if (!m_code || !m_code->HasSelection())
        {
            return false;
        }
        bool truncated = false;
        std::wstring const text = m_code->SelectedText(&truncated);
        if (SetClipboardText(m_ctx ? m_ctx->hwnd : nullptr, text) && truncated)
        {
            m_code->ShowNotice(FormatPattern(Str(L"Markdown", L"CopyTruncated"), {FormatNumber(text.size())}));
        }
        return true;
    }

    bool MarkdownPreviewer::ScrollRendered(winrt::Windows::System::VirtualKey key)
    {
        using winrt::Windows::System::VirtualKey;
        if (!m_rendered)
        {
            return false;
        }
        double const page = std::max(40.0, m_rendered.ViewportHeight() * 0.9);
        double const offset = m_rendered.VerticalOffset();
        switch (key)
        {
        case VirtualKey::PageDown:
            m_rendered.ScrollToVerticalOffset(offset + page);
            return true;
        case VirtualKey::PageUp:
            m_rendered.ScrollToVerticalOffset(std::max(0.0, offset - page));
            return true;
        case VirtualKey::Home:
            m_rendered.ScrollToVerticalOffset(0);
            return true;
        case VirtualKey::End:
            m_rendered.ScrollToVerticalOffset(m_rendered.ScrollableHeight());
            return true;
        default:
            return false;
        }
    }

    bool MarkdownPreviewer::OnKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift)
    {
        using winrt::Windows::System::VirtualKey;
        if (!m_source)
        {
            if (ctrl && !shift && key == VirtualKey::C)
            {
                // Selected text of the rendered view; without a selection the window copies the file.
                if (m_images)
                {
                    std::wstring const text{m_images->SelectedText()};
                    if (!text.empty())
                    {
                        SetClipboardText(m_ctx ? m_ctx->hwnd : nullptr, text);
                        return true;
                    }
                }
                return false;
            }
            return !ctrl && !shift && ScrollRendered(key);
        }
        if (!m_code)
        {
            return false;
        }
        if (ctrl && !shift && key == VirtualKey::A)
        {
            m_code->SelectAll();
            return true;
        }
        if (ctrl && !shift && key == VirtualKey::C)
        {
            return CopySelection(); // without a selection the window copies the file
        }
        return m_code->OnScrollKey(key, ctrl, shift);
    }

    bool MarkdownPreviewer::Execute(std::wstring_view command)
    {
        using winrt::Windows::System::VirtualKey;
        if (command == L"source")
        {
            if (m_sourceOnly || !m_root)
            {
                return false;
            }
            ToggleMode();
            return true;
        }
        VirtualKey key{};
        if (command == L"pageup") key = VirtualKey::PageUp;
        else if (command == L"pagedown") key = VirtualKey::PageDown;
        else if (command == L"home") key = VirtualKey::Home;
        else if (command == L"end") key = VirtualKey::End;
        else return false;
        return OnKey(key, false, false);
    }

    void MarkdownPreviewer::Zoom(ZoomCommand command)
    {
        if (!m_source || !m_code)
        {
            return;
        }
        switch (command)
        {
        case ZoomCommand::In:
            m_code->ZoomSteps(1);
            break;
        case ZoomCommand::Out:
            m_code->ZoomSteps(-1);
            break;
        case ZoomCommand::Reset:
            m_code->ResetZoom();
            break;
        }
    }

    std::shared_ptr<IPreviewer> CreateMarkdownPreviewer()
    {
        return std::make_shared<MarkdownPreviewer>();
    }
}
