#include <gtest/gtest.h>

#define private public
#include <lscq/scqp.hpp>
#undef private

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

namespace {

struct SpinStart {
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};

    void arrive_and_wait() {
        ready.fetch_add(1, std::memory_order_relaxed);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void release_when_all_ready(std::size_t expected) {
        while (ready.load(std::memory_order_relaxed) < expected) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
    }
};

struct ErrorState {
    std::atomic<bool> ok{true};
    std::atomic<std::uint32_t> kind{0};
    std::atomic<std::uint64_t> value{0};

    void set(std::uint32_t k, std::uint64_t v) {
        ok.store(false, std::memory_order_relaxed);
        kind.store(k, std::memory_order_relaxed);
        value.store(v, std::memory_order_relaxed);
    }
};

inline bool ptr_to_index(const std::uint64_t* base, std::size_t size, const std::uint64_t* p,
                         std::uint64_t& out) {
    if (p < base || p >= base + size) {
        return false;
    }
    out = static_cast<std::uint64_t>(p - base);
    return true;
}

inline bool bitmap_try_set(std::vector<std::atomic<std::uint64_t>>& bm, std::uint64_t idx) {
    const std::size_t word_idx = static_cast<std::size_t>(idx / 64u);
    const std::uint64_t bit_mask = 1ull << (idx % 64u);
    const std::uint64_t old = bm[word_idx].fetch_or(bit_mask, std::memory_order_relaxed);
    return (old & bit_mask) == 0;
}

inline std::vector<std::atomic<std::uint64_t>> make_atomic_bitmap(std::size_t bits) {
    const std::size_t words = (bits + 63u) / 64u;
    std::vector<std::atomic<std::uint64_t>> map(words);
    for (auto& w : map) {
        w.store(0u, std::memory_order_relaxed);
    }
    return map;
}

}  // namespace

// Performance benchmark: Compare different queue sizes on AMD Ryzen 9 5900X
// Tests 12P+12C @ 1200 elements with queue sizes: 2K, 4K, 8K, 16K, 32K
// Uses 12P+12C (24 threads total) to match 5900X's 24 threads and avoid cross-CCD scheduling
TEST(SCQP_Performance, QueueSizeComparison_12P12C_1200_5900X) {
    if (!lscq::has_cas2_support()) {
        GTEST_SKIP() << "CAS2 not supported - skipping performance benchmark";
    }

    constexpr std::size_t kProducers = 12;
    constexpr std::size_t kConsumers = 12;
    constexpr std::uint64_t kTotal = 1'200;  // 1200 elements (1200 % 12 = 0)
    static_assert(kTotal % kProducers == 0);
    constexpr std::uint64_t kItersPerProducer = kTotal / kProducers;

    // Test different queue sizes: 2K, 4K, 8K, 16K, 32K
    const std::vector<std::size_t> queue_sizes = {2048, 4096, 8192, 16384, 32768};

    std::cout << "\n=== Performance Benchmark: 12P+12C @ 1200 elements ===" << std::endl;
    std::cout << "CPU: AMD Ryzen 9 5900X (12-core 24-thread, dual-CCD)" << std::endl;
    std::cout << "Threads: 12P+12C = 24 threads (matches CPU, avoids cross-CCD)" << std::endl;
    std::cout << "CAS2: Native 128-bit atomic operations" << std::endl;
    std::cout << "================================================================" << std::endl;
    std::cout << std::endl;

    for (std::size_t qsize : queue_sizes) {
        lscq::SCQP<std::uint64_t> q(qsize, false);  // Native CAS2 mode
        ASSERT_FALSE(q.is_using_fallback());

        SpinStart gate;
        ErrorState err;
        std::atomic<std::uint64_t> consumed{0};
        auto seen = make_atomic_bitmap(static_cast<std::size_t>(kTotal));

        std::vector<std::uint64_t> values(static_cast<std::size_t>(kTotal));
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            values[static_cast<std::size_t>(i)] = i;
        }

        std::vector<std::thread> threads;
        threads.reserve(kProducers + kConsumers);

        // Producers
        for (std::size_t p = 0; p < kProducers; ++p) {
            threads.emplace_back([&, p]() {
                gate.arrive_and_wait();
                const std::uint64_t base = static_cast<std::uint64_t>(p) * kItersPerProducer;
                for (std::uint64_t i = 0; i < kItersPerProducer; ++i) {
                    auto* ptr = &values[static_cast<std::size_t>(base + i)];
                    while (!q.enqueue(ptr)) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        // Consumers
        for (std::size_t c = 0; c < kConsumers; ++c) {
            threads.emplace_back([&]() {
                gate.arrive_and_wait();
                while (consumed.load(std::memory_order_relaxed) < kTotal &&
                       err.ok.load(std::memory_order_relaxed)) {
                    auto* p = q.dequeue();
                    if (p == nullptr) {
                        std::this_thread::yield();
                        continue;
                    }

                    std::uint64_t u = 0;
                    if (!ptr_to_index(values.data(), values.size(), p, u) || u >= kTotal) {
                        err.set(1u, u);
                        break;
                    }
                    if (!bitmap_try_set(seen, u)) {
                        err.set(2u, u);
                        break;
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Measure execution time
        const auto start = std::chrono::steady_clock::now();
        gate.release_when_all_ready(kProducers + kConsumers);
        for (auto& t : threads) {
            t.join();
        }
        const auto end = std::chrono::steady_clock::now();
        const auto duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // Calculate throughput and memory usage
        const double ops_per_sec = (kTotal * 1000.0) / duration_ms;
        const double memory_mb =
            (qsize * sizeof(typename lscq::SCQP<std::uint64_t>::EntryP)) / (1024.0 * 1024.0);

        std::cout << "Queue Size: " << std::setw(6) << qsize << " (" << std::fixed
                  << std::setprecision(2) << std::setw(6) << memory_mb << " MB)"
                  << " | Time: " << std::setw(6) << duration_ms << " ms"
                  << " | Throughput: " << std::setw(8) << std::fixed << std::setprecision(0)
                  << ops_per_sec << " ops/s" << std::endl;

        ASSERT_TRUE(err.ok.load()) << "Queue size=" << qsize << ", error kind=" << err.kind.load()
                                   << " value=" << err.value.load();
        ASSERT_EQ(consumed.load(), kTotal) << "Queue size=" << qsize;
    }

    std::cout << "\n=== Benchmark Complete ===" << std::endl;
}
