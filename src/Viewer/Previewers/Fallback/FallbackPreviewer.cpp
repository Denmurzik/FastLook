#include "pch.h"
#include "Previewers/Factories.h"

#include "Diagnostics/Log.h"
#include "FileTypes/StoreLinks.h"
#include "Previewers/Common/InfoCard.h"
#include "Previewers/Common/ShellInfo.h"
#include "Util/Async.h"
#include "Util/Str.h"
#include "WindowActions.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;

        constexpr int kIconDip = 256;

        class FallbackPreviewer final : public IPreviewer
        {
        public:
            winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override
            {
                auto self = shared_from_this();
                auto cancel = co_await winrt::get_cancellation_token();
                auto const ui = ctx->ui;

                // Never reads the file content: attributes and names only; the image comes from the shell's cache or icon.
                co_await winrt::resume_background();
                ShellInfo const info = QueryShellInfo(item.ref, item.cloudPlaceholder);
                if (cancel())
                {
                    co_return;
                }

                if (!co_await ResumeOn(ui))
                {
                    co_return;
                }
                if (item.cloudPlaceholder)
                {
                    FL_LOG_DEBUG(L"placeholder: no content read");
                }
                auto const image = co_await LoadThumbnailAsync(item.ref, static_cast<int>(kIconDip * ctx->scale), item.cloudPlaceholder);
                if (cancel() || !co_await ResumeOn(ui))
                {
                    co_return;
                }

                InfoCardModel model;
                model.image = image;
                model.imageSizeDip = kIconDip;
                model.title = winrt::hstring{info.displayName};
                if (ctx->fallback)
                {
                    model.message = winrt::hstring{ctx->fallback->message};
                }
                AddShellRows(model, info, true);

                model.buttons.push_back({Str(L"Common", L"Open"), [item] { actions::OpenDefault(item); }, false});
                if (!item.ref.isVirtual)
                {
                    model.buttons.push_back({Str(L"Common", L"OpenWith"), [item, hwnd = ctx->hwnd] { actions::OpenWith(hwnd, item); }, false});
                }
                if (ctx->fallback && ctx->fallback->storeProductId)
                {
                    std::wstring const uri = core::StoreUri(*ctx->fallback->storeProductId);
                    model.buttons.push_back({Str(L"Fallback", L"GetCodec"), [uri] {
                                                 ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                             },
                                             true});
                }
                m_model = std::move(model);
            }

            UIElement View() override
            {
                m_view = BuildInfoCard(m_model);
                return m_view;
            }

            winrt::Windows::Foundation::Size PreferredSize() const override { return {720, 540}; }

            void Unload() override
            {
                m_view = nullptr;
                m_model = {};
            }

        private:
            InfoCardModel m_model;
            UIElement m_view{nullptr};
        };
    }

    std::shared_ptr<IPreviewer> CreateFallbackPreviewer()
    {
        return std::make_shared<FallbackPreviewer>();
    }
}
