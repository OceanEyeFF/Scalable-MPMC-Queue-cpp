#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <lscq/detail/atomic_ptr.hpp>
#include <lscq/detail/likely.hpp>
#include <lscq/detail/ncq_impl.hpp>
#include <lscq/scqp.hpp>
#include <mutex>
#include <new>

namespace lscq {

namespace {

constexpr std::size_t CACHE_LINE_SIZE = 64;

inline bool cycle_less(std::uint64_t a, std::uint64_t b) noexcept {
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

template <class T>
inline bool entryp_equal(const typename SCQP<T>::EntryP& a,
                         const typename SCQP<T>::EntryP& b) noexcept {
    return a.cycle_flags == b.cycle_flags && a.ptr == b.ptr;
}

template <class T>
inline bool cas2p_mutex(typename SCQP<T>::EntryP* ptr, typename SCQP<T>::EntryP& expected,
                        const typename SCQP<T>::EntryP& desired) noexcept {
    std::lock_guard<std::mutex> lock(lscq::detail::cas2_fallback_mutex_for(ptr));
    const typename SCQP<T>::EntryP current = *ptr;
    if (!entryp_equal<T>(current, expected)) {
        expected = current;
        return false;
    }
    *ptr = desired;
    return true;
}

#if LSCQ_ARCH_X86_64 && LSCQ_PLATFORM_WINDOWS && LSCQ_COMPILER_MSVC
template <class T>
inline bool cas2p_native(typename SCQP<T>::EntryP* ptr, typename SCQP<T>::EntryP& expected,
                         const typename SCQP<T>::EntryP& desired) noexcept {
    if (!lscq::detail::is_aligned_16(ptr)) {
        return cas2p_mutex<T>(ptr, expected, desired);
    }

    const std::uint64_t desired_low = desired.cycle_flags;
    std::uint64_t desired_high = 0;
    std::memcpy(&desired_high, &desired.ptr, sizeof(desired_high));

    alignas(16) long long comparand[2];
    comparand[0] = lscq::detail::u64_to_ll(expected.cycle_flags);
    std::uint64_t expected_high = 0;
    std::memcpy(&expected_high, &expected.ptr, sizeof(expected_high));
    comparand[1] = lscq::detail::u64_to_ll(expected_high);

    const char ok = _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(ptr),
                                                   lscq::detail::u64_to_ll(desired_high),
                                                   lscq::detail::u64_to_ll(desired_low), comparand);

    expected.cycle_flags = lscq::detail::ll_to_u64(comparand[0]);
    const std::uint64_t out_high = lscq::detail::ll_to_u64(comparand[1]);
    std::memcpy(&expected.ptr, &out_high, sizeof(expected.ptr));
    return ok != 0;
}
#elif LSCQ_ARCH_X86_64 && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16) && \
    (defined(__GNUC__) || defined(__clang__))
template <class T>
inline bool cas2p_native(typename SCQP<T>::EntryP* ptr, typename SCQP<T>::EntryP& expected,
                         const typename SCQP<T>::EntryP& desired) noexcept {
    if (!lscq::detail::is_aligned_16(ptr)) {
        return cas2p_mutex<T>(ptr, expected, desired);
    }

    typename SCQP<T>::EntryP expected_local = expected;
    typename SCQP<T>::EntryP desired_local = desired;
    const bool ok = __atomic_compare_exchange(ptr, &expected_local, &desired_local, false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    expected = expected_local;
    return ok;
}
#else
template <class T>
inline bool cas2p_native(typename SCQP<T>::EntryP* ptr, typename SCQP<T>::EntryP& expected,
                         const typename SCQP<T>::EntryP& desired) noexcept {
    return cas2p_mutex<T>(ptr, expected, desired);
}
#endif

template <class T>
inline bool cas2p(typename SCQP<T>::EntryP* ptr, typename SCQP<T>::EntryP& expected,
                  const typename SCQP<T>::EntryP& desired) noexcept {
    if (ptr == nullptr) {
        return false;
    }

#if LSCQ_ENABLE_CAS2
    if (lscq::has_cas2_support()) {
        return cas2p_native<T>(ptr, expected, desired);
    }
#endif
    return cas2p_mutex<T>(ptr, expected, desired);
}

template <class T>
inline typename SCQP<T>::EntryP entryp_load(typename SCQP<T>::EntryP* ptr) noexcept {
    typename SCQP<T>::EntryP expected{0, nullptr};
    (void)cas2p<T>(ptr, expected, expected);
    return expected;
}

inline bool queue_is_full(std::uint64_t head, std::uint64_t tail, std::uint64_t scqsize) noexcept {
    return tail >= head && (tail - head) >= scqsize;
}

}  // namespace

template <class T>
SCQP<T>::SCQP(std::size_t scqsize, bool force_fallback)
    : entries_p_(nullptr),
      entries_i_(nullptr),
      ptr_array_(nullptr),
      scqsize_(scqsize),
      qsize_(0),
      bottom_(0),
      using_fallback_(false),
      head_(0),
      tail_(0),
      threshold_(0),
      deq_success_(0),
      enq_success_(0) {
    static_assert(sizeof(EntryP) == 16);
    static_assert(alignof(EntryP) == 16);
    static_assert(sizeof(Entry) == 16);
    static_assert(alignof(Entry) == 16);

    if (scqsize_ < 4) {
        scqsize_ = 4;
    }
    scqsize_ = round_up_pow2(scqsize_);
    qsize_ = scqsize_ / 2;
    if (qsize_ == 0) {
        qsize_ = 1;
        scqsize_ = 2;
    }
    bottom_ = static_cast<std::uint64_t>(scqsize_ - 1);

    using_fallback_ = force_fallback || !lscq::has_cas2_support();

    if (using_fallback_) {
        Entry* raw = static_cast<Entry*>(
            ::operator new[](scqsize_ * sizeof(Entry), std::align_val_t(CACHE_LINE_SIZE)));
        entries_i_.reset(raw);
        for (std::size_t i = 0; i < scqsize_; ++i) {
            new (&entries_i_[i]) Entry{pack_cycle_flags(0, true), kEmptyIndex};
        }

        ptr_array_ = std::make_unique<T*[]>(scqsize_);
        for (std::size_t i = 0; i < scqsize_; ++i) {
            ptr_array_[i] = nullptr;
        }
    } else {
        EntryP* raw = static_cast<EntryP*>(
            ::operator new[](scqsize_ * sizeof(EntryP), std::align_val_t(CACHE_LINE_SIZE)));
        entries_p_.reset(raw);
        for (std::size_t i = 0; i < scqsize_; ++i) {
            new (&entries_p_[i]) EntryP{pack_cycle_flags(0, true), nullptr};
        }
    }

    head_.store(static_cast<std::uint64_t>(scqsize_), std::memory_order_relaxed);
    tail_.store(static_cast<std::uint64_t>(scqsize_), std::memory_order_relaxed);
    // 4 * QSIZE - 1, with QSIZE = SCQSIZE / 2 (SCQSIZE is power-of-two).
    threshold_.store(static_cast<std::int64_t>((static_cast<std::uint64_t>(scqsize_) << 1u) - 1u),
                     std::memory_order_relaxed);
    deq_success_.store(0, std::memory_order_relaxed);
    enq_success_.store(0, std::memory_order_relaxed);
}

template <class T>
SCQP<T>::~SCQP() = default;

template <class T>
std::size_t SCQP<T>::cache_remap(std::size_t idx) const noexcept {
    // entries_per_line is 4 (64B line / 16B Entry). Use bit ops on the hot path.
    constexpr std::size_t entries_per_line = CACHE_LINE_SIZE / sizeof(Entry);  // 4
    static_assert(entries_per_line == 4, "Entry size must be 16B for cache_remap bit-ops");

    const std::size_t line = idx >> 2u;
    const std::size_t offset = idx & 3u;
    const std::size_t num_lines = scqsize_ >> 2u;
    return offset * num_lines + line;
}

template <class T>
bool SCQP<T>::enqueue(T* ptr) {
    if (LSCQ_UNLIKELY(ptr == nullptr)) {
        return false;
    }
    return using_fallback_ ? enqueue_index(ptr) : enqueue_ptr(ptr);
}

template <class T>
T* SCQP<T>::dequeue() {
    return using_fallback_ ? dequeue_index() : dequeue_ptr();
}

template <class T>
bool SCQP<T>::enqueue_ptr(T* ptr) {
    const std::uint64_t scqsize = static_cast<std::uint64_t>(scqsize_);
    const std::int64_t threshold_reset = static_cast<std::int64_t>((scqsize << 1u) - 1u);

    while (true) {
        const std::uint64_t head = deq_success_.load(std::memory_order_acquire);
        const std::uint64_t tail = enq_success_.load(std::memory_order_acquire);
        if (queue_is_full(head, tail, scqsize)) {
            return false;
        }

        const std::uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
        const std::uint64_t cycle_t = t / scqsize;
        const std::size_t j = cache_remap(static_cast<std::size_t>(t & bottom_));

        while (true) {
            const EntryP ent = entryp_load<T>(&entries_p_[j]);
            const std::uint64_t cycle_e = unpack_cycle(ent.cycle_flags);

            if (LSCQ_LIKELY(cycle_less(cycle_e, cycle_t) && ent.ptr == nullptr)) {
                const bool is_safe = unpack_is_safe(ent.cycle_flags);
                if (LSCQ_LIKELY(is_safe || head_.load(std::memory_order_acquire) <= t)) {
                    EntryP expected = ent;
                    const EntryP desired{pack_cycle_flags(cycle_t, true), ptr};
                    if (cas2p<T>(&entries_p_[j], expected, desired)) {
                        if (threshold_.load(std::memory_order_relaxed) != threshold_reset) {
                            threshold_.store(threshold_reset, std::memory_order_release);
                        }
                        enq_success_.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }
                    continue;
                }
            }
            break;
        }
    }
}

template <class T>
T* SCQP<T>::dequeue_ptr() {
    const std::uint64_t scqsize = static_cast<std::uint64_t>(scqsize_);
    const std::int64_t threshold_reset = static_cast<std::int64_t>((scqsize << 1u) - 1u);

    if (LSCQ_UNLIKELY(threshold_.load(std::memory_order_acquire) < 0)) {
        // Threshold exhausted - check if queue is truly empty before returning nullptr.
        // This handles the case where producers have completed but queue still has elements.
        const std::uint64_t head_now = head_.load(std::memory_order_acquire);
        const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

        // If tail > head, queue is not empty - reset threshold and continue.
        if (tail_now > head_now) {
            threshold_.store(threshold_reset, std::memory_order_release);
            // Fall through to normal dequeue logic
        } else {
            // Queue appears empty or threshold legitimately exhausted
            return nullptr;
        }
    }

    while (true) {
        const std::uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
        const std::uint64_t cycle_h = h / scqsize;
        const std::size_t j = cache_remap(static_cast<std::size_t>(h & bottom_));

        // Safety limit for inner loop to prevent livelock
        constexpr int MAX_INNER_RETRIES = 1024;
        int inner_retries = 0;

        while (true) {
            const EntryP ent = entryp_load<T>(&entries_p_[j]);
            const std::uint64_t cycle_e = unpack_cycle(ent.cycle_flags);

            if (LSCQ_LIKELY(cycle_e == cycle_h)) {
                // Check ptr first - if nullptr, no value to dequeue regardless of is_safe
                T* value = ent.ptr;
                if (value == nullptr) {
                    break;
                }

                if (LSCQ_UNLIKELY(!unpack_is_safe(ent.cycle_flags))) {
                    // Safety valve: prevent infinite spin when is_safe is false
                    if (++inner_retries > MAX_INNER_RETRIES) {
                        break;
                    }
                    continue;
                }

                (void)detail::atomic_exchange_ptr(&entries_p_[j].ptr, static_cast<T*>(nullptr));
                deq_success_.fetch_add(1, std::memory_order_relaxed);
                return value;
            }

            EntryP desired{pack_cycle_flags(cycle_e, false), ent.ptr};
            if (ent.ptr == nullptr) {
                desired =
                    EntryP{pack_cycle_flags(cycle_h, unpack_is_safe(ent.cycle_flags)), nullptr};
            }

            if (cycle_less(cycle_e, cycle_h)) {
                EntryP expected = ent;
                if (!cas2p<T>(&entries_p_[j], expected, desired)) {
                    continue;
                }
            }
            break;
        }

        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (LSCQ_UNLIKELY(t <= h + 1)) {
            const std::int64_t prev = threshold_.fetch_sub(1, std::memory_order_acq_rel);
            const std::int64_t next = prev - 1;
            if (next <= 0) {
                const std::uint64_t head_now = head_.load(std::memory_order_acquire);
                const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

                // Queue appears empty or severely lagging - call fixState if needed
                if (head_now > tail_now && (head_now - tail_now) > scqsize) {
                    fixState();
                    threshold_.store(threshold_reset, std::memory_order_release);
                }
            }
            return nullptr;
        }

        const std::int64_t prev = threshold_.fetch_sub(1, std::memory_order_acq_rel);
        const std::int64_t next = prev - 1;
        if (LSCQ_UNLIKELY(next <= 0)) {
            const std::uint64_t head_now = head_.load(std::memory_order_acquire);
            const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

            // If queue is not empty (tail > head), reset threshold and retry
            if (tail_now > head_now) {
                threshold_.store(threshold_reset, std::memory_order_release);
                continue;  // Retry dequeue
            }

            // Queue appears empty or severely lagging - call fixState if needed
            if (head_now > tail_now && (head_now - tail_now) > scqsize) {
                fixState();
                threshold_.store(threshold_reset, std::memory_order_release);
            }
            return nullptr;
        }
    }
}

template <class T>
bool SCQP<T>::enqueue_index(T* ptr) {
    const std::uint64_t scqsize = static_cast<std::uint64_t>(scqsize_);
    const std::int64_t threshold_reset = static_cast<std::int64_t>((scqsize << 1u) - 1u);

    while (true) {
        const std::uint64_t head = deq_success_.load(std::memory_order_acquire);
        const std::uint64_t tail = enq_success_.load(std::memory_order_acquire);
        if (queue_is_full(head, tail, scqsize)) {
            return false;
        }

        const std::uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
        const std::uint64_t cycle_t = t / scqsize;
        const std::size_t j = cache_remap(static_cast<std::size_t>(t & bottom_));

        while (true) {
            const Entry ent = detail::entry_load(&entries_i_[j]);
            const std::uint64_t cycle_e = unpack_cycle(ent.cycle_flags);

            if (LSCQ_LIKELY(cycle_less(cycle_e, cycle_t) && ent.index_or_ptr == kEmptyIndex)) {
                const bool is_safe = unpack_is_safe(ent.cycle_flags);
                if (LSCQ_LIKELY(is_safe || head_.load(std::memory_order_acquire) <= t)) {
                    T* expected_ptr = nullptr;
                    if (!detail::atomic_compare_exchange_ptr(&ptr_array_[j], expected_ptr, ptr)) {
                        break;
                    }

                    Entry expected = ent;
                    const Entry desired{pack_cycle_flags(cycle_t, true),
                                        static_cast<std::uint64_t>(j)};
                    if (lscq::cas2(&entries_i_[j], expected, desired)) {
                        if (threshold_.load(std::memory_order_relaxed) != threshold_reset) {
                            threshold_.store(threshold_reset, std::memory_order_release);
                        }
                        enq_success_.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }

                    T* rollback_expected = ptr;
                    (void)detail::atomic_compare_exchange_ptr(&ptr_array_[j], rollback_expected,
                                                              static_cast<T*>(nullptr));
                    continue;
                }
            }

            break;
        }
    }
}

template <class T>
T* SCQP<T>::dequeue_index() {
    const std::uint64_t scqsize = static_cast<std::uint64_t>(scqsize_);
    const std::int64_t threshold_reset = static_cast<std::int64_t>((scqsize << 1u) - 1u);

    if (LSCQ_UNLIKELY(threshold_.load(std::memory_order_acquire) < 0)) {
        // Threshold exhausted - check if queue is truly empty before returning nullptr.
        // This handles the case where producers have completed but queue still has elements.
        const std::uint64_t head_now = head_.load(std::memory_order_acquire);
        const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

        // If tail > head, queue is not empty - reset threshold and continue.
        if (tail_now > head_now) {
            threshold_.store(threshold_reset, std::memory_order_release);
            // Fall through to normal dequeue logic
        } else {
            // Queue appears empty or threshold legitimately exhausted
            return nullptr;
        }
    }

    while (true) {
        const std::uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
        const std::uint64_t cycle_h = h / scqsize;
        const std::size_t j = cache_remap(static_cast<std::size_t>(h & bottom_));

        // Safety limit for inner loop to prevent livelock
        constexpr int MAX_INNER_RETRIES = 1024;
        int inner_retries = 0;

        while (true) {
            const Entry ent = detail::entry_load(&entries_i_[j]);
            const std::uint64_t cycle_e = unpack_cycle(ent.cycle_flags);

            if (LSCQ_LIKELY(cycle_e == cycle_h)) {
                // Check index first - if empty, no value to dequeue regardless of is_safe
                const std::uint64_t idx = ent.index_or_ptr;
                if (idx == kEmptyIndex) {
                    break;
                }

                if (LSCQ_UNLIKELY(!unpack_is_safe(ent.cycle_flags))) {
                    // Safety valve: prevent infinite spin when is_safe is false
                    if (++inner_retries > MAX_INNER_RETRIES) {
                        break;
                    }
                    continue;
                }

                T* value = detail::atomic_exchange_ptr(&ptr_array_[idx], static_cast<T*>(nullptr));
                (void)detail::atomic_exchange_u64(&entries_i_[j].index_or_ptr, kEmptyIndex);
                deq_success_.fetch_add(1, std::memory_order_relaxed);
                return value;
            }

            Entry desired{pack_cycle_flags(cycle_e, false), ent.index_or_ptr};
            if (ent.index_or_ptr == kEmptyIndex) {
                desired =
                    Entry{pack_cycle_flags(cycle_h, unpack_is_safe(ent.cycle_flags)), kEmptyIndex};
            }

            if (cycle_less(cycle_e, cycle_h)) {
                Entry expected = ent;
                if (!lscq::cas2(&entries_i_[j], expected, desired)) {
                    continue;
                }
            }
            break;
        }

        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (LSCQ_UNLIKELY(t <= h + 1)) {
            const std::int64_t prev = threshold_.fetch_sub(1, std::memory_order_acq_rel);
            const std::int64_t next = prev - 1;
            if (next <= 0) {
                const std::uint64_t head_now = head_.load(std::memory_order_acquire);
                const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

                // Queue appears empty or severely lagging - call fixState if needed
                if (head_now > tail_now && (head_now - tail_now) > scqsize) {
                    fixState();
                    threshold_.store(threshold_reset, std::memory_order_release);
                }
            }
            return nullptr;
        }

        const std::int64_t prev = threshold_.fetch_sub(1, std::memory_order_acq_rel);
        const std::int64_t next = prev - 1;
        if (LSCQ_UNLIKELY(next <= 0)) {
            const std::uint64_t head_now = head_.load(std::memory_order_acquire);
            const std::uint64_t tail_now = tail_.load(std::memory_order_acquire);

            // If queue is not empty (tail > head), reset threshold and retry
            if (tail_now > head_now) {
                threshold_.store(threshold_reset, std::memory_order_release);
                continue;  // Retry dequeue
            }

            // Queue appears empty or severely lagging - call fixState if needed
            if (head_now > tail_now && (head_now - tail_now) > scqsize) {
                fixState();
                threshold_.store(threshold_reset, std::memory_order_release);
            }
            return nullptr;
        }
    }
}

template <class T>
void SCQP<T>::fixState() {
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
bool SCQP<T>::is_empty() const noexcept {
    const std::uint64_t head = deq_success_.load(std::memory_order_relaxed);
    const std::uint64_t tail = enq_success_.load(std::memory_order_relaxed);
    return tail <= head;
}

template <class T>
bool SCQP<T>::reset_for_reuse() noexcept {
    // Contract: only call when empty and with exclusive access (no concurrent enqueue/dequeue).
    if (!is_empty()) {
        assert(false && "SCQP::reset_for_reuse requires an empty queue with no concurrent users");
        return false;
    }

    if (using_fallback_) {
        for (std::size_t i = 0; i < scqsize_; ++i) {
            if (entries_i_[i].index_or_ptr != kEmptyIndex || ptr_array_[i] != nullptr) {
                assert(false &&
                       "SCQP::reset_for_reuse requires all slots empty (no residual payloads)");
                return false;
            }
        }
    } else {
        for (std::size_t i = 0; i < scqsize_; ++i) {
            if (entries_p_[i].ptr != nullptr) {
                assert(false &&
                       "SCQP::reset_for_reuse requires all slots empty (no residual payloads)");
                return false;
            }
        }
    }

    const std::uint64_t scqsize_u64 = static_cast<std::uint64_t>(scqsize_);
    const std::int64_t threshold_reset = static_cast<std::int64_t>((scqsize_u64 << 1u) - 1u);

    if (using_fallback_) {
        for (std::size_t i = 0; i < scqsize_; ++i) {
            entries_i_[i] = Entry{pack_cycle_flags(0, true), kEmptyIndex};
            ptr_array_[i] = nullptr;
        }
    } else {
        for (std::size_t i = 0; i < scqsize_; ++i) {
            entries_p_[i] = EntryP{pack_cycle_flags(0, true), nullptr};
        }
    }

    head_.store(scqsize_u64, std::memory_order_relaxed);
    tail_.store(scqsize_u64, std::memory_order_relaxed);
    threshold_.store(threshold_reset, std::memory_order_relaxed);
    deq_success_.store(0, std::memory_order_relaxed);
    enq_success_.store(0, std::memory_order_relaxed);
    return true;
}

template class lscq::SCQP<std::uint64_t>;
template class lscq::SCQP<std::uint32_t>;

}  // namespace lscq
