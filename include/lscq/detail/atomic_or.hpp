#pragma once

#include <cstdint>

#include <lscq/config.hpp>
#include <lscq/detail/platform.hpp>

#if (LSCQ_ARCH_X86_64 || LSCQ_ARCH_X86_32) && LSCQ_COMPILER_MSVC
#include <intrin.h>
#endif

namespace lscq::detail {

inline void atomic_or_u64(std::uint64_t* ptr, std::uint64_t mask) noexcept {
    if (ptr == nullptr) {
        return;
    }

#if LSCQ_COMPILER_MSVC
    // MSVC/clang-cl: `_InterlockedOr64` is a full barrier.
    (void)_InterlockedOr64(reinterpret_cast<volatile long long*>(ptr), static_cast<long long>(mask));
#elif defined(__clang__) || defined(__GNUC__)
    (void)__atomic_fetch_or(ptr, mask, __ATOMIC_RELEASE);
#else
    // Conservative fallback: CAS loop.
    std::uint64_t cur = 0;
    (void)__atomic_load(ptr, &cur, __ATOMIC_RELAXED);
    while (true) {
        const std::uint64_t desired = cur | mask;
        if (__atomic_compare_exchange(ptr, &cur, &desired, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            return;
        }
    }
#endif
}

}  // namespace lscq::detail

