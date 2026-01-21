#include <lscq/ebr.hpp>
#include <lscq/lscq.hpp>

#include <iostream>
#include <thread>
#include <vector>

int main() {
    std::cout << "Testing LSCQ with 2 threads..." << std::endl;

    lscq::EBRManager ebr;
    lscq::LSCQ<uint64_t> q(ebr, 4096);

    constexpr int NUM_THREADS = 2;
    constexpr int OPS_PER_THREAD = 1000;

    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&q, i]() {
            std::cout << "Thread " << i << " started" << std::endl;

            for (int j = 0; j < OPS_PER_THREAD; ++j) {
                static thread_local uint64_t value = 0;
                value = static_cast<uint64_t>(i * 1000000 + j);

                // Enqueue
                while (!q.enqueue(&value)) {
                    std::this_thread::yield();
                }

                // Dequeue
                uint64_t* out = nullptr;
                while ((out = q.dequeue()) == nullptr) {
                    std::this_thread::yield();
                }
            }

            std::cout << "Thread " << i << " finished" << std::endl;
        });
    }

    std::cout << "Waiting for threads to finish..." << std::endl;
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Test completed successfully!" << std::endl;
    return 0;
}
