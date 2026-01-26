#include <gtest/gtest.h>

// AddressSanitizer note:
// - These tests intentionally exercise Get/Put/Clear under concurrency.
// - They avoid use-after-Put (callers must not access an object after Put()).
// - Prefer running with ASan to catch leaks/UAF, and with TSan to catch data races.

#define private public
#include <lscq/object_pool.hpp>
#undef private
#include <lscq/object_pool_map.hpp>
#include <lscq/object_pool_tls.hpp>

#include "test_object_pool_suite.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
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

template <class Pool>
std::size_t shard_size(Pool& pool, std::size_t shard_index) {
    auto& shard = pool.shards_[shard_index];
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.objects.size();
}

template <class Pool>
std::vector<std::size_t> shard_sizes(Pool& pool) {
    std::vector<std::size_t> out;
    out.reserve(pool.shards_.size());
    for (std::size_t i = 0; i < pool.shards_.size(); ++i) {
        out.push_back(shard_size(pool, i));
    }
    return out;
}

struct Tracked {
    inline static std::atomic<std::size_t> destroyed{0};
    std::uint32_t payload = 0;
    ~Tracked() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};

struct Tracked2 {
    inline static std::atomic<std::size_t> destroyed{0};
    ~Tracked2() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};

struct Item {
    inline static std::atomic<std::size_t> destroyed{0};
    std::uint64_t id = 0;
    std::uint64_t payload = 0;
    ~Item() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};

}  // namespace

TEST(ObjectPoolTest, BasicGetPut) {
    lscq::ObjectPool<int> pool([] { return new int(0); }, 1);

    EXPECT_EQ(pool.Size(), 0u);

    int* p = pool.Get();
    ASSERT_NE(p, nullptr);
    *p = 7;
    pool.Put(p);

    EXPECT_EQ(pool.Size(), 1u);

    int* q = pool.Get();
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(*q, 7);
    pool.Put(q);

    EXPECT_EQ(pool.Size(), 1u);
    pool.Put(nullptr);
    EXPECT_EQ(pool.Size(), 1u);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolTest, ObjectReuse) {
    lscq::ObjectPool<int> pool([] { return new int(0); }, 1);

    int* first = pool.Get();
    ASSERT_NE(first, nullptr);
    pool.Put(first);

    int* second = pool.Get();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second, first);
    pool.Put(second);
    pool.Clear();
}

TEST(ObjectPoolTest, FactoryCreation) {
    std::atomic<std::size_t> created{0};
    lscq::ObjectPool<int> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new int(123);
        },
        1);

    EXPECT_EQ(created.load(), 0u);
    int* p = pool.Get();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(created.load(), 1u);
    pool.Put(p);

    int* q = pool.Get();  // should reuse instead of calling factory again
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(created.load(), 1u);
    pool.Put(q);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);

    int* r = pool.Get();  // pool empty again => factory
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(created.load(), 2u);
    pool.Put(r);
    pool.Clear();

    lscq::ObjectPool<int> null_factory_pool(lscq::ObjectPool<int>::Factory{}, 1);
    EXPECT_EQ(null_factory_pool.Get(), nullptr);
}

TEST(ObjectPoolTest, ClearPool) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    {
        lscq::ObjectPool<Tracked> pool([] { return new Tracked(); }, 1);

        constexpr std::size_t kCount = 10;
        std::vector<Tracked*> items;
        items.reserve(kCount);

        for (std::size_t i = 0; i < kCount; ++i) {
            Tracked* p = pool.Get();
            ASSERT_NE(p, nullptr);
            p->payload = static_cast<std::uint32_t>(i);
            items.push_back(p);
        }
        for (Tracked* p : items) {
            pool.Put(p);
        }

        EXPECT_EQ(pool.Size(), kCount);
        EXPECT_EQ(Tracked::destroyed.load(), 0u);

        pool.Clear();
        EXPECT_EQ(pool.Size(), 0u);
        EXPECT_EQ(Tracked::destroyed.load(), kCount);
    }

    // Pool destruction also clears remaining objects.
    Tracked2::destroyed.store(0u, std::memory_order_relaxed);

    {
        lscq::ObjectPool<Tracked2> pool([] { return new Tracked2(); }, 1);
        Tracked2* p0 = pool.Get();
        Tracked2* p1 = pool.Get();
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);
        pool.Put(p0);
        pool.Put(p1);
        EXPECT_EQ(pool.Size(), 2u);
        EXPECT_EQ(Tracked2::destroyed.load(), 0u);
    }
    EXPECT_EQ(Tracked2::destroyed.load(), 2u);
}

