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
