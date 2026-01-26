#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <vector>

#define private public
#include <lscq/object_pool_tls_v2.hpp>
#undef private
#include <lscq/detail/numa_utils.hpp>

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

template <typename Cache>
void ResetCache(Cache& cache) {
    cache.owner.store(nullptr, std::memory_order_release);
    if (auto* p = cache.fast_slot.exchange(nullptr, std::memory_order_acq_rel)) {
        delete p;
    }
    for (auto& slot : cache.batch) {
        if (auto* p = slot.exchange(nullptr, std::memory_order_acq_rel)) {
            delete p;
        }
    }
    cache.batch_count.store(0, std::memory_order_release);
    cache.effective_batch_size.store(
        std::max<std::size_t>(1u, cache.batch.size() / 2), std::memory_order_release);
    cache.hit_count.store(0, std::memory_order_release);
    cache.miss_count.store(0, std::memory_order_release);
    cache.op_count.store(0, std::memory_order_release);
}

}  // namespace

TEST(ObjectPoolTLSv2Test, SingleThreadFastSlotBatchAndShared) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    using Pool = lscq::ObjectPoolTLSv2<Tracked, 3>;
    auto& cache = Pool::tls_cache_.Get();
    ResetCache(cache);

    std::atomic<std::size_t> created{0};
    Pool pool([&] {
        created.fetch_add(1, std::memory_order_relaxed);
        return new Tracked();
    },
              1);

    EXPECT_EQ(cache.owner.load(std::memory_order_acquire), nullptr);
    EXPECT_EQ(cache.fast_slot.load(std::memory_order_relaxed), nullptr);
    EXPECT_EQ(cache.batch_count.load(std::memory_order_relaxed), 0u);

    std::vector<Tracked*> objs;
    objs.reserve(5);
    for (int i = 0; i < 5; ++i) {
        objs.push_back(pool.Get());
    }
    EXPECT_EQ(created.load(std::memory_order_relaxed), 5u);

    pool.Put(objs[0]);
    EXPECT_EQ(cache.fast_slot.load(std::memory_order_relaxed), objs[0]);

    pool.Put(objs[1]);
    pool.Put(objs[2]);
    pool.Put(objs[3]);
    EXPECT_EQ(cache.batch_count.load(std::memory_order_relaxed), 3u);
    EXPECT_EQ(pool.SizeApprox(), 0u);

    pool.Put(objs[4]);
    EXPECT_EQ(cache.batch_count.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(pool.SizeApprox(), 4u);

    Tracked* p_fast = pool.Get();
    ASSERT_NE(p_fast, nullptr);
    EXPECT_EQ(p_fast, objs[0]);
    EXPECT_EQ(cache.fast_slot.load(std::memory_order_relaxed), nullptr);

    Tracked* p_batch = pool.Get();
    ASSERT_NE(p_batch, nullptr);
    EXPECT_EQ(cache.batch_count.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(pool.SizeApprox(), 1u);

    pool.Put(p_fast);
    pool.Put(p_batch);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));

    ResetCache(cache);
}

TEST(ObjectPoolTLSv2Test, GetHitsBatchCache) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    using Pool = lscq::ObjectPoolTLSv2<Tracked, 2>;
    auto& cache = Pool::tls_cache_.Get();
    ResetCache(cache);

    Pool pool([] { return new Tracked(); }, 1);

    cache.owner.store(&pool, std::memory_order_release);
    cache.fast_slot.store(nullptr, std::memory_order_release);
    cache.batch[0].store(new Tracked(), std::memory_order_release);
    cache.batch_count.store(1u, std::memory_order_release);

    Tracked* from_batch = pool.Get();
    ASSERT_NE(from_batch, nullptr);
    pool.Put(from_batch);

    pool.Clear();
    ResetCache(cache);
}

TEST(ObjectPoolTLSv2Test, PutFallsBackToSharedWhenOwnerMismatch) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    using Pool = lscq::ObjectPoolTLSv2<Tracked, 2>;
    auto& cache = Pool::tls_cache_.Get();
    ResetCache(cache);

    Pool pool([] { return new Tracked(); }, 1);

    cache.owner.store(reinterpret_cast<Pool*>(0x1), std::memory_order_release);
    pool.Put(new Tracked());
    cache.owner.store(nullptr, std::memory_order_release);

    EXPECT_GT(pool.SizeApprox(), 0u);
    pool.Clear();
    ResetCache(cache);
}

