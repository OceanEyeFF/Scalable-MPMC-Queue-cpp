#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <lscq/object_pool.hpp>
#include <lscq/object_pool_map.hpp>
#include <lscq/object_pool_tls.hpp>

namespace lscq::test {

struct SuiteSpinStart {
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

struct TestObject {
    inline static std::atomic<std::size_t> destroyed{0};
    std::uint64_t payload = 0;
    ~TestObject() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};

template <typename PoolType>
class ObjectPoolSuite : public ::testing::Test {};

using PoolTypes = ::testing::Types<lscq::ObjectPool<TestObject>,
                                   lscq::ObjectPoolTLS<TestObject>,
                                   lscq::ObjectPoolMap<TestObject>>;

TYPED_TEST_SUITE(ObjectPoolSuite, PoolTypes);

TYPED_TEST(ObjectPoolSuite, SingleThread_GetPut) {
    using PoolType = TypeParam;
    TestObject::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    PoolType pool([&] {
        created.fetch_add(1, std::memory_order_relaxed);
        return new TestObject();
    }, 1);

    EXPECT_EQ(pool.Size(), 0u);

    TestObject* p = pool.Get();
    ASSERT_NE(p, nullptr);
    p->payload = 7;
    pool.Put(p);

    EXPECT_EQ(pool.Size(), 1u);

    TestObject* q = pool.Get();
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p);
    EXPECT_EQ(pool.Size(), 0u);
    pool.Put(q);

    EXPECT_EQ(pool.Size(), 1u);
    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(TestObject::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));
}

TYPED_TEST(ObjectPoolSuite, MultiThread_ConcurrentGetPut) {
    using PoolType = TypeParam;
    TestObject::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    PoolType pool([&] {
        created.fetch_add(1, std::memory_order_relaxed);
        return new TestObject();
    }, 4);

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t iters = 2000;
#else
    const std::size_t hc = std::max<std::size_t>(2u, std::thread::hardware_concurrency());
    const std::size_t thread_count = std::min<std::size_t>(16u, hc);
    constexpr std::size_t iters = 8000;
#endif

    SuiteSpinStart start;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    std::atomic<bool> ok{true};

    for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < iters; ++i) {
                TestObject* p = pool.Get();
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
    EXPECT_GT(created_total, 0u);
    EXPECT_EQ(pool.Size(), created_total);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(TestObject::destroyed.load(std::memory_order_relaxed), created_total);
}

TYPED_TEST(ObjectPoolSuite, EmptyPool_Get) {
    using PoolType = TypeParam;
    PoolType pool(typename PoolType::Factory{}, 1);
    EXPECT_EQ(pool.Get(), nullptr);
    EXPECT_EQ(pool.Size(), 0u);
}

TYPED_TEST(ObjectPoolSuite, NullPut_Ignored) {
    using PoolType = TypeParam;
    PoolType pool([] { return new TestObject(); }, 1);
    EXPECT_EQ(pool.Size(), 0u);
    pool.Put(nullptr);
    EXPECT_EQ(pool.Size(), 0u);
    pool.Clear();
}

TYPED_TEST(ObjectPoolSuite, DestructorCleanup) {
    using PoolType = TypeParam;
    TestObject::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    {
        PoolType pool([&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new TestObject();
        }, 1);

        TestObject* p0 = pool.Get();
        TestObject* p1 = pool.Get();
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);
        pool.Put(p0);
        pool.Put(p1);
        EXPECT_EQ(pool.Size(), 2u);
    }

    EXPECT_EQ(TestObject::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));
}

TYPED_TEST(ObjectPoolSuite, ConcurrentClear) {
    using PoolType = TypeParam;
    TestObject::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    PoolType pool([&] {
        created.fetch_add(1, std::memory_order_relaxed);
        return new TestObject();
    });

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t iters = 3000;
#else
    const std::size_t hc = std::max<std::size_t>(2u, std::thread::hardware_concurrency());
    const std::size_t thread_count = std::min<std::size_t>(16u, hc);
    constexpr std::size_t iters = 10'000;
#endif

    SuiteSpinStart start;
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
                TestObject* p = pool.Get();
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
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(TestObject::destroyed.load(std::memory_order_relaxed), created_total);
}

}  // namespace lscq::test
