#pragma once

#include <cstdint>
#include <lscq/config.hpp>

#if LSCQ_COMPILER_MSVC
#include <intrin.h>
#endif

namespace lscq::detail {

// Count trailing zeros for non-zero 64-bit value.
inline unsigned ctz_u64(std::uint64_t v) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return static_cast<unsigned>(__builtin_ctzll(v));
#elif LSCQ_COMPILER_MSVC
    unsigned long idx = 0;
#if defined(_WIN64)
    _BitScanForward64(&idx, v);
    return static_cast<unsigned>(idx);
#else
    const std::uint32_t lo = static_cast<std::uint32_t>(v);
    if (lo != 0u) {
        _BitScanForward(&idx, lo);
        return static_cast<unsigned>(idx);
    }
    const std::uint32_t hi = static_cast<std::uint32_t>(v >> 32u);
    _BitScanForward(&idx, hi);
    return static_cast<unsigned>(idx + 32u);
#endif
#else
    unsigned n = 0;
    while ((v & 1u) == 0u) {
        v >>= 1u;
        ++n;
    }
    return n;
#endif
}

// log2(v) for v being a power-of-two (and non-zero).
inline unsigned log2_pow2_u64(std::uint64_t v) noexcept { return ctz_u64(v); }

}  // namespace lscq::detail
