#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#define private public
#define protected public
#include <lscq/detail/object_pool_core.hpp>
#undef protected
#undef private

namespace {

struct Tracked {
    inline static std::atomic<std::size_t> destroyed{0};
    std::uint64_t payload = 0;
    ~Tracked() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};

}  // namespace

TEST(ObjectPoolCoreTest, ShardCountZeroTreatedAsOne) {
    lscq::detail::ObjectPoolCore<int> core([] { return new int(0); }, 0);
    EXPECT_EQ(core.ShardCount(), 1u);
    EXPECT_EQ(core.CurrentShardIndex(), 0u);
}

TEST(ObjectPoolCoreTest, GetPutClearAndSizeApprox) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    lscq::detail::ObjectPoolCore<Tracked> core([] { return new Tracked(); }, 2);
    EXPECT_EQ(core.SizeApprox(), 0u);

    constexpr std::size_t kCount = 8;
    std::vector<Tracked*> held;
    held.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        Tracked* p = core.GetShared();
        ASSERT_NE(p, nullptr);
        p->payload = i;
        held.push_back(p);
    }

    for (Tracked* p : held) {
        core.PutShared(p);
    }
    EXPECT_EQ(core.SizeApprox(), kCount);

    core.ClearShared();
    EXPECT_EQ(core.SizeApprox(), 0u);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed), kCount);
}

TEST(ObjectPoolCoreTest, WorkStealing) {
    std::atomic<std::size_t> factory_calls{0};
    lscq::detail::ObjectPoolCore<int> core(
        [&] {
            factory_calls.fetch_add(1, std::memory_order_relaxed);
            return new int(777);
        },
        4);

    const std::size_t local = core.CurrentShardIndex();
    ASSERT_GE(core.shards_.size(), 2u);
    const std::size_t donor = (local + 1) % core.shards_.size();

    int* expected = new int(42);
    {
        auto& shard = core.shards_[donor];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.objects.emplace_back(expected);
        shard.approx_size.fetch_add(1, std::memory_order_relaxed);
    }

    EXPECT_EQ(core.SizeApprox(), 1u);

    int* stolen = core.GetShared();
    ASSERT_NE(stolen, nullptr);
    EXPECT_EQ(stolen, expected);
    EXPECT_EQ(factory_calls.load(std::memory_order_relaxed), 0u);

    core.PutShared(stolen);

    EXPECT_EQ(core.SizeApprox(), 1u);
    core.ClearShared();
    EXPECT_EQ(core.SizeApprox(), 0u);
}

TEST(ObjectPoolCoreTest, ShardBatchHelpers) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    lscq::detail::ObjectPoolShard<Tracked> shard;
    Tracked* items[3] = {new Tracked(), new Tracked(), nullptr};
    EXPECT_EQ(shard.PutBatch(items, 0), 0u);
    EXPECT_EQ(shard.PutBatch(items, 3), 2u);
    EXPECT_EQ(shard.approx_size.load(std::memory_order_relaxed), 2u);

    Tracked* out[3] = {};
    EXPECT_EQ(shard.GetBatch(out, 0), 0u);
    EXPECT_EQ(shard.GetBatch(out, 1), 1u);
    EXPECT_NE(out[0], nullptr);
    EXPECT_EQ(shard.approx_size.load(std::memory_order_relaxed), 1u);

    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        Tracked* blocked_out[2] = {};
        EXPECT_EQ(shard.TryStealBatch(blocked_out, 2), 0u);
    }

    Tracked* steal_out[2] = {};
    EXPECT_EQ(shard.TryStealBatch(steal_out, 2), 1u);
    EXPECT_EQ(shard.approx_size.load(std::memory_order_relaxed), 0u);

    delete out[0];
    delete steal_out[0];

    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed), 2u);
}

TEST(ObjectPoolCoreTest, GetPutSharedBatchBasic) {
    Tracked::destroyed.store(0u, std::memory_order_relaxed);

    lscq::detail::ObjectPoolCore<Tracked> core([] { return new Tracked(); }, 2);

    Tracked* items[5] = {new Tracked(), new Tracked(), nullptr, new Tracked(), new Tracked()};
    core.PutSharedBatch(items, 0);
    core.PutSharedBatch(items, 5);
    EXPECT_EQ(core.SizeApprox(), 4u);

    Tracked* out[6] = {};
    EXPECT_EQ(core.GetSharedBatch(out, 0), 0u);
    std::size_t got = core.GetSharedBatch(out, 6);
    EXPECT_EQ(got, 4u);
    EXPECT_EQ(core.SizeApprox(), 0u);

    core.PutSharedBatch(out, got);
    EXPECT_EQ(core.SizeApprox(), 4u);

    core.ClearShared();
    EXPECT_EQ(core.SizeApprox(), 0u);
    EXPECT_EQ(Tracked::destroyed.load(std::memory_order_relaxed), 4u);
}

TEST(ObjectPoolCoreTest, GetSharedBatchWorkStealing) {
    std::atomic<std::size_t> factory_calls{0};
    lscq::detail::ObjectPoolCore<int> core(
        [&] {
            factory_calls.fetch_add(1, std::memory_order_relaxed);
            return new int(123);
        },
        4);

    const std::size_t local = core.CurrentShardIndex();
    const std::size_t donor = (local + 1) % core.shards_.size();

    int* a = new int(1);
    int* b = new int(2);
    int* c = new int(3);
    {
        auto& shard = core.shards_[donor];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.objects.emplace_back(a);
        shard.objects.emplace_back(b);
        shard.objects.emplace_back(c);
        shard.approx_size.fetch_add(3, std::memory_order_relaxed);
    }

    int* out[4] = {};
    std::size_t got = core.GetSharedBatch(out, 4);
    EXPECT_EQ(got, 3u);
    EXPECT_EQ(factory_calls.load(std::memory_order_relaxed), 0u);

    std::unordered_set<int*> expected{a, b, c};
    for (std::size_t i = 0; i < got; ++i) {
        EXPECT_EQ(expected.erase(out[i]), 1u);
    }
    EXPECT_TRUE(expected.empty());

    core.PutSharedBatch(out, got);
    EXPECT_EQ(core.SizeApprox(), 3u);
    core.ClearShared();
    EXPECT_EQ(core.SizeApprox(), 0u);
}

TEST(ObjectPoolCoreTest, BatchThreadSafety) {
    lscq::detail::ObjectPoolCore<int> core([] { return new int(0); }, 4);

    constexpr std::size_t kSeedCount = 64;
    std::vector<int*> seed(kSeedCount);
    for (std::size_t i = 0; i < kSeedCount; ++i) {
        seed[i] = new int(static_cast<int>(i));
    }
    core.PutSharedBatch(seed.data(), seed.size());
    EXPECT_EQ(core.SizeApprox(), kSeedCount);

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            int* local[8] = {};
            for (int iter = 0; iter < 200; ++iter) {
                std::size_t got = core.GetSharedBatch(local, 8);
                if (got > 0) {
                    core.PutSharedBatch(local, got);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(core.SizeApprox(), kSeedCount);
    core.ClearShared();
    EXPECT_EQ(core.SizeApprox(), 0u);
}
