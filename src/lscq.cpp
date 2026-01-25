#include <lscq/lscq.hpp>
#include <thread>  // for std::this_thread::yield()

namespace lscq {

namespace {
class ActiveOpsGuard {
   public:
    explicit ActiveOpsGuard(std::atomic<int>& counter) noexcept : counter_(counter) {
        counter_.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveOpsGuard() noexcept { counter_.fetch_sub(1, std::memory_order_acq_rel); }

    ActiveOpsGuard(const ActiveOpsGuard&) = delete;
    ActiveOpsGuard& operator=(const ActiveOpsGuard&) = delete;
    ActiveOpsGuard(ActiveOpsGuard&&) = delete;
    ActiveOpsGuard& operator=(ActiveOpsGuard&&) = delete;

   private:
    std::atomic<int>& counter_;
};

template <class T>
inline void prepare_node_for_use(typename LSCQ<T>::Node* node, std::size_t scqsize) {
    if (node == nullptr) {
        return;
    }

    node->next.store(nullptr, std::memory_order_relaxed);
    node->finalized.store(false, std::memory_order_relaxed);

    if (!node->scqp.reset_for_reuse()) {
        node->scqp.~SCQP<T>();
        new (&node->scqp) SCQP<T>(scqsize);
    }
}
}  // namespace

// ============================================================================
// Node Implementation
// ============================================================================

template <class T>
LSCQ<T>::Node::Node(std::size_t scqsize) : scqp(scqsize), next(nullptr), finalized(false) {}

// ============================================================================
// LSCQ Implementation
// ============================================================================

template <class T>
LSCQ<T>::LSCQ(std::size_t scqsize)
    : head_(nullptr),
      tail_(nullptr),
      scqsize_(scqsize),
      pool_([scqsize] { return new Node(scqsize); }),
      legacy_ebr_(nullptr) {
    // Create the initial node
    Node* initial = pool_.Get();
    prepare_node_for_use<T>(initial, scqsize_);
    head_.store(initial, std::memory_order_relaxed);
    tail_.store(initial, std::memory_order_relaxed);
}

template <class T>
LSCQ<T>::LSCQ(EBRManager& ebr, std::size_t scqsize) : LSCQ(scqsize) {
    legacy_ebr_ = &ebr;
}

template <class T>
LSCQ<T>::~LSCQ() {
    closing_.store(true, std::memory_order_release);

    while (active_ops_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // Reclaim all nodes in the linked list, then clear the pool (which also contains
    // previously retired nodes).
    Node* current = head_.load(std::memory_order_relaxed);
    while (current != nullptr) {
        Node* next = current->next.load(std::memory_order_relaxed);
        pool_.Put(current);
        current = next;
    }

    head_.store(nullptr, std::memory_order_relaxed);
    tail_.store(nullptr, std::memory_order_relaxed);

    pool_.Clear();
}

template <class T>
bool LSCQ<T>::enqueue(T* ptr) {
    if (ptr == nullptr) {
        return false;
    }

    if (closing_.load(std::memory_order_acquire)) {
        return false;
    }

    ActiveOpsGuard active_guard(active_ops_);

    // Re-check after publishing to active_ops_ to avoid a destructor race window.
    if (closing_.load(std::memory_order_acquire)) {
        return false;
    }

    constexpr int MAX_RETRIES = 16;  // Increased for high-contention scenarios
    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
        Node* tail = tail_.load(std::memory_order_acquire);

        // 1. Try to enqueue to the tail node's SCQP
        if (tail->scqp.enqueue(ptr)) {
            return true;
        }

        // 2. SCQP is full, execute Finalize mechanism
        bool expected_finalized = false;
        if (tail->finalized.compare_exchange_strong(
                expected_finalized, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            // 2.1 Create a new node
            Node* new_node = pool_.Get();
            prepare_node_for_use<T>(new_node, scqsize_);

            // 2.2 Link to tail->next
            Node* expected_next = nullptr;
            if (!tail->next.compare_exchange_strong(expected_next, new_node,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                // Another thread already linked a node
                pool_.Put(new_node);
            }
        }

        // 3. Advance tail_ pointer
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next != nullptr) {
            tail_.compare_exchange_strong(tail, next, std::memory_order_acq_rel,
                                          std::memory_order_acquire);
        } else {
            // next not set yet, yield to allow the finalizing thread to complete
            std::this_thread::yield();
        }
    }

    // Maximum retries reached (should theoretically never happen)
    return false;
}

template <class T>
T* LSCQ<T>::dequeue() {
    if (closing_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    ActiveOpsGuard active_guard(active_ops_);

    // Re-check after publishing to active_ops_ to avoid a destructor race window.
    if (closing_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // Maximum retries to prevent infinite wait when finalized but next not yet linked
    constexpr int MAX_WAIT_RETRIES = 1024;
    int wait_retries = 0;

    while (true) {
        Node* head = head_.load(std::memory_order_acquire);

        // 1. Try to dequeue from the head node's SCQP
        T* result = head->scqp.dequeue();
        if (result != nullptr) {
            return result;
        }

        // 2. dequeue() returned nullptr - check next and finalized status
        Node* next = head->next.load(std::memory_order_acquire);
        bool is_finalized = head->finalized.load(std::memory_order_acquire);

        // 3. Only return nullptr if truly empty: not finalized AND no next node
        if (!is_finalized && next == nullptr) {
            // Single node, not finalized, queue is truly empty
            return nullptr;
        }

        // 4. Otherwise (finalized OR has next), retry dequeue to handle threshold false negatives
        constexpr int MAX_SCQP_RETRIES = 3;
        for (int retry = 0; retry < MAX_SCQP_RETRIES; ++retry) {
            std::this_thread::yield();  // Allow enqueue to reset threshold
            T* retry_result = head->scqp.dequeue();
            if (retry_result != nullptr) {
                return retry_result;
            }
        }

        // 5. If node is finalized, verify empty and advance head
        if (is_finalized) {
            // Multiple retries failed - verify SCQP is truly empty before advancing
            if (!head->scqp.is_empty()) {
                // SCQP still has elements, continue retrying
                std::this_thread::yield();
                continue;
            }

            // SCQP is empty and finalized, safe to advance head
            if (next == nullptr) {
                // finalized but next not set yet, yield to allow enqueue thread to complete
                if (++wait_retries > MAX_WAIT_RETRIES) {
                    // Safety valve: avoid infinite wait, return nullptr and let caller retry
                    return nullptr;
                }
                std::this_thread::yield();
                continue;
            }

            // Reset wait counter when we successfully find next
            wait_retries = 0;

            // Advance head_ pointer
            if (head_.compare_exchange_strong(head, next, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                // Successfully advanced head, return the old node to the pool
                pool_.Put(head);
            }
        } else {
            // 6. Not finalized but has next (unusual state) or retry failed
            // Continue retrying instead of returning nullptr
            std::this_thread::yield();
            continue;
        }
    }
}

// ============================================================================
// Explicit Template Instantiation
// ============================================================================

template class LSCQ<std::uint64_t>;
template class LSCQ<std::uint32_t>;

}  // namespace lscq
