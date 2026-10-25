#pragma once

#include <memory>
#include <stop_token>
#include <string>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "Previewers/IPreviewer.h"
#include "Previewers/Image/AnimatedImage.h"
#include "Previewers/Image/SvgImage.h"
#include "Previewers/Image/WicDecoder.h"
#include "Previewers/Image/ZoomPanView.h"

namespace fastlook::viewer
{
    // Raster images through WIC: decoded directly to the window's size (never full resolution unless zoomed), EXIF orientation
    // applied, zoom and pan, and a re-decode at higher resolution when a zoom settles. Animated GIF/APNG/WebP play through
    // AnimatedImage; SVG is drawn by Direct2D (SvgImage) and re-rendered at the zoomed size.
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
        void OnSvgZoomSettled(double zoom);
        void CloseZoomSource();
        winrt::fire_and_forget Redecode(core::SizeU target, std::stop_token stop, uint64_t generation);

        std::filesystem::path m_path;
        std::shared_ptr<PreviewContext const> m_ctx;
        core::SizeU m_native{};      // oriented
        core::SizeU m_baseSize{};    // pixels of the base bitmap
        winrt::Windows::Foundation::Size m_preferred{};
        winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource m_baseSource{nullptr};
        winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource m_zoomSource{nullptr}; // current re-decode, if any
        std::shared_ptr<AnimatedImage> m_animation;
        std::shared_ptr<SvgImage> m_svg;
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_svgBase{nullptr};  // fitted render, shown at zoom 1
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_svgZoom{nullptr};  // render for the current zoom, if any
        std::unique_ptr<ZoomPanView> m_zoomView;
        std::stop_source m_redecodeStop;
        uint64_t m_generation = 0; // UI thread
    };
}
