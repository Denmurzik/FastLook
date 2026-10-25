#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include <d2d1_3.h>

#include "Geometry.h"

namespace fastlook::viewer
{
    // An SVG drawn by Direct2D only (ID2D1DeviceContext5::CreateSvgDocument): no scripts, no external resources, no <text>.
    // Rendered into a SurfaceImageSource at whatever pixel size is asked for, so zooming re-renders instead of scaling a bitmap.
    class SvgImage
    {
    public:
        // Background thread. `device` must be a reference taken on the UI thread (`DeviceResources::Get().D2DDevice()`).
        // Throws PreviewError when the file cannot be read or parsed.
        static std::shared_ptr<SvgImage> Load(std::filesystem::path const& path, winrt::com_ptr<ID2D1Device5> device);

        // Intrinsic size in CSS px (width/height, else viewBox, else 512x512).
        core::SizeD IntrinsicSize() const { return m_size; }

        // UI thread. Null when the device is unavailable or the draw failed.
        winrt::Microsoft::UI::Xaml::Media::Imaging::SurfaceImageSource Render(core::SizeU pixels);

    private:
        SvgImage() = default;
        bool EnsureDocument(ID2D1Device5* device);

        std::vector<uint8_t> m_bytes;
        winrt::com_ptr<ID2D1Device5> m_device; // the device m_document belongs to
        winrt::com_ptr<ID2D1SvgDocument> m_document;
        core::SizeD m_size{512, 512};
    };
}