TEST(ObjectPoolTLSv2Test, Coverage_ManualCacheEdges) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    using Pool = lscq::ObjectPoolTLSv2<Tracked, 2>;
    auto& cache = Pool::tls_cache_.Get();
    ResetCache(cache);

    Pool pool([] { return new Tracked(); }, 1);
    cache.owner.store(&pool, std::memory_order_release);

    cache.effective_batch_size.store(0u, std::memory_order_release);
    EXPECT_EQ(pool.EffectiveBatchSize(cache), Pool::kMinBatchSize);
    cache.effective_batch_size.store(Pool::kMaxBatchSize + 1, std::memory_order_release);
    EXPECT_EQ(pool.EffectiveBatchSize(cache), Pool::kMaxBatchSize);

    for (auto& slot : cache.batch) {
        slot.store(nullptr, std::memory_order_relaxed);
    }
    cache.batch[0].store(new Tracked(), std::memory_order_relaxed);
    cache.batch_count.store(cache.batch.size(), std::memory_order_release);
    Tracked* popped = pool.TryPopBatch(cache);
    ASSERT_NE(popped, nullptr);
    delete popped;

    cache.batch[0].store(new Tracked(), std::memory_order_relaxed);
    cache.batch[1].store(new Tracked(), std::memory_order_relaxed);
    cache.batch_count.store(0u, std::memory_order_release);
    cache.effective_batch_size.store(2u, std::memory_order_release);
    Tracked* extra = new Tracked();
    EXPECT_FALSE(pool.TryPushBatch(cache, extra));
    delete extra;
    for (auto& slot : cache.batch) {
        if (auto* p = slot.exchange(nullptr, std::memory_order_acq_rel)) {
            delete p;
        }
    }
    cache.batch_count.store(0u, std::memory_order_release);

    EXPECT_EQ(pool.DrainBatch(cache, nullptr, 0u), 0u);

    pool.Clear();
    ResetCache(cache);
}

TEST(ObjectPoolTLSv2Test, BatchSizeTemplateInstantiation) {
    lscq::ObjectPoolTLSv2<int, 1> pool([] { return new int(7); }, 1);
    int* p = pool.Get();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 7);
    pool.Put(p);
    pool.Clear();
}

TEST(ObjectPoolTLSv2Test, SizeIncludesFastSlotBatchAndShared) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    using Pool = lscq::ObjectPoolTLSv2<Tracked, 2>;
    auto& cache = Pool::tls_cache_.Get();
    ResetCache(cache);

    Pool pool([] { return new Tracked(); }, 1);

    Tracked* a = pool.Get();
    Tracked* b = pool.Get();
    Tracked* c = pool.Get();
    Tracked* d = pool.Get();

    pool.Put(a);  // fast slot
    pool.Put(b);  // batch[0]
    pool.Put(c);  // batch[1]
    pool.Put(d);  // flush to shared

    EXPECT_EQ(pool.SizeApprox(), 3u);

    Tracked* p_fast = pool.Get();
    Tracked* p_shared = pool.Get();
    ASSERT_NE(p_fast, nullptr);
    ASSERT_NE(p_shared, nullptr);

    pool.Put(p_fast);  // refill fast slot

    EXPECT_EQ(pool.Size(), 3u);  // fast slot + batch(1) + shared(1)

    pool.Put(p_shared);
    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);

    ResetCache(cache);
}

TEST(ObjectPoolTLSv2Test, ThreadExitFlushesBatchAndUnregisters) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    using Pool = lscq::ObjectPoolTLSv2<Tracked, 2>;
    Pool pool([] { return new Tracked(); }, 1);

    std::thread th([&] {
        Tracked* a = pool.Get();
        Tracked* b = pool.Get();
        Tracked* c = pool.Get();
        pool.Put(a);
        pool.Put(b);
        pool.Put(c);
    });
    th.join();

    EXPECT_EQ(pool.registry_.size(), 0u);
    EXPECT_EQ(pool.SizeApprox(), 3u);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolTLSv2Test, ConcurrentGetPutAndClear) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolTLSv2<Tracked, 4> pool([&] {
        const std::size_t id = created.fetch_add(1, std::memory_order_relaxed);
        auto* p = new Tracked();
        p->payload = static_cast<std::uint64_t>(id);
        return p;
    });

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t iters = 4000;
#else
    const std::size_t hc = std::max<std::size_t>(2u, std::thread::hardware_concurrency());
    const std::size_t thread_count = std::min<std::size_t>(16u, hc);
    constexpr std::size_t iters = 15'000;
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
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));
}

