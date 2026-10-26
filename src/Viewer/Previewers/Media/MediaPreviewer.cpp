#include "pch.h"
#include "Previewers/Media/MediaPreviewer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <coroutine>
#include <cwctype>
#include <functional>
#include <thread>

#include <winrt/Windows.UI.Text.h>

#include "Diagnostics/Log.h"
#include "FileTypes/StoreLinks.h"
#include "Previewers/Common/ShellInfo.h"
#include "Previewers/Factories.h"
#include "Util/Async.h"
#include "Util/Str.h"
#include "ViewerHost.h"

namespace fastlook::viewer
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using winrt::Windows::Media::Playback::MediaPlayer;

        constexpr auto kOpenTimeout = std::chrono::seconds(3);
        constexpr auto kCloseDelay = std::chrono::milliseconds(100);
        constexpr auto kVolumeSaveDelay = std::chrono::milliseconds(500);
        constexpr double kCoverDip = 160;
        constexpr int kCoverRequestPx = 256;

        bool IsAudioExtension(std::filesystem::path const& path)
        {
            auto ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            for (auto const* known : {L".mp3", L".wav", L".flac", L".m4a", L".aac", L".ogg", L".oga", L".opus", L".wma", L".mka", L".aiff", L".aif", L".amr"})
            {
                if (ext == known)
                {
                    return true;
                }
            }
            return false;
        }

        // Owns every MediaPlayer (see PrepareAsync): a dedicated thread in the multithreaded apartment that runs a message loop.
        // Started on first use and never stopped (it only waits for messages).
        class MediaThread
        {
        public:
            static MediaThread& Get()
            {
                static auto* const thread = new MediaThread; // intentionally leaked, like the other process-lifetime statics
                return *thread;
            }

            // `co_await MediaThread::Get().Resume()` continues the coroutine on the media thread; yields false (and continues in
            // place) when the thread could not take the work.
            auto Resume()
            {
                struct Awaiter
                {
                    MediaThread& thread;
                    bool posted = false;
                    bool await_ready() const noexcept { return false; }
                    bool await_suspend(std::coroutine_handle<> handle)
                    {
                        posted = PostThreadMessageW(thread.m_id, kRun, 0, reinterpret_cast<LPARAM>(handle.address())) != FALSE;
                        return posted;
                    }
                    bool await_resume() const noexcept { return posted; }
                };
                return Awaiter{*this};
            }

            // Runs `work` on the media thread; false when the thread could not take it.
            bool Post(std::function<void()> work)
            {
                auto* call = new std::function<void()>(std::move(work));
                if (PostThreadMessageW(m_id, kCall, 0, reinterpret_cast<LPARAM>(call)))
                {
                    return true;
                }
                delete call;
                return false;
            }

        private:
            static constexpr UINT kRun = WM_APP + 1;
            static constexpr UINT kCall = WM_APP + 2;

            MediaThread()
            {
                winrt::handle ready{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
                std::thread([this, event = ready.get()] {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    MSG msg;
                    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // creates the queue before anyone posts to it
                    m_id = GetCurrentThreadId();
                    ::SetEvent(event);
                    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                    {
                        if (msg.hwnd == nullptr && msg.message == kRun)
                        {
                            std::coroutine_handle<>::from_address(reinterpret_cast<void*>(msg.lParam)).resume();
                            continue;
                        }
                        if (msg.hwnd == nullptr && msg.message == kCall)
                        {
                            std::unique_ptr<std::function<void()>> const call{reinterpret_cast<std::function<void()>*>(msg.lParam)};
                            try
                            {
                                (*call)();
                            }
                            catch (...)
                            {
                                FL_LOG_INFO(L"A media thread call failed");
                            }
                            continue;
                        }
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }).detach();
                ::WaitForSingleObject(ready.get(), INFINITE);
            }

            DWORD m_id = 0;
        };

        // Shared with the MediaPlayer's event handlers, which outlive the coroutine frame when it is cancelled.
        struct OpenState
        {
            winrt::handle signal{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            std::atomic<bool> failed{false};
            void Set() const { ::SetEvent(signal.get()); }
        };

        void ClosePlayer(MediaPlayer const& player, winrt::Windows::Media::Core::MediaSource const& source)
        {
            try
            {
                player.Source(nullptr);
                player.Close();
            }
            catch (winrt::hresult_error const&)
            {
                FL_LOG_INFO(L"Closing the media player failed");
            }
            try
            {
                if (source)
                {
                    source.Close();
                }
            }
            catch (winrt::hresult_error const&)
            {
                FL_LOG_INFO(L"Closing the media source failed");
            }
        }

        // Closes a player that was shown, in a fixed order: the element is already detached; once the compositor has produced a
        // frame without the video (or after kCloseDelay when the window is hidden and renders nothing), the player and its source
        // are closed on the media thread that created them.
        void CloseAfterFrame(MediaPlayer player, winrt::Windows::Media::Core::MediaSource source,
                             winrt::Microsoft::UI::Dispatching::DispatcherQueue const& ui)
        {
            using winrt::Microsoft::UI::Xaml::Media::CompositionTarget;
            struct Pending
            {
                MediaPlayer player{nullptr};
                winrt::Windows::Media::Core::MediaSource source{nullptr};
                winrt::event_token rendering{};
                winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
                bool done = false;
            };
            auto pending = std::make_shared<Pending>();
            pending->player = std::move(player);
            pending->source = std::move(source);
            auto finish = [pending] {
                if (std::exchange(pending->done, true))
                {
                    return;
                }
                if (pending->rendering)
                {
                    CompositionTarget::Rendering(pending->rendering);
                }
                if (pending->timer)
                {
                    pending->timer.Stop();
                    pending->timer = nullptr; // drops the Tick handler that holds `pending`
                }
                auto player = std::move(pending->player);
                auto source = std::move(pending->source);
                if (!MediaThread::Get().Post([player, source] { ClosePlayer(player, source); }))
                {
                    ClosePlayer(player, source);
                }
            };
            try
            {
                // The first Rendering event is the frame being built now: close after the next one.
                auto frames = std::make_shared<int>(0);
                pending->rendering = CompositionTarget::Rendering([finish, frames](auto const&, auto const&) {
                    if (++*frames >= 2)
                    {
                        finish();
                    }
                });
                pending->timer = ui.CreateTimer();
                pending->timer.Interval(kCloseDelay);
                pending->timer.IsRepeating(false);
                pending->timer.Tick([finish](auto const&, auto const&) { finish(); });
                pending->timer.Start();
            }
            catch (...)
            {
                finish();
            }
        }

        // Closes the player on every exit from PrepareAsync that did not hand it over to the previewer.
        struct PlayerGuard
        {
            MediaPlayer player{nullptr};
            winrt::Windows::Media::Core::MediaSource source{nullptr};
            ~PlayerGuard()
            {
                try
                {
                    if (player)
                    {
                        player.Pause();
                        player.Source(nullptr);
                        player.Close();
                    }
                    if (source)
                    {
                        source.Close();
                    }
                }
                catch (...)
                {
                }
            }
        };

        std::wstring FormatDuration(std::chrono::milliseconds duration)
        {
            auto const total = static_cast<long long>(duration.count() / 1000);
            auto const h = total / 3600, m = (total / 60) % 60, s = total % 60;
            wchar_t separator[8] = L":";
            ::GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIME, separator, static_cast<int>(std::size(separator)));
            auto const pad = [](long long value) {
                auto text = FormatNumber(static_cast<uint64_t>(value));
                return value < 10 ? FormatNumber(0) + text : text;
            };
            std::wstring result = h > 0 ? FormatNumber(static_cast<uint64_t>(h)) + separator + pad(m) : FormatNumber(static_cast<uint64_t>(m));
            return result + separator + pad(s);
        }

        Controls::TextBlock MakeText(std::wstring const& text, double size, bool bold, double opacity)
        {
            Controls::TextBlock block;
            block.Text(winrt::hstring{text});
            block.FontSize(size);
            block.Opacity(opacity);
            block.TextWrapping(TextWrapping::Wrap);
            if (bold)
            {
                block.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            return block;
        }
    }

    winrt::Windows::Foundation::IAsyncAction MediaPreviewer::PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx)
    {
        auto self = shared_from_this();
        auto cancel = co_await winrt::get_cancellation_token();
        m_ctx = ctx;
        auto const ui = ctx->ui;
        std::filesystem::path const path{item.ref.path};
        m_fileName = path.filename().wstring();
        bool const audioByName = IsAudioExtension(path);

        auto state = std::make_shared<OpenState>();
        cancel.callback([state] { state->Set(); });

        PlayerGuard guard;
        bool fileOpened = false;
        bool notFound = false;
        try
        {
            auto const file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(winrt::hstring{path.wstring()});
            fileOpened = true;
            if (cancel())
            {
                co_return;
            }
            guard.source = winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file);
        }
        catch (winrt::hresult_error const& e)
        {
            if (cancel())
            {
                co_return;
            }
            notFound = e.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || e.code() == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        }
        if (!fileOpened)
        {
            throw PreviewError{std::wstring{Str(L"Common", notFound ? L"NotFound" : L"PreviewFailed")}, std::nullopt};
        }

        // The player is created on the media thread: one created on a thread-pool thread (where the file open resumes) leaves a
        // hidden window, its audio-state monitor and two handles behind on every open, because that thread pumps no messages.
        // A thread of our own rather than the UI thread: the open stays off the thread that animates the window.
        if (!co_await MediaThread::Get().Resume())
        {
            throw PreviewError{std::wstring{Str(L"Common", L"PreviewFailed")}, std::nullopt};
        }
        if (cancel())
        {
            co_return;
        }
        guard.player = MediaPlayer{};
        guard.player.AutoPlay(false);
        guard.player.MediaOpened([state](auto const&, auto const&) { state->Set(); });
        guard.player.MediaFailed([state](auto const&, auto const&) {
            state->failed = true;
            state->Set();
        });
        guard.player.Source(guard.source);

        bool const signalled = co_await winrt::resume_on_signal(state->signal.get(), kOpenTimeout);
        if (cancel())
        {
            co_return;
        }
        if (!signalled)
        {
            throw PreviewError{std::wstring{Str(L"Common", L"TimedOut")}, std::nullopt};
        }
        if (state->failed)
        {
            std::optional<std::wstring> product;
            if (!audioByName)
            {
                product = std::wstring{core::StoreProductId(core::MissingCodec::Hevc)};
            }
            throw PreviewError{std::wstring{Str(L"Media", L"Unsupported")}, product};
        }

        auto const session = guard.player.PlaybackSession();
        auto const width = session.NaturalVideoWidth();
        auto const height = session.NaturalVideoHeight();
        bool const video = width > 0 && height > 0;
        if (!video && !audioByName)
        {
            // A video container whose video codec is missing still opens when its audio decodes.
            throw PreviewError{std::wstring{Str(L"Media", L"Unsupported")}, std::wstring{core::StoreProductId(core::MissingCodec::Hevc)}};
        }
        AudioInfo audio;
        if (!video)
        {
            audio = ReadAudioInfo(path); // background thread
        }

        if (!co_await ResumeOn(ui) || cancel())
        {
            co_return;
        }
        winrt::Microsoft::UI::Xaml::Media::ImageSource cover{nullptr};
        if (!video)
        {
            try
            {
                cover = co_await LoadThumbnailAsync(item.ref, kCoverRequestPx, false);
            }
            catch (winrt::hresult_error const&)
            {
                FL_LOG_INFO(L"Cover thumbnail failed; showing the audio preview without it");
            }
            if (cancel() || !co_await ResumeOn(ui))
            {
                co_return;
            }
        }

        m_video = video;
        m_audio = std::move(audio);
        m_cover = cover;
        m_preferred = video ? winrt::Windows::Foundation::Size{static_cast<float>(width / ctx->scale), static_cast<float>(height / ctx->scale)}
                            : winrt::Windows::Foundation::Size{720, 360};
        m_player = std::move(guard.player);
        m_source = std::move(guard.source);
        guard.player = nullptr;
        guard.source = nullptr;
    }

    winrt::Microsoft::UI::Xaml::UIElement MediaPreviewer::View()
    {
        m_unloaded = false;
        auto const& media = m_ctx->settings.preview;
        m_savedVolume = std::clamp(media.mediaVolume, 0.0, 1.0);
        m_pendingVolume = m_savedVolume;
        m_player.Volume(m_savedVolume);

        std::weak_ptr<IPreviewer> weak = shared_from_this();
        auto const ui = m_ctx->ui;
        m_volumeToken = m_player.VolumeChanged([weak, ui](MediaPlayer const& player, auto const&) {
            double const volume = player.Volume();
            ui.TryEnqueue([weak, volume] {
                if (auto const strong = weak.lock())
                {
                    static_cast<MediaPreviewer&>(*strong).OnVolumeChanged(volume);
                }
            });
        });

        m_element = Controls::MediaPlayerElement{};
        m_element.AreTransportControlsEnabled(true);
        m_element.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
        m_element.SetMediaPlayer(m_player);

        m_failedToken = m_player.MediaFailed([weak, ui](auto const&, auto const&) {
            ui.TryEnqueue([weak] {
                if (auto const strong = weak.lock())
                {
                    static_cast<MediaPreviewer&>(*strong).OnMediaFailed();
                }
            });
        });

        UIElement content = m_element;
        if (!m_video)
        {
            Controls::Image image;
            image.Source(m_cover);
            image.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
            Controls::Border coverFrame;
            coverFrame.Width(kCoverDip);
            coverFrame.Height(kCoverDip);
            coverFrame.CornerRadius({8, 8, 8, 8});
            coverFrame.Child(image);

            std::wstring const title = m_audio.title.empty() ? m_fileName : m_audio.title;
            Controls::StackPanel text;
            text.Spacing(6);
            text.VerticalAlignment(VerticalAlignment::Center);
            text.Children().Append(MakeText(title, 22, true, 1.0));
            if (!m_audio.artist.empty())
            {
                text.Children().Append(MakeText(m_audio.artist, 15, false, 0.85));
            }
            if (!m_audio.album.empty())
            {
                text.Children().Append(MakeText(m_audio.album, 14, false, 0.65));
            }
            if (m_audio.duration.count() > 0)
            {
                text.Children().Append(MakeText(FormatDuration(m_audio.duration), 14, false, 0.65));
            }

            Controls::Grid header;
            header.ColumnSpacing(24);
            header.VerticalAlignment(VerticalAlignment::Center);
            Controls::ColumnDefinition coverColumn, textColumn;
            coverColumn.Width(GridLengthHelper::Auto());
            textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            header.ColumnDefinitions().Append(coverColumn);
            header.ColumnDefinitions().Append(textColumn);
            Controls::Grid::SetColumn(coverFrame, 0);
            Controls::Grid::SetColumn(text, 1);
            header.Children().Append(coverFrame);
            header.Children().Append(text);

            m_element.VerticalAlignment(VerticalAlignment::Bottom);
            Controls::Grid grid;
            grid.Padding({24, 24, 24, 8});
            Controls::RowDefinition headerRow, barRow;
            headerRow.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            barRow.Height(GridLengthHelper::Auto());
            grid.RowDefinitions().Append(headerRow);
            grid.RowDefinitions().Append(barRow);
            Controls::Grid::SetRow(header, 0);
            Controls::Grid::SetRow(m_element, 1);
            grid.Children().Append(header);
            grid.Children().Append(m_element);
            content = grid;
        }
        m_host = Controls::Grid{};
        m_host.Children().Append(content);

        if (media.autoplayMedia)
        {
            m_player.Play();
        }
        return m_host;
    }

    // An error after the open succeeded (a damaged stream, a lost decoder): stop and say so in place of the dead player.
    void MediaPreviewer::OnMediaFailed()
    {
        if (m_unloaded || !m_host || m_failureShown)
        {
            return;
        }
        m_failureShown = true;
        try
        {
            m_player.Pause();
        }
        catch (winrt::hresult_error const&)
        {
        }
        Controls::Border overlay;
        overlay.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
            m_ctx->dark ? winrt::Windows::UI::Color{0xE6, 0x20, 0x20, 0x20} : winrt::Windows::UI::Color{0xE6, 0xF3, 0xF3, 0xF3}});
        auto message = MakeText(std::wstring{Str(L"Media", L"Unsupported")}, 16, false, 1.0);
        message.HorizontalAlignment(HorizontalAlignment::Center);
        message.VerticalAlignment(VerticalAlignment::Center);
        overlay.Child(message);
        m_host.Children().Append(overlay);
    }

    void MediaPreviewer::OnVolumeChanged(double volume)
    {
        if (m_unloaded || std::abs(volume - m_savedVolume) < 0.005)
        {
            return;
        }
        m_pendingVolume = volume;
        if (!m_volumeTimer)
        {
            m_volumeTimer = m_ctx->ui.CreateTimer();
            m_volumeTimer.IsRepeating(false);
            m_volumeTimer.Interval(kVolumeSaveDelay);
            std::weak_ptr<IPreviewer> weak = shared_from_this();
            m_volumeTimer.Tick([weak](auto const&, auto const&) {
                if (auto const strong = weak.lock())
                {
                    static_cast<MediaPreviewer&>(*strong).FlushVolume();
                }
            });
        }
        m_volumeTimer.Stop();
        m_volumeTimer.Start();
    }

    void MediaPreviewer::FlushVolume()
    {
        if (m_volumeTimer)
        {
            m_volumeTimer.Stop();
        }
        if (std::abs(m_pendingVolume - m_savedVolume) < 0.005)
        {
            return;
        }
        auto settings = ViewerHost::Get().Settings();
        settings.preview.mediaVolume = m_pendingVolume;
        if (ViewerHost::Get().SaveSettings(settings))
        {
            m_savedVolume = m_pendingVolume;
        }
    }

    void MediaPreviewer::CloseMedia()
    {
        // Detach the element and stop playback now; the player and its source are closed after the next frame (CloseAfterFrame).
        auto const attempt = [](auto&& step) {
            try
            {
                step();
            }
            catch (winrt::hresult_error const&)
            {
                FL_LOG_INFO(L"Closing the media player failed");
            }
        };
        attempt([&] {
            if (m_element)
            {
                m_element.SetMediaPlayer(nullptr);
            }
        });
        attempt([&] {
            if (m_player)
            {
                m_player.VolumeChanged(m_volumeToken);
                m_player.MediaFailed(m_failedToken);
                m_player.Pause();
            }
        });
        if (m_player)
        {
            CloseAfterFrame(std::exchange(m_player, nullptr), std::exchange(m_source, nullptr), m_ctx->ui);
        }
        m_player = nullptr;
        m_source = nullptr;
        m_element = nullptr;
        m_host = nullptr;
        m_failureShown = false;
    }

    void MediaPreviewer::Unload()
    {
        if (!m_unloaded && m_volumeTimer)
        {
            FlushVolume();
        }
        m_unloaded = true;
        m_volumeTimer = nullptr;
        CloseMedia();
        m_cover = nullptr;
    }

    std::shared_ptr<IPreviewer> CreateMediaPreviewer()
    {
        return std::make_shared<MediaPreviewer>();
    }
}
