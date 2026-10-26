#include "pch.h"
#include "Previewers/Image/WicDecoder.h"

#include <MemoryBuffer.h>

#include <algorithm>

#include "Diagnostics/PerfLog.h"
#include "FileTypes/StoreLinks.h"
#include "Graphics/DeviceResources.h"
#include "Imaging/DecodeMath.h"
#include "Previewers/PreviewContext.h"
#include "Util/Str.h"

namespace fastlook::viewer
{
    namespace
    {
        using winrt::check_hresult;
        using winrt::com_ptr;

        [[noreturn]] void ThrowNoCodec(std::filesystem::path const& path)
        {
            std::optional<std::wstring> product;
            if (auto const codec = core::MissingCodecForImage(path.filename().wstring()))
            {
                product = std::wstring{core::StoreProductId(*codec)};
            }
            throw PreviewError{std::wstring{Str(L"Image", L"NoCodec")}, std::move(product)};
        }

        [[noreturn]] void ThrowDecodeFailed()
        {
            throw PreviewError{std::wstring{Str(L"Image", L"DecodeFailed")}, std::nullopt};
        }

        uint16_t ReadOrientation(IWICBitmapFrameDecode* frame)
        {
            com_ptr<IWICMetadataQueryReader> reader;
            if (FAILED(frame->GetMetadataQueryReader(reader.put())))
            {
                return 1;
            }
            // JPEG keeps EXIF in APP1; TIFF and HEIF at the top level.
            for (auto const* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"})
            {
                PROPVARIANT value;
                PropVariantInit(&value);
                bool const ok = SUCCEEDED(reader->GetMetadataByName(query, &value)) && value.vt == VT_UI2;
                uint16_t const orientation = ok ? value.uiVal : 0;
                PropVariantClear(&value);
                if (orientation >= 1 && orientation <= 8)
                {
                    return orientation;
                }
            }
            return 1;
        }

        com_ptr<IWICBitmapFrameDecode> PickFrame(IWICBitmapDecoder* decoder, uint32_t frameCount, bool largest)
        {
            com_ptr<IWICBitmapFrameDecode> best;
            uint64_t bestArea = 0;
            uint32_t const count = largest ? frameCount : 1;
            for (uint32_t i = 0; i < count; ++i)
            {
                com_ptr<IWICBitmapFrameDecode> frame;
                check_hresult(decoder->GetFrame(i, frame.put()));
                UINT w = 0, h = 0;
                check_hresult(frame->GetSize(&w, &h));
                if (!best || static_cast<uint64_t>(w) * h > bestArea)
                {
                    best = frame;
                    bestArea = static_cast<uint64_t>(w) * h;
                }
            }
            return best;
        }

        // Asks the codec for a natively scaled copy (JPEG DCT scaling), so a 24 MP photo never exists at full size.
        // Returns null when the codec cannot scale or the result would not be smaller than the source.
        com_ptr<IWICBitmapSource> TryNativeScale(IWICImagingFactory2* wic, com_ptr<IWICBitmapFrameDecode> const& frame, core::SizeU native,
                                                 core::SizeU want)
        {
            auto transform = frame.try_as<IWICBitmapSourceTransform>();
            if (!transform)
            {
                return nullptr;
            }
            UINT w = want.w, h = want.h;
            if (FAILED(transform->GetClosestSize(&w, &h)) || w == 0 || h == 0 || w < want.w || h < want.h || (w >= native.w && h >= native.h))
            {
                return nullptr;
            }
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
            if (FAILED(transform->GetClosestPixelFormat(&format)))
            {
                return nullptr;
            }
            com_ptr<IWICComponentInfo> info;
            UINT bpp = 0;
            if (FAILED(wic->CreateComponentInfo(format, info.put())))
            {
                return nullptr;
            }
            auto formatInfo = info.try_as<IWICPixelFormatInfo>();
            if (!formatInfo || FAILED(formatInfo->GetBitsPerPixel(&bpp)) || bpp == 0 || bpp % 8 != 0)
            {
                return nullptr;
            }
            // Decode straight into the bitmap's own buffer: one copy of the DCT-scaled pixels, not two.
            com_ptr<IWICBitmap> bitmap;
            if (FAILED(wic->CreateBitmap(w, h, format, WICBitmapCacheOnLoad, bitmap.put())))
            {
                return nullptr;
            }
            {
                com_ptr<IWICBitmapLock> lock;
                WICRect const all{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
                UINT stride = 0, size = 0;
                BYTE* data = nullptr;
                if (FAILED(bitmap->Lock(&all, WICBitmapLockWrite, lock.put())) || FAILED(lock->GetStride(&stride)) ||
                    FAILED(lock->GetDataPointer(&size, &data)) ||
                    FAILED(transform->CopyPixels(nullptr, w, h, &format, WICBitmapTransformRotate0, stride, size, data)))
                {
                    return nullptr;
                }
            }
            return bitmap.as<IWICBitmapSource>();
        }

        com_ptr<IWICBitmapSource> ToPbgra(IWICImagingFactory2* wic, com_ptr<IWICBitmapSource> const& source)
        {
            WICPixelFormatGUID format{};
            check_hresult(source->GetPixelFormat(&format));
            if (format == GUID_WICPixelFormat32bppPBGRA)
            {
                return source;
            }
            com_ptr<IWICFormatConverter> converter;
            check_hresult(wic->CreateFormatConverter(converter.put()));
            check_hresult(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                                WICBitmapPaletteTypeCustom));
            return converter.as<IWICBitmapSource>();
        }

        com_ptr<IWICBitmapSource> Scale(IWICImagingFactory2* wic, com_ptr<IWICBitmapSource> const& source, core::SizeU size)
        {
            UINT w = 0, h = 0;
            check_hresult(source->GetSize(&w, &h));
            if (w == size.w && h == size.h)
            {
                return source;
            }
            com_ptr<IWICBitmapScaler> scaler;
            check_hresult(wic->CreateBitmapScaler(scaler.put()));
            check_hresult(scaler->Initialize(source.get(), size.w, size.h, WICBitmapInterpolationModeHighQualityCubic));
            return scaler.as<IWICBitmapSource>();
        }

        // Rotation first, then the horizontal mirror, as two stages: unambiguous whatever order a combined option implies.
        com_ptr<IWICBitmapSource> Orient(IWICImagingFactory2* wic, com_ptr<IWICBitmapSource> source, uint16_t orientation)
        {
            int const degrees = core::ExifRotationDegrees(orientation);
            if (degrees != 0)
            {
                com_ptr<IWICBitmapFlipRotator> rotator;
                check_hresult(wic->CreateBitmapFlipRotator(rotator.put()));
                auto const option = degrees == 90 ? WICBitmapTransformRotate90 : degrees == 180 ? WICBitmapTransformRotate180 : WICBitmapTransformRotate270;
                check_hresult(rotator->Initialize(source.get(), option));
                source = rotator.as<IWICBitmapSource>();
            }
            if (core::ExifFlipsHorizontally(orientation))
            {
                com_ptr<IWICBitmapFlipRotator> flipper;
                check_hresult(wic->CreateBitmapFlipRotator(flipper.put()));
                check_hresult(flipper->Initialize(source.get(), WICBitmapTransformFlipHorizontal));
                source = flipper.as<IWICBitmapSource>();
            }
            return source;
        }

        DecodedImage Decode(std::filesystem::path const& path, core::SizeU target, std::stop_token const& stop, com_ptr<IWICImagingFactory2> const& wicRef)
        {
            if (!wicRef)
            {
                ThrowDecodeFailed();
            }
            auto* wic = wicRef.get();

            com_ptr<IWICBitmapDecoder> decoder;
            HRESULT const hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put());
            if (hr == WINCODEC_ERR_COMPONENTNOTFOUND)
            {
                ThrowNoCodec(path);
            }
            check_hresult(hr);

            GUID container{};
            check_hresult(decoder->GetContainerFormat(&container));
            UINT frameCount = 1;
            check_hresult(decoder->GetFrameCount(&frameCount));
            auto frame = PickFrame(decoder.get(), std::max(frameCount, 1u), container == GUID_ContainerFormatIco);

            core::perf::Mark("dec_opened");
            UINT nativeW = 0, nativeH = 0;
            check_hresult(frame->GetSize(&nativeW, &nativeH));
            DecodedImage result;
            result.native = {nativeW, nativeH};
            result.frameCount = frameCount;
            result.orientation = ReadOrientation(frame.get());
            if (stop.stop_requested())
            {
                return result;
            }

            // The target is in oriented space; WIC decodes in stored space, and orientation swaps are their own inverse.
            auto const oriented = core::OrientedSize(result.native, result.orientation);
            auto const fitted = core::ComputeDecodeSize(oriented, {static_cast<double>(target.w), static_cast<double>(target.h)}, 1.0);
            core::SizeU const stored = core::OrientedSize(fitted, result.orientation);

            com_ptr<IWICBitmapSource> source = TryNativeScale(wic, frame, result.native, stored);
            core::perf::Mark("dec_native_scaled", source ? "dct=1" : "dct=0");
            if (!source)
            {
                source = frame.as<IWICBitmapSource>();
            }
            source = ToPbgra(wic, source);
            source = Scale(wic, source, stored);
            source = Orient(wic, std::move(source), result.orientation);

            if (stop.stop_requested())
            {
                return result;
            }

            using namespace winrt::Windows::Graphics::Imaging;
            SoftwareBitmap bitmap{BitmapPixelFormat::Bgra8, static_cast<int32_t>(fitted.w), static_cast<int32_t>(fitted.h), BitmapAlphaMode::Premultiplied};
            {
                auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                auto const stride = static_cast<UINT>(buffer.GetPlaneDescription(0).Stride);
                auto reference = buffer.CreateReference();
                BYTE* data = nullptr;
                UINT capacity = 0;
                check_hresult(reference.as<::Windows::Foundation::IMemoryBufferByteAccess>()->GetBuffer(&data, &capacity));
                WICRect const rect{0, 0, static_cast<INT>(fitted.w), static_cast<INT>(fitted.h)};
                HRESULT const copied = source->CopyPixels(&rect, stride, capacity, data);
                reference.Close();
                buffer.Close();
                check_hresult(copied);
            }
            core::perf::Mark("dec_copied", "w=" + std::to_string(fitted.w) + " h=" + std::to_string(fitted.h));
            result.bitmap = std::move(bitmap);
            return result;
        }
    }

    winrt::com_ptr<IWICImagingFactory2> WicFactory()
    {
        winrt::com_ptr<IWICImagingFactory2> factory;
        factory.copy_from(DeviceResources::Get().Wic());
        return factory;
    }

    DecodedImage DecodeToSize(std::filesystem::path const& path, core::SizeU target, std::stop_token stop,
                              winrt::com_ptr<IWICImagingFactory2> wic)
    {
        try
        {
            return Decode(path, target, stop, wic);
        }
        catch (PreviewError const&)
        {
            throw;
        }
        catch (winrt::hresult_error const& e)
        {
            if (e.code() == WINCODEC_ERR_COMPONENTNOTFOUND || core::MissingCodecForImage(path.filename().wstring()))
            {
                ThrowNoCodec(path);
            }
            ThrowDecodeFailed();
        }
        catch (std::exception const&)
        {
            ThrowDecodeFailed();
        }
    }
}
