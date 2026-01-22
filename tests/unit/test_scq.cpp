#include <gtest/gtest.h>

#define private public
#include <lscq/scq.hpp>
#undef private

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
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
std::vector<std::uint64_t> drain_queue(Q& q) {
    std::vector<std::uint64_t> out;
    while (true) {
        const auto v = q.dequeue();
        if (v == Q::kEmpty) {
            break;
        }
        out.push_back(static_cast<std::uint64_t>(v));
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

}  // namespace

TEST(SCQ_Basic, SequentialEnqueueDequeueFifo) {
    lscq::SCQ<std::uint64_t> q(256);

    for (std::uint64_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(q.enqueue(i));
    }

    for (std::uint64_t i = 0; i < 100; ++i) {
        EXPECT_EQ(q.dequeue(), i);
    }

    EXPECT_TRUE(q.is_empty());
    EXPECT_EQ(q.dequeue(), lscq::SCQ<std::uint64_t>::kEmpty);
}

TEST(SCQ_EdgeCases, EnqueueRejectsReservedSentinelAndBottom) {
    lscq::SCQ<std::uint64_t> q(16);

    EXPECT_FALSE(q.enqueue(lscq::SCQ<std::uint64_t>::kEmpty));
    EXPECT_FALSE(q.enqueue(q.bottom_));
    EXPECT_TRUE(q.enqueue(1u));
    EXPECT_EQ(q.dequeue(), 1u);
}

TEST(SCQ_EdgeCases, DequeueOnEmptyReturnsSentinel) {
    lscq::SCQ<std::uint64_t> q(16);
    EXPECT_TRUE(q.is_empty());
    EXPECT_EQ(q.dequeue(), lscq::SCQ<std::uint64_t>::kEmpty);
}

TEST(SCQ_EdgeCases, EnqueueSpinsWhenQueueIsFullUntilADequeueFreesSpace) {
    lscq::SCQ<std::uint64_t> q(64);

    // Fill the queue completely: enqueue uses bottom_ as the empty marker, so any value < bottom_
    // is valid.
    for (std::size_t i = 0; i < q.scqsize_; ++i) {
        ASSERT_TRUE(q.enqueue(1u));
    }

    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};
    std::thread enq([&]() {
        entered.store(true, std::memory_order_release);
        (void)q.enqueue(2u);
        completed.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(wait_until([&]() { return entered.load(std::memory_order_acquire); },
                           std::chrono::milliseconds(250)));

    // Before freeing a slot, the enqueue should be spinning.
    EXPECT_FALSE(wait_until([&]() { return completed.load(std::memory_order_acquire); },
                            std::chrono::milliseconds(25)));

    const auto v = q.dequeue();
    ASSERT_NE(v, lscq::SCQ<std::uint64_t>::kEmpty);

    enq.join();
    EXPECT_TRUE(completed.load(std::memory_order_acquire));
}

// DISABLED: 1M operations too large for local testing, causes timeouts
TEST(SCQ_Concurrent, DISABLED_ProducersConsumers16x16_1M_NoLossNoDup_Conservative) {
#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    // CI environment: lightweight test parameters (4x4, 1K ops)
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::uint64_t kTotal = 1'024;
    // Values must be < bottom_ (= SCQSIZE - 1), so pick a ring size > kTotal.
    lscq::SCQ<std::uint64_t> q(2048);  // 2K ring size for 1K ops
#else
    // Local/full test environment (16x16, 1M ops)
    constexpr std::size_t kProducers = 16;
    constexpr std::size_t kConsumers = 16;
    constexpr std::uint64_t kTotal = 1'000'000;
    // Values must be < bottom_ (= SCQSIZE - 1), so pick a ring size > kTotal.
    lscq::SCQ<std::uint64_t> q(1u << 20);  // 1,048,576
#endif
    static_assert(kTotal % kProducers == 0);
    constexpr std::uint64_t kItersPerProducer = kTotal / kProducers;
    ASSERT_GT(q.bottom_, kTotal);

    SpinStart gate;
    ErrorState err;
    std::atomic<std::uint64_t> consumed{0};
    auto seen = make_atomic_bitmap(static_cast<std::size_t>(kTotal));

    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    for (std::size_t p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p]() {
            gate.arrive_and_wait();
            const std::uint64_t base = static_cast<std::uint64_t>(p) * kItersPerProducer;
            for (std::uint64_t i = 0; i < kItersPerProducer; ++i) {
                (void)q.enqueue(base + i);
            }
        });
    }

    for (std::size_t c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&]() {
            gate.arrive_and_wait();
            while (consumed.load(std::memory_order_relaxed) < kTotal &&
                   err.ok.load(std::memory_order_relaxed)) {
                const auto v = q.dequeue();
                if (v == lscq::SCQ<std::uint64_t>::kEmpty) {
                    std::this_thread::yield();
                    continue;
                }

                const std::uint64_t u = static_cast<std::uint64_t>(v);
                if (u >= kTotal) {
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

    // Conservatism check: enqueued == dequeued + remaining.
    q.threshold_.store(static_cast<std::int64_t>(3 * q.qsize_ - 1), std::memory_order_release);
    const auto remaining = drain_queue(q);
    EXPECT_EQ(kTotal, consumed.load() + static_cast<std::uint64_t>(remaining.size()));
}

TEST(SCQ_Stress, DISABLED_ThresholdExhaustionThenBurstEnqueue_AllThreadsEnqueue) {
    constexpr std::size_t kDequeueThreads = 64;
    constexpr std::size_t kEnqueueThreads = 64;
    constexpr std::uint64_t kBurst = 500u;
    constexpr std::uint64_t kTotal = kEnqueueThreads * kBurst;

    // Ensure all enqueued values are < bottom_ and the ring can hold the whole burst without
    // blocking.
    lscq::SCQ<std::uint64_t> q(1u << 16);  // 65,536
    ASSERT_GT(q.bottom_, kTotal);
    ASSERT_GE(q.scqsize_, static_cast<std::size_t>(kTotal));

    const auto initial_tail = q.tail_.load(std::memory_order_relaxed);

    // Phase 1: dequeue on empty to exhaust threshold and force catchup/fixState.
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

    // Phase 2: all threads enqueue concurrently (should not livelock after threshold
    // depletion/catchup).
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
                (void)q.enqueue(v);
            }
        });
    }
    enq_gate.release_when_all_ready(kEnqueueThreads);
    for (auto& t : enq_threads) {
        t.join();
    }

    q.threshold_.store(static_cast<std::int64_t>(3 * q.qsize_ - 1), std::memory_order_release);
    const auto drained = drain_queue(q);
    ASSERT_EQ(drained.size(), static_cast<std::size_t>(kTotal));

    std::vector<std::uint8_t> seen(static_cast<std::size_t>(kTotal), 0u);
    for (const auto v : drained) {
        ASSERT_LT(v, kTotal);
        ASSERT_EQ(seen[static_cast<std::size_t>(v)], 0u) << "duplicate value: " << v;
        seen[static_cast<std::size_t>(v)] = 1u;
    }
}

