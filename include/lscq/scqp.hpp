/**
 * @file scqp.hpp
 * @brief SCQP: pointer-oriented SCQ variant (bounded MPMC ring).
 * @author lscq contributors
 * @version 0.1.0
 *
 * SCQP provides a pointer API: enqueue accepts non-null pointers and dequeue returns nullptr when
 * the queue is empty.
 */

#ifndef LSCQ_SCQP_HPP_
#define LSCQ_SCQP_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <lscq/cas2.hpp>
#include <lscq/config.hpp>
#include <memory>
#include <new>
#include <type_traits>

namespace lscq {

/**
 * @class SCQP
 * @brief Scalable Circular Queue (SCQ) variant that stores pointers (T*).
 *
 * This queue exposes a pointer API: @ref enqueue accepts non-null pointers, and @ref dequeue
 * returns nullptr to indicate an empty queue.
 *
 * When 128-bit CAS2 (CMPXCHG16B) is available at runtime, SCQP stores pointers directly in the slot
 * entry
 * (@ref EntryP). Otherwise it automatically falls back to an index-based entry format plus a side
 * pointer array.
 *
 * @tparam T Pointee type. The queue stores pointers to T (T*).
 *
 * Thread-safety: @ref enqueue, @ref dequeue, and @ref is_empty are safe for concurrent callers.
 *
 * Complexity: O(1) expected per operation (may spin under contention). @ref enqueue returns false
 * when the queue is full.
 *
 * Example:
 * @code
 * lscq::SCQP<std::uint64_t> q(256);
 * q.enqueue(new std::uint64_t(7));
 * if (auto* p = q.dequeue()) {
 *     delete p;
 * }
 * @endcode
 */
template <class T>
class SCQP {
   public:
    /**
     * @brief 16-byte CAS2 payload used by the pointer fast path.
     *
     * @note When native 128-bit CAS is unavailable, SCQP falls back to an index-based entry format.
     */
    struct alignas(16) EntryP {
        /** @brief Packed cycle counter and flags (implementation-defined bit layout). */
        std::uint64_t cycle_flags;
        /** @brief Pointer payload (nullptr indicates an empty slot). */
        T* ptr;
    };

    static_assert(sizeof(EntryP) == 16, "EntryP must be 16 bytes (CAS2 payload)");
    static_assert(alignof(EntryP) == 16, "EntryP must be 16-byte aligned (CAS2 payload)");
    static_assert(sizeof(T*) == 8, "SCQP requires 64-bit pointers");

    /**
     * @brief Construct an SCQP with the given ring size.
     *
     * @param scqsize Ring buffer size (2n). Implementations may clamp/round this to meet algorithm
     * constraints.
     * @param force_fallback If true, forces the index+side-array fallback even if CAS2 is
     * available.
     *
     * @throws std::bad_alloc If internal storage allocation fails.
     */
    explicit SCQP(std::size_t scqsize = config::DEFAULT_SCQSIZE, bool force_fallback = false);
    ~SCQP();

    SCQP(const SCQP&) = delete;
    SCQP& operator=(const SCQP&) = delete;
    SCQP(SCQP&&) = delete;
    SCQP& operator=(SCQP&&) = delete;

    /**
     * @brief Enqueue a non-null pointer.
     * @param ptr Pointer to enqueue (must not be nullptr).
     * @return true if enqueued; false if @p ptr is null or the queue is full.
     */
    bool enqueue(T* ptr);

    /**
     * @brief Dequeue a pointer.
     * @return Dequeued pointer, or nullptr if the queue is empty.
     */
    T* dequeue();

    /**
     * @brief Check whether the queue is empty.
     * @return true if empty, false otherwise.
     *
     * @note This is a moment-in-time check and may become stale immediately under concurrency.
     */
    bool is_empty() const noexcept;

    /** @brief Return whether the queue is currently using the fallback implementation. */
    bool is_using_fallback() const noexcept { return using_fallback_; }
    /** @brief Return the ring size (SCQSIZE = 2n). */
    std::size_t scqsize() const noexcept { return scqsize_; }
    /** @brief Return the usable capacity (QSIZE = n). */
    std::size_t qsize() const noexcept { return qsize_; }

   private:
    static constexpr std::uint64_t kIsSafeMask = 1ULL;
    static constexpr std::uint64_t kEmptyIndex = std::numeric_limits<std::uint64_t>::max();

    static constexpr std::uint64_t pack_cycle_flags(std::uint64_t cycle, bool is_safe) noexcept {
        return (cycle << 1) | (is_safe ? 1ULL : 0ULL);
    }
    static constexpr std::uint64_t unpack_cycle(std::uint64_t cycle_flags) noexcept {
        return cycle_flags >> 1;
    }
    static constexpr bool unpack_is_safe(std::uint64_t cycle_flags) noexcept {
        return (cycle_flags & kIsSafeMask) != 0;
    }

    struct EntriesPDeleter {
        void operator()(EntryP* p) const noexcept { ::operator delete[](p, std::align_val_t(64)); }
    };
    struct EntriesDeleter {
        void operator()(Entry* p) const noexcept { ::operator delete[](p, std::align_val_t(64)); }
    };

    std::unique_ptr<EntryP[], EntriesPDeleter> entries_p_;
    std::unique_ptr<Entry[], EntriesDeleter> entries_i_;
    std::unique_ptr<T*[]> ptr_array_;

    std::size_t scqsize_;   // Ring size (2n).
    std::size_t qsize_;     // QSIZE (n).
    std::uint64_t bottom_;  // Index mask: SCQSIZE - 1.

    bool using_fallback_;

    alignas(64) std::atomic<std::uint64_t> head_;
    alignas(64) std::atomic<std::uint64_t> tail_;
    alignas(64) std::atomic<std::int64_t> threshold_;  // Dynamic threshold (init: 4 * QSIZE - 1).
    alignas(64) std::atomic<std::uint64_t> deq_success_;  // Number of successful dequeues.
    alignas(64) std::atomic<std::uint64_t> enq_success_;  // Number of successful enqueues.

    std::size_t cache_remap(std::size_t idx) const noexcept;

    bool enqueue_ptr(T* ptr);
    T* dequeue_ptr();
    bool enqueue_index(T* ptr);
    T* dequeue_index();

    void fixState();
};

extern template class SCQP<std::uint64_t>;
extern template class SCQP<std::uint32_t>;

}  // namespace lscq

#endif  // LSCQ_SCQP_HPP_
