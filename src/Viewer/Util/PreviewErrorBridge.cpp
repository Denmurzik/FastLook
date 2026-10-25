#include "pch.h"
#include "Util/PreviewErrorBridge.h"

#include <new>
#include <stdexcept>

#include "Diagnostics/Log.h"

namespace fastlook::viewer
{
    namespace
    {
        // Customer bit set: never collides with a system HRESULT.
        constexpr int32_t kPreviewErrorHr = static_cast<int32_t>(0xA0FA1001);

        // Same conversions as the C++/WinRT default handler, plus PreviewError. Message layout: "<productId>\n<message>".
        int32_t __stdcall ToHresult(void*) noexcept
        {
            try
            {
                throw;
            }
            catch (PreviewError const& e)
            {
                std::wstring text = e.storeProductId.value_or(std::wstring{});
                text += L'\n';
                text += e.message;
                return winrt::hresult_error(kPreviewErrorHr, winrt::hstring{text}).to_abi();
            }
            catch (winrt::hresult_error const& e)
            {
                return e.to_abi();
            }
            catch (std::bad_alloc const&)
            {
                return static_cast<int32_t>(E_OUTOFMEMORY);
            }
            catch (std::out_of_range const& e)
            {
                return winrt::hresult_out_of_bounds(winrt::to_hstring(e.what())).to_abi();
            }
            catch (std::invalid_argument const& e)
            {
                return winrt::hresult_invalid_argument(winrt::to_hstring(e.what())).to_abi();
            }
            catch (std::exception const& e)
            {
                return winrt::hresult_error(E_FAIL, winrt::to_hstring(e.what())).to_abi();
            }
            catch (...)
            {
                FL_LOG_INFO(L"Unknown exception type crossed an async boundary");
                return static_cast<int32_t>(E_FAIL);
            }
        }
    }

    void InstallPreviewErrorBridge()
    {
        ::winrt_to_hresult_handler = &ToHresult;
    }

    std::optional<PreviewError> TryDecodePreviewError(winrt::hresult_error const& error)
    {
        if (error.code().value != kPreviewErrorHr)
        {
            return std::nullopt;
        }
        std::wstring_view const text = error.message();
        auto const split = text.find(L'\n');
        if (split == std::wstring_view::npos)
        {
            return PreviewError{std::wstring{text}, std::nullopt};
        }
        PreviewError result{std::wstring{text.substr(split + 1)}, std::nullopt};
        if (split > 0)
        {
            result.storeProductId = std::wstring{text.substr(0, split)};
        }
        return result;
    }
}
