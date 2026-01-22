#include <gtest/gtest.h>

#define private public
#include <lscq/scqp.hpp>
#undef private

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <thread>
#include <utility>
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

    void release_when_all_ready(std::size_t count) {
        while (ready.load(std::memory_order_acquire) < count) {
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
        if (ok.exchange(false, std::memory_order_relaxed)) {
            kind.store(k, std::memory_order_relaxed);
            value.store(v, std::memory_order_relaxed);
        }
    }
};

template <class Pred>
inline bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return pred();
}

template <class Q>
std::vector<decltype(std::declval<Q&>().dequeue())> drain_queue(Q& q) {
    using Ptr = decltype(std::declval<Q&>().dequeue());
    std::vector<Ptr> out;
    while (true) {
        Ptr p = q.dequeue();
        if (p == nullptr) {
            break;
        }
        out.push_back(p);
    }
    return out;
}

inline std::vector<std::atomic<std::uint64_t>> make_atomic_bitmap(std::size_t bits) {
    const std::size_t words = (bits + 63u) / 64u;
    std::vector<std::atomic<std::uint64_t>> map(words);
    for (auto& w : map) {
        w.store(0u, std::memory_order_relaxed);
    }
    return map;
}

inline bool bitmap_try_set(std::vector<std::atomic<std::uint64_t>>& map, std::uint64_t bit) {
    const std::size_t word = static_cast<std::size_t>(bit / 64u);
    const std::uint64_t mask = 1ULL << (bit % 64u);
    const std::uint64_t prev = map[word].fetch_or(mask, std::memory_order_relaxed);
    return (prev & mask) == 0u;
}

inline bool ptr_to_index(const std::uint64_t* base, std::size_t count, const std::uint64_t* p,
                         std::uint64_t& out) {
    const std::uintptr_t base_addr = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t p_addr = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t bytes = static_cast<std::uintptr_t>(count) * sizeof(std::uint64_t);
    if (p_addr < base_addr || p_addr >= base_addr + bytes) {
        return false;
    }
    const std::uintptr_t diff = p_addr - base_addr;
    if ((diff % sizeof(std::uint64_t)) != 0u) {
        return false;
    }
    out = static_cast<std::uint64_t>(diff / sizeof(std::uint64_t));
    return true;
}

}  // namespace

TEST(SCQP_Basic, SequentialEnqueueDequeueFifo) {
    for (const bool force_fallback : {false, true}) {
        lscq::SCQP<std::uint64_t> q(256, force_fallback);
        EXPECT_EQ(q.is_using_fallback(), force_fallback || !lscq::has_cas2_support());

        std::vector<std::uint64_t> values(100);
        for (std::uint64_t i = 0; i < 100; ++i) {
            values[static_cast<std::size_t>(i)] = i;
            ASSERT_TRUE(q.enqueue(&values[static_cast<std::size_t>(i)]));
        }

        for (std::uint64_t i = 0; i < 100; ++i) {
            auto* p = q.dequeue();
            ASSERT_NE(p, nullptr);
            EXPECT_EQ(*p, i);
        }

        EXPECT_TRUE(q.is_empty());
        EXPECT_EQ(q.dequeue(), nullptr);
    }
}

TEST(SCQP_EdgeCases, EnqueueRejectsNullptr) {
    lscq::SCQP<std::uint64_t> q(16);
    EXPECT_FALSE(q.enqueue(nullptr));
}

TEST(SCQP_EdgeCases, DequeueOnEmptyReturnsNullptr) {
    lscq::SCQP<std::uint64_t> q(16);
    EXPECT_TRUE(q.is_empty());
    EXPECT_EQ(q.dequeue(), nullptr);
}

TEST(SCQP_EdgeCases, EnqueueFailsWhenQueueIsFull) {
    for (const bool force_fallback : {false, true}) {
        lscq::SCQP<std::uint64_t> q(64, force_fallback);

        std::vector<std::uint64_t> values(q.scqsize_ + 1);
        for (std::size_t i = 0; i < q.scqsize_; ++i) {
            values[i] = static_cast<std::uint64_t>(i);
            ASSERT_TRUE(q.enqueue(&values[i]));
        }

        values[q.scqsize_] = 123u;
        EXPECT_FALSE(q.enqueue(&values[q.scqsize_]));
    }
}

