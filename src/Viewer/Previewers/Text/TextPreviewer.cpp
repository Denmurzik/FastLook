#include "pch.h"
#include "Previewers/Text/TextPreviewer.h"

#include <chrono>

#include <winrt/Windows.UI.h>

#include "Diagnostics/Log.h"
#include "Previewers/Factories.h"
#include "Ui/Placement.h"
#include "Util/Async.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace std::chrono_literals;

        bool SetClipboardText(HWND owner, std::wstring const& text)
        {
            if (!OpenClipboard(owner))
            {
                FL_LOG_INFO(L"Copying text: OpenClipboard failed, error {}", GetLastError());
                return false;
            }
            bool ok = false;
            if (EmptyClipboard())
            {
                size_t const bytes = (text.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL const memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
                {
                    if (void* const data = GlobalLock(memory))
                    {
                        memcpy(data, text.c_str(), bytes);
                        GlobalUnlock(memory);
                        ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
                    }
                    if (!ok)
                    {
                        GlobalFree(memory);
                    }
                }
            }
            CloseClipboard();
            return ok;
        }
    }

    TextPreviewer::~TextPreviewer()
    {
        StopIndexing();
    }

    winrt::Windows::Foundation::IAsyncAction TextPreviewer::PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx)
    {
        auto self = std::static_pointer_cast<TextPreviewer>(shared_from_this());
        auto cancel = co_await winrt::get_cancellation_token();
        m_ctx = ctx;
        auto const ui = ctx->ui;
        std::wstring const path = item.ref.path;

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
            throw PreviewError{std::wstring{Str(L"Text", L"UnsupportedEncoding")}, std::nullopt};
        }

        auto state = std::make_shared<IndexState>();
        std::weak_ptr<IPreviewer> const weak = self;
        m_doc = doc;
        m_index = state;
        m_indexThread = std::jthread([doc, state, weak, ui](std::stop_token stop) {
            try
            {
                doc->BuildIndex(stop, [&](uint64_t lines, bool complete) {
                    state->lines = lines;
                    state->complete = complete;
                    {
                        std::lock_guard lock{state->mutex};
                        state->firstReported = true;
                    }
                    state->changed.notify_all();
                    if (!state->uiPending.exchange(true))
                    {
                        if (!ui.TryEnqueue([weak, state] {
                                state->uiPending = false;
                                if (auto const previewer = weak.lock())
                                {
                                    static_cast<TextPreviewer&>(*previewer).DrainProgress();
                                }
                            }))
                        {
                            state->uiPending = false;
                        }
                    }
                });
            }
            catch (...)
            {
                FL_LOG_INFO(L"Indexing the text file failed");
            }
            {
                std::lock_guard lock{state->mutex};
                state->firstReported = true;
            }
            state->changed.notify_all();
        });

        // The first chunk (4 MiB, well past the 64 KiB the first screen needs) is indexed in milliseconds.
        {
            std::unique_lock lock{state->mutex};
            while (!state->firstReported && !cancel())
            {
                state->changed.wait_for(lock, 20ms);
            }
        }
        if (cancel())
        {
            co_return;
        }
        if (doc->Faulted())
        {
            throw PreviewError{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
        }

        if (!co_await ResumeOn(ui))
        {
            co_return;
        }
    }

    winrt::Microsoft::UI::Xaml::UIElement TextPreviewer::View()
    {
        m_view = std::make_unique<CodeView>(m_doc);
        m_view->SetDark(m_ctx->dark);
        m_view->SetFontSize(static_cast<float>(m_ctx->settings.preview.codeFontSize));
        m_view->SetWrap(m_ctx->settings.preview.wordWrap);
        DrainProgress();
        return m_view->Root();
    }

    winrt::Windows::Foundation::Size TextPreviewer::PreferredSize() const
    {
        return {static_cast<float>(core::kDefaultTextSize.w), static_cast<float>(core::kDefaultTextSize.h)};
    }

    void TextPreviewer::StopIndexing()
    {
        if (m_indexThread.joinable())
        {
            m_indexThread.request_stop();
            m_indexThread.join(); // BuildIndex checks the stop token between 4 MiB chunks
        }
    }

    void TextPreviewer::Unload()
    {
        StopIndexing();
        m_wrapButton = nullptr;
        m_view.reset();
        m_index.reset();
        m_doc.reset();
    }

    void TextPreviewer::DrainProgress()
    {
        if (m_view && m_index)
        {
            m_view->OnIndexProgress(m_index->lines, m_index->complete);
        }
    }

    std::vector<winrt::Microsoft::UI::Xaml::UIElement> TextPreviewer::ToolbarItems()
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton;

        ToggleButton button;
        button.Content(winrt::box_value(Str(L"Text", L"Wrap")));
        button.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
        button.BorderThickness({0, 0, 0, 0});
        button.Padding({10, 6, 10, 6});
        button.VerticalAlignment(VerticalAlignment::Center);
        button.IsTabStop(false);
        button.IsChecked(m_view && m_view->Wrap());
        Controls::ToolTipService::SetToolTip(button, winrt::box_value(Str(L"Text", L"Wrap")));
        button.Click([this](auto const&, auto const&) { ToggleWrap(); });
        m_wrapButton = button;
        return {button};
    }

    void TextPreviewer::ToggleWrap()
    {
        if (!m_view)
        {
            return;
        }
        m_view->SetWrap(!m_view->Wrap());
        if (m_wrapButton)
        {
            m_wrapButton.IsChecked(m_view->Wrap());
        }
    }

    bool TextPreviewer::CopySelection()
    {
        if (!m_view || !m_view->HasSelection())
        {
            return false;
        }
        SetClipboardText(m_ctx ? m_ctx->hwnd : nullptr, m_view->SelectedText());
        return true;
    }

    bool TextPreviewer::OnKey(winrt::Windows::System::VirtualKey key, bool ctrl, bool shift)
    {
        using winrt::Windows::System::VirtualKey;
        if (!m_view)
        {
            return false;
        }
        if (ctrl && !shift && key == VirtualKey::A)
        {
            m_view->SelectAll();
            return true;
        }
        if (ctrl && !shift && key == VirtualKey::C)
        {
            return CopySelection(); // without a selection the window copies the file
        }
        return m_view->OnScrollKey(key, ctrl, shift);
    }

    bool TextPreviewer::Execute(std::wstring_view command)
    {
        if (!m_view)
        {
            return false;
        }
        if (command == L"wrap")
        {
            ToggleWrap();
            return true;
        }
        if (command == L"selectall")
        {
            m_view->SelectAll();
            return true;
        }
        if (command == L"copyselection")
        {
            return CopySelection();
        }
        // Scrolling by script (UI verification): cmd:pageup / pagedown / home / end.
        using winrt::Windows::System::VirtualKey;
        if (command == L"pageup") return m_view->OnScrollKey(VirtualKey::PageUp, false, false);
        if (command == L"pagedown") return m_view->OnScrollKey(VirtualKey::PageDown, false, false);
        if (command == L"home") return m_view->OnScrollKey(VirtualKey::Home, false, false);
        if (command == L"end") return m_view->OnScrollKey(VirtualKey::End, false, false);
        return false;
    }

    void TextPreviewer::Zoom(ZoomCommand command)
    {
        if (!m_view)
        {
            return;
        }
        float const size = m_view->FontSize();
        switch (command)
        {
        case ZoomCommand::In:
            m_view->ZoomBy((size + 1.0f) / size);
            break;
        case ZoomCommand::Out:
            m_view->ZoomBy(std::max(1.0f, size - 1.0f) / size);
            break;
        case ZoomCommand::Reset:
            m_view->ResetZoom();
            break;
        }
    }

    std::shared_ptr<IPreviewer> CreateTextPreviewer()
    {
        return std::make_shared<TextPreviewer>();
    }
}
