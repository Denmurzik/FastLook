#include "pch.h"
#include "Graphics/DeviceResources.h"

#include "Diagnostics/Log.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace fastlook::viewer
{
    DeviceResources& DeviceResources::Get()
    {
        // Intentionally leaked: the COM objects must not be released by CRT static destruction, after WinRT teardown.
        static DeviceResources& instance = *new DeviceResources;
        return instance;
    }

    ID3D11Device* DeviceResources::D3D()
    {
        std::lock_guard lock{m_mutex};
        if (!m_d3d)
        {
            UINT const flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            D3D_FEATURE_LEVEL const levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                           m_d3d.put(), nullptr, nullptr);
            if (FAILED(hr))
            {
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                       m_d3d.put(), nullptr, nullptr);
            }
            if (FAILED(hr))
            {
                FL_LOG_INFO(L"D3D11CreateDevice failed: 0x{:08X}", static_cast<uint32_t>(hr));
                m_d3d = nullptr;
            }
        }
        return m_d3d.get();
    }

    ID2D1Factory6* DeviceResources::D2DFactory()
    {
        std::lock_guard lock{m_mutex};
        if (!m_d2dFactory)
        {
            D2D1_FACTORY_OPTIONS options{};
            HRESULT const hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory6), &options, m_d2dFactory.put_void());
            if (FAILED(hr))
            {
                FL_LOG_INFO(L"D2D1CreateFactory failed: 0x{:08X}", static_cast<uint32_t>(hr));
                m_d2dFactory = nullptr;
            }
        }
        return m_d2dFactory.get();
    }

    ID2D1Device5* DeviceResources::D2DDevice()
    {
        std::lock_guard lock{m_mutex};
        if (!m_d2dDevice)
        {
            auto* factory = D2DFactory();
            auto* d3d = D3D();
            if (factory == nullptr || d3d == nullptr)
            {
                return nullptr;
            }
            winrt::com_ptr<ID3D11Device> d3dRef;
            d3dRef.copy_from(d3d);
            auto const dxgi = d3dRef.as<IDXGIDevice>();
            winrt::com_ptr<ID2D1Device5> device;
            HRESULT const hr = factory->CreateDevice(dxgi.get(), device.put());
            if (FAILED(hr))
            {
                FL_LOG_INFO(L"ID2D1Factory6::CreateDevice failed: 0x{:08X}", static_cast<uint32_t>(hr));
                return nullptr;
            }
            m_d2dDevice = std::move(device);
        }
        return m_d2dDevice.get();
    }

    IDWriteFactory3* DeviceResources::DWrite()
    {
        std::lock_guard lock{m_mutex};
        if (!m_dwrite)
        {
            HRESULT const hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(m_dwrite.put_void()));
            if (FAILED(hr))
            {
                FL_LOG_INFO(L"DWriteCreateFactory failed: 0x{:08X}", static_cast<uint32_t>(hr));
                m_dwrite = nullptr;
            }
        }
        return m_dwrite.get();
    }

    IWICImagingFactory2* DeviceResources::Wic()
    {
        std::lock_guard lock{m_mutex};
        if (!m_wic)
        {
            HRESULT const hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(m_wic.put()));
            if (FAILED(hr))
            {
                FL_LOG_INFO(L"WIC factory creation failed: 0x{:08X}", static_cast<uint32_t>(hr));
                m_wic = nullptr;
            }
        }
        return m_wic.get();
    }

    void DeviceResources::ReleaseAll()
    {
        std::lock_guard lock{m_mutex};
        m_d2dDevice = nullptr;
        m_d2dFactory = nullptr;
        m_d3d = nullptr;
        m_dwrite = nullptr;
        m_wic = nullptr;
    }

    bool DeviceResources::HandleDeviceLost(HRESULT hr)
    {
        if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET && hr != D2DERR_RECREATE_TARGET)
        {
            return false;
        }
        FL_LOG_INFO(L"Graphics device lost: 0x{:08X}", static_cast<uint32_t>(hr));
        ReleaseAll();
        return true;
    }
}
