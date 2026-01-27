/**
 * @file object_pool_tls_v2.hpp
 * @brief Object pool with three-tier TLS cache (fast slot + batch array + shared shards).
 * @author lscq contributors
 * @version 0.2.0
 *
 * Phase 2 TLS cache implementation:
 * - L1: per-thread fast slot (single atomic pointer)
 * - L2: per-thread batch array (BatchSize pointers)
 * - L3: shared sharded storage (ObjectPoolCore)
 *
 * Design highlights:
 * - Fast path is lock-free: fast_slot exchange or local batch pop.
 * - Batch operations amortize shared shard lock overhead.
 * - Per-pool registry supports cross-thread Clear() and destructor cleanup.
 * - OpGuard + closing gate preserve Phase 1 safety guarantees.
 */

#ifndef LSCQ_OBJECT_POOL_TLS_V2_HPP_
#define LSCQ_OBJECT_POOL_TLS_V2_HPP_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <lscq/detail/numa_utils.hpp>
#include <lscq/detail/object_pool_core.hpp>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#if defined(__clang__)
#if __has_builtin(__builtin_prefetch)
#define LSCQ_HAS_BUILTIN_PREFETCH 1
#endif
#elif defined(__GNUC__)
#define LSCQ_HAS_BUILTIN_PREFETCH 1
#endif
#ifndef LSCQ_HAS_BUILTIN_PREFETCH
#define LSCQ_HAS_BUILTIN_PREFETCH 0
#endif

namespace lscq {

/**
 * @class ObjectPoolTLSv2
 * @brief Thread-safe object pool with fast-slot + batch TLS cache.
 *
 * @tparam T Object type managed by the pool.
 * @tparam BatchSize Size of the per-thread batch cache (default: 8).
 *
 * Ownership model matches @ref lscq::ObjectPool:
 * - `Get()` returns a raw pointer that is owned by the caller.
 * - The caller should return it via `Put()` when done.
 * - After `Put(obj)`, `obj` must not be used again.
 */
template <class T, std::size_t BatchSize = 8>
class ObjectPoolTLSv2 : private detail::ObjectPoolCore<T> {
   public:
    using value_type = T;
    using pointer = T*;
    using Factory = std::function<pointer()>;

    static_assert(BatchSize > 0, "BatchSize must be greater than 0");

    explicit ObjectPoolTLSv2(
        Factory factory, std::size_t shard_count = detail::ObjectPoolCore<T>::DefaultShardCount())
        : detail::ObjectPoolCore<T>(std::move(factory), shard_count) {}

    ObjectPoolTLSv2(const ObjectPoolTLSv2&) = delete;
    ObjectPoolTLSv2& operator=(const ObjectPoolTLSv2&) = delete;
    ObjectPoolTLSv2(ObjectPoolTLSv2&&) = delete;
    ObjectPoolTLSv2& operator=(ObjectPoolTLSv2&&) = delete;

    ~ObjectPoolTLSv2() { CloseAndClear(); }

