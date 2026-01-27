#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#define private public
#include <lscq/object_pool_tls.hpp>
#undef private

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

struct Tracked {
    inline static std::atomic<std::size_t> destroyed{0};
    std::uint64_t payload = 0;
    ~Tracked() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};

}  // namespace

TEST(ObjectPoolTLSTest, SingleThreadFastPathHitAndMiss) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolTLS<Tracked> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new Tracked();
        },
        1);

    auto& cache = lscq::ObjectPoolTLS<Tracked>::tls_fast_cache_;
    EXPECT_EQ(cache.owner.load(std::memory_order_acquire), nullptr);
    EXPECT_EQ(cache.private_obj.load(std::memory_order_relaxed), nullptr);

    Tracked* p0 = pool.Get();  // miss => factory
    ASSERT_NE(p0, nullptr);
    EXPECT_EQ(created.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(cache.owner.load(std::memory_order_acquire), &pool);
    EXPECT_EQ(cache.private_obj.load(std::memory_order_relaxed), nullptr);

    pool.Put(p0);  // hit => stored in TLS slot
    EXPECT_EQ(cache.private_obj.load(std::memory_order_relaxed), p0);
    EXPECT_EQ(pool.SizeApprox(), 0u);  // nothing in shared shards

    Tracked* p1 = pool.Get();  // hit => TLS
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(p1, p0);
    EXPECT_EQ(cache.private_obj.load(std::memory_order_relaxed), nullptr);

    Tracked* p2 = pool.Get();  // miss => factory again
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p2, p0);
    EXPECT_EQ(created.load(std::memory_order_relaxed), 2u);

    pool.Put(p1);  // TLS empty => goes to TLS
    EXPECT_EQ(cache.private_obj.load(std::memory_order_relaxed), p0);
    EXPECT_EQ(pool.SizeApprox(), 0u);

    pool.Put(p2);  // TLS full => shared fallback
    EXPECT_EQ(cache.private_obj.load(std::memory_order_relaxed), p0);
    EXPECT_EQ(pool.SizeApprox(), 1u);

    // Clear deletes both TLS and shared.
    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));
    EXPECT_EQ(cache.owner.load(std::memory_order_acquire), &pool);
    EXPECT_EQ(cache.private_obj.load(std::memory_order_relaxed), nullptr);
}

TEST(ObjectPoolTLSTest, ConcurrentGetPutAndClear) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolTLS<Tracked> pool([&] {
        const std::size_t id = created.fetch_add(1, std::memory_order_relaxed);
        Tracked* p = new Tracked();
        p->payload = static_cast<std::uint64_t>(id);
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
                Tracked* p = pool.Get();
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
    EXPECT_EQ(pool.Size(), 0u);
    const std::size_t created_total = created.load(std::memory_order_relaxed);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed), created_total);
}

TEST(ObjectPoolTLSTest, ThreadExitFlushesToSharedAndUnregisters) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolTLS<Tracked> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new Tracked();
        },
        1);

    std::thread th([&] {
        Tracked* p = pool.Get();
        ASSERT_NE(p, nullptr);
        pool.Put(p);  // should end up in that thread's TLS slot
    });
    th.join();  // includes TLS destructors for that thread

    // The worker thread's TLS slot should have been flushed back to shared storage and
    // unregistered.
    EXPECT_EQ(pool.registry_.size(), 0u);
    EXPECT_EQ(pool.SizeApprox(), 1u);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));
}

TEST(ObjectPoolTLSTest, DestructorClearsTlsSlotAndSharedStorage) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    {
        lscq::ObjectPoolTLS<Tracked> pool(
            [&] {
                created.fetch_add(1, std::memory_order_relaxed);
                return new Tracked();
            },
            1);

        Tracked* p0 = pool.Get();
        Tracked* p1 = pool.Get();
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);
        pool.Put(p0);  // TLS
        pool.Put(p1);  // shared fallback
        EXPECT_EQ(pool.Size(), 2u);
    }

    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));

    auto& cache = lscq::ObjectPoolTLS<Tracked>::tls_fast_cache_;
    EXPECT_EQ(cache.owner.load(std::memory_order_acquire), nullptr);
    EXPECT_EQ(cache.private_obj.load(std::memory_order_relaxed), nullptr);
}

TEST(ObjectPoolTLSTest, ClosingBlocksOpsAndDeletesOnPut) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    lscq::ObjectPoolTLS<Tracked> pool([] { return new Tracked(); }, 1);

    pool.closing_.store(true, std::memory_order_release);

    EXPECT_EQ(pool.Get(), nullptr);
    pool.Put(nullptr);

    Tracked* p = new Tracked();
    pool.Put(p);  // should delete when closing
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed), 1u);

    pool.Clear();  // should early-return when closing
}

TEST(ObjectPoolTLSTest, CoverageEdgeCases) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    auto& cache = lscq::ObjectPoolTLS<Tracked>::tls_fast_cache_;
    cache.private_obj.store(nullptr, std::memory_order_relaxed);
    cache.owner.store(nullptr, std::memory_order_release);

    lscq::ObjectPoolTLS<Tracked> pool_a([] { return new Tracked(); }, 1);
    lscq::ObjectPoolTLS<Tracked> pool_b([] { return new Tracked(); }, 1);

    struct CacheReset {
        decltype(cache)& cache_ref;
        ~CacheReset() {
            cache_ref.private_obj.store(nullptr, std::memory_order_relaxed);
            cache_ref.owner.store(nullptr, std::memory_order_release);
        }
    } reset{cache};

    // EnsureRegistered() compare_exchange failure path: owner already non-null (not this).
    cache.owner.store(&pool_a, std::memory_order_release);
    pool_b.EnsureRegistered(cache);

    // Size()/Clear()/CloseAndClear() skip branches for null cache and owner mismatch.
    pool_b.registry_.push_back(nullptr);
    pool_b.registry_.push_back(&cache);  // owner mismatch (owner == pool_a)

    (void)pool_b.Size();
    pool_b.Clear();

    // Force WaitForActiveOpsAtMost() to take the timeout return path.
    pool_b.active_ops_.store(2, std::memory_order_release);
    EXPECT_FALSE(pool_b.WaitForActiveOpsAtMost(0, std::chrono::milliseconds(0)));
    pool_b.active_ops_.store(0, std::memory_order_release);

    // Unregister nullptr and not-found path.
    pool_b.Unregister(nullptr);
    lscq::ObjectPoolTLS<Tracked>::LocalCache fake;
    fake.private_obj.store(new Tracked(), std::memory_order_relaxed);
    pool_b.closing_.store(true, std::memory_order_release);
    pool_b.OnThreadExit(&fake);    // should delete when closing
    pool_b.OnThreadExit(nullptr);  // nullptr early return

    // CloseAndClear() skip branches.
    pool_b.CloseAndClear();
}
