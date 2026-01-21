/**
 * @file config.hpp
 * @brief Public configuration constants and build/target detection macros.
 * @author lscq contributors
 * @version 0.1.0
 *
 * This header is intentionally standalone and can be included by all public queue headers.
 *
 * Example:
 * @code
 * // Construct a queue using the project default capacity.
 * lscq::SCQ<std::uint64_t> q(lscq::config::DEFAULT_SCQSIZE);
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>

/** @def LSCQ_ENABLE_CAS2
 * @brief Build-time toggle to enable the CAS2 fast path.
 *
 * This macro is typically provided by CMake. When disabled, the library will not attempt to use
 * native 128-bit CAS instructions even if the CPU supports them.
 */
#ifndef LSCQ_ENABLE_CAS2
#define LSCQ_ENABLE_CAS2 0
#endif

/** @def LSCQ_ENABLE_SANITIZERS
 * @brief Build-time toggle indicating sanitizer instrumentation is enabled.
 *
 * This macro is typically provided by CMake. It is used to slightly adjust implementation choices
 * for sanitizer-friendly behavior.
 */
#ifndef LSCQ_ENABLE_SANITIZERS
#define LSCQ_ENABLE_SANITIZERS 0
#endif

/** @def LSCQ_COMPILER_CLANG
 * @brief Build-time toggle indicating the active compiler is Clang.
 *
 * This macro is typically provided by CMake and is used for some platform/compiler-specific paths.
 */
#ifndef LSCQ_COMPILER_CLANG
#define LSCQ_COMPILER_CLANG 0
#endif

/** @def LSCQ_PLATFORM_WINDOWS
 * @brief Defined to 1 when building for Windows, otherwise 0.
 */
#if defined(_WIN32) || defined(_WIN64)
#define LSCQ_PLATFORM_WINDOWS 1
#else
#define LSCQ_PLATFORM_WINDOWS 0
#endif

/** @def LSCQ_PLATFORM_LINUX
 * @brief Defined to 1 when building for Linux, otherwise 0.
 */
#if defined(__linux__)
#define LSCQ_PLATFORM_LINUX 1
#else
#define LSCQ_PLATFORM_LINUX 0
#endif

/** @def LSCQ_PLATFORM_MACOS
 * @brief Defined to 1 when building for macOS, otherwise 0.
 */
#if defined(__APPLE__)
#define LSCQ_PLATFORM_MACOS 1
#else
#define LSCQ_PLATFORM_MACOS 0
#endif

/** @def LSCQ_ARCH_X86_64
 * @brief Defined to 1 when building for x86_64, otherwise 0.
 */
#if defined(_M_X64) || defined(__x86_64__)
#define LSCQ_ARCH_X86_64 1
#else
#define LSCQ_ARCH_X86_64 0
#endif

/** @def LSCQ_ARCH_X86_32
 * @brief Defined to 1 when building for x86_32, otherwise 0.
 */
#if defined(_M_IX86) || defined(__i386__)
#define LSCQ_ARCH_X86_32 1
#else
#define LSCQ_ARCH_X86_32 0
#endif

/** @def LSCQ_ARCH_ARM64
 * @brief Defined to 1 when building for AArch64/ARM64, otherwise 0.
 */
#if defined(_M_ARM64) || defined(_M_ARM64EC) || defined(__aarch64__) || defined(__arm64__)
#define LSCQ_ARCH_ARM64 1
#else
#define LSCQ_ARCH_ARM64 0
#endif

/** @def LSCQ_COMPILER_MSVC
 * @brief Defined to 1 when building with MSVC, otherwise 0.
 */
#if defined(_MSC_VER)
#define LSCQ_COMPILER_MSVC 1
#else
#define LSCQ_COMPILER_MSVC 0
#endif

/** @def LSCQ_COMPILER_GCC
 * @brief Defined to 1 when building with GCC (not Clang), otherwise 0.
 */
#if defined(__GNUC__) && !defined(__clang__)
#define LSCQ_COMPILER_GCC 1
#else
#define LSCQ_COMPILER_GCC 0
#endif

namespace lscq {

/** @brief ABI version for public headers (bumped on breaking changes). */
inline constexpr std::uint32_t kAbiVersion = 0;
/** @brief Whether CAS2 support is enabled at build time (may still depend on runtime CPU support).
 */
inline constexpr bool kEnableCas2 = (LSCQ_ENABLE_CAS2 != 0);
/** @brief Whether sanitizers are enabled at build time. */
inline constexpr bool kEnableSanitizers = (LSCQ_ENABLE_SANITIZERS != 0);
/** @brief Whether the active compiler is Clang (as detected by build system). */
inline constexpr bool kCompilerIsClang = (LSCQ_COMPILER_CLANG != 0);

namespace config {

/**
 * @brief Default capacity (in slots) for NCQ/SCQ-family ring queues.
 *
 * @note This is a convenience default used by queue constructors. Specific queue implementations
 * may clamp or round this value to meet alignment/cache-line constraints.
 */
inline constexpr std::size_t DEFAULT_SCQSIZE = 65536;

/**
 * @brief Default capacity (in slots) for generic queue-like components.
 *
 * @note This constant is intentionally separate from @ref DEFAULT_SCQSIZE so benchmarks and
 * experiments can tune ring-queue and non-ring-queue defaults independently.
 */
inline constexpr std::size_t DEFAULT_QSIZE = 32768;

}  // namespace config

}  // namespace lscq
