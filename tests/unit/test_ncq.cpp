#include <gtest/gtest.h>

#define private public
#include <lscq/ncq.hpp>
#undef private

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>
#include <unordered_set>
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

inline std::size_t cache_remap_for(std::size_t capacity, std::size_t idx) {
    constexpr std::size_t kCacheLineSize = 64;
    constexpr std::size_t kEntriesPerLine = kCacheLineSize / sizeof(lscq::Entry);  // 4
    const std::size_t line = idx / kEntriesPerLine;
    const std::size_t offset = idx % kEntriesPerLine;
    const std::size_t num_lines = capacity / kEntriesPerLine;
    return offset * num_lines + line;
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

}  // namespace

TEST(BasicOperation, Enqueue100Dequeue100AndFifoOrder) {
    lscq::NCQ<std::uint64_t> q(256);

    for (std::uint64_t i = 0; i < 100; ++i) {
        EXPECT_TRUE(q.enqueue(i));
    }

    for (std::uint64_t i = 0; i < 100; ++i) {
        EXPECT_EQ(q.dequeue(), i);
    }

    EXPECT_TRUE(q.is_empty());
    EXPECT_EQ(q.dequeue(), lscq::NCQ<std::uint64_t>::kEmpty);
}

TEST(FIFO_Order, StrictlyIncreasing0To999) {
    lscq::NCQ<std::uint64_t> q(2048);

    for (std::uint64_t i = 0; i < 1000; ++i) {
        ASSERT_TRUE(q.enqueue(i));
    }

    for (std::uint64_t i = 0; i < 1000; ++i) {
        EXPECT_EQ(q.dequeue(), i);
    }

    EXPECT_EQ(q.dequeue(), lscq::NCQ<std::uint64_t>::kEmpty);
}

TEST(EdgeCases, EnqueueRejectsReservedEmptySentinel) {
    lscq::NCQ<std::uint64_t> q(16);
    EXPECT_FALSE(q.enqueue(lscq::NCQ<std::uint64_t>::kEmpty));
    EXPECT_TRUE(q.is_empty());
}

TEST(EdgeCases, DequeueOnEmptyReturnsSentinel) {
    lscq::NCQ<std::uint64_t> q(16);
    EXPECT_TRUE(q.is_empty());
    EXPECT_EQ(q.dequeue(), lscq::NCQ<std::uint64_t>::kEmpty);
}

TEST(EdgeCases, ConstructorClampsCapacityToMinimumAndRoundsToCacheLine) {
    {
        lscq::NCQ<std::uint64_t> q(0);
        EXPECT_GE(q.capacity_, 4u);
        EXPECT_EQ(q.capacity_ % 4u, 0u);
        EXPECT_TRUE(q.enqueue(1u));
        EXPECT_EQ(q.dequeue(), 1u);
    }
    {
        lscq::NCQ<std::uint64_t> q(5);
        EXPECT_GE(q.capacity_, 8u);
        EXPECT_EQ(q.capacity_ % 4u, 0u);
    }
}

TEST(EdgeCases, EnqueueSkipsSlotWhenTailAppearsFull) {
    lscq::NCQ<std::uint64_t> q(16);

    const std::uint64_t t = q.tail_.load(std::memory_order_relaxed);
    const std::uint64_t n = static_cast<std::uint64_t>(q.capacity_);
    const std::uint64_t cycle_t = t / n;
    const std::size_t j = static_cast<std::size_t>(t % n);
    const std::size_t remapped_j = cache_remap_for(q.capacity_, j);

    // Force the first probed slot to look "busy" for this cycle.
    q.entries_[remapped_j].cycle_flags = cycle_t;
    q.entries_[remapped_j].index_or_ptr = 0u;

    ASSERT_TRUE(q.enqueue(42u));
    const std::vector<std::uint64_t> drained = drain_queue(q);
    ASSERT_EQ(std::count(drained.begin(), drained.end(), 42u), 1);
}

