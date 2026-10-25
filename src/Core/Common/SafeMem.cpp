#include "Common/SafeMem.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace fastlook::core
{
    namespace
    {
        int Filter(EXCEPTION_POINTERS* info, uintptr_t lo, uintptr_t hi)
        {
            auto const* rec = info->ExceptionRecord;
            if ((rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR || rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) &&
                rec->NumberParameters >= 2)
            {
                uintptr_t const address = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
                if (address >= lo && address < hi) return EXCEPTION_EXECUTE_HANDLER;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        struct CopyArgs
        {
            void* dst;
            const void* src;
            size_t n;
        };
    }

    bool SafeCall(void (*fn)(void*), void* ctx, const void* base, size_t size)
    {
        uintptr_t const lo = reinterpret_cast<uintptr_t>(base);
        uintptr_t const hi = lo + size;
        __try
        {
            fn(ctx);
            return true;
        }
        __except (Filter(GetExceptionInformation(), lo, hi))
        {
            return false;
        }
    }

    bool SafeCopy(void* dst, const void* src, size_t n) noexcept
    {
        CopyArgs args{dst, src, n};
        return SafeCall([](void* p) {
            auto* a = static_cast<CopyArgs*>(p);
            std::memcpy(a->dst, a->src, a->n);
        }, &args, src, n);
    }
}
