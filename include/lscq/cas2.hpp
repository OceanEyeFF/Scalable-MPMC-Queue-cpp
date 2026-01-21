/**
 * @file cas2.hpp
 * @brief 128-bit compare-and-swap (CAS2) abstraction layer.
 * @author lscq contributors
 * @version 0.1.0
 *
 * This header provides a small, portable API for atomically updating a 16-byte slot payload
 * (two 64-bit words). When the platform supports a native 128-bit CAS instruction, the fast
 * path is used; otherwise the implementation falls back to a striped mutex.
 *
 * Thread-safety: All public functions are thread-safe.
 *
 * Complexity: O(1) expected. The fallback path may block on a mutex under contention.
 *
 * Example:
 * @code
 * lscq::Entry slot{0, 0};
 * lscq::Entry expected = slot;
 * lscq::Entry desired{1, 42};
 *
 * if (!lscq::cas2(&slot, expected, desired)) {
 *     // CAS failed; expected now contains the observed value from slot.
 * }
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <lscq/config.hpp>
#include <lscq/detail/platform.hpp>
#include <mutex>
#include <new>

#if LSCQ_ARCH_X86_64 && LSCQ_PLATFORM_WINDOWS && LSCQ_COMPILER_MSVC
#include <intrin.h>
#endif

#if defined(__clang__) || defined(__GNUC__)
#define LSCQ_DETAIL_ATTR_TARGET_CX16 __attribute__((target("cx16")))
#else
#define LSCQ_DETAIL_ATTR_TARGET_CX16
#endif

namespace lscq {

/**
 * @brief 16-byte slot payload used by NCQ/SCQ-family queues.
 *
 * This type represents two adjacent 64-bit words that are updated together:
 * - @c cycle_flags typically stores an epoch/cycle counter plus small flags packed in the low bits.
 * - @c index_or_ptr stores a 64-bit index value (NCQ/SCQ) or pointer bits (SCQP fast path).
 *
 * @note This type is intentionally POD and is required to be exactly 16 bytes and 16-byte aligned.
 *       The CAS2 fast path requires 16-byte alignment; if misaligned, the implementation falls back
 *       to a striped mutex.
 */
struct alignas(16) Entry {
    /** @brief Packed cycle counter and flags (implementation-defined bit layout). */
    std::uint64_t cycle_flags;
    /** @brief Slot payload (index value or pointer bits, depending on the queue/fast path). */
    std::uint64_t index_or_ptr;
};

static_assert(sizeof(Entry) == 16, "Entry must be 16 bytes");
static_assert(alignof(Entry) == 16, "Entry must be 16-byte aligned");

/**
 * @brief Compare two @ref Entry values for equality.
 * @param a Left-hand side.
 * @param b Right-hand side.
 * @return true if both 64-bit words match.
 */
inline constexpr bool operator==(const Entry& a, const Entry& b) noexcept {
    return a.cycle_flags == b.cycle_flags && a.index_or_ptr == b.index_or_ptr;
}

/**
 * @brief Compare two @ref Entry values for inequality.
 * @param a Left-hand side.
 * @param b Right-hand side.
 * @return true if either 64-bit word differs.
 */
inline constexpr bool operator!=(const Entry& a, const Entry& b) noexcept { return !(a == b); }

