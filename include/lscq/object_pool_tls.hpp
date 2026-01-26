/**
 * @file object_pool_tls.hpp
 * @brief Object pool with a per-thread fast slot (thread_local) + per-pool registry.
 * @author lscq contributors
 * @version 0.1.0
 *
 * This implementation adds a single per-thread cached pointer ("fast slot") to reduce
 * contention on the shared sharded storage under typical Get/Put reuse patterns.
 *
 * Design highlights:
 * - Hot path (steady-state hit) is lock-free: owner check + atomic pointer exchange/store.
 * - Each pool maintains a registry of thread-local cache addresses to support cross-thread
 *   Clear() and destructor cleanup.
 * - TLS slots are atomics so Clear() can safely invalidate caches from other threads.
 *
 * Notes:
 * - The TLS fast slot is shared across all pools per (T, thread). Only the first pool that
 *   claims it (owner==this) can use it; other pools fall back to shared storage.
 * - Clear() is safe to call concurrently with Get()/Put(); it clears both TLS slots (best
 *   effort via atomics) and the shared shards.
 */

#ifndef LSCQ_OBJECT_POOL_TLS_HPP_
#define LSCQ_OBJECT_POOL_TLS_HPP_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <lscq/detail/object_pool_core.hpp>

namespace lscq {

/**
 * @class ObjectPoolTLS
 * @brief Thread-safe object pool with a single thread_local fast slot.
 *
 * @tparam T Object type managed by the pool.
 *
 * Ownership model matches @ref lscq::ObjectPool:
 * - `Get()` returns a raw pointer that is owned by the caller.
 * - The caller should return it via `Put()` when done.
 * - After `Put(obj)`, `obj` must not be used again.
 */
template <class T>
class ObjectPoolTLS : private detail::ObjectPoolCore<T> {
   public:
    using value_type = T;
    using pointer = T*;
    using Factory = std::function<pointer()>;

    explicit ObjectPoolTLS(
        Factory factory,
        std::size_t shard_count = detail::ObjectPoolCore<T>::DefaultShardCount())
        : detail::ObjectPoolCore<T>(std::move(factory), shard_count) {}

    ObjectPoolTLS(const ObjectPoolTLS&) = delete;
    ObjectPoolTLS& operator=(const ObjectPoolTLS&) = delete;
    ObjectPoolTLS(ObjectPoolTLS&&) = delete;
    ObjectPoolTLS& operator=(ObjectPoolTLS&&) = delete;

    ~ObjectPoolTLS() { CloseAndClear(); }

