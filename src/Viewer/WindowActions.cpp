#include "pch.h"
#include "WindowActions.h"

#include <memory>
#include <string>

#include <shlobj.h>

#include "Diagnostics/Log.h"

namespace fastlook::viewer::actions
{
    namespace
    {
        struct PidlDeleter
        {
            using pointer = PIDLIST_ABSOLUTE;
            void operator()(PIDLIST_ABSOLUTE pidl) const { CoTaskMemFree(pidl); }
        };
        using UniquePidl = std::unique_ptr<ITEMIDLIST_ABSOLUTE, PidlDeleter>;

        UniquePidl Parse(std::wstring const& path)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)))
            {
                return nullptr;
            }
            return UniquePidl{pidl};
        }

        bool SetClipboardBlob(UINT format, void const* data, SIZE_T size)
        {
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
            if (mem == nullptr)
            {
                return false;
            }
            if (void* dst = GlobalLock(mem))
            {
                memcpy(dst, data, size);
                GlobalUnlock(mem);
            }
            if (SetClipboardData(format, mem) == nullptr)
            {
                GlobalFree(mem);
                return false;
            }
            return true; // the clipboard owns the memory now
        }
    }

    void OpenDefault(PreviewItem const& item)
    {
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.lpVerb = L"open";
        info.nShow = SW_SHOWNORMAL;
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        UniquePidl pidl;
        if (item.ref.isVirtual)
        {
            pidl = Parse(item.ref.path);
            if (!pidl)
            {
                FL_LOG_INFO(L"actions::OpenDefault: cannot parse virtual item '{}'", item.ref.path);
                return;
            }
            info.fMask |= SEE_MASK_IDLIST;
            info.lpIDList = pidl.get();
        }
        else
        {
            info.lpFile = item.ref.path.c_str();
        }
        if (!ShellExecuteExW(&info))
        {
            FL_LOG_INFO(L"actions::OpenDefault: ShellExecuteEx failed, error {}", GetLastError());
        }
    }

    void OpenWith(HWND owner, PreviewItem const& item)
    {
        if (item.ref.isVirtual)
        {
            FL_LOG_INFO(L"actions::OpenWith: virtual items have no file to open");
            return;
        }
        OPENASINFO info{};
        info.pcszFile = item.ref.path.c_str();
        info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC;
        HRESULT const hr = SHOpenWithDialog(owner, &info);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            FL_LOG_INFO(L"actions::OpenWith: SHOpenWithDialog failed, hr {:#x}", static_cast<unsigned>(hr));
        }
    }

    void Reveal(PreviewItem const& item)
    {
        auto const pidl = Parse(item.ref.path);
        if (!pidl)
        {
            FL_LOG_INFO(L"actions::Reveal: cannot parse '{}'", item.ref.path);
            return;
        }
        HRESULT const hr = SHOpenFolderAndSelectItems(pidl.get(), 0, nullptr, 0);
        if (FAILED(hr))
        {
            FL_LOG_INFO(L"actions::Reveal: SHOpenFolderAndSelectItems failed, hr {:#x}", static_cast<unsigned>(hr));
        }
    }

    bool CopyFile(HWND owner, PreviewItem const& item)
    {
        if (item.ref.isVirtual)
        {
            return false;
        }
        // DROPFILES followed by a double-NUL terminated wide path list.
        std::wstring const& path = item.ref.path;
        SIZE_T const chars = path.size() + 2;
        std::wstring list(path);
        list.push_back(L'\0');
        list.push_back(L'\0');
        SIZE_T const size = sizeof(DROPFILES) + chars * sizeof(wchar_t);
        auto buffer = std::make_unique<char[]>(size);
        auto* drop = reinterpret_cast<DROPFILES*>(buffer.get());
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        memcpy(buffer.get() + sizeof(DROPFILES), list.data(), chars * sizeof(wchar_t));

        if (!OpenClipboard(owner))
        {
            FL_LOG_INFO(L"actions::CopyFile: OpenClipboard failed, error {}", GetLastError());
            return false;
        }
        EmptyClipboard();
        DWORD const effect = DROPEFFECT_COPY;
        bool const ok = SetClipboardBlob(CF_HDROP, buffer.get(), size) &&
                        SetClipboardBlob(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), &effect, sizeof(effect));
        CloseClipboard();
        if (!ok)
        {
            FL_LOG_INFO(L"actions::CopyFile: SetClipboardData failed");
        }
        return ok;
    }
}