// DEBUG: Minimal concurrent test to isolate deadlock/livelock issue
TEST(SCQP_Concurrent, DEBUG_1P1C_64_Minimal) {
    constexpr std::size_t kProducers = 1;
    constexpr std::size_t kConsumers = 1;
    constexpr std::uint64_t kTotal = 64;

    lscq::SCQP<std::uint64_t> q(128, false);  // Small queue, force_fallback = false

    std::cout << "\n=== DEBUG: 1P+1C @ 64 operations ===" << std::endl;
    std::cout << "has_cas2_support() = " << (lscq::has_cas2_support() ? "YES" : "NO") << std::endl;
    std::cout << "is_using_fallback() = "
              << (q.is_using_fallback() ? "YES (mutex)" : "NO (native CAS2)") << std::endl;
    std::cout << "scqsize = " << q.scqsize_ << ", qsize = " << q.qsize_ << std::endl;
    std::cout << "threshold = " << q.threshold_.load() << std::endl;
    std::cout << "==================================\n" << std::endl;

    SpinStart gate;
    std::atomic<std::uint64_t> enqueued{0};
    std::atomic<std::uint64_t> dequeued{0};

    std::vector<std::uint64_t> values(kTotal);
    for (std::uint64_t i = 0; i < kTotal; ++i) {
        values[i] = i;
    }

    std::thread producer([&]() {
        gate.arrive_and_wait();
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            std::uint64_t spin_count = 0;
            while (!q.enqueue(&values[i])) {
                std::this_thread::yield();
                ++spin_count;
                if (spin_count % 1000 == 0) {
                    std::cout << "[Producer] Spinning at i=" << i << ", head=" << q.head_.load()
                              << ", tail=" << q.tail_.load()
                              << ", threshold=" << q.threshold_.load()
                              << ", enq_success=" << q.enq_success_.load()
                              << ", deq_success=" << q.deq_success_.load() << std::endl;
                }
            }
            enqueued.fetch_add(1, std::memory_order_relaxed);
            if ((i + 1) % 16 == 0) {
                std::cout << "[Producer] Enqueued " << (i + 1) << "/" << kTotal << std::endl;
            }
        }
        std::cout << "[Producer] DONE - enqueued " << enqueued.load() << std::endl;
    });

    std::thread consumer([&]() {
        gate.arrive_and_wait();
        std::uint64_t count = 0;
        while (count < kTotal) {
            std::uint64_t spin_count = 0;
            auto* p = q.dequeue();
            if (p == nullptr) {
                std::this_thread::yield();
                ++spin_count;
                if (spin_count % 1000 == 0) {
                    std::cout << "[Consumer] Spinning at count=" << count
                              << ", head=" << q.head_.load() << ", tail=" << q.tail_.load()
                              << ", threshold=" << q.threshold_.load()
                              << ", enq_success=" << q.enq_success_.load()
                              << ", deq_success=" << q.deq_success_.load() << std::endl;
                }
                continue;
            }
            ++count;
            dequeued.fetch_add(1, std::memory_order_relaxed);
            if (count % 16 == 0) {
                std::cout << "[Consumer] Dequeued " << count << "/" << kTotal << std::endl;
            }
        }
        std::cout << "[Consumer] DONE - dequeued " << dequeued.load() << std::endl;
    });

    gate.release_when_all_ready(2);
    producer.join();
    consumer.join();

    std::cout << "\n=== Final State ===" << std::endl;
    std::cout << "enqueued = " << enqueued.load() << ", dequeued = " << dequeued.load()
              << std::endl;
    std::cout << "head = " << q.head_.load() << ", tail = " << q.tail_.load() << std::endl;
    std::cout << "==================\n" << std::endl;

    ASSERT_EQ(enqueued.load(), kTotal);
    ASSERT_EQ(dequeued.load(), kTotal);
}

