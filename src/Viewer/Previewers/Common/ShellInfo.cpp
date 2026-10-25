#include "pch.h"
#include "Previewers/Common/ShellInfo.h"

#include <robuffer.h>
#include <shlobj.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "Diagnostics/Log.h"
#include "FileTypes/FileName.h"
#include "Util/Async.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "uuid.lib")

namespace fastlook::viewer
{
    namespace
    {
        using winrt::Microsoft::UI::Xaml::Media::ImageSource;

        struct HbitmapDeleter
        {
            using pointer = HBITMAP;
            void operator()(HBITMAP bitmap) const { DeleteObject(bitmap); }
        };
        using UniqueHbitmap = std::unique_ptr<HBITMAP, HbitmapDeleter>;

        std::wstring PropertyString(IShellItem2* item, PROPERTYKEY const& key)
        {
            wchar_t* text = nullptr;
            if (SUCCEEDED(item->GetString(key, &text)) && text != nullptr)
            {
                std::wstring result = text;
                CoTaskMemFree(text);
                return result;
            }
            return {};
        }

        // Real files and folders, placeholders included: attributes from the file system, type text from the extension.
        ShellInfo QueryPathInfo(std::wstring const& path)
        {
            ShellInfo info;
            info.displayName = std::wstring{core::BaseName(path)};

            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                info.attributes = data.dwFileAttributes;
                info.created = data.ftCreationTime;
                info.modified = data.ftLastWriteTime;
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    info.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                }
            }

