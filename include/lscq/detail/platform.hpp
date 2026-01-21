#pragma once

#include <cstddef>
#include <cstdint>
#include <lscq/config.hpp>

#ifndef LSCQ_ARCH_ARM64
#if defined(_M_ARM64) || defined(_M_ARM64EC) || defined(__aarch64__) || defined(__arm64__)
#define LSCQ_ARCH_ARM64 1
#else
#define LSCQ_ARCH_ARM64 0
#endif
#endif

#if (LSCQ_ARCH_X86_64 || LSCQ_ARCH_X86_32) && LSCQ_COMPILER_MSVC
#include <intrin.h>
#endif

#if (LSCQ_ARCH_X86_64 || LSCQ_ARCH_X86_32) && !LSCQ_COMPILER_MSVC
#if defined(__has_include) && __has_include(<cpuid.h>)
#include <cpuid.h>
#define LSCQ_DETAIL_HAS_CPUID_H 1
#else
#define LSCQ_DETAIL_HAS_CPUID_H 0
#endif
#endif

namespace lscq::detail {

inline bool is_aligned_16(const void* ptr) noexcept {
    return (reinterpret_cast<std::uintptr_t>(ptr) & 0x0F) == 0;
}

inline bool cpu_has_cmpxchg16b() noexcept {
#if (LSCQ_ARCH_X86_64 || LSCQ_ARCH_X86_32)
    // CPUID.(EAX=1):ECX.CX16[bit 13] indicates CMPXCHG16B support.
    // https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html (Vol. 2A
    // CPUID)
    constexpr std::uint32_t kCx16Bit = 1u << 13;

    // MSVC/clang-cl
#if LSCQ_COMPILER_MSVC
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 1);
    return (static_cast<std::uint32_t>(regs[2]) & kCx16Bit) != 0;
#else
    // GCC/Clang (non-MSVC)
#if LSCQ_DETAIL_HAS_CPUID_H
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (ecx & kCx16Bit) != 0;
#else
    return false;
#endif
#endif
#else
    return false;
#endif
}

}  // namespace lscq::detail
