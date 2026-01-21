/**
 * @file msqueue.hpp
 * @brief Michael-Scott (M&S) lock-free queue baseline implementation.
 * @author lscq contributors
 * @version 0.1.0
 *
 * This queue is provided as a reference/baseline for benchmarks and comparisons.
 */

#ifndef LSCQ_MSQUEUE_HPP_
#define LSCQ_MSQUEUE_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace lscq {

/**
 * @class MSQueue
 * @brief Simplified Michael-Scott lock-free queue (M&S queue).
 *
 * @note This implementation intentionally avoids immediate node reclamation during dequeue to
 *       sidestep hazard-pointer/epoch requirements. Removed nodes are retired to an internal list
 *       and freed in the destructor. Callers must ensure no other threads are accessing the queue
 *       during destruction.
 *
 * @tparam T Value type stored in the queue.
 *
 * Thread-safety: @ref enqueue and @ref dequeue are safe for concurrent multi-producer and
 * multi-consumer use.
 *
 * Complexity: O(1) expected per operation (may spin under contention).
 *
 * Example:
 * @code
 * lscq::MSQueue<std::uint64_t> q;
 * q.enqueue(1);
 * std::uint64_t v = 0;
 * if (q.dequeue(v)) {
 *     // v == 1
 * }
 * @endcode
 */
template <class T>
class MSQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next;
        Node* retired_next;

        Node() : data(), next(nullptr), retired_next(nullptr) {}
        explicit Node(const T& v) : data(v), next(nullptr), retired_next(nullptr) {}
    };

public:
    /** @brief Construct an empty queue containing an internal dummy node. */
    MSQueue();
    /** @brief Destroy the queue and free all remaining nodes (must not race with other threads). */
    ~MSQueue();

    MSQueue(const MSQueue&) = delete;
    MSQueue& operator=(const MSQueue&) = delete;
    MSQueue(MSQueue&&) = delete;
    MSQueue& operator=(MSQueue&&) = delete;

    /**
     * @brief Enqueue a value.
     * @param value Value to enqueue.
     * @return true on success.
     *
     * @throws std::bad_alloc If allocating a new node fails.
     */
    bool enqueue(const T& value);

    /**
     * @brief Dequeue a value.
     * @param[out] value Receives the dequeued value.
     * @return true if an element was dequeued; false if the queue was empty.
     */
    bool dequeue(T& value);

    /**
     * @brief Return the internal node size in bytes.
     * @return Size of one node allocation.
     */
    static constexpr std::size_t node_size_bytes() noexcept { return sizeof(Node); }

private:
    void retire_node(Node* node) noexcept;
    void delete_chain(Node* first) noexcept;
    void delete_retired() noexcept;

    alignas(64) std::atomic<Node*> head_;
    alignas(64) std::atomic<Node*> tail_;
    alignas(64) std::atomic<Node*> retired_;
};

extern template class MSQueue<std::uint64_t>;
extern template class MSQueue<std::uint32_t>;

}  // namespace lscq

#endif  // LSCQ_MSQUEUE_HPP_
