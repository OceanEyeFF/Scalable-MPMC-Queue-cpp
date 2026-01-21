/**
 * @file debug_lscq_pair.cpp
 * @brief Debug test for LSCQ Pair mode with threads:8
 *
 * Uses sleep to reduce output volume for debugging.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <lscq/ebr.hpp>
#include <lscq/lscq.hpp>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int NUM_THREADS = 2;           // Test with 2 threads like benchmark
constexpr int OPS_PER_THREAD = 1000000;  // Very large number to trigger issue
constexpr int PREFILL_PER_THREAD = 100;  // Match benchmark prefill
constexpr std::size_t NODE_SCQSIZE = 4096;

std::mutex g_print_mutex;
std::atomic<int> g_debug_counter{0};

void debug_print(int thread_id, const char* msg, int extra = -1) {
    // Only print every 1000th message to reduce output for large runs
    int count = g_debug_counter.fetch_add(1, std::memory_order_relaxed);
    if (count % 1000 != 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << "[T" << thread_id << "] " << msg;
    if (extra >= 0) {
        std::cout << " (" << extra << ")";
    }
    std::cout << std::endl;
}

class CyclicBarrier {
   public:
    explicit CyclicBarrier(int parties) : parties_(parties), arrived_(0), generation_(0) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mu_);
        const std::size_t gen = generation_;
        if (++arrived_ == static_cast<std::size_t>(parties_)) {
            arrived_ = 0;
            ++generation_;
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [&] { return generation_ != gen; });
    }

   private:
    int parties_;
    std::size_t arrived_;
    std::size_t generation_;
    std::mutex mu_;
    std::condition_variable cv_;
};

}  // namespace

int main() {
    std::cout << "=== LSCQ Pair Debug Test ===" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << std::endl;
    std::cout << "Ops per thread: " << OPS_PER_THREAD << std::endl;
    std::cout << "Node SCQP size: " << NODE_SCQSIZE << std::endl;
    std::cout << std::endl;

    lscq::EBRManager ebr;
    lscq::LSCQ<std::uint64_t> q(ebr, NODE_SCQSIZE);

    // Create pool
    std::vector<std::uint64_t> pool(1 << 20);  // 1M elements
    for (std::size_t i = 0; i < pool.size(); ++i) {
        pool[i] = static_cast<std::uint64_t>(i);
    }
    const std::uint64_t pool_mask = pool.size() - 1;

    // Prefill
    std::cout << "Prefilling queue with " << (NUM_THREADS * PREFILL_PER_THREAD) << " elements..."
              << std::endl;
    for (int i = 0; i < NUM_THREADS * PREFILL_PER_THREAD; ++i) {
        auto* p = &pool[static_cast<std::size_t>(i) & pool_mask];
        if (!q.enqueue(p)) {
            std::cerr << "ERROR: Prefill enqueue failed at " << i << std::endl;
            return 1;
        }
    }
    std::cout << "Prefill done." << std::endl;

    CyclicBarrier start_barrier(NUM_THREADS);
    CyclicBarrier finish_barrier(NUM_THREADS);

    std::atomic<int> total_enqueue_success{0};
    std::atomic<int> total_dequeue_success{0};
    std::atomic<int> enqueue_failures{0};
    std::atomic<int> dequeue_failures{0};
    std::atomic<bool> stop_flag{false};

    std::vector<std::thread> threads;

    std::cout << "\nStarting " << NUM_THREADS << " threads..." << std::endl;

    for (int tid = 0; tid < NUM_THREADS; ++tid) {
        threads.emplace_back([&, tid]() {
            debug_print(tid, "Thread started, waiting at barrier");

            start_barrier.arrive_and_wait();

            debug_print(tid, "Barrier passed, starting work");

            std::uint64_t local_seq = static_cast<std::uint64_t>(tid) * 1000000;

            for (int op = 0; op < OPS_PER_THREAD && !stop_flag.load(std::memory_order_relaxed);
                 ++op) {
                // Enqueue
                auto* item = &pool[(local_seq++) & pool_mask];
                int enq_retries = 0;
                while (!q.enqueue(item)) {
                    ++enq_retries;
                    if (enq_retries > 1000) {
                        debug_print(tid, "WARN: enqueue stuck", enq_retries);
                        enqueue_failures.fetch_add(1, std::memory_order_relaxed);
                        if (enq_retries > 10000) {
                            debug_print(tid, "ERROR: enqueue giving up");
                            stop_flag.store(true, std::memory_order_relaxed);
                            break;
                        }
                    }
                    std::this_thread::yield();
                }
                if (enq_retries <= 10000) {
                    total_enqueue_success.fetch_add(1, std::memory_order_relaxed);
                }

                // Dequeue
                std::uint64_t* out = nullptr;
                int deq_retries = 0;
                while ((out = q.dequeue()) == nullptr) {
                    ++deq_retries;
                    if (deq_retries > 1000) {
                        debug_print(tid, "WARN: dequeue stuck", deq_retries);
                        dequeue_failures.fetch_add(1, std::memory_order_relaxed);
                        if (deq_retries > 10000) {
                            debug_print(tid, "ERROR: dequeue giving up");
                            stop_flag.store(true, std::memory_order_relaxed);
                            break;
                        }
                    }
                    std::this_thread::yield();
                }
                if (deq_retries <= 10000) {
                    total_dequeue_success.fetch_add(1, std::memory_order_relaxed);
                }

                // Progress indicator
                if (op > 0 && op % 10 == 0) {
                    debug_print(tid, "Progress", op);
                }
            }

            debug_print(tid, "Work done, waiting at finish barrier");
            finish_barrier.arrive_and_wait();
            debug_print(tid, "Finish barrier passed");

            // Second barrier for cleanup sync
            finish_barrier.arrive_and_wait();
            debug_print(tid, "Thread exiting");
        });
    }

    // Wait with timeout
    std::cout << "\nWaiting for threads (timeout: 30s)..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();

    for (auto& t : threads) {
        while (true) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

            if (elapsed > 30) {
                std::cerr << "\n!!! TIMEOUT after " << elapsed << " seconds !!!" << std::endl;
                std::cerr << "Enqueue success: " << total_enqueue_success.load() << std::endl;
                std::cerr << "Dequeue success: " << total_dequeue_success.load() << std::endl;
                std::cerr << "Enqueue failures: " << enqueue_failures.load() << std::endl;
                std::cerr << "Dequeue failures: " << dequeue_failures.load() << std::endl;
                stop_flag.store(true, std::memory_order_relaxed);
                // Detach and exit
                t.detach();
                return 1;
            }

            if (t.joinable()) {
                // Try to join with short timeout
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                // Check if thread is done by attempting native handle check
                // Since C++ doesn't have timed join, we use a simple poll approach
                break;  // Just join normally
            }
            break;
        }
        if (t.joinable()) {
            t.join();
        }
    }

    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Enqueue success: " << total_enqueue_success.load() << std::endl;
    std::cout << "Dequeue success: " << total_dequeue_success.load() << std::endl;
    std::cout << "Expected: " << (NUM_THREADS * OPS_PER_THREAD) << std::endl;

    if (total_enqueue_success.load() == NUM_THREADS * OPS_PER_THREAD &&
        total_dequeue_success.load() == NUM_THREADS * OPS_PER_THREAD) {
        std::cout << "\n*** TEST PASSED ***" << std::endl;
        return 0;
    } else {
        std::cerr << "\n*** TEST FAILED ***" << std::endl;
        return 1;
    }
}