TEST(ObjectPoolTLSv2Test, ClosingBlocksOpsAndDeletesOnPut) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    lscq::ObjectPoolTLSv2<Tracked, 2> pool([] { return new Tracked(); }, 1);

    pool.closing_.store(true, std::memory_order_release);

    EXPECT_EQ(pool.Get(), nullptr);
    pool.Put(nullptr);

    Tracked* p = new Tracked();
    pool.Put(p);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed), 1u);

    pool.Clear();
}

TEST(ObjectPoolTLSv2Test, AdaptiveBatchSizeAdjustsWithinBounds) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    using Pool = lscq::ObjectPoolTLSv2<Tracked, 4>;
    auto& cache = Pool::tls_cache_.Get();
    ResetCache(cache);

    Pool pool([] { return new Tracked(); }, 1);

    const std::size_t max_size = Pool::kMaxBatchSize;
    const std::size_t min_size = Pool::kMinBatchSize;
    const std::size_t interval = Pool::kAdaptiveCheckInterval;

    EXPECT_EQ(cache.effective_batch_size.load(std::memory_order_relaxed), 4u);

    const std::size_t grow_steps = (max_size > 4u) ? (max_size - 4u) : 0u;
    for (std::size_t step = 0; step < grow_steps; ++step) {
        for (std::size_t i = 0; i < interval; ++i) {
            pool.RecordCacheHit(cache);
        }
    }
    EXPECT_EQ(cache.effective_batch_size.load(std::memory_order_relaxed), max_size);

    const std::size_t shrink_steps = (max_size > min_size) ? (max_size - min_size) : 0u;
    for (std::size_t step = 0; step < shrink_steps; ++step) {
        for (std::size_t i = 0; i < interval; ++i) {
            pool.RecordCacheMiss(cache);
        }
    }
    EXPECT_EQ(cache.effective_batch_size.load(std::memory_order_relaxed), min_size);

    pool.Clear();
    ResetCache(cache);
}

TEST(ObjectPoolTLSv2Test, NumaAllocationFallback) {
    struct NumaPayload {
        int value = 0;
        explicit NumaPayload(int v) : value(v) {}
    };

    auto allocation = lscq::detail::numa::Allocate(sizeof(NumaPayload));
    ASSERT_NE(allocation.ptr, nullptr);
#if !LSCQ_HAS_NUMA
    EXPECT_FALSE(allocation.on_numa);
#endif
    auto* value = new (allocation.ptr) NumaPayload(42);
    EXPECT_EQ(value->value, 42);
    value->~NumaPayload();
    lscq::detail::numa::Free(allocation, sizeof(NumaPayload));
}

TEST(ObjectPoolTLSv2Test, CoverageEdgeCases) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    using Pool = lscq::ObjectPoolTLSv2<Tracked, 2>;
    auto& cache = Pool::tls_cache_.Get();
    ResetCache(cache);

    Pool pool_a([] { return new Tracked(); }, 1);
    Pool pool_b([] { return new Tracked(); }, 1);

    struct CacheReset {
        decltype(cache)& cache_ref;
        ~CacheReset() { ResetCache(cache_ref); }
    } reset{cache};

    cache.owner.store(&pool_a, std::memory_order_release);
    pool_b.EnsureRegistered(cache);

    pool_b.registry_.push_back(nullptr);
    pool_b.registry_.push_back(&cache);

    (void)pool_b.Size();
    pool_b.Clear();

    pool_b.active_ops_.store(2, std::memory_order_release);
    EXPECT_FALSE(pool_b.WaitForActiveOpsAtMost(0, std::chrono::milliseconds(0)));
    pool_b.active_ops_.store(0, std::memory_order_release);

    pool_b.Unregister(nullptr);

    cache.batch_count.store(1, std::memory_order_release);
    (void)pool_b.TryPopBatch(cache);

    Tracked* extra = new Tracked();
    cache.effective_batch_size.store(cache.batch.size(), std::memory_order_release);
    cache.batch_count.store(cache.batch.size() - 1, std::memory_order_release);
    cache.batch[cache.batch.size() - 1].store(new Tracked(), std::memory_order_relaxed);
    EXPECT_TRUE(pool_b.TryPushBatch(cache, extra));
    if (Tracked* popped = pool_b.TryPopBatch(cache)) {
        delete popped;
    }

    typename Pool::LocalCache fake;
    fake.fast_slot.store(new Tracked(), std::memory_order_relaxed);
    fake.batch[0].store(new Tracked(), std::memory_order_relaxed);
    fake.batch_count.store(1, std::memory_order_relaxed);
    pool_b.closing_.store(true, std::memory_order_release);
    pool_b.OnThreadExit(&fake);
    pool_b.OnThreadExit(nullptr);

    pool_b.CloseAndClear();
}
