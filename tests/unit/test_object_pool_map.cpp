#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

#define private public
#define protected public
#include <lscq/object_pool_map.hpp>
#undef protected
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

struct Item {
    inline static std::atomic<std::size_t> destroyed{0};
    std::uint64_t payload = 0;
    ~Item() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};

}  // namespace

TEST(ObjectPoolMapTest, RegistersOnFirstPutAndUsesLocalSlot) {
    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolMap<int> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new int(0);
        },
        1);

    EXPECT_EQ(pool.Size(), 0u);

    int* p = pool.Get();
    ASSERT_NE(p, nullptr);
    *p = 123;
    pool.Put(p);

    const std::thread::id tid = std::this_thread::get_id();
    {
        std::shared_lock lock(pool.cache_mutex_);
        auto it = pool.caches_.find(tid);
        ASSERT_NE(it, pool.caches_.end());
        int* cached = it->second.private_obj.load(std::memory_order_acquire);
        ASSERT_NE(cached, nullptr);
        EXPECT_EQ(*cached, 123);
    }

    EXPECT_EQ(pool.SizeApprox(), 0u);  // local slot only
    EXPECT_EQ(pool.Size(), 1u);

    int* q = pool.Get();
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p);  // hit local slot
    pool.Put(q);
    pool.Clear();
}

TEST(ObjectPoolMapTest, SlotFullFallsBackToSharedPool) {
    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolMap<int> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new int(0);
        },
        1);

    int* p0 = pool.Get();
    int* p1 = pool.Get();
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);
    *p0 = 7;
    *p1 = 9;

    pool.Put(p0);  // local slot
    pool.Put(p1);  // shared pool (slot full)

    EXPECT_EQ(pool.SizeApprox(), 1u);
    EXPECT_EQ(pool.Size(), 2u);

    int* q0 = pool.Get();  // local hit
    ASSERT_NE(q0, nullptr);
    EXPECT_EQ(*q0, 7);

    int* q1 = pool.Get();  // shared pop
    ASSERT_NE(q1, nullptr);
    EXPECT_EQ(*q1, 9);

    pool.Put(q0);
    pool.Put(q1);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolMapTest, ClearClearsLocalSlotsAndSharedPool) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);
    std::atomic<std::size_t> created{0};

    lscq::ObjectPoolMap<Tracked> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            auto* p = new Tracked();
            p->payload = created.load(std::memory_order_relaxed);
            return p;
        },
        1);

    Tracked* p0 = pool.Get();
    Tracked* p1 = pool.Get();
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);

    pool.Put(p0);  // local
    pool.Put(p1);  // shared

    EXPECT_EQ(pool.Size(), 2u);
    EXPECT_EQ(pool.SizeApprox(), 1u);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed), 0u);

    pool.Clear();
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(pool.SizeApprox(), 0u);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));

    const std::thread::id tid = std::this_thread::get_id();
    std::shared_lock lock(pool.cache_mutex_);
    auto it = pool.caches_.find(tid);
    ASSERT_NE(it, pool.caches_.end());
    EXPECT_EQ(it->second.private_obj.load(std::memory_order_acquire), nullptr);
}

TEST(ObjectPoolMapTest, DestructorReclaimsCachedAndSharedObjects) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);
    std::atomic<std::size_t> created{0};

    {
        lscq::ObjectPoolMap<Tracked> pool(
            [&] {
                created.fetch_add(1, std::memory_order_relaxed);
                return new Tracked();
            },
            1);

        Tracked* p0 = pool.Get();
        Tracked* p1 = pool.Get();
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);
        pool.Put(p0);  // local
        pool.Put(p1);  // shared
        EXPECT_EQ(pool.Size(), 2u);
    }

    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));
}

TEST(ObjectPoolMapTest, ConcurrentRegistrationAndSteadyStateAccess) {
    Item::destroyed.store(0u, std::memory_order_relaxed);
    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolMap<Item> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new Item();
        },
        8);

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t iters = 5000;
#else
    const std::size_t hc = std::max<std::size_t>(2u, std::thread::hardware_concurrency());
    const std::size_t thread_count = std::min<std::size_t>(16u, hc);
    constexpr std::size_t iters = 20'000;
#endif

    SpinStart start;
    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < iters; ++i) {
                Item* p = pool.Get();
                if (!p) {
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
    EXPECT_EQ(created.load(std::memory_order_relaxed), thread_count);

    {
        std::shared_lock lock(pool.cache_mutex_);
        EXPECT_EQ(pool.caches_.size(), thread_count);
    }

    pool.Clear();
    EXPECT_EQ(Item::destroyed.load(std::memory_order_relaxed),
              created.load(std::memory_order_relaxed));
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(ObjectPoolMapTest, LockUpgradeForRegistrationRequiresUniqueLock) {
    lscq::ObjectPoolMap<int> pool([] { return new int(0); }, 1);

    std::atomic<bool> reader_locked{false};
    std::atomic<bool> reader_release{false};
    std::atomic<bool> put_started{false};
    std::atomic<bool> put_finished{false};
    std::thread::id put_tid{};

    std::thread reader([&] {
        std::shared_lock lock(pool.cache_mutex_);
        reader_locked.store(true, std::memory_order_release);
        while (!reader_release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    while (!reader_locked.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread writer([&] {
        put_tid = std::this_thread::get_id();
        put_started.store(true, std::memory_order_release);
        pool.Put(new int(1));  // forces registration path
        put_finished.store(true, std::memory_order_release);
    });

    while (!put_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Give writer thread time to attempt lock acquisition
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Release reader to allow writer to proceed
    reader_release.store(true, std::memory_order_release);
    reader.join();
    writer.join();

    EXPECT_TRUE(put_finished.load(std::memory_order_acquire));

    {
        std::shared_lock lock(pool.cache_mutex_);
        auto it = pool.caches_.find(put_tid);
        ASSERT_NE(it, pool.caches_.end());
        EXPECT_NE(it->second.private_obj.load(std::memory_order_acquire), nullptr);
    }

    pool.Clear();
}

TEST(ObjectPoolMapTest, GetDoesNotRegisterThread) {
    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolMap<int> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new int(0);
        },
        1);

    std::atomic<bool> reader_locked{false};
    std::atomic<bool> reader_release{false};

    std::atomic<bool> get_done{false};
    std::thread::id get_tid{};
    int* got = nullptr;

    std::thread reader([&] {
        std::shared_lock lock(pool.cache_mutex_);
        reader_locked.store(true, std::memory_order_release);
        while (!reader_release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    while (!reader_locked.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread getter([&] {
        get_tid = std::this_thread::get_id();
        got = pool.Get();
        get_done.store(true, std::memory_order_release);
    });

    while (!get_done.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    reader_release.store(true, std::memory_order_release);
    reader.join();
    getter.join();

    ASSERT_NE(got, nullptr);
    delete got;

    {
        std::shared_lock lock(pool.cache_mutex_);
        EXPECT_EQ(pool.caches_.find(get_tid), pool.caches_.end());
    }

    EXPECT_EQ(created.load(std::memory_order_relaxed), 1u);
    pool.Clear();
}
