#include "pch.h"
#include "Previewers/Placeholder.h"

#include <cstdlib>

#include "FileTypes/FileName.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;

        DWORD TestPrepareDelayMs()
        {
            wchar_t buffer[16]{};
            auto const n = GetEnvironmentVariableW(L"FASTLOOK_TEST_PREPARE_DELAY_MS", buffer, ARRAYSIZE(buffer));
            if (n == 0 || n >= ARRAYSIZE(buffer))
            {
                return 0;
            }
            return static_cast<DWORD>(wcstoul(buffer, nullptr, 10));
        }

        class PlaceholderPreviewer final : public IPreviewer
        {
        public:

            winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override
            {
                auto self = shared_from_this();
                auto cancel = co_await winrt::get_cancellation_token();

                m_text = std::wstring{core::BaseName(item.ref.path)} + L"\n" +
                         (ctx->fallback ? ctx->fallback->message : std::wstring{Str(L"Common", L"PreviewFailed")});

                if (DWORD const delay = TestPrepareDelayMs())
                {
                    co_await winrt::resume_background();
                    for (DWORD slept = 0; slept < delay; slept += 10)
                    {
                        if (cancel())
                        {
                            co_return;
                        }
                        Sleep(10);
                    }
                }
            }

            UIElement View() override
            {
                winrt::Microsoft::UI::Xaml::Controls::TextBlock text;
                text.Text(m_text);
                text.TextAlignment(TextAlignment::Center);
                text.TextWrapping(TextWrapping::Wrap);
                text.HorizontalAlignment(HorizontalAlignment::Center);
                text.VerticalAlignment(VerticalAlignment::Center);
                m_view = text;
                return text;
            }

            winrt::Windows::Foundation::Size PreferredSize() const override { return {0, 0}; }

            void Unload() override { m_view = nullptr; }

        private:
            std::wstring m_text;
            UIElement m_view{nullptr};
        };
    }

    std::shared_ptr<IPreviewer> MakePlaceholderPreviewer()
    {
        return std::make_shared<PlaceholderPreviewer>();
    }
}
