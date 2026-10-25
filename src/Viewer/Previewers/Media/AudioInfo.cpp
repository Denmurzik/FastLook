#include "pch.h"
#include "Previewers/Media/AudioInfo.h"

#include <propvarutil.h>

#include <winrt/base.h>

namespace fastlook::viewer
{
    namespace
    {
        // Artist is a string vector in the shell's schema; the other text properties are plain strings.
        std::wstring ReadString(IPropertyStore* store, PROPERTYKEY const& key)
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (FAILED(store->GetValue(key, &value)))
            {
                return {};
            }
            std::wstring result;
            if (value.vt == VT_LPWSTR && value.pwszVal)
            {
                result = value.pwszVal;
            }
            else if (value.vt == (VT_VECTOR | VT_LPWSTR))
            {
                for (ULONG i = 0; i < value.calpwstr.cElems; ++i)
                {
                    if (value.calpwstr.pElems[i])
                    {
                        result += (i ? L"; " : L"");
                        result += value.calpwstr.pElems[i];
                    }
                }
            }
            PropVariantClear(&value);
            return result;
        }
    }

    AudioInfo ReadAudioInfo(std::filesystem::path const& path)
    {
        AudioInfo info;
        winrt::com_ptr<IPropertyStore> store;
        if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT | GPS_BESTEFFORT, IID_PPV_ARGS(store.put()))))
        {
            return info;
        }
        info.title = ReadString(store.get(), PKEY_Title);
        info.artist = ReadString(store.get(), PKEY_Music_Artist);
        info.album = ReadString(store.get(), PKEY_Music_AlbumTitle);

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(store->GetValue(PKEY_Media_Duration, &duration)) && duration.vt == VT_UI8)
        {
            info.duration = std::chrono::milliseconds{static_cast<int64_t>(duration.uhVal.QuadPart / 10000)}; // 100 ns units
        }
        PropVariantClear(&duration);
        return info;
    }
}