TEST(ObjectPoolTest, PoolSize) {
    std::atomic<std::size_t> created{0};
    lscq::ObjectPool<int> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new int(0);
        },
        1);

    EXPECT_EQ(pool.Size(), 0u);

    int* a = pool.Get();
    int* b = pool.Get();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(created.load(), 2u);

    pool.Put(a);
    EXPECT_EQ(pool.Size(), 1u);
    pool.Put(b);
    EXPECT_EQ(pool.Size(), 2u);

    int* c = pool.Get();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(pool.Size(), 1u);
    pool.Put(c);
    EXPECT_EQ(pool.Size(), 2u);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolTest, ConcurrentGetPut) {
    Item::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    lscq::ObjectPool<Item> pool([&] {
        const std::size_t id = created.fetch_add(1, std::memory_order_relaxed);
        Item* p = new Item();
        p->id = static_cast<std::uint64_t>(id);
        return p;
    });

    const std::size_t hc = std::max<std::size_t>(2u, std::thread::hardware_concurrency());
    const std::size_t thread_count = std::min<std::size_t>(8u, hc);
    const std::size_t iters = 2000;

    SpinStart start;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    std::atomic<bool> ok{true};

    for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < iters; ++i) {
                Item* p = pool.Get();
                if (p == nullptr) {
                    ok.store(false, std::memory_order_relaxed);
                    break;
                }
                p->payload = (static_cast<std::uint64_t>(t) << 32u) ^ static_cast<std::uint64_t>(i);
                pool.Put(p);
            }
        });
    }

    start.release_when_all_ready(thread_count);
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_TRUE(ok.load(std::memory_order_relaxed));

    const std::size_t created_total = created.load(std::memory_order_relaxed);
    EXPECT_EQ(pool.Size(), created_total);

    pool.Clear();
    EXPECT_EQ(Item::destroyed.load(std::memory_order_relaxed), created_total);
}

TEST(ObjectPoolTest, ShardDistribution) {
    constexpr std::size_t kShards = 16;
    lscq::ObjectPool<int> pool([] { return new int(0); }, kShards);

    constexpr std::size_t kThreads = 32;
    std::vector<std::size_t> thread_shards(kThreads, 0);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    SpinStart start;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            start.arrive_and_wait();
            const std::size_t idx = pool.CurrentShardIndex();
            thread_shards[i] = idx;
            pool.Put(new int(static_cast<int>(i)));
        });
    }

    start.release_when_all_ready(kThreads);
    for (auto& th : threads) {
        th.join();
    }

    std::vector<std::size_t> expected(kShards, 0);
    std::unordered_set<std::size_t> unique;
    unique.reserve(kThreads);
    for (std::size_t idx : thread_shards) {
        ASSERT_LT(idx, kShards);
        expected[idx] += 1;
        unique.insert(idx);
    }

    EXPECT_GT(unique.size(), 1u);

    const auto actual = shard_sizes(pool);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i], expected[i]);
    }

    EXPECT_EQ(pool.Size(), kThreads);
    pool.Clear();
}

