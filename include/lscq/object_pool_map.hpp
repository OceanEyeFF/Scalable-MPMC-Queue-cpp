/**
 * @file object_pool_map.hpp
 * @brief Object pool with per-thread single-slot cache using per-pool unordered_map + shared_mutex.
 * @author lscq contributors
 * @version 0.1.0
 *
 * Phase 1 local-cache implementation (Scheme A: ObjectPoolMap):
 * - Per pool: std::shared_mutex + std::unordered_map<std::thread::id, LocalCache>
 * - Each LocalCache has a single atomic pointer slot
 * - First-time registration uses unique_lock; steady-state uses shared_lock
 * - Clear() is safe to call concurrently with Get()/Put() via a closing gate
 */

#ifndef LSCQ_OBJECT_POOL_MAP_HPP_
#define LSCQ_OBJECT_POOL_MAP_HPP_

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include <lscq/detail/object_pool_core.hpp>

namespace lscq {

/**
 * @class ObjectPoolMap
 * @brief Thread-safe object pool with per-thread single-object cache (per-pool map).
 *
 * @tparam T Object type managed by the pool.
 *
 * Ownership model matches ObjectPool:
 * - `Get()` returns a raw pointer owned by the caller.
 * - The caller should return it via `Put()` when done.
 * - After `Put(obj)`, `obj` must not be used again.
 *
 * Concurrency notes:
 * - Steady state: Get/Put take a shared lock on the per-pool cache map and use an atomic slot.
 * - First-time per-thread registration: Put upgrades to a unique lock to insert a LocalCache entry.
 * - Clear() uses a "closing gate" (closing_ + active_ops_) to safely synchronize with concurrent Get/Put.
 */
template <class T>
class ObjectPoolMap : private detail::ObjectPoolCore<T> {
   public:
    using value_type = T;
    using pointer = T*;
    using Factory = std::function<pointer()>;

    explicit ObjectPoolMap(Factory factory,
                           std::size_t shard_count = detail::ObjectPoolCore<T>::DefaultShardCount())
        : detail::ObjectPoolCore<T>(std::move(factory), shard_count) {}

    ObjectPoolMap(const ObjectPoolMap&) = delete;
    ObjectPoolMap& operator=(const ObjectPoolMap&) = delete;
    ObjectPoolMap(ObjectPoolMap&&) = delete;
    ObjectPoolMap& operator=(ObjectPoolMap&&) = delete;

    ~ObjectPoolMap() { Clear(); }

    pointer Get() {
        OpGuard guard(*this);
        const std::thread::id tid = std::this_thread::get_id();

        {
            std::shared_lock lock(cache_mutex_);
            auto it = caches_.find(tid);
            if (it != caches_.end()) {
                if (pointer cached = it->second.private_obj.exchange(nullptr, std::memory_order_acq_rel);
                    cached != nullptr) {
                    return cached;
                }
            }
        }

        return this->GetShared();
    }

    void Put(pointer obj) {
        if (obj == nullptr) {
            return;
        }

        OpGuard guard(*this);
        const std::thread::id tid = std::this_thread::get_id();

        {
            std::shared_lock lock(cache_mutex_);
            auto it = caches_.find(tid);
            if (it != caches_.end()) {
                pointer expected = nullptr;
                if (it->second.private_obj.compare_exchange_strong(expected,
                                                                   obj,
                                                                   std::memory_order_acq_rel,
                                                                   std::memory_order_relaxed)) {
                    return;
                }
                // Slot already occupied: fall back to shared pool.
                this->PutShared(obj);
                return;
            }
        }

        // Registration path: insert a LocalCache entry for this thread id.
        {
            std::unique_lock lock(cache_mutex_);
            auto [it, inserted] = caches_.try_emplace(tid);
            (void)inserted;
            pointer expected = nullptr;
            if (it->second.private_obj.compare_exchange_strong(
                    expected, obj, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return;
            }
        }

        this->PutShared(obj);
    }

    void Clear() {
        // Close the gate to prevent new operations, then wait for in-flight ops.
        closing_.store(true, std::memory_order_release);
        while (active_ops_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }

        {
            std::unique_lock lock(cache_mutex_);
            for (auto& kv : caches_) {
                if (pointer p = kv.second.private_obj.exchange(nullptr, std::memory_order_acq_rel);
                    p != nullptr) {
                    delete p;
                }
            }
        }

        this->ClearShared();
        closing_.store(false, std::memory_order_release);
    }

    std::size_t Size() const {
        OpGuard guard(*this);
        std::size_t local = 0;
        {
            std::shared_lock lock(cache_mutex_);
            for (const auto& kv : caches_) {
                if (kv.second.private_obj.load(std::memory_order_acquire) != nullptr) {
                    ++local;
                }
            }
        }
        return this->SizeApprox() + local;
    }

   private:
    struct LocalCache {
        std::atomic<pointer> private_obj{nullptr};
    };

    class OpGuard {
       public:
        explicit OpGuard(const ObjectPoolMap& pool) noexcept : pool_(pool) {
            for (;;) {
                if (pool_.closing_.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                    continue;
                }

                pool_.active_ops_.fetch_add(1, std::memory_order_acquire);

                // If Close() races between the load and the increment, we are still counted
                // as in-flight and Clear() will wait for us to finish.
                if (!pool_.closing_.load(std::memory_order_acquire)) {
                    break;
                }

                pool_.active_ops_.fetch_sub(1, std::memory_order_release);
            }
        }

        ~OpGuard() { pool_.active_ops_.fetch_sub(1, std::memory_order_release); }

        OpGuard(const OpGuard&) = delete;
        OpGuard& operator=(const OpGuard&) = delete;
        OpGuard(OpGuard&&) = delete;
        OpGuard& operator=(OpGuard&&) = delete;

       private:
        const ObjectPoolMap& pool_;
    };

    alignas(64) mutable std::shared_mutex cache_mutex_;
    mutable std::unordered_map<std::thread::id, LocalCache> caches_;

    // Closing gate: same pattern as LSCQ (prevent clearing while operations are active).
    alignas(64) mutable std::atomic<int> active_ops_{0};
    alignas(64) mutable std::atomic<bool> closing_{false};
};

}  // namespace lscq

#endif  // LSCQ_OBJECT_POOL_MAP_HPP_
