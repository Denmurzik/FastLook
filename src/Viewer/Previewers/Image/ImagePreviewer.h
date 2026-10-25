#pragma once

#include <memory>
#include <stop_token>
#include <string>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "Previewers/IPreviewer.h"
#include "Previewers/Image/WicDecoder.h"
#include "Previewers/Image/ZoomPanView.h"

namespace fastlook::viewer
{
    // Raster images through WIC: decoded directly to the window's size (never full resolution unless zoomed), EXIF orientation
    // applied, zoom and pan, and a re-decode at higher resolution when a zoom settles.
    class ImagePreviewer final : public IPreviewer, public IZoomable
    {
    public:
        winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem item, std::shared_ptr<PreviewContext const> ctx) override;
        winrt::Microsoft::UI::Xaml::UIElement View() override;
        winrt::Windows::Foundation::Size PreferredSize() const override { return m_preferred; }
        bool KeepAspect() const override { return true; }
        void Unload() override;

        void Zoom(ZoomCommand command) override;

    private:
        void OnZoomSettled(double zoom);
        void CloseZoomSource();
        winrt::fire_and_forget Redecode(core::SizeU target, std::stop_token stop, uint64_t generation);

        std::filesystem::path m_path;
        std::shared_ptr<PreviewContext const> m_ctx;
        core::SizeU m_native{};      // oriented
        core::SizeU m_baseSize{};    // pixels of the base bitmap
        winrt::Windows::Foundation::Size m_preferred{};
        winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource m_baseSource{nullptr};
        winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource m_zoomSource{nullptr}; // current re-decode, if any
        std::unique_ptr<ZoomPanView> m_zoomView;
        std::stop_source m_redecodeStop;
        uint64_t m_generation = 0; // UI thread
    };
}
