#include <lscq/msqueue.hpp>

#include <cstdint>

namespace lscq {

template <class T>
MSQueue<T>::MSQueue() : head_(nullptr), tail_(nullptr), retired_(nullptr) {
    Node* dummy = new Node();
    head_.store(dummy, std::memory_order_relaxed);
    tail_.store(dummy, std::memory_order_relaxed);
}

template <class T>
MSQueue<T>::~MSQueue() {
    delete_retired();

    Node* head = head_.load(std::memory_order_relaxed);
    delete_chain(head);
}

template <class T>
void MSQueue<T>::retire_node(Node* node) noexcept {
    if (node == nullptr) {
        return;
    }

    Node* old = retired_.load(std::memory_order_relaxed);
    do {
        node->retired_next = old;
    } while (!retired_.compare_exchange_weak(old, node, std::memory_order_release,
                                            std::memory_order_relaxed));
}

template <class T>
void MSQueue<T>::delete_chain(Node* first) noexcept {
    Node* cur = first;
    while (cur != nullptr) {
        Node* next = cur->next.load(std::memory_order_relaxed);
        delete cur;
        cur = next;
    }
}

template <class T>
void MSQueue<T>::delete_retired() noexcept {
    Node* cur = retired_.exchange(nullptr, std::memory_order_acq_rel);
    while (cur != nullptr) {
        Node* next = cur->retired_next;
        delete cur;
        cur = next;
    }
}

template <class T>
bool MSQueue<T>::enqueue(const T& value) {
    Node* node = new Node(value);

    while (true) {
        Node* tail = tail_.load(std::memory_order_acquire);
        Node* next = tail->next.load(std::memory_order_acquire);

        if (tail != tail_.load(std::memory_order_acquire)) {
            continue;
        }

        if (next == nullptr) {
            if (tail->next.compare_exchange_weak(next, node, std::memory_order_release,
                                                 std::memory_order_relaxed)) {
                (void)tail_.compare_exchange_weak(tail, node, std::memory_order_release,
                                                  std::memory_order_relaxed);
                return true;
            }
        } else {
            (void)tail_.compare_exchange_weak(tail, next, std::memory_order_release,
                                              std::memory_order_relaxed);
        }
    }
}

template <class T>
bool MSQueue<T>::dequeue(T& value) {
    while (true) {
        Node* head = head_.load(std::memory_order_acquire);
        Node* tail = tail_.load(std::memory_order_acquire);
        Node* next = head->next.load(std::memory_order_acquire);

        if (head != head_.load(std::memory_order_acquire)) {
            continue;
        }

        if (next == nullptr) {
            return false;  // Empty.
        }

        if (head == tail) {
            (void)tail_.compare_exchange_weak(tail, next, std::memory_order_release,
                                              std::memory_order_relaxed);
            continue;
        }

        value = next->data;
        if (head_.compare_exchange_weak(head, next, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            retire_node(head);
            return true;
        }
    }
}

template class MSQueue<std::uint64_t>;
template class MSQueue<std::uint32_t>;

}  // namespace lscq