namespace detail {

#ifndef LSCQ_CAS2_FALLBACK_STRIPE_COUNT
#define LSCQ_CAS2_FALLBACK_STRIPE_COUNT 32u
#endif

inline constexpr std::size_t kCas2FallbackStripeCount =
    static_cast<std::size_t>(LSCQ_CAS2_FALLBACK_STRIPE_COUNT);

static_assert((kCas2FallbackStripeCount & (kCas2FallbackStripeCount - 1u)) == 0u,
              "LSCQ_CAS2_FALLBACK_STRIPE_COUNT must be a power of two");
static_assert(kCas2FallbackStripeCount == 16u || kCas2FallbackStripeCount == 32u,
              "LSCQ_CAS2_FALLBACK_STRIPE_COUNT must be 16 or 32");

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kCas2FallbackCacheLineSize = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCas2FallbackCacheLineSize = 64u;
#endif

struct alignas(kCas2FallbackCacheLineSize) Cas2FallbackStripeLock {
    std::mutex mutex;
};

static_assert(alignof(Cas2FallbackStripeLock) == kCas2FallbackCacheLineSize,
              "Cas2FallbackStripeLock must be cache-line aligned");
static_assert(sizeof(Cas2FallbackStripeLock) % kCas2FallbackCacheLineSize == 0u,
              "Cas2FallbackStripeLock must not share cache lines");

inline Cas2FallbackStripeLock g_cas2_fallback_stripe_locks[kCas2FallbackStripeCount];

inline std::size_t cas2_fallback_stripe_index(const void* ptr) noexcept {
    // Keep the hash cheap and stable:
    // - shift by 4 to ignore low alignment bits
    // - mask with (N-1) where N is a power-of-two
    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(ptr);
    const std::uintptr_t mask = static_cast<std::uintptr_t>(kCas2FallbackStripeCount - 1u);
    return static_cast<std::size_t>((p >> 4u) & mask);
}

inline std::mutex& cas2_fallback_mutex_for(const void* ptr) noexcept {
    return g_cas2_fallback_stripe_locks[cas2_fallback_stripe_index(ptr)].mutex;
}

inline bool cas2_mutex(Entry* ptr, Entry& expected, const Entry& desired) noexcept {
    std::lock_guard<std::mutex> lock(cas2_fallback_mutex_for(ptr));
    const Entry current = *ptr;
    if (current != expected) {
        expected = current;
        return false;
    }
    *ptr = desired;
    return true;
}

#if LSCQ_ARCH_X86_64 && LSCQ_PLATFORM_WINDOWS && LSCQ_COMPILER_MSVC
static_assert(sizeof(long long) == 8, "long long must be 64-bit for CMPXCHG16B intrinsics");

inline long long u64_to_ll(std::uint64_t v) noexcept {
    long long out = 0;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}
inline std::uint64_t ll_to_u64(long long v) noexcept {
    std::uint64_t out = 0;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}

LSCQ_DETAIL_ATTR_TARGET_CX16 inline bool cas2_native(Entry* ptr, Entry& expected,
                                                     const Entry& desired) noexcept {
    if (!detail::is_aligned_16(ptr)) {
        return cas2_mutex(ptr, expected, desired);
    }

    alignas(16) long long comparand[2];
    comparand[0] = u64_to_ll(expected.cycle_flags);
    comparand[1] = u64_to_ll(expected.index_or_ptr);

    const long long exchange_low = u64_to_ll(desired.cycle_flags);
    const long long exchange_high = u64_to_ll(desired.index_or_ptr);

    const char ok = _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(ptr),
                                                   exchange_high, exchange_low, comparand);

    expected.cycle_flags = ll_to_u64(comparand[0]);
    expected.index_or_ptr = ll_to_u64(comparand[1]);
    return ok != 0;
}
#elif LSCQ_ARCH_X86_64 && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16) && \
    (defined(__GNUC__) || defined(__clang__))
