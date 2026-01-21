#include <lscq/lscq.hpp>

#include <thread>  // for std::this_thread::yield()

namespace lscq {

// ============================================================================
// Node Implementation
// ============================================================================

template <class T>
LSCQ<T>::Node::Node(std::size_t scqsize)
    : scqp(scqsize), next(nullptr), finalized(false) {}

// ============================================================================
// LSCQ Implementation
// ============================================================================

template <class T>
LSCQ<T>::LSCQ(EBRManager& ebr, std::size_t scqsize)
    : head_(nullptr), tail_(nullptr), ebr_(ebr), scqsize_(scqsize) {
    // Create the initial node
    Node* initial = new Node(scqsize_);
    head_.store(initial, std::memory_order_relaxed);
    tail_.store(initial, std::memory_order_relaxed);
}

template <class T>
LSCQ<T>::~LSCQ() {
    // Reclaim all nodes in the linked list
    Node* current = head_.load(std::memory_order_relaxed);
    while (current != nullptr) {
        Node* next = current->next.load(std::memory_order_relaxed);
        delete current;
        current = next;
    }
}

template <class T>
bool LSCQ<T>::enqueue(T* ptr) {
    if (ptr == nullptr) {
        return false;
    }

    EpochGuard guard(ebr_);  // EBR critical section

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
                expected_finalized, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {

            // 2.1 Create a new node
            Node* new_node = new Node(scqsize_);

            // 2.2 Link to tail->next
            Node* expected_next = nullptr;
            if (!tail->next.compare_exchange_strong(
                    expected_next, new_node,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // Another thread already linked a node
                delete new_node;
            }
        }

        // 3. Advance tail_ pointer
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next != nullptr) {
            tail_.compare_exchange_strong(
                tail, next,
                std::memory_order_acq_rel,
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
    EpochGuard guard(ebr_);  // EBR critical section

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

        // 2. SCQP is empty, check if we should advance head
        Node* next = head->next.load(std::memory_order_acquire);

        if (next == nullptr) {
            // Check if the node is finalized
            bool is_finalized = head->finalized.load(std::memory_order_acquire);
            if (!is_finalized) {
                // Truly empty queue
                return nullptr;
            }
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

        // 3. Advance head_ pointer
        if (head_.compare_exchange_strong(
                head, next,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // Successfully advanced head, retire the old node
            ebr_.retire(head);
        }
    }
}

// ============================================================================
// Explicit Template Instantiation
// ============================================================================

template class LSCQ<std::uint64_t>;
template class LSCQ<std::uint32_t>;

}  // namespace lscq
