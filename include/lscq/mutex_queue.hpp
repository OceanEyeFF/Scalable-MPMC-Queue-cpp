/**
 * @file mutex_queue.hpp
 * @brief Mutex-based queue baseline implementation.
 * @author lscq contributors
 * @version 0.1.0
 *
 * This queue is provided as a reference/baseline for benchmarks and comparisons.
 */

#ifndef LSCQ_MUTEX_QUEUE_HPP_
#define LSCQ_MUTEX_QUEUE_HPP_

#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

namespace lscq {

/**
 * @class MutexQueue
 * @brief Simple queue protected by a single mutex (baseline).
 *
 * @tparam T Value type stored in the queue.
 *
 * Thread-safety: All public methods are thread-safe, but operations are blocking.
 *
 * Complexity: O(1) per operation (amortized), plus mutex contention.
 *
 * Example:
 * @code
 * lscq::MutexQueue<int> q;
 * q.enqueue(1);
 * int v = 0;
 * if (q.dequeue(v)) {
 *     // v == 1
 * }
 * @endcode
 */
template <class T>
class MutexQueue {
   public:
    /** @brief Value type stored in the queue. */
    using value_type = T;

    MutexQueue() = default;
    /**
     * @brief Construct the queue (capacity is accepted for API compatibility).
     * @param capacity Ignored.
     */
    explicit MutexQueue(std::size_t capacity) { (void)capacity; }

    MutexQueue(const MutexQueue&) = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;
    MutexQueue(MutexQueue&&) = delete;
    MutexQueue& operator=(MutexQueue&&) = delete;

    /**
     * @brief Enqueue a value by copying.
     * @param value Value to enqueue.
     * @return true on success.
     */
    bool enqueue(const T& value) {
        std::lock_guard<std::mutex> lock(mu_);
        q_.push(value);
        return true;
    }

    /**
     * @brief Enqueue a value by moving.
     * @param value Value to enqueue.
     * @return true on success.
     */
    bool enqueue(T&& value) {
        std::lock_guard<std::mutex> lock(mu_);
        q_.push(std::move(value));
        return true;
    }

    /**
     * @brief Dequeue a value.
     * @param[out] out Receives the dequeued value.
     * @return true if an element was dequeued; false if the queue was empty.
     */
    bool dequeue(T& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (q_.empty()) {
            return false;
        }
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

   private:
    mutable std::mutex mu_;
    std::queue<T> q_;
};

}  // namespace lscq

#endif  // LSCQ_MUTEX_QUEUE_HPP_