TEST(ConcurrentEnqueue, EightThreadsEnqueue80000NoLossNoDup) {
    constexpr std::size_t kThreads = 8;
    constexpr std::uint64_t kItersPerThread = 10000;
    constexpr std::uint64_t kTotal = kThreads * kItersPerThread;

    // Keep capacity comfortably above the maximum expected occupancy to avoid pathological "full" behavior.
    lscq::NCQ<std::uint64_t> q(131072);

    SpinStart gate;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (std::size_t tid = 0; tid < kThreads; ++tid) {
        threads.emplace_back([&, tid]() {
            gate.arrive_and_wait();
            const std::uint64_t base = static_cast<std::uint64_t>(tid) * kItersPerThread;
            for (std::uint64_t i = 0; i < kItersPerThread; ++i) {
                (void)q.enqueue(base + i);
            }
        });
    }

    gate.release_when_all_ready(kThreads);
    for (auto& t : threads) {
        t.join();
    }

    const std::vector<std::uint64_t> drained = drain_queue(q);
    ASSERT_EQ(drained.size(), static_cast<std::size_t>(kTotal));

    std::vector<std::uint8_t> seen(static_cast<std::size_t>(kTotal), 0u);
    for (const auto v : drained) {
        ASSERT_LT(v, kTotal);
        ASSERT_EQ(seen[static_cast<std::size_t>(v)], 0u) << "duplicate value: " << v;
        seen[static_cast<std::size_t>(v)] = 1u;
    }
}

TEST(ConcurrentEnqueueDequeue, FourProducersFourConsumersConserveCountNoDup) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::uint64_t kItersPerProducer = 10000;
    constexpr std::uint64_t kTotal = kProducers * kItersPerProducer;

    lscq::NCQ<std::uint64_t> q(131072);

    SpinStart gate;
    std::atomic<std::uint64_t> consumed{0};

    std::vector<std::unordered_set<std::uint64_t>> consumer_sets(kConsumers);
    for (auto& s : consumer_sets) {
        s.reserve(static_cast<std::size_t>(kTotal / kConsumers));
    }

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
        threads.emplace_back([&, c]() {
            gate.arrive_and_wait();
            auto& local = consumer_sets[c];
            while (true) {
                const std::uint64_t done = consumed.load(std::memory_order_relaxed);
                if (done >= kTotal) {
                    break;
                }

                const std::uint64_t v = q.dequeue();
                if (v == lscq::NCQ<std::uint64_t>::kEmpty) {
                    std::this_thread::yield();
                    continue;
                }
                local.insert(v);
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    gate.release_when_all_ready(kProducers + kConsumers);
    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(consumed.load(), kTotal);

    std::vector<std::uint8_t> seen(static_cast<std::size_t>(kTotal), 0u);
    std::size_t merged_count = 0;
    for (const auto& local : consumer_sets) {
        merged_count += local.size();
        for (const auto v : local) {
            ASSERT_LT(v, kTotal);
            ASSERT_EQ(seen[static_cast<std::size_t>(v)], 0u) << "duplicate value: " << v;
            seen[static_cast<std::size_t>(v)] = 1u;
        }
    }

    EXPECT_EQ(merged_count, static_cast<std::size_t>(kTotal));
}

TEST(CycleWrap, TailAndHeadOverflowDoNotBreakSingleSlotSimulation) {
    lscq::NCQ<std::uint64_t> q(4);

    // We intentionally put the queue into a single-slot simulation mode so that:
    //   cycle = (head/tail) / n == head/tail
    // which allows exercising 64-bit wrap-around in a small test.
    q.capacity_ = 1;

    const std::uint64_t initial = std::numeric_limits<std::uint64_t>::max() - 1000u;
    q.head_.store(initial, std::memory_order_relaxed);
    q.tail_.store(initial, std::memory_order_relaxed);

    // Empty state invariant for enqueue/dequeue: entry.cycle == cycle(head) - 1.
    q.entries_[0].cycle_flags = initial - 1u;
    q.entries_[0].index_or_ptr = 0u;

    for (std::uint64_t i = 0; i < 2000; ++i) {
        const std::uint64_t v = i + 1u;
        ASSERT_TRUE(q.enqueue(v));
        ASSERT_EQ(q.dequeue(), v);
    }

    EXPECT_EQ(q.dequeue(), lscq::NCQ<std::uint64_t>::kEmpty);
    EXPECT_LT(q.head_.load(std::memory_order_relaxed), 2000u);
    EXPECT_LT(q.tail_.load(std::memory_order_relaxed), 2000u);
}
