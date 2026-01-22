#include <atomic>
#include <cstddef>
#include <cstdint>
#include <lscq/detail/atomic_or.hpp>
#include <lscq/detail/bit.hpp>
#include <lscq/detail/likely.hpp>
#include <lscq/detail/ncq_impl.hpp>
#include <lscq/scq.hpp>
#include <new>

namespace lscq {

namespace {

constexpr std::size_t CACHE_LINE_SIZE = 64;

inline bool cycle_less(std::uint64_t a, std::uint64_t b) noexcept {
    // Signed subtraction handles wraparounds (paper, Figure 8 note).
    return static_cast<std::int64_t>(a - b) < 0;
}

constexpr bool is_power_of_two(std::size_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

constexpr std::size_t round_up_pow2(std::size_t v) noexcept {
    if (v <= 1) {
        return 1;
    }
    if (is_power_of_two(v)) {
        return v;
    }
    std::size_t out = 1;
    while (out < v) {
        out <<= 1;
    }
    return out;
}

}  // namespace

template <class T>
SCQ<T>::SCQ(std::size_t scqsize)
    : entries_(nullptr),
      scqsize_(scqsize),
      qsize_(0),
      bottom_(0),
      head_(0),
      tail_(0),
      threshold_(0) {
    static_assert(sizeof(Entry) == 16);
    static_assert(alignof(Entry) == 16);

    // SCQ requires a power-of-two ring. The algorithm is defined for SCQSIZE = 2n.
    if (scqsize_ < 4) {
        scqsize_ = 4;
    }
    scqsize_ = round_up_pow2(scqsize_);

    // Ensure 2n shape (scqsize even). Power-of-two implies even for scqsize >= 4.
    qsize_ = scqsize_ / 2;
    if (qsize_ == 0) {
        qsize_ = 1;
        scqsize_ = 2;
    }
    bottom_ = static_cast<std::uint64_t>(scqsize_ - 1);

    Entry* raw = static_cast<Entry*>(
        ::operator new[](scqsize_ * sizeof(Entry), std::align_val_t(CACHE_LINE_SIZE)));
    entries_.reset(raw);

    for (std::size_t i = 0; i < scqsize_; ++i) {
        new (&entries_[i]) Entry{pack_cycle_flags(0, true), bottom_};
    }

    // Initialize head/tail to SCQSIZE (cycle 1) while all entries start with cycle 0.
    head_.store(static_cast<std::uint64_t>(scqsize_), std::memory_order_relaxed);
    tail_.store(static_cast<std::uint64_t>(scqsize_), std::memory_order_relaxed);

    // 3 * QSIZE - 1, with QSIZE = SCQSIZE / 2 (SCQSIZE is power-of-two).
    threshold_.store(static_cast<std::int64_t>(static_cast<std::uint64_t>(scqsize_) +
                                               (static_cast<std::uint64_t>(scqsize_) >> 1u) - 1u),
                     std::memory_order_relaxed);
}

template <class T>
SCQ<T>::~SCQ() = default;

template <class T>
std::size_t SCQ<T>::cache_remap(std::size_t idx) const noexcept {
    // entries_per_line is 4 (64B line / 16B Entry). Use bit ops on the hot path.
    constexpr std::size_t entries_per_line = CACHE_LINE_SIZE / sizeof(Entry);  // 4
    static_assert(entries_per_line == 4, "Entry size must be 16B for cache_remap bit-ops");

    const std::size_t line = idx >> 2u;
    const std::size_t offset = idx & 3u;
    const std::size_t num_lines = scqsize_ >> 2u;
    return offset * num_lines + line;
}

template <class T>
bool SCQ<T>::enqueue(T index) {
    if (LSCQ_UNLIKELY(index == kEmpty)) {
        return false;
    }
    const std::uint64_t value = static_cast<std::uint64_t>(index);
    if (LSCQ_UNLIKELY(value >= bottom_)) {
        return false;
    }

    const std::uint64_t scqsize = static_cast<std::uint64_t>(scqsize_);
    const unsigned scq_shift = detail::log2_pow2_u64(scqsize);
    const std::int64_t threshold_reset =
        static_cast<std::int64_t>(scqsize + (scqsize >> 1u) - 1u);  // 3 * (SCQSIZE/2) - 1

    while (true) {
        const std::uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
        const std::uint64_t cycle_t = t >> scq_shift;
        const std::size_t j = cache_remap(static_cast<std::size_t>(t & bottom_));

        while (true) {
            const Entry ent = detail::entry_load(&entries_[j]);
            const std::uint64_t cycle_e = unpack_cycle(ent.cycle_flags);

            if (LSCQ_LIKELY(cycle_less(cycle_e, cycle_t) && ent.index_or_ptr == bottom_)) {
                const bool is_safe = unpack_is_safe(ent.cycle_flags);
                if (LSCQ_LIKELY(is_safe || head_.load(std::memory_order_acquire) <= t)) {
                    Entry expected = ent;
                    const Entry desired{pack_cycle_flags(cycle_t, true), value};
                    if (lscq::cas2(&entries_[j], expected, desired)) {
                        if (threshold_.load(std::memory_order_relaxed) != threshold_reset) {
                            threshold_.store(threshold_reset, std::memory_order_release);
                        }
                        return true;
                    }
                    continue;  // Retry same slot (Figure 8 line 19).
                }
            }

            break;  // Give up on this ticket and try a new Tail.
        }
    }
}

template <class T>
T SCQ<T>::dequeue() {
    const std::uint64_t scqsize = static_cast<std::uint64_t>(scqsize_);
    const unsigned scq_shift = detail::log2_pow2_u64(scqsize);
    const std::int64_t threshold_reset =
        static_cast<std::int64_t>(scqsize + (scqsize >> 1u) - 1u);  // 3 * (SCQSIZE/2) - 1

    // Figure 8 line 24: negative threshold is a fast empty check.
    if (LSCQ_UNLIKELY(threshold_.load(std::memory_order_acquire) < 0)) {
        // Threshold exhausted - check if queue is truly empty before returning kEmpty.
        // This handles the case where producers have completed but queue still has elements.
        const std::uint64_t head_now = head_.load(std::memory_order_acquire);
        const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

        // If tail > head, queue is not empty - reset threshold and continue.
        if (tail_now > head_now) {
            threshold_.store(threshold_reset, std::memory_order_release);
            // Fall through to normal dequeue logic
        } else {
            // Queue appears empty or threshold legitimately exhausted
            return kEmpty;
        }
    }

    while (true) {
        const std::uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
        const std::uint64_t cycle_h = h >> scq_shift;
        const std::size_t j = cache_remap(static_cast<std::size_t>(h & bottom_));

        // Retry loading/casing the same slot (Figure 8 line 38 goto 29).
        while (true) {
            const Entry ent = detail::entry_load(&entries_[j]);
            const std::uint64_t cycle_e = unpack_cycle(ent.cycle_flags);

            if (LSCQ_LIKELY(cycle_e == cycle_h)) {
                if (LSCQ_UNLIKELY(!unpack_is_safe(ent.cycle_flags))) {
                    continue;
                }
                const std::uint64_t value = ent.index_or_ptr;
                if (value == bottom_) {
                    break;
                }

                // Consume: atomic OR sets all index bits to 1 while preserving Cycle/IsSafe.
                detail::atomic_or_u64(&entries_[j].index_or_ptr, bottom_);
                return static_cast<T>(value);
            }

            // Default: clear IsSafe (Figure 8 line 33). If empty, advance Cycle to Cycle(H) and
            // preserve IsSafe (Figure 8 line 35).
            Entry desired{pack_cycle_flags(cycle_e, false), ent.index_or_ptr};
            if (ent.index_or_ptr == bottom_) {
                desired =
                    Entry{pack_cycle_flags(cycle_h, unpack_is_safe(ent.cycle_flags)), bottom_};
            }

            if (cycle_less(cycle_e, cycle_h)) {
                Entry expected = ent;
                if (!lscq::cas2(&entries_[j], expected, desired)) {
                    continue;
                }
            }

            break;
        }

        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (LSCQ_UNLIKELY(t <= h + 1)) {
            // Queue is empty: help tail catch up (catchup/fixState) and penalize threshold.
            const std::int64_t prev = threshold_.fetch_sub(1, std::memory_order_acq_rel);
            const std::int64_t next = prev - 1;
            if (next <= 0) {
                const std::uint64_t head_now = head_.load(std::memory_order_acquire);
                const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

                // Reset threshold if queue might not be empty, but don't retry here
                // to avoid head incrementing again (entry check will handle retry)
                if (tail_now > head_now) {
                    threshold_.store(threshold_reset, std::memory_order_release);
                }
                // Queue appears empty or severely lagging - call fixState if needed
                else if (head_now > tail_now && (head_now - tail_now) > scqsize) {
                    fixState();
                    threshold_.store(threshold_reset, std::memory_order_release);
                }
            }
            return kEmpty;
        }

        // Dequeue retry optimization: spin for a bounded number of iterations (threshold) before
        // returning empty.
        const std::int64_t prev = threshold_.fetch_sub(1, std::memory_order_acq_rel);
        const std::int64_t next = prev - 1;
        if (LSCQ_UNLIKELY(next <= 0)) {
            const std::uint64_t head_now = head_.load(std::memory_order_acquire);
            const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

            // Reset threshold if queue might not be empty, but don't retry here
            // to avoid head incrementing again (entry check will handle retry)
            if (tail_now > head_now) {
                threshold_.store(threshold_reset, std::memory_order_release);
            }
            // Queue appears empty or severely lagging - call fixState if needed
            else if (head_now > tail_now && (head_now - tail_now) > scqsize) {
                fixState();
                threshold_.store(threshold_reset, std::memory_order_release);
            }
            return kEmpty;
        }
    }
}

template <class T>
void SCQ<T>::fixState() {
    const std::uint64_t scqsize = static_cast<std::uint64_t>(scqsize_);

    while (true) {
        std::uint64_t h = head_.load(std::memory_order_acquire);
        std::uint64_t t = tail_.load(std::memory_order_acquire);

        if (h <= t || (h - t) <= scqsize) {
            return;
        }

        if (tail_.compare_exchange_weak(t, h, std::memory_order_release,
                                        std::memory_order_relaxed)) {
            return;
        }
    }
}

template <class T>
bool SCQ<T>::is_empty() const noexcept {
    return head_.load(std::memory_order_relaxed) >= tail_.load(std::memory_order_relaxed);
}

template class lscq::SCQ<std::uint64_t>;
template class lscq::SCQ<std::uint32_t>;

}  // namespace lscq
