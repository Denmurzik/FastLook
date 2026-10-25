#pragma once

#include <memory>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>

#include "Previewers/IPreviewer.h"
#include "Previewers/Media/AudioInfo.h"

namespace fastlook::viewer
{
    // Audio and video through MediaPlayer + MediaPlayerElement. PrepareAsync opens the media (so a codec failure or the
    // natural video size are known before the window is sized); View() attaches the prepared player; Unload() closes the
    // player and its source completely (no audio session, no decoder memory left).
    class MediaPreviewer final : public IPreviewer
    {
    public:
        winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override;
        winrt::Microsoft::UI::Xaml::UIElement View() override;
        winrt::Windows::Foundation::Size PreferredSize() const override { return m_preferred; }
        bool KeepAspect() const override { return m_video; }
        void Unload() override;

    private:
        void OnVolumeChanged(double volume);
        void FlushVolume();
        void OnMediaFailed();
        void CloseMedia();

        std::shared_ptr<PreviewContext const> m_ctx;
        winrt::Windows::Media::Playback::MediaPlayer m_player{nullptr};
        winrt::Windows::Media::Core::MediaSource m_source{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::MediaPlayerElement m_element{nullptr};
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_cover{nullptr};
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_volumeTimer{nullptr};
        winrt::event_token m_volumeToken{};
        winrt::event_token m_failedToken{};
        winrt::Microsoft::UI::Xaml::Controls::Grid m_host{nullptr};
        AudioInfo m_audio;
        std::wstring m_fileName;
        winrt::Windows::Foundation::Size m_preferred{720, 360};
        double m_savedVolume = 0.0;
        double m_pendingVolume = 0.0;
        bool m_video = false;
        bool m_unloaded = false;
        bool m_failureShown = false;
    };
}