LSCQ_DETAIL_ATTR_TARGET_CX16 inline bool cas2_native(Entry* ptr, Entry& expected,
                                                     const Entry& desired) noexcept {
    if (!detail::is_aligned_16(ptr)) {
        return cas2_mutex(ptr, expected, desired);
    }

    Entry expected_local = expected;
    Entry desired_local = desired;
    const bool ok = __atomic_compare_exchange(ptr, &expected_local, &desired_local, false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    expected = expected_local;
    return ok;
}
#elif LSCQ_ARCH_ARM64 && (defined(__clang__) || defined(__GNUC__))
inline bool cas2_native(Entry* ptr, Entry& expected, const Entry& desired) noexcept {
    if (!detail::is_aligned_16(ptr)) {
        return cas2_mutex(ptr, expected, desired);
    }

    // AArch64:
    // - Prefer LSE CASP (when enabled for the target).
    // - Fall back to the compiler's 16-byte atomics lowering (typically LDAXP/STLXP loop).
    //
    // We use seq_cst to match other implementations' semantics.
    const std::uint64_t expected0_in = expected.cycle_flags;
    const std::uint64_t expected1_in = expected.index_or_ptr;

#if defined(__ARM_FEATURE_ATOMICS) && !(LSCQ_PLATFORM_WINDOWS && LSCQ_COMPILER_MSVC)
    std::uint64_t expected0 = expected0_in;
    std::uint64_t expected1 = expected1_in;
    const std::uint64_t desired0 = desired.cycle_flags;
    const std::uint64_t desired1 = desired.index_or_ptr;

    // NOTE: `caspal` returns the observed memory value in the "expected" registers regardless of
    // success/failure (like other CAS instructions). Success is detected by checking whether the
    // returned pair equals the original expected pair.
    __asm__ __volatile__(
        "dmb ish\n"
        "caspal %x[expected0], %x[expected1], %x[desired0], %x[desired1], [%x[ptr]]\n"
        "dmb ish\n"
        : [expected0] "+r"(expected0), [expected1] "+r"(expected1)
        : [ptr] "r"(ptr), [desired0] "r"(desired0), [desired1] "r"(desired1)
        : "cc", "memory");

    expected.cycle_flags = expected0;
    expected.index_or_ptr = expected1;
    return (expected0 == expected0_in) && (expected1 == expected1_in);
#else
    Entry expected_local = expected;
    Entry desired_local = desired;
    const bool ok = __atomic_compare_exchange(ptr, &expected_local, &desired_local, false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    expected = expected_local;
    return ok;
#endif
}
#else
inline bool cas2_native(Entry* ptr, Entry& expected, const Entry& desired) noexcept {
    return cas2_mutex(ptr, expected, desired);
}
#endif

}  // namespace detail

/**
 * @brief Check whether the current process can use the native CAS2 fast path.
 *
 * On x86_64 this performs a one-time CPUID check for CMPXCHG16B and caches the result.
 *
 * @return true if native 128-bit CAS is enabled and available at runtime; false otherwise.
 *
 * @note This function is safe to call from hot paths; the capability result is cached.
 */
inline bool has_cas2_support() noexcept {
#if !LSCQ_ENABLE_CAS2
    return false;
#elif LSCQ_ARCH_X86_64
    // Avoid executing CMPXCHG16B on CPUs that don't support it.
    //
    // This capability check is used on the hot path (enqueue/dequeue), so we cache it to avoid
    // repeated CPUID overhead.
    static const bool supported = detail::cpu_has_cmpxchg16b();
    return supported;
#elif LSCQ_ARCH_ARM64 && (defined(__clang__) || defined(__GNUC__))
    // AArch64 always supports an atomic 16-byte CAS via LDAXP/STLXP loop, and may use CASP when
    // compiling for an LSE-enabled target.
    return true;
#else
    return false;
#endif
}

/**
 * @brief Atomically compare-and-swap a 16-byte @ref Entry.
 *
 * If @p *ptr equals @p expected, writes @p desired into @p *ptr and returns true. Otherwise updates
 * @p expected with the current value from @p *ptr and returns false.
 *
 * @param ptr Pointer to the @ref Entry to update.
 * @param[in,out] expected Expected old value; updated with the observed value on failure.
 * @param desired Desired new value.
 * @return true if the swap succeeded; false otherwise.
 *
 * @note If @p ptr is null, this function returns false and leaves @p expected unchanged.
 * @note When the CAS2 fast path is unavailable, a striped mutex fallback is used.
 *
 * Thread-safety: Safe for concurrent callers; the fallback may block.
 *
 * Complexity: O(1) expected.
 */
inline bool cas2(Entry* ptr, Entry& expected, const Entry& desired) noexcept {
    if (ptr == nullptr) {
        return false;
    }

#if LSCQ_ENABLE_CAS2
    if (has_cas2_support()) {
        return detail::cas2_native(ptr, expected, desired);
    }
#endif
    return detail::cas2_mutex(ptr, expected, desired);
}

}  // namespace lscq

#undef LSCQ_DETAIL_ATTR_TARGET_CX16
