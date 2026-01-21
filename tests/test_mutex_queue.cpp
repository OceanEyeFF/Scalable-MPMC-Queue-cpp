#include <gtest/gtest.h>

#include <lscq/mutex_queue.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

TEST(MutexQueueTest, EmptyInitially) {
    lscq::MutexQueue<std::uint64_t> q;
    std::uint64_t out = 0;
    EXPECT_FALSE(q.dequeue(out));
}

TEST(MutexQueueTest, SingleThreadFifo) {
    lscq::MutexQueue<std::uint64_t> q;

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

TEST(MutexQueueTest, MultiProducerSingleConsumerFifoAndNoLossNoDup) {
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint32_t kItemsPerProducer = 5000;
    constexpr std::size_t kTotalItems =
        static_cast<std::size_t>(kProducers) * static_cast<std::size_t>(kItemsPerProducer);

    lscq::MutexQueue<std::uint64_t> q;

    std::vector<std::uint64_t> results;
    results.reserve(kTotalItems);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kProducers + 1));

    for (std::uint32_t p = 0; p < kProducers; ++p) {
        threads.emplace_back([p, &q]() {
            for (std::uint32_t i = 0; i < kItemsPerProducer; ++i) {
                const std::uint64_t v = (static_cast<std::uint64_t>(p) << 32) | i;
                ASSERT_TRUE(q.enqueue(v));
            }
        });
    }

    threads.emplace_back([&]() {
        for (std::size_t i = 0; i < kTotalItems; ++i) {
            std::uint64_t v = 0;
            while (!q.dequeue(v)) {
                std::this_thread::yield();
            }
            results.push_back(v);
        }
    });

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(results.size(), kTotalItems);

    std::vector<std::uint32_t> last_seen(kProducers, 0);
    std::vector<bool> seen_any(kProducers, false);
    for (const std::uint64_t v : results) {
        const std::uint32_t producer = static_cast<std::uint32_t>(v >> 32);
        const std::uint32_t seq = static_cast<std::uint32_t>(v & 0xffffffffu);
        ASSERT_LT(producer, kProducers);
        ASSERT_LT(seq, kItemsPerProducer);
        if (seen_any[producer]) {
            ASSERT_GT(seq, last_seen[producer]) << "FIFO violated within producer " << producer;
        }
        seen_any[producer] = true;
        last_seen[producer] = seq;
    }

    std::vector<std::uint64_t> expected;
    expected.reserve(kTotalItems);
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        for (std::uint32_t i = 0; i < kItemsPerProducer; ++i) {
            expected.push_back((static_cast<std::uint64_t>(p) << 32) | i);
        }
    }

    std::sort(results.begin(), results.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(results, expected);
}

TEST(MutexQueueTest, MultiProducerMultiConsumerNoLossNoDup) {
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint32_t kConsumers = 4;
    constexpr std::uint32_t kItemsPerProducer = 5000;
    constexpr std::size_t kTotalItems =
        static_cast<std::size_t>(kProducers) * static_cast<std::size_t>(kItemsPerProducer);

    lscq::MutexQueue<std::uint64_t> q;

    std::atomic<std::size_t> consumed{0};
    std::mutex results_mu;
    std::vector<std::uint64_t> results;
    results.reserve(kTotalItems);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kProducers + kConsumers));

    for (std::uint32_t p = 0; p < kProducers; ++p) {
        threads.emplace_back([p, &q]() {
            for (std::uint32_t i = 0; i < kItemsPerProducer; ++i) {
                const std::uint64_t v = (static_cast<std::uint64_t>(p) << 32) | i;
                ASSERT_TRUE(q.enqueue(v));
            }
        });
    }

    for (std::uint32_t c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&]() {
            while (true) {
                const std::size_t done = consumed.load(std::memory_order_relaxed);
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
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(consumed.load(), kTotalItems);
    ASSERT_EQ(results.size(), kTotalItems);

    std::vector<std::uint64_t> expected;
    expected.reserve(kTotalItems);
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        for (std::uint32_t i = 0; i < kItemsPerProducer; ++i) {
            expected.push_back((static_cast<std::uint64_t>(p) << 32) | i);
        }
    }

    std::sort(results.begin(), results.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(results, expected);
}