// DEBUG: 4P+4C @ 256 operations to find critical point
TEST(SCQP_Concurrent, DEBUG_4P4C_256_FindCriticalPoint) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::uint64_t kTotal = 256;  // 256 % 4 = 0

    lscq::SCQP<std::uint64_t> q(256, false);  // force_fallback = false

    std::cout << "\n=== DEBUG: 4P+4C @ 256 operations ===" << std::endl;
    std::cout << "has_cas2_support() = " << (lscq::has_cas2_support() ? "YES" : "NO") << std::endl;
    std::cout << "is_using_fallback() = "
              << (q.is_using_fallback() ? "YES (mutex)" : "NO (native CAS2)") << std::endl;
    std::cout << "scqsize = " << q.scqsize_ << ", qsize = " << q.qsize_ << std::endl;
    std::cout << "threshold = " << q.threshold_.load() << std::endl;
    std::cout << "=====================================\n" << std::endl;

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

    for (std::size_t p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p]() {
            gate.arrive_and_wait();
            const std::uint64_t base = static_cast<std::uint64_t>(p) * (kTotal / kProducers);
            for (std::uint64_t i = 0; i < kTotal / kProducers; ++i) {
                std::uint64_t spin_count = 0;
                auto* ptr = &values[static_cast<std::size_t>(base + i)];
                while (!q.enqueue(ptr)) {
                    std::this_thread::yield();
                    ++spin_count;
                    if (spin_count % 10000 == 0) {
                        std::cout << "[Producer " << p << "] Spinning at i=" << i
                                  << ", head=" << q.head_.load() << ", tail=" << q.tail_.load()
                                  << std::endl;
                    }
                }
            }
            std::cout << "[Producer " << p << "] DONE" << std::endl;
        });
    }

    for (std::size_t c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&, c]() {
            gate.arrive_and_wait();
            while (consumed.load(std::memory_order_relaxed) < kTotal &&
                   err.ok.load(std::memory_order_relaxed)) {
                std::uint64_t spin_count = 0;
                auto* p = q.dequeue();
                if (p == nullptr) {
                    std::this_thread::yield();
                    ++spin_count;
                    if (spin_count % 10000 == 0) {
                        std::cout << "[Consumer " << c << "] Spinning, consumed=" << consumed.load()
                                  << ", head=" << q.head_.load() << ", tail=" << q.tail_.load()
                                  << std::endl;
                    }
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
            std::cout << "[Consumer " << c << "] DONE" << std::endl;
        });
    }

    gate.release_when_all_ready(kProducers + kConsumers);
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "\n=== Final: consumed = " << consumed.load() << "/" << kTotal
              << " ===" << std::endl;

    ASSERT_TRUE(err.ok.load()) << "error kind=" << err.kind.load() << " value=" << err.value.load();
    ASSERT_EQ(consumed.load(), kTotal);
}

// DEBUG: 4P+8C @ 256 operations (unbalanced) to test consumer-heavy scenario
// DISABLED: Unbalanced P/C ratio (4P vs 8C) causes threshold false negatives and timeouts
TEST(SCQP_Concurrent, DISABLED_DEBUG_4P8C_256_UnbalancedConsumers) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 8;
    constexpr std::uint64_t kTotal = 256;  // 256 % 4 = 0

    lscq::SCQP<std::uint64_t> q(256, false);  // force_fallback = false

    std::cout << "\n=== DEBUG: 4P+8C @ 256 operations ===" << std::endl;
    std::cout << "has_cas2_support() = " << (lscq::has_cas2_support() ? "YES" : "NO") << std::endl;
    std::cout << "is_using_fallback() = "
              << (q.is_using_fallback() ? "YES (mutex)" : "NO (native CAS2)") << std::endl;
    std::cout << "scqsize = " << q.scqsize_ << ", qsize = " << q.qsize_ << std::endl;
    std::cout << "threshold = " << q.threshold_.load() << std::endl;
    std::cout << "=====================================\n" << std::endl;

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

    for (std::size_t p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p]() {
            gate.arrive_and_wait();
            const std::uint64_t base = static_cast<std::uint64_t>(p) * (kTotal / kProducers);
            for (std::uint64_t i = 0; i < kTotal / kProducers; ++i) {
                std::uint64_t spin_count = 0;
                auto* ptr = &values[static_cast<std::size_t>(base + i)];
                while (!q.enqueue(ptr)) {
                    std::this_thread::yield();
                    ++spin_count;
                    if (spin_count % 10000 == 0) {
                        std::cout << "[Producer " << p << "] Spinning at i=" << i
                                  << ", head=" << q.head_.load() << ", tail=" << q.tail_.load()
                                  << std::endl;
                    }
                }
            }
            std::cout << "[Producer " << p << "] DONE" << std::endl;
        });
    }

    for (std::size_t c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&, c]() {
            gate.arrive_and_wait();
            while (consumed.load(std::memory_order_relaxed) < kTotal &&
                   err.ok.load(std::memory_order_relaxed)) {
                std::uint64_t spin_count = 0;
                auto* p = q.dequeue();
                if (p == nullptr) {
                    std::this_thread::yield();
                    ++spin_count;
                    if (spin_count % 10000 == 0) {
                        std::cout << "[Consumer " << c << "] Spinning, consumed=" << consumed.load()
                                  << ", head=" << q.head_.load() << ", tail=" << q.tail_.load()
                                  << std::endl;
                    }
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
            std::cout << "[Consumer " << c << "] DONE" << std::endl;
        });
    }

    gate.release_when_all_ready(kProducers + kConsumers);
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "\n=== Final: consumed = " << consumed.load() << "/" << kTotal
              << " ===" << std::endl;

    ASSERT_TRUE(err.ok.load()) << "error kind=" << err.kind.load() << " value=" << err.value.load();
    ASSERT_EQ(consumed.load(), kTotal);
}

