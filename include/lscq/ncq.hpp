/**
 * @file ncq.hpp
 * @brief NCQ: Naive/Non-blocking Circular Queue (bounded MPMC ring).
 * @author lscq contributors
 * @version 0.1.0
 *
 * NCQ is a bounded MPMC queue that stores unsigned integral values in a circular buffer and uses
 * a 16-byte (CAS2) entry representation to coordinate concurrent producers/consumers.
 */

#ifndef LSCQ_NCQ_HPP_
#define LSCQ_NCQ_HPP_

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
 * @class NCQ
 * @brief Non-blocking circular queue (NCQ) with bounded capacity.
 *
 * This is a practical implementation of the Naive Circular Queue control flow described in the paper (Figure 5).
 *
 * @tparam T Value type stored in the queue (must be an unsigned integral type)
 * @note The queue uses a reserved sentinel value (@ref kEmpty) to indicate emptiness. Callers must not enqueue
 *       this value.
 * Thread-safety: @ref enqueue, @ref dequeue, and @ref is_empty are safe for concurrent callers.
 *
 * Complexity: O(1) expected per operation (may spin under contention).
 *
 * @note The queue is bounded and does not provide a bounded "queue full" failure mode: when the ring is full,
 *       @ref enqueue will spin until space becomes available.
 *
 * Example:
 * @code
 * lscq::NCQ<std::uint64_t> q(1024);
 * q.enqueue(7);
 * const auto v = q.dequeue();
 * if (v != lscq::NCQ<std::uint64_t>::kEmpty) {
 *     // got a value
 * }
 * @endcode
 */
template <class T>
class NCQ {
public:
    /** @brief Slot entry type used by the queue */
    using Entry = lscq::Entry;

    static_assert(std::is_integral_v<T>, "NCQ<T>: T must be an integral type");
    static_assert(std::is_unsigned_v<T>, "NCQ<T>: T must be an unsigned integral type");
    static_assert(sizeof(Entry) == 16);
    static_assert(alignof(Entry) == 16);

    /** @brief Sentinel value returned by @ref dequeue when the queue is empty */
    static constexpr T kEmpty = std::numeric_limits<T>::max();

    /**
     * @brief Construct a bounded circular queue with a given capacity
     *
     * @param capacity Number of slots in the queue
     * @note Implementations may clamp the capacity to a minimum and round it up to a cache-line-friendly multiple.
     * @note Thread-safe: construction must complete before the queue is shared with other threads.
     */
    explicit NCQ(std::size_t capacity = config::DEFAULT_SCQSIZE);

    /**
     * @brief Destroy the queue and release all internal storage
     *
     * @note Thread-safe: callers must ensure no other thread is accessing the queue during destruction.
     */
    ~NCQ();

    NCQ(const NCQ&) = delete;
    NCQ& operator=(const NCQ&) = delete;
    NCQ(NCQ&&) = delete;
    NCQ& operator=(NCQ&&) = delete;

    /**
     * @brief Enqueue an index into the circular queue
     *
     * @param index Index value to enqueue (must not equal @ref kEmpty)
     * @return true if the value was enqueued, false only if @p index equals @ref kEmpty
     * @note This follows the NCQ algorithm (Figure 5): when the queue is full, enqueue will spin
     *       (i.e., it does not provide a bounded "queue full" failure mode). The value @ref kEmpty
     *       is reserved and must not be enqueued.
     */
    bool enqueue(T index);

    /**
     * @brief Dequeue an index from the circular queue
     *
     * @return Dequeued index, or @ref kEmpty if the queue is empty
     * @note Intended to be thread-safe for concurrent callers. The returned value @ref kEmpty
     *       indicates the queue was empty at the time of the operation.
     */
    T dequeue();

    /**
     * @brief Check whether the queue is empty
     *
     * @return true if empty, false otherwise
     * @note Intended to be thread-safe for concurrent callers. This is a moment-in-time check and
     *       may become stale immediately in concurrent scenarios.
     */
    bool is_empty() const noexcept;

private:
    struct EntriesDeleter {
        void operator()(Entry* p) const noexcept { ::operator delete[](p, std::align_val_t(64)); }
    };

    std::unique_ptr<Entry[], EntriesDeleter> entries_;
    std::size_t capacity_;
    alignas(64) std::atomic<std::uint64_t> head_;
    alignas(64) std::atomic<std::uint64_t> tail_;

    std::size_t cache_remap(std::size_t idx) const noexcept;
};

extern template class NCQ<std::uint64_t>;
extern template class NCQ<std::uint32_t>;

}  // namespace lscq

#endif  // LSCQ_NCQ_HPP_