    pointer Get() {
        OpGuard guard(*this);
        if (!guard) {
            return nullptr;
        }

        LocalCache& cache = tls_cache_.Get();
        EnsureRegistered(cache);

        if (cache.owner.load(std::memory_order_acquire) == this) {
            // L1: fast slot.
            if (cache.fast_slot.load(std::memory_order_relaxed) != nullptr) {
                if (pointer p = cache.fast_slot.exchange(nullptr, std::memory_order_acq_rel)) {
                    RecordCacheHit(cache);
                    return p;
                }
            }

            // L2: batch cache.
            if (pointer p = TryPopBatch(cache)) {
                RecordCacheHit(cache);
                return p;
            }

            // L3: shared shards, refill batch on success.
            const std::size_t target = EffectiveBatchSize(cache);
            std::array<pointer, kMaxBatchSize> batch{};
            const std::size_t got = this->GetSharedBatch(batch.data(), target);
            RecordCacheMiss(cache);
            if (got > 0) {
                pointer result = batch[got - 1];
                const std::size_t cached = got - 1;
                for (std::size_t i = 0; i < cached; ++i) {
                    if (i + 1 < cached) {
                        PrefetchPointer(batch[i + 1]);
                    }
                    cache.batch[i].store(batch[i], std::memory_order_release);
                }
                for (std::size_t i = cached; i < cache.batch.size(); ++i) {
                    cache.batch[i].store(nullptr, std::memory_order_release);
                }
                cache.batch_count.store(cached, std::memory_order_release);
                return result;
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

        LocalCache& cache = tls_cache_.Get();
        EnsureRegistered(cache);

        if (cache.owner.load(std::memory_order_acquire) == this) {
            pointer expected = nullptr;
            if (cache.fast_slot.compare_exchange_strong(expected, obj, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
                RecordCacheHit(cache);
                return;
            }

            if (TryPushBatch(cache, obj)) {
                RecordCacheHit(cache);
                return;
            }

            // Batch full: flush to shared shards.
            RecordCacheMiss(cache);
            pointer batch_items[kMaxBatchSize + 1] = {};
            std::size_t count = DrainBatch(cache, batch_items, kMaxBatchSize);
            batch_items[count++] = obj;
            this->PutSharedBatch(batch_items, count);
            return;
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
            if (cache->fast_slot.load(std::memory_order_relaxed) != nullptr) {
                total += 1;
            }
            for (const auto& slot : cache->batch) {
                if (slot.load(std::memory_order_relaxed) != nullptr) {
                    total += 1;
                }
            }
        }
        return total;
    }

   private:
    static constexpr std::size_t kMinBatchSize = (BatchSize / 2 == 0) ? 1 : (BatchSize / 2);
    static constexpr std::size_t kMaxBatchSize = BatchSize * 2;
    static constexpr std::size_t kAdaptiveCheckInterval =
        (BatchSize * 8 < 16) ? 16 : (BatchSize * 8);
    static constexpr double kHitIncreaseThreshold = 0.75;
    static constexpr double kHitDecreaseThreshold = 0.25;

    struct LocalCache {
        std::atomic<pointer> fast_slot{nullptr};
        std::array<std::atomic<pointer>, kMaxBatchSize> batch;
        std::atomic<std::size_t> batch_count{0};
        std::atomic<std::size_t> effective_batch_size{BatchSize};
        std::atomic<std::size_t> hit_count{0};
        std::atomic<std::size_t> miss_count{0};
        std::atomic<std::size_t> op_count{0};
        std::atomic<ObjectPoolTLSv2*> owner{nullptr};

        LocalCache() {
            for (auto& slot : batch) {
                slot.store(nullptr, std::memory_order_relaxed);
            }
        }

        ~LocalCache() noexcept {
            ObjectPoolTLSv2* pool = owner.load(std::memory_order_acquire);
            if (pool != nullptr) {
                pool->OnThreadExit(this);
            }
        }
    };

    struct LocalCacheHolder {
        LocalCacheHolder() {
            allocation_ = detail::numa::Allocate(sizeof(LocalCache));
            if (allocation_.ptr == nullptr) {
                throw std::bad_alloc();
            }
            cache_ = new (allocation_.ptr) LocalCache();
        }

        LocalCacheHolder(const LocalCacheHolder&) = delete;
        LocalCacheHolder& operator=(const LocalCacheHolder&) = delete;

        ~LocalCacheHolder() noexcept {
            if (cache_ == nullptr) {
                return;
            }
            cache_->~LocalCache();
            detail::numa::Free(allocation_, sizeof(LocalCache));
        }

        LocalCache& Get() noexcept { return *cache_; }
        const LocalCache& Get() const noexcept { return *cache_; }

       private:
        LocalCache* cache_{nullptr};
        detail::numa::Allocation allocation_{};
    };

    inline static thread_local LocalCacheHolder tls_cache_{};

    class OpGuard {
       public:
        explicit OpGuard(ObjectPoolTLSv2& pool) noexcept : pool_(pool) {
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
        ObjectPoolTLSv2& pool_;
        bool active_{false};
    };

    void EnsureRegistered(LocalCache& cache) {
        if (cache.owner.load(std::memory_order_acquire) == this) {
            return;
        }

        ObjectPoolTLSv2* expected = nullptr;
        if (!cache.owner.compare_exchange_strong(expected, this, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
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

    pointer TryPopBatch(LocalCache& cache) noexcept {
        const std::size_t capacity = cache.batch.size();
        std::size_t count = cache.batch_count.load(std::memory_order_acquire);
        if (count > capacity) {
            count = capacity;
        }
        while (count > 0) {
            if (cache.batch_count.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
                pointer p = cache.batch[count - 1].exchange(nullptr, std::memory_order_acq_rel);
                if (p != nullptr) {
                    return p;
                }
            }
        }

        for (std::size_t i = 0; i < capacity; ++i) {
            if (i + 1 < capacity) {
                PrefetchPointer(cache.batch[i + 1].load(std::memory_order_relaxed));
            }
            pointer p = cache.batch[i].exchange(nullptr, std::memory_order_acq_rel);
            if (p != nullptr) {
                std::size_t current = cache.batch_count.load(std::memory_order_acquire);
                while (current > 0) {
                    if (cache.batch_count.compare_exchange_weak(current, current - 1,
                                                                std::memory_order_acq_rel,
                                                                std::memory_order_acquire)) {
                        break;
                    }
                }
                return p;
            }
        }

        cache.batch_count.store(0, std::memory_order_release);
        return nullptr;
    }

    void IncrementBatchCount(LocalCache& cache) noexcept {
        std::size_t prev = cache.batch_count.fetch_add(1, std::memory_order_acq_rel);
        if (prev >= kMaxBatchSize) {
            cache.batch_count.store(kMaxBatchSize, std::memory_order_release);
        }
    }

    bool TryPushBatch(LocalCache& cache, pointer obj) noexcept {
        const std::size_t limit = EffectiveBatchSize(cache);
        std::size_t count = cache.batch_count.load(std::memory_order_relaxed);
        if (count >= limit) {
            return false;
        }
        if (count < cache.batch.size()) {
            pointer expected = nullptr;
            if (cache.batch[count].compare_exchange_strong(expected, obj, std::memory_order_acq_rel,
                                                           std::memory_order_relaxed)) {
                IncrementBatchCount(cache);
                return true;
            }
        }

        for (std::size_t i = 0; i < limit; ++i) {
            pointer expected = nullptr;
            if (cache.batch[i].compare_exchange_strong(expected, obj, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
                IncrementBatchCount(cache);
                return true;
            }
        }

        return false;
    }

    std::size_t DrainBatch(LocalCache& cache, pointer* out, std::size_t max_count) noexcept {
        if (out == nullptr || max_count == 0) {
            return 0;
        }
        std::size_t count = 0;
        for (std::size_t i = 0; i < cache.batch.size() && count < max_count; ++i) {
            if (i + 1 < cache.batch.size()) {
                PrefetchPointer(cache.batch[i + 1].load(std::memory_order_relaxed));
            }
            pointer p = cache.batch[i].exchange(nullptr, std::memory_order_acq_rel);
            if (p != nullptr) {
                out[count++] = p;
            }
        }
        cache.batch_count.store(0, std::memory_order_release);
        return count;
    }

    void ClearTlsSlotsLocked() noexcept {
        for (LocalCache* cache : registry_) {
            if (cache == nullptr) {
                continue;
            }
            if (cache->owner.load(std::memory_order_acquire) != this) {
                continue;
            }
            if (pointer p = cache->fast_slot.exchange(nullptr, std::memory_order_acq_rel)) {
                delete p;
            }
            for (auto& slot : cache->batch) {
                if (pointer p = slot.exchange(nullptr, std::memory_order_acq_rel)) {
                    delete p;
                }
            }
            cache->batch_count.store(0, std::memory_order_release);
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
                if (pointer p = cache->fast_slot.exchange(nullptr, std::memory_order_acq_rel)) {
                    delete p;
                }
                for (auto& slot : cache->batch) {
                    if (pointer p = slot.exchange(nullptr, std::memory_order_acq_rel)) {
                        delete p;
                    }
                }
                cache->batch_count.store(0, std::memory_order_release);
            }
            registry_.clear();
        }

        this->ClearShared();
    }

    void OnThreadExit(LocalCache* cache) noexcept {
        if (cache == nullptr) {
            return;
        }

        active_ops_.fetch_add(1, std::memory_order_acq_rel);
        cache->owner.store(nullptr, std::memory_order_release);

        pointer batch_items[kMaxBatchSize + 1] = {};
        std::size_t count = 0;
        if (pointer p = cache->fast_slot.exchange(nullptr, std::memory_order_acq_rel)) {
            batch_items[count++] = p;
        }
        for (auto& slot : cache->batch) {
            if (pointer p = slot.exchange(nullptr, std::memory_order_acq_rel)) {
                batch_items[count++] = p;
            }
        }
        cache->batch_count.store(0, std::memory_order_release);

        if (count > 0) {
            if (!closing_.load(std::memory_order_acquire)) {
                this->PutSharedBatch(batch_items, count);
            } else {
                for (std::size_t i = 0; i < count; ++i) {
                    delete batch_items[i];
                }
            }
        }

        Unregister(cache);
        active_ops_.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::size_t EffectiveBatchSize(const LocalCache& cache) const noexcept {
        std::size_t size = cache.effective_batch_size.load(std::memory_order_relaxed);
        if (size < kMinBatchSize) {
            return kMinBatchSize;
        }
        if (size > kMaxBatchSize) {
            return kMaxBatchSize;
        }
        return size;
    }

    void RecordCacheHit(LocalCache& cache) noexcept { UpdateAdaptiveBatchSize(cache, true); }

    void RecordCacheMiss(LocalCache& cache) noexcept { UpdateAdaptiveBatchSize(cache, false); }

    void UpdateAdaptiveBatchSize(LocalCache& cache, bool hit) noexcept {
        if (hit) {
            cache.hit_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            cache.miss_count.fetch_add(1, std::memory_order_relaxed);
        }

        const std::size_t ops = cache.op_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ops < kAdaptiveCheckInterval) {
            return;
        }
        cache.op_count.store(0, std::memory_order_relaxed);

        const std::size_t hits = cache.hit_count.exchange(0, std::memory_order_relaxed);
        const std::size_t misses = cache.miss_count.exchange(0, std::memory_order_relaxed);
        const std::size_t total = hits + misses;
        if (total == 0) {
            return;
        }

        const double ratio = static_cast<double>(hits) / static_cast<double>(total);
        std::size_t current = cache.effective_batch_size.load(std::memory_order_relaxed);
        if (ratio >= kHitIncreaseThreshold && current < kMaxBatchSize) {
            current += 1;
        } else if (ratio <= kHitDecreaseThreshold && current > kMinBatchSize) {
            current -= 1;
        }
        cache.effective_batch_size.store(current, std::memory_order_relaxed);
    }

    void PrefetchPointer(pointer ptr) const noexcept {
#if LSCQ_HAS_BUILTIN_PREFETCH
        if (ptr != nullptr) {
            __builtin_prefetch(ptr, 0, 3);
        }
#else
        (void)ptr;
#endif
    }

    mutable std::mutex registry_mutex_;
    std::vector<LocalCache*> registry_;

    std::atomic<bool> closing_{false};
    std::atomic<int> active_ops_{0};
};

}  // namespace lscq

#endif  // LSCQ_OBJECT_POOL_TLS_V2_HPP_