TEST(ObjectPoolTest, WorkStealing) {
    std::atomic<std::size_t> factory_calls{0};
    lscq::ObjectPool<int> pool(
        [&] {
            factory_calls.fetch_add(1, std::memory_order_relaxed);
            return new int(777);
        },
        4);

    const std::size_t local = pool.CurrentShardIndex();
    ASSERT_GE(pool.shards_.size(), 2u);
    const std::size_t donor = (local + 1) % pool.shards_.size();

    // Seed another shard so Get() must steal (local shard is empty).
    int* expected = new int(42);
    {
        auto& shard = pool.shards_[donor];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.objects.emplace_back(expected);
        shard.approx_size.fetch_add(1, std::memory_order_relaxed);
    }

    EXPECT_EQ(shard_size(pool, donor), 1u);
    EXPECT_EQ(pool.Size(), 1u);

    int* stolen = pool.Get();
    ASSERT_NE(stolen, nullptr);
    EXPECT_EQ(stolen, expected);
    EXPECT_EQ(factory_calls.load(std::memory_order_relaxed), 0u);

    pool.Put(stolen);

    EXPECT_EQ(shard_size(pool, donor), 0u);
    EXPECT_EQ(pool.Size(), 1u);

    // Reset to an empty pool for the "locked donor" scenario below.
    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);

    // If the donor shard is locked, stealing should fail and we should fall back to factory.
    int* expected2 = new int(99);
    {
        auto& shard = pool.shards_[donor];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.objects.emplace_back(expected2);
        shard.approx_size.fetch_add(1, std::memory_order_relaxed);
    }

    auto& donor_shard = pool.shards_[donor];
    std::unique_lock<std::mutex> hold(donor_shard.mutex);
    EXPECT_EQ(donor_shard.objects.size(), 1u);

    int* created = pool.Get();
    ASSERT_NE(created, nullptr);
    EXPECT_NE(created, expected2);
    EXPECT_EQ(factory_calls.load(std::memory_order_relaxed), 1u);
    pool.Put(created);

    hold.unlock();

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolTest, ShardCountZeroTreatedAsOne) {
    lscq::ObjectPool<int> pool([] { return new int(0); }, 0);
    ASSERT_EQ(pool.shards_.size(), 1u);
    EXPECT_EQ(pool.CurrentShardIndex(), 0u);

    int* p = pool.Get();
    ASSERT_NE(p, nullptr);
    *p = 123;
    pool.Put(p);
    EXPECT_EQ(pool.Size(), 1u);
    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolTest, FactoryMayReturnNull) {
    std::atomic<int> calls{0};
    lscq::ObjectPool<int> pool(
        [&] {
            const int c = calls.fetch_add(1, std::memory_order_relaxed);
            if ((c % 2) == 0) {
                return static_cast<int*>(nullptr);
            }
            return new int(c);
        },
        1);

    EXPECT_EQ(pool.Get(), nullptr);

    int* p = pool.Get();
    ASSERT_NE(p, nullptr);
    pool.Put(p);
    EXPECT_EQ(pool.Size(), 1u);

    pool.Put(nullptr);
    EXPECT_EQ(pool.Size(), 1u);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolTest, ConcurrentClearWithGetPut) {
    Item::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    lscq::ObjectPool<Item> pool([&] {
        const std::size_t id = created.fetch_add(1, std::memory_order_relaxed);
        Item* p = new Item();
        p->id = static_cast<std::uint64_t>(id);
        return p;
    });

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t iters = 5000;
#else
    const std::size_t hc = std::max<std::size_t>(2u, std::thread::hardware_concurrency());
    const std::size_t thread_count = std::min<std::size_t>(16u, hc);
    constexpr std::size_t iters = 20'000;
#endif

    SpinStart start;
    std::atomic<bool> stop_clear{false};
    std::atomic<bool> ok{true};

    std::thread clearer([&] {
        start.arrive_and_wait();
        while (!stop_clear.load(std::memory_order_relaxed)) {
            pool.Clear();
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < iters; ++i) {
                Item* p = pool.Get();
                if (p == nullptr) {
                    ok.store(false, std::memory_order_relaxed);
                    break;
                }
                p->payload = (static_cast<std::uint64_t>(t) << 32u) ^ static_cast<std::uint64_t>(i);
                pool.Put(p);
            }
        });
    }

    start.release_when_all_ready(thread_count + 1);
    for (auto& th : threads) {
        th.join();
    }
    stop_clear.store(true, std::memory_order_relaxed);
    clearer.join();

    EXPECT_TRUE(ok.load(std::memory_order_relaxed));

    pool.Clear();
    const std::size_t created_total = created.load(std::memory_order_relaxed);
    EXPECT_EQ(Item::destroyed.load(std::memory_order_relaxed), created_total);
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolTest, StressManyThreadsMixedOwnership) {
    Item::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    lscq::ObjectPool<Item> pool([&] {
        const std::size_t id = created.fetch_add(1, std::memory_order_relaxed);
        Item* p = new Item();
        p->id = static_cast<std::uint64_t>(id);
        return p;
    });

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t iters = 10'000;
    constexpr std::size_t hold_max = 16;
#else
    const std::size_t hc = std::max<std::size_t>(2u, std::thread::hardware_concurrency());
    const std::size_t thread_count = std::min<std::size_t>(32u, hc);
    constexpr std::size_t iters = 50'000;
    constexpr std::size_t hold_max = 64;
#endif

    SpinStart start;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    std::atomic<bool> ok{true};

    for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            std::vector<Item*> held;
            held.reserve(hold_max);

            for (std::size_t i = 0; i < iters; ++i) {
                // Occasionally hold objects instead of immediately returning them, to simulate
                // bursty ownership patterns.
                if ((i % 7u) == 0u && held.size() < hold_max) {
                    Item* p = pool.Get();
                    if (p == nullptr) {
                        ok.store(false, std::memory_order_relaxed);
                        break;
                    }
                    p->payload =
                        (static_cast<std::uint64_t>(t) << 32u) ^ static_cast<std::uint64_t>(i);
                    held.push_back(p);
                    continue;
                }

                if ((i % 5u) == 0u && !held.empty()) {
                    Item* p = held.back();
                    held.pop_back();
                    pool.Put(p);
                    continue;
                }

                Item* p = pool.Get();
                if (p == nullptr) {
                    ok.store(false, std::memory_order_relaxed);
                    break;
                }
                p->payload = (static_cast<std::uint64_t>(t) << 32u) ^ static_cast<std::uint64_t>(i);
                pool.Put(p);
            }

            for (Item* p : held) {
                pool.Put(p);
            }
        });
    }

    start.release_when_all_ready(thread_count);
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_TRUE(ok.load(std::memory_order_relaxed));

    const std::size_t created_total = created.load(std::memory_order_relaxed);
    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(Item::destroyed.load(std::memory_order_relaxed), created_total);
}
