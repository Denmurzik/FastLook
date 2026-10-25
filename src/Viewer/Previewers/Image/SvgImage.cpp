#include "pch.h"
#include "Previewers/Image/SvgImage.h"

#include <algorithm>
#include <optional>
#include <string>

#include "Diagnostics/Log.h"
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

        constexpr uintmax_t kMaxFileBytes = 32ull * 1024 * 1024;
        constexpr uint32_t kMaxSurface = 8192;

        [[noreturn]] void ThrowParseFailed()
        {
            throw PreviewError{std::wstring{Str(L"Image", L"SvgParseFailed")}, std::nullopt};
        }

        std::vector<uint8_t> ReadWholeFile(std::filesystem::path const& path)
        {
            winrt::file_handle file{::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
            LARGE_INTEGER size{};
            if (!file || !::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || static_cast<uintmax_t>(size.QuadPart) > kMaxFileBytes)
            {
                ThrowParseFailed();
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (!::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
            {
                ThrowParseFailed();
            }
            return bytes;
        }

        // Null when the bytes are not an SVG document.
        com_ptr<ID2D1SvgDocument> Parse(ID2D1Device5* device, std::vector<uint8_t> const& bytes, D2D1_SIZE_F viewport)
        {
            com_ptr<ID2D1DeviceContext5> context;
            if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.put())))
            {
                return nullptr;
            }
            com_ptr<IStream> stream;
            stream.attach(::SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
            com_ptr<ID2D1SvgDocument> document;
            if (!stream || FAILED(context->CreateSvgDocument(stream.get(), viewport, document.put())))
            {
                return nullptr;
            }
            return document;
        }

        // A length attribute in user units; percentages and unsupported units count as absent.
        std::optional<double> ReadLength(ID2D1SvgElement* root, wchar_t const* name)
        {
            D2D1_SVG_LENGTH length{};
            if (!root->IsAttributeSpecified(name, nullptr) ||
                FAILED(root->GetAttributeValue(name, D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &length, sizeof(length))) ||
                length.units != D2D1_SVG_LENGTH_UNITS_NUMBER)
            {
                return std::nullopt;
            }
            return length.value;
        }

        std::optional<core::SizeD> ReadViewBox(ID2D1SvgElement* root)
        {
            D2D1_SVG_VIEWBOX box{};
            if (!root->IsAttributeSpecified(L"viewBox", nullptr) ||
                FAILED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &box, sizeof(box))))
            {
                return std::nullopt;
            }
            return core::SizeD{box.width, box.height};
        }

        bool IsSvgRoot(ID2D1SvgElement* root)
        {
            UINT32 const length = root->GetTagNameLength();
            std::wstring name(length + 1, L'\0');
            return SUCCEEDED(root->GetTagName(name.data(), length + 1)) && std::wstring_view{name.c_str()} == L"svg";
        }
    }

    std::shared_ptr<SvgImage> SvgImage::Load(std::filesystem::path const& path, com_ptr<ID2D1Device5> device)
    {
        if (!device)
        {
            ThrowParseFailed();
        }
        std::shared_ptr<SvgImage> image{new SvgImage};
        image->m_bytes = ReadWholeFile(path);
        auto document = Parse(device.get(), image->m_bytes, {512.0f, 512.0f});
        if (!document)
        {
            ThrowParseFailed();
        }
        com_ptr<ID2D1SvgElement> root;
        document->GetRoot(root.put());
        if (!root || !IsSvgRoot(root.get()))
        {
            ThrowParseFailed();
        }
        image->m_size = core::SvgIntrinsicSize(ReadLength(root.get(), L"width"), ReadLength(root.get(), L"height"), ReadViewBox(root.get()));
        image->m_device = std::move(device);
        image->m_document = std::move(document);
        return image;
    }

    bool SvgImage::EnsureDocument(ID2D1Device5* device)
    {
        if (m_document && m_device.get() == device)
        {
            return true;
        }
        // The graphics device was re-created since Load: the document belongs to the old one.
        auto document = Parse(device, m_bytes, {512.0f, 512.0f});
        if (!document)
        {
            return false;
        }
        m_device.copy_from(device);
        m_document = std::move(document);
        return true;
    }

    winrt::Microsoft::UI::Xaml::Media::Imaging::SurfaceImageSource SvgImage::Render(core::SizeU pixels)
    {
        using winrt::Microsoft::UI::Xaml::Media::Imaging::SurfaceImageSource;

        auto& resources = DeviceResources::Get();
        com_ptr<ID2D1Device5> device;
        device.copy_from(resources.D2DDevice());
        if (!device || !EnsureDocument(device.get()))
        {
            FL_LOG_INFO(L"SVG render: no Direct2D device or document");
            return nullptr;
        }
        uint32_t const width = std::clamp(pixels.w, 1u, kMaxSurface);
        uint32_t const height = std::clamp(pixels.h, 1u, kMaxSurface);

        SurfaceImageSource source{static_cast<int32_t>(width), static_cast<int32_t>(height), false};
        auto native = source.as<ISurfaceImageSourceNativeWithD2D>();
        HRESULT hr = native->SetDevice(device.get());
        if (FAILED(hr))
        {
            FL_LOG_INFO(L"SVG render: SetDevice failed: 0x{:08X}", static_cast<uint32_t>(hr));
            resources.HandleDeviceLost(hr);
            return nullptr;
        }

        RECT const update{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        POINT offset{};
        com_ptr<ID2D1DeviceContext> base;
        hr = native->BeginDraw(update, IID_PPV_ARGS(base.put()), &offset);
        if (FAILED(hr))
        {
            FL_LOG_INFO(L"SVG render: BeginDraw failed: 0x{:08X}", static_cast<uint32_t>(hr));
            resources.HandleDeviceLost(hr);
            return nullptr;
        }

        auto context = base.try_as<ID2D1DeviceContext5>();
        if (!context)
        {
            FL_LOG_INFO(L"SVG render: the surface context has no SVG support");
            base = nullptr;
            native->EndDraw();
            return nullptr;
        }
        m_document->SetViewportSize({static_cast<float>(m_size.w), static_cast<float>(m_size.h)});
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(offset.x), static_cast<float>(offset.y),
                                                 static_cast<float>(offset.x + static_cast<LONG>(width)), static_cast<float>(offset.y + static_cast<LONG>(height))),
                                     D2D1_ANTIALIAS_MODE_ALIASED);
        context->Clear(D2D1::ColorF(0, 0.0f));
        context->SetTransform(D2D1::Matrix3x2F::Scale(static_cast<float>(width / m_size.w), static_cast<float>(height / m_size.h)) *
                              D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
        context->DrawSvgDocument(m_document.get());
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->PopAxisAlignedClip();
        context = nullptr; // release our references before EndDraw
        base = nullptr;
        hr = native->EndDraw();
        if (FAILED(hr))
        {
            FL_LOG_INFO(L"SVG render: EndDraw failed: 0x{:08X}", static_cast<uint32_t>(hr));
            resources.HandleDeviceLost(hr);
            return nullptr;
        }
        return source;
    }
}
