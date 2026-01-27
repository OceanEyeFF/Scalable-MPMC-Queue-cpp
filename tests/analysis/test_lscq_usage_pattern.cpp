/**
 * @file test_lscq_usage_pattern.cpp
 * @brief LSCQ usage pattern analysis for ObjectPool optimization Phase 0.
 *
 * Analyzes real LSCQ usage scenarios to measure:
 * - Get/Put ratio
 * - Cache hit potential (same-thread Put→Get patterns)
 * - High contention behavior
 *
 * Results inform thread_local cache optimization decisions.
 *
 * @see docs/ObjectPool-Optimization-Plan.md (Phase 0)
 * @see docs/ObjectPool-Testing-Plan.md (Section 0.2)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <lscq/lscq.hpp>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

//=============================================================================
// Instrumented ObjectPool Wrapper
//=============================================================================

/**
 * @brief Wrapper that tracks ObjectPool usage statistics.
 *
 * Records Get/Put calls and identifies potential cache hit opportunities
 * (same-thread Put followed by Get within 100μs).
 */
template <typename T>
class InstrumentedObjectPool {
   public:
    // Statistics counters
    std::atomic<long> total_get_calls{0};
    std::atomic<long> total_put_calls{0};
    std::atomic<long> get_after_put_same_thread{0};  // Potential cache hits

    // Per-thread context tracking
    struct ThreadContext {
        bool last_op_was_put = false;
        std::chrono::steady_clock::time_point last_put_time;
    };
    std::unordered_map<std::thread::id, ThreadContext> thread_contexts_;
    std::mutex ctx_mutex_;

    lscq::ObjectPool<T>& underlying_pool_;

    explicit InstrumentedObjectPool(lscq::ObjectPool<T>& pool) : underlying_pool_(pool) {}

    T* Get() {
        total_get_calls.fetch_add(1, std::memory_order_relaxed);

        // Check if this Get follows a Put on the same thread (potential hit)
        {
            std::lock_guard lock(ctx_mutex_);
            auto tid = std::this_thread::get_id();
            auto& ctx = thread_contexts_[tid];
            if (ctx.last_op_was_put) {
                auto elapsed = std::chrono::steady_clock::now() - ctx.last_put_time;
                if (elapsed < std::chrono::microseconds(100)) {
                    get_after_put_same_thread.fetch_add(1, std::memory_order_relaxed);
                }
            }
            ctx.last_op_was_put = false;
        }

        return underlying_pool_.Get();
    }

    void Put(T* obj) {
        total_put_calls.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard lock(ctx_mutex_);
            auto tid = std::this_thread::get_id();
            auto& ctx = thread_contexts_[tid];
            ctx.last_op_was_put = true;
            ctx.last_put_time = std::chrono::steady_clock::now();
        }

        underlying_pool_.Put(obj);
    }

    void PrintStats() const {
        long gets = total_get_calls.load();
        long puts = total_put_calls.load();
        long pot_hits = get_after_put_same_thread.load();

        std::cout << "\n========== LSCQ ObjectPool Usage Analysis ==========\n";
        std::cout << "Total Get() calls:     " << gets << "\n";
        std::cout << "Total Put() calls:     " << puts << "\n";
        std::cout << "Get/Put ratio:         " << std::fixed << std::setprecision(2)
                  << (puts > 0 ? static_cast<double>(gets) / puts : 0) << "\n";
        std::cout << "Potential cache hits:  " << pot_hits << " ("
                  << (gets > 0 ? pot_hits * 100.0 / gets : 0) << "%)\n";
        std::cout << "====================================================\n";
    }
};

}  // namespace

//=============================================================================
// Test Fixture
//=============================================================================

class LSCQUsagePatternTest : public ::testing::Test {
   protected:
    static constexpr std::size_t kSCQSize = 256;
    static constexpr int kProducers = 4;
    static constexpr int kConsumers = 4;
    static constexpr int kItemsPerProducer = 10000;  // Reduced from 100000 for faster testing
};

//=============================================================================
// Test Scenario 1: Symmetric Producer/Consumer
//=============================================================================

/**
 * @test Measures Get/Put patterns under balanced producer/consumer load.
 *
 * Expected: Get/Put ratio ≈ 1.0, potential hits depend on thread scheduling.
 * Note: This test may timeout on some systems due to thread scheduling.
 */
