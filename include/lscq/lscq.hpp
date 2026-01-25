/**
 * @file lscq.hpp
 * @brief Linked Scalable Circular Queue (LSCQ): an unbounded MPMC queue.
 * @author lscq contributors
 * @version 0.1.0
 *
 * LSCQ is an unbounded multi-producer multi-consumer (MPMC) queue that chains multiple bounded
 * SCQP nodes. When the tail node becomes full, a new node is allocated and linked. When the head
 * node is drained, it is returned to an internal ObjectPool for reuse.
 */

#ifndef LSCQ_LSCQ_HPP_
#define LSCQ_LSCQ_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <lscq/config.hpp>
#include <lscq/object_pool.hpp>
#include <lscq/scqp.hpp>

namespace lscq {

class EBRManager;  // Legacy (backward-compat) constructor overload only.

/**
 * @class LSCQ
 * @brief Linked Scalable Circular Queue (LSCQ): unbounded MPMC queue storing pointers.
 *
 * LSCQ is an unbounded multi-producer multi-consumer (MPMC) queue that chains
 * multiple SCQP nodes together. When a node becomes full, a new node is created
 * and linked to the tail. Empty nodes at the head are returned to an internal ObjectPool for
 * reuse.
 *
 * Key features:
 * - Unbounded capacity (grows dynamically)
 * - Lock-free operations (enqueue/dequeue)
 * - Node recycling via ObjectPool
 * - Cache-line aligned to minimize false sharing
 *
 * @tparam T Pointee type. The queue stores pointers to T (T*).
 *
 * Thread-safety: @ref enqueue and @ref dequeue are safe for concurrent calls by multiple producers
 * and consumers.
 *
 * Complexity:
 * - @ref enqueue / @ref dequeue - O(1) expected (may allocate when extending; may spin/yield under
 * contention)
 *
 * @note The queue stores raw pointers and does not manage the lifetime of the pointee objects.
 *
 * Example:
 * @code
 * lscq::LSCQ<std::uint64_t> q(256);  // scqsize
 *
 * q.enqueue(new std::uint64_t(123));
 * if (auto* p = q.dequeue()) {
 *     // use *p ...
 *     delete p;
 * }
 * @endcode
 */
template <class T>
class LSCQ {
   public:
    /**
     * @brief Internal node containing an SCQP ring plus linkage metadata.
     *
     * Each node is cache-line aligned (64 bytes) to prevent false sharing.
     * The next pointer and finalized flag each occupy their own cache line
     * to avoid contention during concurrent enqueue/dequeue operations.
     *
     * @note This is an implementation detail exposed as a public nested type for template linkage.
     */
    struct alignas(64) Node {
        /** @brief Embedded bounded ring storing user pointers. */
        SCQP<T> scqp;

        /** @brief Next node in the linked list (published by enqueue when extending). */
        alignas(64) std::atomic<Node*> next;
        /** @brief Set to true once this node is considered full and a successor is linked. */
        alignas(64) std::atomic<bool> finalized;

        /**
         * @brief Construct a new Node with the given SCQP size
         *
         * @param scqsize Size of the embedded SCQP
         *
         * @note Construction must complete before the node is published to other threads.
         */
        explicit Node(std::size_t scqsize);
    };

    /**
     * @brief Construct an LSCQ with the given SCQP size
     *
     * @param scqsize Size of each SCQP node (default: config::DEFAULT_SCQSIZE)
     *
     * @throws std::bad_alloc If allocating the initial node fails.
     */
    explicit LSCQ(std::size_t scqsize = config::DEFAULT_SCQSIZE);

    /**
     * @brief Backward-compatible constructor overload.
     *
     * The EBR parameter is ignored. Node memory management is handled internally via ObjectPool.
     */
    explicit LSCQ(EBRManager& ebr, std::size_t scqsize = config::DEFAULT_SCQSIZE);

    /**
     * @brief Destroy the LSCQ and reclaim all remaining nodes
     *
     * @note Callers must ensure no other thread is concurrently accessing the queue during
     * destruction.
     */
    ~LSCQ();

    LSCQ(const LSCQ&) = delete;
    LSCQ& operator=(const LSCQ&) = delete;
    LSCQ(LSCQ&&) = delete;
    LSCQ& operator=(LSCQ&&) = delete;

    /**
     * @brief Enqueue a pointer to the queue
     *
     * This operation is lock-free and thread-safe. If the current tail node
     * is full, a new node will be created and linked to the tail.
     *
     * @param ptr Pointer to enqueue (must not be nullptr).
     * @return true if the pointer was successfully enqueued, false otherwise
     *
     * @note Returns false if @p ptr is null, or if an internal retry limit is hit under extreme
     * contention.
     */
    bool enqueue(T* ptr);

    /**
     * @brief Dequeue a pointer from the queue
     *
     * This operation is lock-free and thread-safe. If the current head node
     * is empty, it will attempt to advance to the next node (if available).
     * Empty nodes are returned to an internal ObjectPool for reuse.
     *
     * @return Pointer dequeued from the queue, or nullptr if the queue is empty (or if a bounded
     *         internal wait limit is hit while observing a finalized-but-not-yet-linked node).
     */
    T* dequeue();

   private:
    alignas(64) std::atomic<Node*> head_;  // Head of the linked list
    alignas(64) std::atomic<Node*> tail_;  // Tail of the linked list

    // Destructor-safety: prevent node reclamation while operations are active.
    alignas(64) std::atomic<int> active_ops_{0};
    alignas(64) std::atomic<bool> closing_{false};

    std::size_t scqsize_;     // Size of each SCQP node
    ObjectPool<Node> pool_;   // Node allocator/recycler (replaces EBR for LSCQ nodes)
    EBRManager* legacy_ebr_;  // Optional legacy pointer (unused; kept for backward compatibility)
};

}  // namespace lscq

#endif  // LSCQ_LSCQ_HPP_
