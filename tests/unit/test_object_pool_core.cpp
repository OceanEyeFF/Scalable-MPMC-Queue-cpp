#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
