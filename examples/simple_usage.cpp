#include <lscq/lscq.hpp>
#include <lscq/msqueue.hpp>
#include <lscq/mutex_queue.hpp>
#include <lscq/scq.hpp>

#include <cstdint>
#include <iostream>
#include <memory>

int main() {
    std::cout << "LSCQ examples - simple usage (single-thread)\n\n";

    // -----------------------------------------------------------------------------
    // 1) LSCQ: unbounded MPMC queue storing pointers (T*)
    // -----------------------------------------------------------------------------
    //
    // LSCQ stores raw pointers. A common and safe ownership pattern is:
    //   producer: create std::unique_ptr<T>, enqueue(ptr.get()), then release()
    //   consumer: dequeue() returns T*, wrap it into std::unique_ptr<T> and use it
    //
    // Note: LSCQ now uses ObjectPool internally for node management (no EBRManager needed).
    lscq::LSCQ<std::uint64_t> q(/*scqsize=*/1024);

    // enqueue(nullptr) is an API error for LSCQ (it returns false).
    if (q.enqueue(nullptr)) {
        std::cerr << "ERROR: enqueue(nullptr) unexpectedly succeeded\n";
        return 1;
    }

    std::cout << "[LSCQ] enqueue 1..5 (pointers)\n";
    for (std::uint64_t v = 1; v <= 5; ++v) {
        auto owned = std::make_unique<std::uint64_t>(v);
        std::uint64_t* raw = owned.get();

        if (!q.enqueue(raw)) {
            std::cerr << "ERROR: [LSCQ] enqueue failed for value " << v << "\n";
            return 1;
        }

        // Transfer ownership to the queue; consumer will delete it.
        (void)owned.release();
    }

    std::cout << "[LSCQ] dequeue until empty:\n";
    while (true) {
        std::uint64_t* raw = q.dequeue();
        if (raw == nullptr) {
            break;  // empty
        }

        // Reclaim ownership immediately and ensure no leaks.
        std::unique_ptr<std::uint64_t> owned(raw);
        std::cout << "  got " << *owned << "\n";
    }

    // Note: LSCQ now manages node lifecycle internally via ObjectPool.
    std::cout << "[LSCQ] Queue operations completed successfully.\n\n";

    // -----------------------------------------------------------------------------
    // 2) Other queues provided by this repo (single-thread demo)
    // -----------------------------------------------------------------------------
    //
    // The headers below are part of the public API and can be used for comparison:
    // - SCQ: bounded, stores values, returns kEmpty when empty
    // - MSQueue: stores values, dequeue returns bool
    // - MutexQueue: stores values, dequeue returns bool
    //

    {
        lscq::SCQ<std::uint64_t> scq(/*scqsize=*/1024);
        std::cout << "[SCQ] enqueue 10,20,30\n";
        if (!scq.enqueue(10) || !scq.enqueue(20) || !scq.enqueue(30)) {
            std::cerr << "ERROR: [SCQ] enqueue failed\n";
            return 1;
        }

        std::cout << "[SCQ] dequeue until empty (kEmpty):\n";
        while (true) {
            const std::uint64_t v = scq.dequeue();
            if (v == lscq::SCQ<std::uint64_t>::kEmpty) {
                break;
            }
            std::cout << "  got " << v << "\n";
        }
        std::cout << "\n";
    }

    {
        lscq::MSQueue<std::uint64_t> msq;
        std::cout << "[MSQueue] enqueue 100,200,300\n";
        if (!msq.enqueue(100) || !msq.enqueue(200) || !msq.enqueue(300)) {
            std::cerr << "ERROR: [MSQueue] enqueue failed\n";
            return 1;
        }

        std::cout << "[MSQueue] dequeue until empty (bool):\n";
        std::uint64_t out = 0;
        while (msq.dequeue(out)) {
            std::cout << "  got " << out << "\n";
        }
        std::cout << "\n";
    }

    {
        lscq::MutexQueue<std::uint64_t> muq;
        std::cout << "[MutexQueue] enqueue 7,8,9\n";
        if (!muq.enqueue(7) || !muq.enqueue(8) || !muq.enqueue(9)) {
            std::cerr << "ERROR: [MutexQueue] enqueue failed\n";
            return 1;
        }

        std::cout << "[MutexQueue] dequeue until empty (bool):\n";
        std::uint64_t out = 0;
        while (muq.dequeue(out)) {
            std::cout << "  got " << out << "\n";
        }
        std::cout << "\n";
    }

    std::cout << "Done.\n";
    return 0;
}

