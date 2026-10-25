#pragma once

#include <cstddef>
#include <type_traits>

namespace fastlook::core
{
    // Runs fn(ctx) and turns an in-page error or access violation whose faulting address lies in
    // [base, base + size) into `false` (e.g. a memory-mapped file truncated by another process).
    // Other exceptions, C++ ones included, propagate unchanged.
    bool SafeCall(void (*fn)(void*), void* ctx, const void* base, size_t size);

    // memcpy that survives a fault while reading the source range. false = the source faulted.
    bool SafeCopy(void* dst, const void* src, size_t n) noexcept;

    template <class F>
    bool SafeRun(const void* base, size_t size, F&& f)
    {
        return SafeCall([](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); }, &f, base, size);
    }
}