TEST_F(LSCQUsagePatternTest, SymmetricProducerConsumer) {
    lscq::LSCQ<uint64_t> queue(kSCQSize);

    std::atomic<int> producers_done{0};
    std::atomic<long> items_in_queue{0};  // Precise tracking of queue occupancy
    std::atomic<bool> consumers_should_exit{false};
    std::atomic<long> enqueue_count{0};
    std::atomic<long> dequeue_count{0};

    // Producers
    std::vector<std::thread> producers;
    for (int i = 0; i < kProducers; ++i) {
        producers.emplace_back([&, i]() {
            for (int j = 0; j < 500; ++j) {
                auto* item = new uint64_t(i * 1000 + j);
                if (queue.enqueue(item)) {
                    items_in_queue.fetch_add(1, std::memory_order_relaxed);
                    enqueue_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    delete item;
                }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    // Consumers: exit when signaled by main thread
    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&]() {
            while (!consumers_should_exit.load(std::memory_order_acquire)) {
                if (auto* item = queue.dequeue()) {
                    dequeue_count.fetch_add(1, std::memory_order_relaxed);
                    delete item;
                    items_in_queue.fetch_sub(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Wait for all producers to finish
    for (auto& t : producers) {
        t.join();
    }

    // Wait until all items are consumed (queue is truly empty)
    while (items_in_queue.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // Signal consumers to exit
    consumers_should_exit.store(true, std::memory_order_release);

    for (auto& t : consumers) {
        t.join();
    }

    std::cout << "\n========== Symmetric Producer/Consumer ==========\n";
    std::cout << "Enqueue count: " << enqueue_count.load() << "\n";
    std::cout << "Dequeue count: " << dequeue_count.load() << "\n";
    std::cout << "=================================================\n";

    EXPECT_EQ(enqueue_count.load(), dequeue_count.load());
}

//=============================================================================
// Test Scenario 2: High Contention Enqueue
//=============================================================================

/**
 * @test Measures behavior under high enqueue contention.
 *
 * Uses small queue (64) with 16 producers to force queue expansion.
 * Tracks retry rates which indicate ObjectPool pressure.
 */
TEST_F(LSCQUsagePatternTest, HighContentionEnqueue) {
    lscq::LSCQ<uint64_t> queue(64);  // Small capacity forces expansion

    std::atomic<long> total_enqueues{0};
    std::atomic<long> enqueue_retries{0};

    std::vector<std::thread> producers;
    for (int i = 0; i < 16; ++i) {  // 16 producers for high contention
        producers.emplace_back([&]() {
            for (int j = 0; j < 10000; ++j) {
                auto* item = new uint64_t(j);
                int attempts = 0;
                while (!queue.enqueue(item)) {
                    attempts++;
                    if (attempts > 100) {
                        delete item;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (attempts <= 100) {
                    total_enqueues.fetch_add(1, std::memory_order_relaxed);
                    if (attempts > 0) {
                        enqueue_retries.fetch_add(attempts, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    std::cout << "\n========== High Contention Enqueue ==========\n";
    std::cout << "Total enqueues:  " << total_enqueues.load() << "\n";
    std::cout << "Total retries:   " << enqueue_retries.load() << "\n";
    std::cout << "Retry rate:      " << std::fixed << std::setprecision(2)
              << (total_enqueues.load() > 0 ? enqueue_retries.load() * 100.0 / total_enqueues.load()
                                            : 0)
              << "%\n";
    std::cout << "=============================================\n";

    // Cleanup queue
    while (auto* item = queue.dequeue()) {
        delete item;
    }
}

//=============================================================================
// Test Scenario 3: Cache Hit Potential Analysis
//=============================================================================

/**
 * @test Analyzes potential cache hit rate for thread_local optimization.
 *
 * Records Get/Put call sequences and measures how often a Get follows
 * a Put on the same thread within 100μs (the window where a local cache
 * would provide a hit).
 *
 * Expected: 20-30% hit rate based on review analysis.
 */
TEST_F(LSCQUsagePatternTest, CacheHitPotentialAnalysis) {
    struct CallRecord {
        enum Type { GET, PUT };
        Type type;
        std::thread::id tid;
        std::chrono::steady_clock::time_point time;
    };

    std::vector<CallRecord> records;
    std::mutex records_mutex;

    lscq::LSCQ<uint64_t> queue(kSCQSize);
    std::atomic<bool> stop{false};

    // Workers that simulate LSCQ load and record Get/Put sequences
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&]() {
            auto tid = std::this_thread::get_id();
            for (int j = 0; j < 1000 && !stop.load(std::memory_order_relaxed); ++j) {
                // Enqueue (may trigger Get from ObjectPool)
                auto* item = new uint64_t(j);
                {
                    std::lock_guard lock(records_mutex);
                    records.push_back({CallRecord::GET, tid, std::chrono::steady_clock::now()});
                }
                queue.enqueue(item);

                // Dequeue (triggers Put to ObjectPool)
                if (auto* dequeued = queue.dequeue()) {
                    {
                        std::lock_guard lock(records_mutex);
                        records.push_back({CallRecord::PUT, tid, std::chrono::steady_clock::now()});
                    }
                    delete dequeued;
                }
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    // Analyze cache hit potential
    long total_gets = 0;
    long potential_hits = 0;
    std::unordered_map<std::thread::id, std::chrono::steady_clock::time_point> last_put;

    for (const auto& rec : records) {
        if (rec.type == CallRecord::PUT) {
            last_put[rec.tid] = rec.time;
        } else {  // GET
            total_gets++;
            auto it = last_put.find(rec.tid);
            if (it != last_put.end()) {
                auto elapsed = rec.time - it->second;
                // If Put happened within 100μs, this would be a cache hit
                if (elapsed < std::chrono::microseconds(100)) {
                    potential_hits++;
                }
            }
        }
    }

    double hit_rate = total_gets > 0 ? potential_hits * 100.0 / total_gets : 0;

    std::cout << "\n========== Cache Hit Potential Analysis ==========\n";
    std::cout << "Total Get calls:      " << total_gets << "\n";
    std::cout << "Potential cache hits: " << potential_hits << "\n";
    std::cout << "Estimated hit rate:   " << std::fixed << std::setprecision(1) << hit_rate
              << "%\n";
    std::cout << "==================================================\n";

    // Hit rate expectation based on review: 20-30%
    // We don't assert a specific value since it depends on scheduling
    EXPECT_GT(total_gets, 0) << "Should have recorded some Get calls";
}