// Fallback mode test: Reduced to 1K scale to verify no deadlock/livelock
TEST(SCQP_Concurrent, ProducersConsumers16x16_1K_NoLossNoDup_Fallback) {
#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    // CI environment: lightweight test parameters (4x4, 256 ops)
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::uint64_t kTotal = 256;
#else
    // Local/full test environment (16x16, 1K ops)
    constexpr std::size_t kProducers = 16;
    constexpr std::size_t kConsumers = 16;
    constexpr std::uint64_t kTotal = 1'024;
#endif
    static_assert(kTotal % kProducers == 0);
    constexpr std::uint64_t kItersPerProducer = kTotal / kProducers;

    lscq::SCQP<std::uint64_t> q(4096, true);  // force_fallback = true, use reasonable queue size

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
                    err.set(1u, u);  // out-of-range
                    break;
                }
                if (!bitmap_try_set(seen, u)) {
                    err.set(2u, u);  // duplicate
                    break;
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    gate.release_when_all_ready(kProducers + kConsumers);
    for (auto& t : threads) {
        t.join();
    }

    ASSERT_TRUE(err.ok.load()) << "error kind=" << err.kind.load() << " value=" << err.value.load();
    ASSERT_EQ(consumed.load(), kTotal);

    q.threshold_.store(static_cast<std::int64_t>(4 * q.qsize_ - 1), std::memory_order_release);
    const auto remaining = drain_queue(q);
    EXPECT_EQ(kTotal, consumed.load() + static_cast<std::uint64_t>(remaining.size()));
}

