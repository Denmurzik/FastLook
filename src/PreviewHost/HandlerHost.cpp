#include "HandlerHost.h"

#include <shlwapi.h>
#include <shobjidl_core.h>

#include "Diagnostics/Log.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace fastlook::host
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        constexpr COLORREF kDarkBackground = RGB(0x20, 0x20, 0x20);
        constexpr COLORREF kDarkText = RGB(0xFF, 0xFF, 0xFF);
        constexpr COLORREF kLightBackground = RGB(0xF3, 0xF3, 0xF3);
        constexpr COLORREF kLightText = RGB(0x00, 0x00, 0x00);
    }

    HandlerHost::~HandlerHost()
    {
        Unload();
    }

    HRESULT HandlerHost::Create(std::wstring_view clsidText, std::wstring const& file)
    {
        CLSID clsid{};
        std::wstring const text(clsidText);
        if (HRESULT hr = CLSIDFromString(text.c_str(), &clsid); FAILED(hr))
        {
            return hr;
        }
        if (HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&m_handler)); FAILED(hr))
        {
            return hr;
        }
        return Initialize(file);
    }

    HRESULT HandlerHost::Initialize(std::wstring const& file)
    {
        if (ComPtr<IInitializeWithFile> withFile; SUCCEEDED(m_handler.As(&withFile)))
        {
            return withFile->Initialize(file.c_str(), STGM_READ);
        }
        if (ComPtr<IInitializeWithStream> withStream; SUCCEEDED(m_handler.As(&withStream)))
        {
            ComPtr<IStream> stream;
            if (HRESULT hr = SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
                FAILED(hr))
            {
                return hr;
            }
            return withStream->Initialize(stream.Get(), STGM_READ);
        }
        if (ComPtr<IInitializeWithItem> withItem; SUCCEEDED(m_handler.As(&withItem)))
        {
            ComPtr<IShellItem> item;
            if (HRESULT hr = SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item)); FAILED(hr))
            {
                return hr;
            }
            return withItem->Initialize(item.Get(), STGM_READ);
        }
        return E_NOINTERFACE;
    }

    HRESULT HandlerHost::Attach(HWND parent, RECT const& rect, bool dark)
    {
        m_frame.Attach(new HandlerFrame(m_keySink));
        if (ComPtr<IObjectWithSite> site; SUCCEEDED(m_handler.As(&site)))
        {
            // The frame must be known before SetWindow: handlers query it while creating their window.
            if (HRESULT hr = site->SetSite(m_frame.Get()); FAILED(hr))
            {
                FL_LOG_INFO(L"SetSite failed: {:#010x}", static_cast<uint32_t>(hr));
            }
        }
        if (HRESULT hr = m_handler->SetWindow(parent, &rect); FAILED(hr))
        {
            return hr;
        }
        m_windowSet = true;
        if (ComPtr<IPreviewHandlerVisuals> visuals; SUCCEEDED(m_handler.As(&visuals)))
        {
            visuals->SetBackgroundColor(dark ? kDarkBackground : kLightBackground);
            visuals->SetTextColor(dark ? kDarkText : kLightText);
        }
        return S_OK;
    }

    HRESULT HandlerHost::DoPreview()
    {
        HRESULT const hr = m_handler->DoPreview();
        m_previewing = SUCCEEDED(hr);
        return hr;
    }

    void HandlerHost::SetRect(RECT const& rect)
    {
        if (m_handler && m_windowSet)
        {
            m_handler->SetRect(&rect);
        }
    }

    void HandlerHost::Unload()
    {
        if (!m_handler)
        {
            return;
        }
        if (m_previewing)
        {
            m_handler->Unload();
            m_previewing = false;
        }
        if (ComPtr<IObjectWithSite> site; SUCCEEDED(m_handler.As(&site)))
        {
            site->SetSite(nullptr);
        }
        m_handler.Reset();
        m_frame.Reset();
    }
}
