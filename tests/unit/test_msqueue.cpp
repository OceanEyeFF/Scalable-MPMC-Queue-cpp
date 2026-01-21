#include <gtest/gtest.h>

#include <lscq/msqueue.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

TEST(MSQueue, EmptyInitially) {
    lscq::MSQueue<std::uint64_t> q;
    std::uint64_t out = 0;
    EXPECT_FALSE(q.dequeue(out));
}

TEST(MSQueue, SingleThreadFifo) {
    lscq::MSQueue<std::uint64_t> q;

    for (std::uint64_t i = 1; i <= 1000; ++i) {
        ASSERT_TRUE(q.enqueue(i));
    }

    for (std::uint64_t i = 1; i <= 1000; ++i) {
        std::uint64_t out = 0;
        ASSERT_TRUE(q.dequeue(out));
        EXPECT_EQ(out, i);
    }

    std::uint64_t out = 0;
    EXPECT_FALSE(q.dequeue(out));
}

TEST(MSQueue, MultiProducerMultiConsumer) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr std::uint64_t kItemsPerProducer = 2000;
    constexpr std::uint64_t kTotalItems = kItemsPerProducer * kProducers;

    lscq::MSQueue<std::uint64_t> q;

    std::atomic<std::uint64_t> consumed_count{0};
    std::mutex results_mu;
    std::vector<std::uint64_t> results;
    results.reserve(static_cast<std::size_t>(kTotalItems));

    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([p, &q]() {
            const std::uint64_t base = static_cast<std::uint64_t>(p) * kItemsPerProducer;
            for (std::uint64_t i = 0; i < kItemsPerProducer; ++i) {
                ASSERT_TRUE(q.enqueue(base + i));
            }
        });
    }

    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&]() {
            while (true) {
                const std::uint64_t done = consumed_count.load(std::memory_order_relaxed);
                if (done >= kTotalItems) {
                    return;
                }

                std::uint64_t v = 0;
                if (!q.dequeue(v)) {
                    std::this_thread::yield();
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(results_mu);
                    results.push_back(v);
                }
                consumed_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(consumed_count.load(), kTotalItems);
    ASSERT_EQ(results.size(), static_cast<std::size_t>(kTotalItems));

    std::sort(results.begin(), results.end());
    for (std::uint64_t i = 1; i < results.size(); ++i) {
        ASSERT_NE(results[i], results[i - 1]) << "duplicate dequeue value";
    }
    EXPECT_EQ(results.front(), 0u);
    EXPECT_EQ(results.back(), kTotalItems - 1);
}

