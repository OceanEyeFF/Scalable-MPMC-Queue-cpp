#pragma once

#include <cstdint>
#include <cstring>

#include <lscq/config.hpp>

#if (LSCQ_ARCH_X86_64 || LSCQ_ARCH_X86_32) && LSCQ_COMPILER_MSVC
#include <intrin.h>
#endif

namespace lscq::detail {

template <class T>
inline T* atomic_exchange_ptr(T** ptr, T* desired) noexcept {
    if (ptr == nullptr) {
        return nullptr;
    }

#if defined(__clang__) || defined(__GNUC__)
    return __atomic_exchange_n(ptr, desired, __ATOMIC_ACQ_REL);
#elif LSCQ_COMPILER_MSVC
    static_assert(sizeof(T*) == sizeof(void*));
    return reinterpret_cast<T*>(
        _InterlockedExchangePointer(reinterpret_cast<void* volatile*>(ptr),
                                    reinterpret_cast<void*>(desired)));
#else
    // Conservative fallback: CAS loop.
    T* cur = nullptr;
    std::memcpy(&cur, ptr, sizeof(cur));
    while (true) {
        T* expected = cur;
        if (__atomic_compare_exchange(ptr, &expected, &desired, true, __ATOMIC_ACQ_REL,
                                      __ATOMIC_RELAXED)) {
            return cur;
        }
        cur = expected;
    }
#endif
}

template <class T>
inline bool atomic_compare_exchange_ptr(T** ptr, T*& expected, T* desired) noexcept {
    if (ptr == nullptr) {
        return false;
    }

#if defined(__clang__) || defined(__GNUC__)
    return __atomic_compare_exchange_n(ptr, &expected, desired, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
#elif LSCQ_COMPILER_MSVC
    static_assert(sizeof(T*) == sizeof(void*));
    void* const prev = _InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(ptr), reinterpret_cast<void*>(desired),
        reinterpret_cast<void*>(expected));
    if (prev == expected) {
        return true;
    }
    expected = reinterpret_cast<T*>(prev);
    return false;
#else
    return __atomic_compare_exchange(ptr, &expected, &desired, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
#endif
}

inline std::uint64_t atomic_exchange_u64(std::uint64_t* ptr, std::uint64_t desired) noexcept {
    if (ptr == nullptr) {
        return 0;
    }

#if defined(__clang__) || defined(__GNUC__)
    return __atomic_exchange_n(ptr, desired, __ATOMIC_ACQ_REL);
#elif LSCQ_COMPILER_MSVC
    static_assert(sizeof(long long) == 8, "long long must be 64-bit");
    return static_cast<std::uint64_t>(
        _InterlockedExchange64(reinterpret_cast<volatile long long*>(ptr),
                               static_cast<long long>(desired)));
#else
    std::uint64_t cur = 0;
    (void)__atomic_load(ptr, &cur, __ATOMIC_RELAXED);
    while (true) {
        std::uint64_t expected = cur;
        if (__atomic_compare_exchange(ptr, &expected, &desired, true, __ATOMIC_ACQ_REL,
                                      __ATOMIC_RELAXED)) {
            return cur;
        }
        cur = expected;
    }
#endif
}

}  // namespace lscq::detail

