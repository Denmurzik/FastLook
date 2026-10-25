#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

#include "HandlerFrame.h"

namespace fastlook::host
{
    // One shell preview handler (IPreviewHandler) living in this STA process.
    class HandlerHost
    {
    public:
        explicit HandlerHost(KeySink keySink) : m_keySink(keySink) {}
        ~HandlerHost();

        // CoCreateInstance(clsid) and IInitializeWithFile -> IInitializeWithStream -> IInitializeWithItem, whichever the handler has.
        HRESULT Create(std::wstring_view clsid, std::wstring const& file);
        // SetSite(frame) strictly before SetWindow, then the theme colours.
        HRESULT Attach(HWND parent, RECT const& rect, bool dark);
        HRESULT DoPreview();
        void SetRect(RECT const& rect);
        // Unload + SetSite(null); safe to call twice.
        void Unload();

    private:
        HRESULT Initialize(std::wstring const& file);

        KeySink m_keySink;
        Microsoft::WRL::ComPtr<IPreviewHandler> m_handler;
        Microsoft::WRL::ComPtr<HandlerFrame> m_frame;
        bool m_windowSet = false;
        bool m_previewing = false;
    };
}
