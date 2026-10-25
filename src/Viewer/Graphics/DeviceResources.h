#pragma once

#include <mutex>

#include <winrt/base.h>

#include <d2d1_3.h>
#include <d3d11_4.h>
#include <dwrite_3.h>
#include <wincodec.h>

namespace fastlook::viewer
{
    // Process-wide GPU/text/imaging factories, created lazily on first use and kept for the process lifetime (decision 55).
    // Hide never releases them; ReleaseAll is for device-lost recovery only. The getters return null when creation failed.
    // The returned pointers stay valid until ReleaseAll.
    class DeviceResources
    {
    public:
        static DeviceResources& Get();

        ID3D11Device* D3D();
        ID2D1Factory6* D2DFactory();
        ID2D1Device5* D2DDevice();
        IDWriteFactory3* DWrite();
        IWICImagingFactory2* Wic();

        void ReleaseAll();
        // True when `hr` is a device-removed/reset error; the device objects were released and are re-created on next use.
        bool HandleDeviceLost(HRESULT hr);

    private:
        DeviceResources() = default;

        std::recursive_mutex m_mutex;
        winrt::com_ptr<ID3D11Device> m_d3d;
        winrt::com_ptr<ID2D1Factory6> m_d2dFactory;
        winrt::com_ptr<ID2D1Device5> m_d2dDevice;
        winrt::com_ptr<IDWriteFactory3> m_dwrite;
        winrt::com_ptr<IWICImagingFactory2> m_wic;
    };
}
