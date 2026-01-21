#pragma once

#include <lscq/config.hpp>

// Branch prediction hints (hot path friendly, no behavioral change).
//
// NOTE: Keep them as macros so they can wrap arbitrary expressions without introducing
// extra function calls in tight loops.
#if defined(__clang__) || defined(__GNUC__)
#define LSCQ_LIKELY(x) (__builtin_expect(!!(x), 1))
#define LSCQ_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define LSCQ_LIKELY(x) (!!(x))
#define LSCQ_UNLIKELY(x) (!!(x))
#endif