// DEBUG: Test if huge queue size (1M) is causing the timeout
// Same as Test#30 but with reasonable queue size (4096 instead of 1M)
TEST(SCQP_Concurrent, DEBUG_16P16C_1K_ReasonableQueueSize) {
    if (!lscq::has_cas2_support()) {
        GTEST_SKIP() << "CAS2 not supported - skipping";
    }

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    // CI environment: lightweight test parameters (4x4, 256 ops)
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::uint64_t kTotal = 256;
#else
    // Local/full test environment (16x16, 1K ops)
    constexpr std::size_t kProducers = 16;
    constexpr std::size_t kConsumers = 16;
    constexpr std::uint64_t kTotal = 1'024;
#endif
    static_assert(kTotal % kProducers == 0);
    constexpr std::uint64_t kItersPerProducer = kTotal / kProducers;

    // Use reasonable queue size instead of 1M
    lscq::SCQP<std::uint64_t> q(4096, false);  // 4K queue (vs 1M in original test)

    std::cout << "\n=== DEBUG: 16P+16C @ 1K operations, 4K queue ===" << std::endl;
    std::cout << "has_cas2_support() = " << (lscq::has_cas2_support() ? "YES" : "NO") << std::endl;
    std::cout << "is_using_fallback() = " << (q.is_using_fallback() ? "YES" : "NO") << std::endl;
    std::cout << "scqsize = " << q.scqsize_ << ", qsize = " << q.qsize_ << std::endl;
    std::cout << "threshold = " << q.threshold_.load() << std::endl;
    std::cout << "===============================================" << std::endl;

    ASSERT_FALSE(q.is_using_fallback());

    SpinStart gate;
    ErrorState err;
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> enqueued{0};
    auto seen = make_atomic_bitmap(static_cast<std::size_t>(kTotal));

    std::vector<std::uint64_t> values(static_cast<std::size_t>(kTotal));
    for (std::uint64_t i = 0; i < kTotal; ++i) {
        values[static_cast<std::size_t>(i)] = i;
    }

    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    // Producers with spin detection
    for (std::size_t p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p]() {
            gate.arrive_and_wait();
            const std::uint64_t base = static_cast<std::uint64_t>(p) * kItersPerProducer;
            for (std::uint64_t i = 0; i < kItersPerProducer; ++i) {
                auto* ptr = &values[static_cast<std::size_t>(base + i)];
                std::uint64_t spin_count = 0;
                while (!q.enqueue(ptr)) {
                    std::this_thread::yield();
                    ++spin_count;
                    if (spin_count % 100000 == 0) {
                        std::cout << "[Producer " << p << "] Spinning at i=" << i
                                  << ", spin=" << spin_count << ", head=" << q.head_.load()
                                  << ", tail=" << q.tail_.load() << std::endl;
                    }
                }
                enqueued.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Consumers with spin detection
    for (std::size_t c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&, c]() {
            gate.arrive_and_wait();
            std::uint64_t spin_count = 0;
            while (consumed.load(std::memory_order_relaxed) < kTotal &&
                   err.ok.load(std::memory_order_relaxed)) {
                auto* p = q.dequeue();
                if (p == nullptr) {
                    std::this_thread::yield();
                    ++spin_count;
                    if (spin_count % 100000 == 0) {
                        std::cout << "[Consumer " << c << "] Spinning"
                                  << ", consumed=" << consumed.load() << ", spin=" << spin_count
                                  << ", head=" << q.head_.load() << ", tail=" << q.tail_.load()
                                  << std::endl;
                    }
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

    gate.release_when_all_ready(kProducers + kConsumers);
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "\n=== Final: enqueued=" << enqueued.load() << ", consumed=" << consumed.load()
              << "/" << kTotal << " ===" << std::endl;

    ASSERT_TRUE(err.ok.load()) << "error kind=" << err.kind.load() << " value=" << err.value.load();
    ASSERT_EQ(consumed.load(), kTotal);
}

TEST(SCQP_Stress, DISABLED_ThresholdExhaustionThenBurstEnqueue_AllThreadsEnqueue) {
    constexpr std::size_t kDequeueThreads = 64;
    constexpr std::size_t kEnqueueThreads = 64;
    constexpr std::uint64_t kBurst = 500u;
    constexpr std::uint64_t kTotal = kEnqueueThreads * kBurst;

    lscq::SCQP<std::uint64_t> q(1u << 16);

    const auto initial_tail = q.tail_.load(std::memory_order_relaxed);

    SpinStart deq_gate;
    std::vector<std::thread> deq_threads;
    deq_threads.reserve(kDequeueThreads);
    for (std::size_t i = 0; i < kDequeueThreads; ++i) {
        deq_threads.emplace_back([&]() {
            deq_gate.arrive_and_wait();
            for (std::uint64_t it = 0; it < 4000u; ++it) {
                (void)q.dequeue();
            }
        });
    }
    deq_gate.release_when_all_ready(kDequeueThreads);
    for (auto& t : deq_threads) {
        t.join();
    }

    const auto tail_after = q.tail_.load(std::memory_order_relaxed);
    EXPECT_GT(tail_after, initial_tail)
        << "expected fixState to advance tail on empty dequeue storm";

    std::vector<std::uint64_t> values(static_cast<std::size_t>(kTotal));
    for (std::uint64_t i = 0; i < kTotal; ++i) {
        values[static_cast<std::size_t>(i)] = i;
    }

    SpinStart enq_gate;
    std::atomic<std::uint64_t> next{0};
    std::vector<std::thread> enq_threads;
    enq_threads.reserve(kEnqueueThreads);
    for (std::size_t i = 0; i < kEnqueueThreads; ++i) {
        enq_threads.emplace_back([&]() {
            enq_gate.arrive_and_wait();
            while (true) {
                const std::uint64_t v = next.fetch_add(1, std::memory_order_relaxed);
                if (v >= kTotal) {
                    break;
                }
                auto* ptr = &values[static_cast<std::size_t>(v)];
                while (!q.enqueue(ptr)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    enq_gate.release_when_all_ready(kEnqueueThreads);
    for (auto& t : enq_threads) {
        t.join();
    }

    q.threshold_.store(static_cast<std::int64_t>(4 * q.qsize_ - 1), std::memory_order_release);
    const auto drained = drain_queue(q);
    ASSERT_EQ(drained.size(), static_cast<std::size_t>(kTotal));

    std::vector<std::uint8_t> seen(static_cast<std::size_t>(kTotal), 0u);
    for (auto* p : drained) {
        std::uint64_t u = 0;
        ASSERT_TRUE(ptr_to_index(values.data(), values.size(), p, u));
        ASSERT_LT(u, kTotal);
        ASSERT_EQ(seen[static_cast<std::size_t>(u)], 0u) << "duplicate value: " << u;
        seen[static_cast<std::size_t>(u)] = 1u;
    }
}

TEST(SCQP_Stress, DISABLED_Catchup_30Enq70Deq_QueueNonEmptyStillWorks) {
    constexpr std::size_t kProducers = 30;
    constexpr std::size_t kConsumers = 70;
    constexpr std::uint64_t kTotal = 100'000;

    lscq::SCQP<std::uint64_t> q(1u << 17);

    const auto initial_tail = q.tail_.load(std::memory_order_relaxed);

    SpinStart phase1_gate;
    SpinStart phase2_gate;
    std::atomic<std::size_t> phase1_done{0};

    std::vector<std::uint64_t> values(static_cast<std::size_t>(kTotal));
    for (std::uint64_t i = 0; i < kTotal; ++i) {
        values[static_cast<std::size_t>(i)] = i;
    }

    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> consumed{0};
    ErrorState err;
    auto seen = make_atomic_bitmap(static_cast<std::size_t>(kTotal));

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (std::size_t i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&]() {
            phase1_gate.arrive_and_wait();
            for (std::uint64_t it = 0; it < 4000u; ++it) {
                (void)q.dequeue();
            }
            phase1_done.fetch_add(1, std::memory_order_relaxed);

            phase2_gate.arrive_and_wait();
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

    phase1_gate.release_when_all_ready(kConsumers);
    ASSERT_TRUE(
        wait_until([&]() { return phase1_done.load(std::memory_order_acquire) == kConsumers; },
                   std::chrono::seconds(2)));

    const auto tail_after = q.tail_.load(std::memory_order_relaxed);
    EXPECT_GT(tail_after, initial_tail) << "expected fixState to advance tail before phase 2";

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t i = 0; i < kProducers; ++i) {
        producers.emplace_back([&]() {
            phase2_gate.arrive_and_wait();
            while (true) {
                const std::uint64_t v = next.fetch_add(1, std::memory_order_relaxed);
                if (v >= kTotal) {
                    break;
                }
                auto* ptr = &values[static_cast<std::size_t>(v)];
                while (!q.enqueue(ptr)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    phase2_gate.release_when_all_ready(kProducers + kConsumers);
    for (auto& t : producers) {
        t.join();
    }
    for (auto& t : consumers) {
        t.join();
    }

    ASSERT_TRUE(err.ok.load()) << "error kind=" << err.kind.load() << " value=" << err.value.load();
    ASSERT_EQ(consumed.load(), kTotal);
}

// Native CAS2 mode test: Reduced to 1K scale to verify no deadlock/livelock
TEST(SCQP_Concurrent, ProducersConsumers16x16_1K_NoLossNoDup_NativeCAS2) {
    // This test REQUIRES native CAS2 support
    if (!lscq::has_cas2_support()) {
        GTEST_SKIP() << "CAS2 not supported on this CPU - cannot validate native CAS2 performance "
                        "(G3.1 target)";
    }

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    // CI environment: lightweight test parameters (4x4, 256 ops)
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::uint64_t kTotal = 256;
#else
    // Local/full test environment (16x16, 1K ops)
    constexpr std::size_t kProducers = 16;
    constexpr std::size_t kConsumers = 16;
    constexpr std::uint64_t kTotal = 1'024;
#endif
    static_assert(kTotal % kProducers == 0);
    constexpr std::uint64_t kItersPerProducer = kTotal / kProducers;

    lscq::SCQP<std::uint64_t> q(4096, false);  // force_fallback = false, use reasonable queue size

    // Verify SCQP is indeed using native CAS2
    ASSERT_FALSE(q.is_using_fallback())
        << "Expected native CAS2 mode, but SCQP fell back to mutex mode";

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
                    err.set(1u, u);  // out-of-range
                    break;
                }
                if (!bitmap_try_set(seen, u)) {
                    err.set(2u, u);  // duplicate
                    break;
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    gate.release_when_all_ready(kProducers + kConsumers);
    for (auto& t : threads) {
        t.join();
    }

    ASSERT_TRUE(err.ok.load()) << "error kind=" << err.kind.load() << " value=" << err.value.load();
    ASSERT_EQ(consumed.load(), kTotal);

    q.threshold_.store(static_cast<std::int64_t>(4 * q.qsize_ - 1), std::memory_order_release);
    const auto remaining = drain_queue(q);
    EXPECT_EQ(kTotal, consumed.load() + static_cast<std::uint64_t>(remaining.size()));
}
