#include "pch.h"
#include "Previewers/Factories.h"

#include <chrono>
#include <stop_token>

#include "Diagnostics/Log.h"
#include "Fs/FolderStats.h"
#include "Previewers/Common/InfoCard.h"
#include "Previewers/Common/ShellInfo.h"
#include "Util/Async.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;

        constexpr int kIconDip = 160;
        constexpr auto kCountDeadline = std::chrono::seconds(5);

        class FolderPreviewer final : public IPreviewer
        {
        public:
            winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override
            {
                auto self = shared_from_this();
                auto cancel = co_await winrt::get_cancellation_token();
                auto const ui = ctx->ui;

                co_await winrt::resume_background();
                ShellInfo const info = QueryShellInfo(item.ref, false);
                if (cancel())
                {
                    co_return;
                }

                if (!co_await ResumeOn(ui))
                {
                    co_return;
                }
                auto const image = co_await LoadThumbnailAsync(item.ref, static_cast<int>(kIconDip * ctx->scale), true);
                if (cancel() || !co_await ResumeOn(ui))
                {
                    co_return;
                }

                InfoCardModel model;
                model.image = image;
                model.imageSizeDip = kIconDip;
                model.title = winrt::hstring{info.displayName};
                model.rows.push_back({Str(L"Fallback", L"Path"), winrt::hstring{item.ref.path}});
                AddShellRows(model, info, false);
                m_countable = !item.ref.isVirtual;
                if (m_countable)
                {
                    model.rows.push_back({Str(L"Fallback", L"Contents"), Str(L"Fallback", L"Counting")});
                }
                m_path = item.ref.path;
                m_ui = ui;
                m_model = std::move(model);
            }

            UIElement View() override
            {
                std::vector<Controls::TextBlock> blocks;
                m_view = BuildInfoCard(m_model, &blocks);
                if (m_countable && !blocks.empty())
                {
                    m_contents = blocks.back();
                    StartCount();
                }
                return m_view;
            }

            winrt::Windows::Foundation::Size PreferredSize() const override { return {560, 440}; }

            void Unload() override
            {
                m_stop.request_stop();
                m_contents = nullptr;
                m_view = nullptr;
                m_model = {};
            }

        private:
            static winrt::fire_and_forget CountAsync(std::weak_ptr<IPreviewer> weak, std::wstring path, std::stop_token stop,
                                                    winrt::Microsoft::UI::Dispatching::DispatcherQueue ui)
            {
                try
                {
                    co_await winrt::resume_background();
                    auto publish = [weak, ui](core::FolderStats const& stats, bool final) {
                        ui.TryEnqueue([weak, stats, final] {
                            if (auto previewer = weak.lock())
                            {
                                static_cast<FolderPreviewer&>(*previewer).ShowStats(stats, final);
                            }
                        });
                    };
                    auto const deadline = std::chrono::steady_clock::now() + kCountDeadline;
                    auto const stats = core::CountFolder(path, stop, deadline, [&](core::FolderStats const& s) { publish(s, false); });
                    if (!stop.stop_requested())
                    {
                        publish(stats, true);
                    }
                }
                catch (...)
                {
                    FL_LOG_INFO(L"FolderPreviewer: counting failed");
                }
            }

            void StartCount()
            {
                m_stop = std::stop_source{};
                CountAsync(weak_from_this(), m_path, m_stop.get_token(), m_ui);
            }

            // UI thread.
            void ShowStats(core::FolderStats const& stats, bool final)
            {
                if (!m_contents || m_stop.stop_requested())
                {
                    return;
                }
                std::wstring text = FormatPattern(Str(L"Fallback", L"Items"), {std::to_wstring(stats.files), std::to_wstring(stats.folders)});
                text += L", " + FormatBytes(stats.bytes);
                if (!final)
                {
                    text += L" …";
                }
                else if (stats.truncated)
                {
                    text = FormatPattern(Str(L"Fallback", L"MoreThan"), {text});
                }
                m_contents.Text(winrt::hstring{text});
            }

            InfoCardModel m_model;
            std::wstring m_path;
            bool m_countable = false;
            winrt::Microsoft::UI::Dispatching::DispatcherQueue m_ui{nullptr};
            std::stop_source m_stop;
            Controls::TextBlock m_contents{nullptr};
            UIElement m_view{nullptr};
        };
    }

    std::shared_ptr<IPreviewer> CreateFolderPreviewer()
    {
        return std::make_shared<FolderPreviewer>();
    }
}
