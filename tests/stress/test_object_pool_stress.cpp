/**
 * @file test_object_pool_stress.cpp
 * @brief High-load stability tests for ObjectPoolTLS and ObjectPoolMap
 *
 * Test scenarios:
 * 1. High concurrency stress (many threads, rapid Get/Put)
 * 2. Long-running stability (detect memory leaks, deadlocks)
 * 3. Thread churn (frequent thread creation/destruction)
 * 4. Mixed operations (concurrent Get/Put/Clear)
 * 5. Memory pressure (large object allocation)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <lscq/object_pool_map.hpp>
#include <lscq/object_pool_tls.hpp>

namespace {

// Test configuration
struct StressConfig {
    std::size_t num_threads = 16;
    std::size_t ops_per_thread = 100000;
    std::chrono::seconds duration{10};
    bool verbose = false;
};

// Tracked object for memory leak detection
struct TrackedObject {
    inline static std::atomic<std::size_t> created{0};
    inline static std::atomic<std::size_t> destroyed{0};
    inline static std::atomic<std::size_t> active{0};

    std::uint64_t id;
    std::uint64_t payload[8];  // 64 bytes payload

    TrackedObject() : id(created.fetch_add(1, std::memory_order_relaxed)) {
        active.fetch_add(1, std::memory_order_relaxed);
        for (auto& p : payload) {
            p = id;
        }
    }

    ~TrackedObject() {
        destroyed.fetch_add(1, std::memory_order_relaxed);
        active.fetch_sub(1, std::memory_order_relaxed);
    }

    bool verify() const {
        for (const auto& p : payload) {
            if (p != id) return false;
        }
        return true;
    }

    static void reset() {
        created.store(0, std::memory_order_relaxed);
        destroyed.store(0, std::memory_order_relaxed);
        active.store(0, std::memory_order_relaxed);
    }

    static void print_stats(const char* label) {
        std::cout << "[" << label << "] created=" << created.load()
                  << " destroyed=" << destroyed.load()
                  << " active=" << active.load() << std::endl;
    }
};

// Barrier for synchronized thread start
class SpinBarrier {
   public:
    explicit SpinBarrier(std::size_t count) : count_(count) {}

    void arrive_and_wait() {
        arrived_.fetch_add(1, std::memory_order_acq_rel);
        while (arrived_.load(std::memory_order_acquire) < count_) {
            std::this_thread::yield();
        }
    }

   private:
    std::size_t count_;
    std::atomic<std::size_t> arrived_{0};
};

// Statistics collector
struct StressStats {
    std::atomic<std::size_t> get_ops{0};
    std::atomic<std::size_t> put_ops{0};
    std::atomic<std::size_t> cache_hits{0};
    std::atomic<std::size_t> cache_misses{0};
    std::atomic<std::size_t> errors{0};

    void print(const char* label, double elapsed_sec) const {
        const std::size_t total = get_ops.load() + put_ops.load();
        const double ops_per_sec = total / elapsed_sec;
        std::cout << "[" << label << "] "
                  << "ops=" << total << " "
                  << "ops/sec=" << static_cast<std::size_t>(ops_per_sec) << " "
                  << "errors=" << errors.load() << std::endl;
    }
};

// ============================================================================
// Test 1: High Concurrency Stress Test
// ============================================================================

template <typename PoolType>
void RunHighConcurrencyStress(const char* pool_name, const StressConfig& config) {
    TrackedObject::reset();
    StressStats stats;

    PoolType pool([] { return new TrackedObject(); });
    SpinBarrier barrier(config.num_threads);
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    threads.reserve(config.num_threads);

    auto start_time = std::chrono::steady_clock::now();

    for (std::size_t t = 0; t < config.num_threads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<unsigned>(t));
            std::uniform_int_distribution<int> dist(0, 1);

            barrier.arrive_and_wait();

            for (std::size_t i = 0; i < config.ops_per_thread && !stop.load(std::memory_order_relaxed); ++i) {
                if (dist(rng) == 0) {
                    // Get operation
                    TrackedObject* obj = pool.Get();
                    if (obj != nullptr) {
                        if (!obj->verify()) {
                            stats.errors.fetch_add(1, std::memory_order_relaxed);
                        }
                        pool.Put(obj);
                        stats.put_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                    stats.get_ops.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // Get-Put pair
                    TrackedObject* obj = pool.Get();
                    stats.get_ops.fetch_add(1, std::memory_order_relaxed);
                    if (obj != nullptr) {
                        pool.Put(obj);
                        stats.put_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    pool.Clear();

    if (config.verbose) {
        stats.print(pool_name, elapsed);
        TrackedObject::print_stats(pool_name);
    }

    // Verify no memory leaks
    EXPECT_EQ(stats.errors.load(), 0u) << pool_name << ": data corruption detected";
    EXPECT_EQ(TrackedObject::active.load(), 0u) << pool_name << ": memory leak detected";
    EXPECT_EQ(TrackedObject::created.load(), TrackedObject::destroyed.load())
        << pool_name << ": created != destroyed";
}

TEST(ObjectPoolStress, HighConcurrency_TLS) {
    StressConfig config;
    config.num_threads = 16;
    config.ops_per_thread = 100000;
    config.verbose = true;

    RunHighConcurrencyStress<lscq::ObjectPoolTLS<TrackedObject>>("ObjectPoolTLS", config);
}

TEST(ObjectPoolStress, HighConcurrency_Map) {
    StressConfig config;
    config.num_threads = 16;
    config.ops_per_thread = 100000;
    config.verbose = true;

    RunHighConcurrencyStress<lscq::ObjectPoolMap<TrackedObject>>("ObjectPoolMap", config);
}

// ============================================================================
// Test 2: Long-Running Stability Test
// ============================================================================

template <typename PoolType>
void RunLongRunningStability(const char* pool_name, std::chrono::seconds duration) {
    TrackedObject::reset();
    StressStats stats;

    PoolType pool([] { return new TrackedObject(); });
    std::atomic<bool> stop{false};

    constexpr std::size_t kNumThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    auto start_time = std::chrono::steady_clock::now();

    for (std::size_t t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<unsigned>(t + 1000));
            std::uniform_int_distribution<int> op_dist(0, 9);

            while (!stop.load(std::memory_order_relaxed)) {
                int op = op_dist(rng);

                if (op < 8) {
                    // 80% Get-Put
                    TrackedObject* obj = pool.Get();
                    stats.get_ops.fetch_add(1, std::memory_order_relaxed);
                    if (obj != nullptr) {
                        if (!obj->verify()) {
                            stats.errors.fetch_add(1, std::memory_order_relaxed);
                        }
                        // Simulate some work
                        std::this_thread::yield();
                        pool.Put(obj);
                        stats.put_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    // 20% Get only (let pool manage cleanup)
                    TrackedObject* obj = pool.Get();
                    stats.get_ops.fetch_add(1, std::memory_order_relaxed);
                    if (obj != nullptr) {
                        pool.Put(obj);
                        stats.put_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    // Run for specified duration
    std::this_thread::sleep_for(duration);
    stop.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    pool.Clear();

    std::cout << "[" << pool_name << " LongRunning] ";
    stats.print(pool_name, elapsed);
    TrackedObject::print_stats(pool_name);

    EXPECT_EQ(stats.errors.load(), 0u) << pool_name << ": data corruption detected";
    EXPECT_EQ(TrackedObject::active.load(), 0u) << pool_name << ": memory leak detected";
}

TEST(ObjectPoolStress, LongRunning_TLS_10s) {
    RunLongRunningStability<lscq::ObjectPoolTLS<TrackedObject>>("ObjectPoolTLS", std::chrono::seconds(10));
}

TEST(ObjectPoolStress, LongRunning_Map_10s) {
    RunLongRunningStability<lscq::ObjectPoolMap<TrackedObject>>("ObjectPoolMap", std::chrono::seconds(10));
}

// Extended long-running tests (60 seconds)
TEST(ObjectPoolStress, LongRunning_TLS_60s) {
    RunLongRunningStability<lscq::ObjectPoolTLS<TrackedObject>>("ObjectPoolTLS", std::chrono::seconds(60));
}

TEST(ObjectPoolStress, LongRunning_Map_60s) {
    RunLongRunningStability<lscq::ObjectPoolMap<TrackedObject>>("ObjectPoolMap", std::chrono::seconds(60));
}

// ============================================================================
// Test 3: Thread Churn Test (frequent thread creation/destruction)
// ============================================================================

template <typename PoolType>
void RunThreadChurnTest(const char* pool_name, std::size_t num_waves, std::size_t threads_per_wave) {
    TrackedObject::reset();
    StressStats stats;

    PoolType pool([] { return new TrackedObject(); });

    auto start_time = std::chrono::steady_clock::now();

    for (std::size_t wave = 0; wave < num_waves; ++wave) {
        std::vector<std::thread> threads;
        threads.reserve(threads_per_wave);

        for (std::size_t t = 0; t < threads_per_wave; ++t) {
            threads.emplace_back([&] {
                // Each short-lived thread does some operations
                for (int i = 0; i < 100; ++i) {
                    TrackedObject* obj = pool.Get();
                    stats.get_ops.fetch_add(1, std::memory_order_relaxed);
                    if (obj != nullptr) {
                        if (!obj->verify()) {
                            stats.errors.fetch_add(1, std::memory_order_relaxed);
                        }
                        pool.Put(obj);
                        stats.put_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& th : threads) {
            th.join();
        }

        // Small delay between waves
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    pool.Clear();

    std::cout << "[" << pool_name << " ThreadChurn] waves=" << num_waves
              << " threads_per_wave=" << threads_per_wave << " ";
    stats.print(pool_name, elapsed);
    TrackedObject::print_stats(pool_name);

    EXPECT_EQ(stats.errors.load(), 0u) << pool_name << ": data corruption detected";
    EXPECT_EQ(TrackedObject::active.load(), 0u) << pool_name << ": memory leak detected";
}

TEST(ObjectPoolStress, ThreadChurn_TLS) {
    // 50 waves of 8 threads each = 400 short-lived threads
    RunThreadChurnTest<lscq::ObjectPoolTLS<TrackedObject>>("ObjectPoolTLS", 50, 8);
}

TEST(ObjectPoolStress, ThreadChurn_Map) {
    RunThreadChurnTest<lscq::ObjectPoolMap<TrackedObject>>("ObjectPoolMap", 50, 8);
}

// ============================================================================
// Test 4: Concurrent Clear Test
// ============================================================================

template <typename PoolType>
void RunConcurrentClearTest(const char* pool_name, std::chrono::seconds duration) {
    TrackedObject::reset();
    StressStats stats;
    std::atomic<std::size_t> clear_ops{0};

    PoolType pool([] { return new TrackedObject(); });
    std::atomic<bool> stop{false};

    constexpr std::size_t kWorkerThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kWorkerThreads + 1);

    // Worker threads: Get/Put
    for (std::size_t t = 0; t < kWorkerThreads; ++t) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                TrackedObject* obj = pool.Get();
                stats.get_ops.fetch_add(1, std::memory_order_relaxed);
                if (obj != nullptr) {
                    pool.Put(obj);
                    stats.put_ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Clearer thread: periodic Clear()
    threads.emplace_back([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pool.Clear();
            clear_ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto start_time = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(duration);
    stop.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    pool.Clear();

    std::cout << "[" << pool_name << " ConcurrentClear] clear_ops=" << clear_ops.load() << " ";
    stats.print(pool_name, elapsed);
    TrackedObject::print_stats(pool_name);

    EXPECT_EQ(TrackedObject::active.load(), 0u) << pool_name << ": memory leak detected";
}

TEST(ObjectPoolStress, ConcurrentClear_TLS) {
    RunConcurrentClearTest<lscq::ObjectPoolTLS<TrackedObject>>("ObjectPoolTLS", std::chrono::seconds(5));
}

TEST(ObjectPoolStress, ConcurrentClear_Map) {
    RunConcurrentClearTest<lscq::ObjectPoolMap<TrackedObject>>("ObjectPoolMap", std::chrono::seconds(5));
}

// ============================================================================
// Test 5: Pool Destruction While Threads Running
// ============================================================================

template <typename PoolType>
void RunPoolDestructionTest(const char* pool_name) {
    TrackedObject::reset();
    std::atomic<std::size_t> ops{0};
    std::atomic<bool> pool_destroyed{false};

    {
        auto pool = std::make_unique<PoolType>([] { return new TrackedObject(); });
        std::atomic<bool> stop{false};

        constexpr std::size_t kNumThreads = 8;
        std::vector<std::thread> threads;
        threads.reserve(kNumThreads);

        for (std::size_t t = 0; t < kNumThreads; ++t) {
            threads.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    if (pool_destroyed.load(std::memory_order_acquire)) {
                        break;
                    }
                    TrackedObject* obj = pool->Get();
                    ops.fetch_add(1, std::memory_order_relaxed);
                    if (obj != nullptr) {
                        pool->Put(obj);
                    }
                }
            });
        }

        // Let threads run for a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Signal stop and destroy pool
        stop.store(true, std::memory_order_release);

        // Wait a bit for threads to notice stop
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Destroy pool while some threads might still be running
        pool.reset();
        pool_destroyed.store(true, std::memory_order_release);

        for (auto& th : threads) {
            th.join();
        }
    }

    std::cout << "[" << pool_name << " PoolDestruction] ops=" << ops.load() << " ";
    TrackedObject::print_stats(pool_name);

    // Note: Some objects might be leaked if threads were holding pointers
    // when the pool was destroyed. This test verifies no crashes occur.
    // The closing_ flag should prevent operations after destruction starts.
}

TEST(ObjectPoolStress, PoolDestruction_TLS) {
    RunPoolDestructionTest<lscq::ObjectPoolTLS<TrackedObject>>("ObjectPoolTLS");
}

TEST(ObjectPoolStress, PoolDestruction_Map) {
    RunPoolDestructionTest<lscq::ObjectPoolMap<TrackedObject>>("ObjectPoolMap");
}

// ============================================================================
// Test 6: Extreme Thread Count
// ============================================================================

template <typename PoolType>
void RunExtremeThreadCountTest(const char* pool_name, std::size_t num_threads) {
    TrackedObject::reset();
    StressStats stats;

    PoolType pool([] { return new TrackedObject(); });
    SpinBarrier barrier(num_threads);

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto start_time = std::chrono::steady_clock::now();

    for (std::size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&] {
            barrier.arrive_and_wait();

            // Each thread does a burst of operations
            for (int i = 0; i < 1000; ++i) {
                TrackedObject* obj = pool.Get();
                stats.get_ops.fetch_add(1, std::memory_order_relaxed);
                if (obj != nullptr) {
                    if (!obj->verify()) {
                        stats.errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    pool.Put(obj);
                    stats.put_ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    pool.Clear();

    std::cout << "[" << pool_name << " ExtremeThreads] threads=" << num_threads << " ";
    stats.print(pool_name, elapsed);
    TrackedObject::print_stats(pool_name);

    EXPECT_EQ(stats.errors.load(), 0u) << pool_name << ": data corruption detected";
    EXPECT_EQ(TrackedObject::active.load(), 0u) << pool_name << ": memory leak detected";
}

TEST(ObjectPoolStress, ExtremeThreadCount_TLS_64) {
    RunExtremeThreadCountTest<lscq::ObjectPoolTLS<TrackedObject>>("ObjectPoolTLS", 64);
}

TEST(ObjectPoolStress, ExtremeThreadCount_Map_64) {
    RunExtremeThreadCountTest<lscq::ObjectPoolMap<TrackedObject>>("ObjectPoolMap", 64);
}

// ============================================================================
// Test 7: Comparison Test (run both pools under identical conditions)
// ============================================================================

TEST(ObjectPoolStress, ComparisonTest) {
    std::cout << "\n========== Comparison Test ==========" << std::endl;

    StressConfig config;
    config.num_threads = 16;
    config.ops_per_thread = 500000;
    config.verbose = true;

    std::cout << "\n--- ObjectPoolTLS ---" << std::endl;
    auto tls_start = std::chrono::steady_clock::now();
    RunHighConcurrencyStress<lscq::ObjectPoolTLS<TrackedObject>>("TLS", config);
    auto tls_end = std::chrono::steady_clock::now();
    double tls_time = std::chrono::duration<double>(tls_end - tls_start).count();

    std::cout << "\n--- ObjectPoolMap ---" << std::endl;
    auto map_start = std::chrono::steady_clock::now();
    RunHighConcurrencyStress<lscq::ObjectPoolMap<TrackedObject>>("Map", config);
    auto map_end = std::chrono::steady_clock::now();
    double map_time = std::chrono::duration<double>(map_end - map_start).count();

    std::cout << "\n--- Summary ---" << std::endl;
    std::cout << "TLS time: " << tls_time << "s" << std::endl;
    std::cout << "Map time: " << map_time << "s" << std::endl;
    std::cout << "TLS is " << (map_time / tls_time) << "x faster" << std::endl;
    std::cout << "=========================================\n" << std::endl;
}

}  // namespace