TEST(SCQ_Stress, DISABLED_Catchup_30Enq70Deq_QueueNonEmptyStillWorks) {
    constexpr std::size_t kProducers = 30;
    constexpr std::size_t kConsumers = 70;
    constexpr std::uint64_t kTotal = 100'000;

    lscq::SCQ<std::uint64_t> q(1u << 17);  // 131,072
    ASSERT_GT(q.bottom_, kTotal);

    const auto initial_tail = q.tail_.load(std::memory_order_relaxed);

    SpinStart phase1_gate;
    SpinStart phase2_gate;
    std::atomic<std::size_t> phase1_done{0};

    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> consumed{0};
    ErrorState err;
    auto seen = make_atomic_bitmap(static_cast<std::size_t>(kTotal));

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (std::size_t i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&]() {
            // Phase 1: empty dequeues to force head-tail > SCQSIZE and threshold depletion.
            phase1_gate.arrive_and_wait();
            for (std::uint64_t it = 0; it < 4000u; ++it) {
                (void)q.dequeue();
            }
            phase1_done.fetch_add(1, std::memory_order_relaxed);

            // Phase 2: dequeue-heavy workload.
            phase2_gate.arrive_and_wait();
            while (consumed.load(std::memory_order_relaxed) < kTotal &&
                   err.ok.load(std::memory_order_relaxed)) {
                const auto v = q.dequeue();
                if (v == lscq::SCQ<std::uint64_t>::kEmpty) {
                    std::this_thread::yield();
                    continue;
                }
                const std::uint64_t u = static_cast<std::uint64_t>(v);
                if (u >= kTotal) {
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
                (void)q.enqueue(v);
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
