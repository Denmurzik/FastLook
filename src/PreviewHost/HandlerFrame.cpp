#include "HandlerFrame.h"

#include "Ipc/HostProtocol.h"

namespace fastlook::host
{
    namespace
    {
        namespace proto = core::host;

        uint32_t HeldModifiers()
        {
            uint32_t mods = 0;
            if (GetKeyState(VK_CONTROL) < 0)
                mods |= proto::kModCtrl;
            if (GetKeyState(VK_SHIFT) < 0)
                mods |= proto::kModShift;
            if (GetKeyState(VK_MENU) < 0)
                mods |= proto::kModAlt;
            return mods;
        }

        // The accelerator table advertises exactly the forwarded set.
        HACCEL CreateForwardedAccelerators(UINT& count)
        {
            ACCEL entries[16]{};
            count = 0;
            WORD id = 1;
            for (int vk : {VK_SPACE, VK_ESCAPE, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_RETURN, VK_F11})
            {
                entries[count++] = ACCEL{FVIRTKEY, static_cast<WORD>(vk), id++};
            }
            entries[count++] = ACCEL{static_cast<BYTE>(FVIRTKEY | FCONTROL), 'W', id++};
            return CreateAcceleratorTableW(entries, static_cast<int>(count));
        }
    }

    bool ForwardKeyMessage(MSG const& msg, KeySink sink)
    {
        if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN)
        {
            return false;
        }
        uint32_t const vk = static_cast<uint32_t>(msg.wParam);
        uint32_t const mods = HeldModifiers();
        if (!proto::IsForwardedHostKey(vk, mods))
        {
            return false;
        }
        sink(vk, mods);
        return true;
    }

    HandlerFrame::~HandlerFrame()
    {
        if (m_accel)
        {
            DestroyAcceleratorTable(m_accel);
        }
    }

    IFACEMETHODIMP HandlerFrame::QueryInterface(REFIID iid, void** object)
    {
        if (!object)
        {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == __uuidof(IPreviewHandlerFrame))
        {
            *object = static_cast<IPreviewHandlerFrame*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) HandlerFrame::Release()
    {
        ULONG const refs = --m_refs;
        if (refs == 0)
        {
            delete this;
        }
        return refs;
    }

    IFACEMETHODIMP HandlerFrame::GetWindowContext(PREVIEWHANDLERFRAMEINFO* info)
    {
        if (!info)
        {
            return E_POINTER;
        }
        if (!m_accel)
        {
            m_accel = CreateForwardedAccelerators(m_accelCount);
            if (!m_accel)
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }
        info->haccel = m_accel;
        info->cAccelEntries = m_accelCount;
        return S_OK;
    }

    IFACEMETHODIMP HandlerFrame::TranslateAccelerator(MSG* msg)
    {
        if (!msg)
        {
            return E_POINTER;
        }
        return ForwardKeyMessage(*msg, m_sink) ? S_OK : S_FALSE;
    }
}