    pointer Get() {
        OpGuard guard(*this);
        if (!guard) {
            return nullptr;
        }

        LocalCache& cache = tls_fast_cache_;
        EnsureRegistered(cache);

        if (cache.owner.load(std::memory_order_acquire) == this) {
            // Fast path: read-then-exchange to avoid an RMW when empty.
            if (cache.private_obj.load(std::memory_order_relaxed) != nullptr) {
                if (pointer p = cache.private_obj.exchange(nullptr, std::memory_order_acq_rel)) {
                    return p;
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
        if (!guard) {
            delete obj;
            return;
        }

        LocalCache& cache = tls_fast_cache_;
        EnsureRegistered(cache);

        if (cache.owner.load(std::memory_order_acquire) == this) {
            pointer expected = nullptr;
            if (cache.private_obj.compare_exchange_strong(
                    expected, obj, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return;
            }
        }

        this->PutShared(obj);
    }

    void Clear() {
        OpGuard guard(*this);
        if (!guard) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            ClearTlsSlotsLocked();
        }

        // Best-effort wait: Clear() is safe under concurrency, but cannot guarantee
        // immediate global quiescence if other threads keep operating.
        (void)WaitForActiveOpsAtMost(1, std::chrono::milliseconds(200));

        this->ClearShared();
    }

    std::size_t Size() const {
        std::size_t total = this->SizeApprox();
        std::lock_guard<std::mutex> lock(registry_mutex_);
        for (LocalCache* cache : registry_) {
            if (cache == nullptr) {
                continue;
            }
            if (cache->owner.load(std::memory_order_acquire) != this) {
                continue;
            }
            if (cache->private_obj.load(std::memory_order_relaxed) != nullptr) {
                total += 1;
            }
        }
        return total;
    }

   private:
    struct LocalCache {
        std::atomic<pointer> private_obj{nullptr};
        std::atomic<ObjectPoolTLS*> owner{nullptr};

        ~LocalCache() noexcept {
            ObjectPoolTLS* pool = owner.load(std::memory_order_acquire);
            if (pool != nullptr) {
                pool->OnThreadExit(this);
            }
        }
    };

    inline static thread_local LocalCache tls_fast_cache_{};

    class OpGuard {
       public:
        explicit OpGuard(ObjectPoolTLS& pool) noexcept : pool_(pool) {
            if (pool_.closing_.load(std::memory_order_acquire)) {
                return;
            }
            pool_.active_ops_.fetch_add(1, std::memory_order_acq_rel);
            if (pool_.closing_.load(std::memory_order_acquire)) {
                pool_.active_ops_.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
            active_ = true;
        }

        OpGuard(const OpGuard&) = delete;
        OpGuard& operator=(const OpGuard&) = delete;

        ~OpGuard() {
            if (active_) {
                pool_.active_ops_.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        explicit operator bool() const noexcept { return active_; }

       private:
        ObjectPoolTLS& pool_;
        bool active_{false};
    };

    void EnsureRegistered(LocalCache& cache) {
        if (cache.owner.load(std::memory_order_acquire) == this) {
            return;
        }

        ObjectPoolTLS* expected = nullptr;
        if (!cache.owner.compare_exchange_strong(
                expected, this, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }

        try {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            registry_.push_back(&cache);
        } catch (...) {
            cache.owner.store(nullptr, std::memory_order_release);
            throw;
        }
    }

    void Unregister(LocalCache* cache) noexcept {
        if (cache == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = std::find(registry_.begin(), registry_.end(), cache);
        if (it != registry_.end()) {
            registry_.erase(it);
        }
    }

    void ClearTlsSlotsLocked() noexcept {
        for (LocalCache* cache : registry_) {
            if (cache == nullptr) {
                continue;
            }
            if (cache->owner.load(std::memory_order_acquire) != this) {
                continue;
            }
            if (pointer p = cache->private_obj.exchange(nullptr, std::memory_order_acq_rel)) {
                delete p;
            }
        }
    }

    bool WaitForActiveOpsAtMost(int threshold, std::chrono::milliseconds timeout) const noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (active_ops_.load(std::memory_order_acquire) <= threshold) {
                return true;
            }
            std::this_thread::yield();
        }
        return active_ops_.load(std::memory_order_acquire) <= threshold;
    }

    void CloseAndClear() noexcept {
        closing_.store(true, std::memory_order_release);
        (void)WaitForActiveOpsAtMost(0, std::chrono::milliseconds(500));

        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            for (LocalCache* cache : registry_) {
                if (cache == nullptr) {
                    continue;
                }
                if (cache->owner.load(std::memory_order_acquire) != this) {
                    continue;
                }
                cache->owner.store(nullptr, std::memory_order_release);
                if (pointer p = cache->private_obj.exchange(nullptr, std::memory_order_acq_rel)) {
                    delete p;
                }
            }
            registry_.clear();
        }

        this->ClearShared();
    }

    void OnThreadExit(LocalCache* cache) noexcept {
        if (cache == nullptr) {
            return;
        }

        // Count thread-exit flushing as an active operation so pool destruction can wait for it.
        active_ops_.fetch_add(1, std::memory_order_acq_rel);

        // Guard against re-entrancy: LocalCache destructor may run late during shutdown.
        cache->owner.store(nullptr, std::memory_order_release);

        pointer p = cache->private_obj.exchange(nullptr, std::memory_order_acq_rel);
        if (p != nullptr) {
            if (!closing_.load(std::memory_order_acquire)) {
                this->PutShared(p);
            } else {
                delete p;
            }
        }

        Unregister(cache);
        active_ops_.fetch_sub(1, std::memory_order_acq_rel);
    }

    mutable std::mutex registry_mutex_;
    std::vector<LocalCache*> registry_;

    std::atomic<bool> closing_{false};
    std::atomic<int> active_ops_{0};
};

}  // namespace lscq

#endif  // LSCQ_OBJECT_POOL_TLS_HPP_
