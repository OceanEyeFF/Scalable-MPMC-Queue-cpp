#pragma once

#include <lscq/cas2.hpp>

namespace lscq::detail {

// TSan-safe "atomic" load of a 16-byte Entry via CAS2.
inline Entry entry_load(Entry* ptr) noexcept {
    Entry expected{0, 0};
    // No-op CAS: desired == expected. On success, the value is unchanged. On failure,
    // cas2 updates `expected` to the current value, giving us an atomic read.
    (void)lscq::cas2(ptr, expected, expected);
    return expected;
}

}  // namespace lscq::detail