            // USEFILEATTRIBUTES: the shell looks at the name and the given attributes only, it never opens the file.
            SHFILEINFOW shellInfo{};
            DWORD const attributes = info.attributes != 0 ? info.attributes : FILE_ATTRIBUTE_NORMAL;
            if (SHGetFileInfoW(path.c_str(), attributes, &shellInfo, sizeof(shellInfo), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
            {
                info.typeName = shellInfo.szTypeName;
            }
            return info;
        }

        ShellInfo QueryVirtualInfo(std::wstring const& path)
        {
            ShellInfo info;
            winrt::com_ptr<IShellItem2> item;
            if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()))))
            {
                info.displayName = path;
                return info;
            }
            wchar_t* name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name != nullptr)
            {
                info.displayName = name;
                CoTaskMemFree(name);
            }
            info.typeName = PropertyString(item.get(), PKEY_ItemTypeText);
            ULONGLONG size = 0;
            if (SUCCEEDED(item->GetUInt64(PKEY_Size, &size)))
            {
                info.size = size;
            }
            FILETIME time{};
            if (SUCCEEDED(item->GetFileTime(PKEY_DateCreated, &time)))
            {
                info.created = time;
            }
            if (SUCCEEDED(item->GetFileTime(PKEY_DateModified, &time)))
            {
                info.modified = time;
            }
            return info;
        }

        // 32-bit top-down BGRA pixels of the bitmap (opaque when the bitmap carries no alpha at all).
        bool ReadPixels(HBITMAP bitmap, int& width, int& height, std::vector<uint8_t>& pixels)
        {
            BITMAP info{};
            if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight == 0)
            {
                return false;
            }
            width = info.bmWidth;
            height = std::abs(info.bmHeight);

            BITMAPINFO header{};
            header.bmiHeader.biSize = sizeof(header.bmiHeader);
            header.bmiHeader.biWidth = width;
            header.bmiHeader.biHeight = -height;
            header.bmiHeader.biPlanes = 1;
            header.bmiHeader.biBitCount = 32;
            header.bmiHeader.biCompression = BI_RGB;
            pixels.assign(static_cast<size_t>(width) * height * 4, 0);

            HDC const dc = GetDC(nullptr);
            int const lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &header, DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            if (lines == 0)
            {
                return false;
            }

            bool anyAlpha = false;
            for (size_t i = 3; i < pixels.size() && !anyAlpha; i += 4)
            {
                anyAlpha = pixels[i] != 0;
            }
            if (!anyAlpha)
            {
                for (size_t i = 3; i < pixels.size(); i += 4)
                {
                    pixels[i] = 0xFF;
                }
            }
            return true;
        }

        UniqueHbitmap RequestImage(IShellItemImageFactory& factory, SIZE size, int flags)
        {
            HBITMAP bitmap = nullptr;
            if (SUCCEEDED(factory.GetImage(size, static_cast<SIIGBF>(flags), &bitmap)) && bitmap != nullptr)
            {
                return UniqueHbitmap{bitmap};
            }
            return nullptr;
        }

        UniqueHbitmap GetShellImage(std::wstring const& path, int sizePx, bool cacheOnly)
        {
            winrt::com_ptr<IShellItemImageFactory> factory;
            if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(factory.put()))))
            {
                return nullptr;
            }
            SIZE const size{sizePx, sizePx};
            if (cacheOnly)
            {
                // INCACHEONLY returns what is already cached; on a miss only the icon (no handler, no content read) is used.
                if (auto cached = RequestImage(*factory, size, SIIGBF_BIGGERSIZEOK | SIIGBF_INCACHEONLY))
                {
                    return cached;
                }
                return RequestImage(*factory, size, SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY);
            }
            if (auto image = RequestImage(*factory, size, SIIGBF_BIGGERSIZEOK))
            {
                return image;
            }
            return RequestImage(*factory, size, SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY);
        }
    }

    ShellInfo QueryShellInfo(core::ipc::Item const& item, bool cloudPlaceholder)
    {
        if (item.isVirtual && !cloudPlaceholder)
        {
            return QueryVirtualInfo(item.path);
        }
        return QueryPathInfo(item.path);
    }

    winrt::Windows::Foundation::IAsyncOperation<ImageSource> LoadThumbnailAsync(core::ipc::Item item, int sizePx, bool cacheOnly)
    {
        auto const ui = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (!ui)
        {
            FL_LOG_INFO(L"LoadThumbnailAsync called off the UI thread");
            co_return nullptr;
        }

        co_await winrt::resume_background();
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
        {
            auto const bitmap = GetShellImage(item.path, std::max(sizePx, 16), cacheOnly);
            if (!bitmap || !ReadPixels(bitmap.get(), width, height, pixels))
            {
                pixels.clear();
            }
        }

        if (!co_await ResumeOn(ui) || pixels.empty())
        {
            co_return nullptr;
        }
        using winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap;
        WriteableBitmap target{width, height};
        auto const buffer = target.PixelBuffer();
        uint8_t* destination = nullptr;
        winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&destination));
        memcpy(destination, pixels.data(), std::min<size_t>(pixels.size(), buffer.Length()));
        target.Invalidate();
        co_return target;
    }

    std::wstring FormatBytes(uint64_t bytes)
    {
        wchar_t text[64]{};
        if (FAILED(StrFormatByteSizeEx(bytes, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, text, ARRAYSIZE(text))))
        {
            return std::to_wstring(bytes);
        }
        return text;
    }

    std::wstring FormatDate(FILETIME time)
    {
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        if (!FileTimeToSystemTime(&time, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        {
            return {};
        }
        wchar_t date[64]{};
        wchar_t clock[64]{};
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, ARRAYSIZE(date), nullptr) == 0)
        {
            return {};
        }
        if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, clock, ARRAYSIZE(clock)) == 0)
        {
            return date;
        }
        return std::wstring{date} + L" " + clock;
    }

    std::wstring FormatAttributes(DWORD attributes)
    {
        std::wstring result;
        auto add = [&](DWORD flag, wchar_t letter) {
            if (attributes & flag)
            {
                if (!result.empty())
                {
                    result += L' ';
                }
                result += letter;
            }
        };
        add(FILE_ATTRIBUTE_READONLY, L'R');
        add(FILE_ATTRIBUTE_HIDDEN, L'H');
        add(FILE_ATTRIBUTE_SYSTEM, L'S');
        add(FILE_ATTRIBUTE_ARCHIVE, L'A');
        return result;
    }

    std::wstring FormatPattern(std::wstring_view pattern, std::initializer_list<std::wstring> args)
    {
        std::wstring result{pattern};
        size_t index = 0;
        for (auto const& arg : args)
        {
            std::wstring const token = L"{" + std::to_wstring(index++) + L"}";
            for (size_t at = result.find(token); at != std::wstring::npos; at = result.find(token, at + arg.size()))
            {
                result.replace(at, token.size(), arg);
            }
        }
        return result;
    }
}
