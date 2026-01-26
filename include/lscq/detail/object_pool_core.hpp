#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include <lscq/detail/object_pool_shard.hpp>

namespace lscq::detail {

/**
 * @brief Shared, sharded storage core for ObjectPool family.
 *
 * This component owns the shared storage (shards + factory + stealing logic) and exposes
 * a small API that higher-level pools (baseline ObjectPool, ObjectPoolTLS, ObjectPoolMap)
 * can reuse.
 *
 * Clear() contract for Phase 1 local-cache implementations:
 * - The public Clear() method MUST remain safe to call concurrently with Get()/Put().
 * - ClearShared() only clears the shared shards owned by this core.
 * - Implementations that add per-thread caches must additionally clear/invalidate those
 *   caches using their own synchronization (e.g. atomic slots), then call ClearShared().
 *
 * Notes:
 * - Objects checked out via GetShared() are owned by the caller and are not tracked.
 * - SizeApprox() intentionally returns an approximate value under concurrency.
 */
template <class T>
class ObjectPoolCore {
   public:
    using value_type = T;
    using pointer = T*;
    using Factory = std::function<pointer()>;

    explicit ObjectPoolCore(Factory factory, std::size_t shard_count)
        : factory_(std::move(factory)), shards_(std::max<std::size_t>(1, shard_count)) {}

    ObjectPoolCore(const ObjectPoolCore&) = delete;
    ObjectPoolCore& operator=(const ObjectPoolCore&) = delete;
    ObjectPoolCore(ObjectPoolCore&&) = delete;
    ObjectPoolCore& operator=(ObjectPoolCore&&) = delete;

    ~ObjectPoolCore() { ClearShared(); }

    pointer GetShared() {
        const std::size_t shard_index = CurrentShardIndex();

        if (pointer from_local = TryPopFromShard(shard_index)) {
            return from_local;
        }

        // Opportunistically steal from other shards if the local shard is empty.
        // This helps under imbalanced workloads where some threads return more
        // objects than they consume.
        for (std::size_t n = 1; n < shards_.size(); ++n) {
            const std::size_t other = (shard_index + n) % shards_.size();
            if (pointer stolen = TryStealFromShard(other)) {
                return stolen;
            }
        }

        return factory_ ? factory_() : nullptr;
    }

    void PutShared(pointer obj) {
        if (obj == nullptr) {
            return;
        }
        PutToShard(CurrentShardIndex(), obj);
    }

    void ClearShared() {
        for (Shard& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.objects.clear();
            shard.approx_size.store(0, std::memory_order_relaxed);
        }
    }

    std::size_t SizeApprox() const noexcept {
        std::size_t total = 0;
        for (const Shard& shard : shards_) {
            total += shard.approx_size.load(std::memory_order_relaxed);
        }
        return total;
    }

    std::size_t ShardCount() const noexcept { return shards_.size(); }

    static std::size_t DefaultShardCount() {
        // std::thread::hardware_concurrency() is allowed to return 0.
        const unsigned int hc = std::thread::hardware_concurrency();
        const std::size_t base = std::max<std::size_t>(1, hc);
        return base * 2;
    }

   protected:
    using Shard = ObjectPoolShard<T>;

    std::size_t CurrentShardIndex() const {
        return std::hash<std::thread::id>{}(std::this_thread::get_id()) % shards_.size();
    }

    pointer TryPopFromShard(std::size_t shard_index) {
        Shard& shard = shards_[shard_index];
        std::unique_ptr<T> obj;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            if (shard.objects.empty()) {
                return nullptr;
            }
            obj = std::move(shard.objects.back());
            shard.objects.pop_back();
            shard.approx_size.fetch_sub(1, std::memory_order_relaxed);
        }
        return obj.release();
    }

    pointer TryStealFromShard(std::size_t shard_index) {
        Shard& shard = shards_[shard_index];
        std::unique_ptr<T> obj;
        {
            std::unique_lock<std::mutex> lock(shard.mutex, std::try_to_lock);
            if (!lock.owns_lock() || shard.objects.empty()) {
                return nullptr;
            }
            obj = std::move(shard.objects.back());
            shard.objects.pop_back();
            shard.approx_size.fetch_sub(1, std::memory_order_relaxed);
        }
        return obj.release();
    }

    void PutToShard(std::size_t shard_index, pointer obj) {
        Shard& shard = shards_[shard_index];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.objects.emplace_back(obj);
        shard.approx_size.fetch_add(1, std::memory_order_relaxed);
    }

   private:
    Factory factory_;
    std::vector<Shard> shards_;
};

}  // namespace lscq::detail
