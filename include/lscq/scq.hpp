/**
 * @file scq.hpp
 * @brief SCQ: Scalable Circular Queue (bounded MPMC ring).
 * @author lscq contributors
 * @version 0.1.0
 *
 * SCQ is a bounded MPMC queue with a scalable design intended to reduce contention compared to
 * simpler ring queues. It stores unsigned integral values and uses CAS2 to update a 16-byte slot.
 */

#ifndef LSCQ_SCQ_HPP_
#define LSCQ_SCQ_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

#include <lscq/cas2.hpp>
#include <lscq/config.hpp>

namespace lscq {

/**
 * @class SCQ
 * @brief Scalable Circular Queue (SCQ) with bounded capacity.
 *
 * SCQ uses a ring of size 2n (SCQSIZE) but provides an effective usable capacity of n (QSIZE).
 * It extends the NCQ-style entry representation with a packed bitfield: cycle (63 bits) + isSafe (1 bit).
 *
 * @tparam T Value type stored in the queue (must be an unsigned integral type)
 * @note The queue uses a reserved sentinel value (@ref kEmpty) to indicate emptiness. Callers must not enqueue
 *       this value.
 *
 * Thread-safety: @ref enqueue, @ref dequeue, and @ref is_empty are safe for concurrent callers.
 *
 * Complexity: O(1) expected per operation (may spin under contention).
 *
 * @note The usable capacity is @ref qsize (half of @ref scqsize).
 *
 * Example:
 * @code
 * lscq::SCQ<std::uint64_t> q(1024);
 * q.enqueue(1);
 * const auto v = q.dequeue();
 * if (v != lscq::SCQ<std::uint64_t>::kEmpty) {
 *     // got a value
 * }
 * @endcode
 */
template <class T>
class SCQ {
public:
    /** @brief Slot entry type used by the queue (128-bit CAS2 payload) */
    using Entry = lscq::Entry;

    static_assert(std::is_integral_v<T>, "SCQ<T>: T must be an integral type");
    static_assert(std::is_unsigned_v<T>, "SCQ<T>: T must be an unsigned integral type");
    static_assert(sizeof(Entry) == 16);
    static_assert(alignof(Entry) == 16);

    /** @brief Sentinel value returned by @ref dequeue when the queue is empty */
    static constexpr T kEmpty = std::numeric_limits<T>::max();

    /**
     * @brief Construct an SCQ with a given ring size
     *
     * @param scqsize Ring buffer size (2n). Implementations may clamp/round this to meet algorithm constraints.
     * @note Thread-safe: construction must complete before the queue is shared with other threads.
     */
    explicit SCQ(std::size_t scqsize = config::DEFAULT_SCQSIZE);

    /**
     * @brief Destroy the queue and release all internal storage
     *
     * @note Thread-safe: callers must ensure no other thread is accessing the queue during destruction.
     */
    ~SCQ();

    SCQ(const SCQ&) = delete;
    SCQ& operator=(const SCQ&) = delete;
    SCQ(SCQ&&) = delete;
    SCQ& operator=(SCQ&&) = delete;

    /**
     * @brief Enqueue an index value.
     *
     * @param index Value to enqueue (must not equal @ref kEmpty and must be less than the internal bottom marker).
     * @return true if enqueued; false if @p index is invalid.
     *
     * @note This is a bounded queue and does not provide a bounded "queue full" failure mode: under full
     *       conditions the algorithm will spin until space becomes available.
     */
    bool enqueue(T index);

    /**
     * @brief Dequeue an index value.
     * @return Dequeued value, or @ref kEmpty if the queue is empty.
     */
    T dequeue();

    /**
     * @brief Check whether the queue is empty.
     * @return true if empty, false otherwise.
     *
     * @note This is a moment-in-time check and may become stale immediately under concurrency.
     */
    bool is_empty() const noexcept;

    /** @brief Return the ring size (SCQSIZE = 2n). */
    std::size_t scqsize() const noexcept { return scqsize_; }
    /** @brief Return the usable capacity (QSIZE = n). */
    std::size_t qsize() const noexcept { return qsize_; }

private:
    static constexpr std::uint64_t kIsSafeMask = 1ULL;

    static constexpr std::uint64_t pack_cycle_flags(std::uint64_t cycle, bool is_safe) noexcept {
        return (cycle << 1) | (is_safe ? 1ULL : 0ULL);
    }
    static constexpr std::uint64_t unpack_cycle(std::uint64_t cycle_flags) noexcept {
        return cycle_flags >> 1;
    }
    static constexpr bool unpack_is_safe(std::uint64_t cycle_flags) noexcept {
        return (cycle_flags & kIsSafeMask) != 0;
    }

    struct EntriesDeleter {
        void operator()(Entry* p) const noexcept { ::operator delete[](p, std::align_val_t(64)); }
    };

    std::unique_ptr<Entry[], EntriesDeleter> entries_;
    std::size_t scqsize_;   // Ring size (2n).
    std::size_t qsize_;     // Usable capacity (n).
    std::uint64_t bottom_;  // ⊥ marker: SCQSIZE - 1 (all 1s within index mask).

    alignas(64) std::atomic<std::uint64_t> head_;
    alignas(64) std::atomic<std::uint64_t> tail_;
    alignas(64) std::atomic<std::int64_t> threshold_;  // Dynamic threshold (init: 3 * QSIZE - 1).

    std::size_t cache_remap(std::size_t idx) const noexcept;

    void fixState();
};

extern template class SCQ<std::uint64_t>;
extern template class SCQ<std::uint32_t>;

}  // namespace lscq

#endif  // LSCQ_SCQ_HPP_
