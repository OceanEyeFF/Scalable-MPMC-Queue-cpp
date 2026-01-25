/**
 * @file object_pool.hpp
 * @brief Simple mutex-protected object pool (sync.Pool-like baseline).
 * @author lscq contributors
 * @version 0.1.0
 *
 * This is a basic, thread-safe object pool implementation intended as a building
 * block for later optimizations. It currently uses sharding to reduce lock
 * contention under multi-threaded workloads.
 */

#ifndef LSCQ_OBJECT_POOL_HPP_
#define LSCQ_OBJECT_POOL_HPP_

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace lscq {

/**
 * @class ObjectPool
 * @brief Thread-safe object pool that recycles objects via Get/Put.
 *
 * @tparam T Object type managed by the pool.
 *
 * The pool stores returned objects in an internal vector and hands them out on
 * demand. When the pool is empty, a user-provided factory is called to create a
 * new object.
 *
 * Thread-safety: All public methods are thread-safe. Internally the pool is
 * sharded so that different threads tend to hit different mutexes.
 *
 * Ownership model:
 * - `Get()` returns a raw pointer that is owned by the caller.
 * - The caller should return the object to the pool via `Put()` when done.
 * - After `Put(obj)`, `obj` must not be used again.
 *
 * Example:
 * @code
 * lscq::ObjectPool<int> pool([] { return new int(0); });
 * int* p = pool.Get();
 * *p = 42;
 * pool.Put(p);
 * @endcode
 */
template <class T>
class ObjectPool {
   public:
    /** @brief Object type managed by the pool. */
    using value_type = T;
    /** @brief Pointer type returned by Get(). */
    using pointer = T*;
    /**
     * @brief Factory function type used to create objects when the pool is empty.
     *
     * The factory must return a pointer to a heap-allocated `T` (or `nullptr` on
     * failure). Ownership of the returned pointer is transferred to the caller
     * of `Get()`.
     */
    using Factory = std::function<pointer()>;

    /**
     * @brief Construct an object pool with a factory function.
     * @param factory Callable used to create new objects when the pool is empty.
     * @param shard_count Number of shards to use. If 0, it will be treated as 1.
     *
     * @note The factory is expected to be valid (non-empty) and to return a
     * heap-allocated object.
     */
    explicit ObjectPool(Factory factory, std::size_t shard_count = DefaultShardCount())
        : factory_(std::move(factory)), shards_(std::max<std::size_t>(1, shard_count)) {}

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /** @brief Destroy the pool and release all objects currently stored in it. */
    ~ObjectPool() { Clear(); }

    /**
     * @brief Get an object from the pool.
     * @return A pointer to an object. If the pool is empty, the factory is used.
     *
     * @note The returned pointer is owned by the caller and should be returned
     * via `Put()` when no longer needed.
     */
    pointer Get() {
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

    /**
     * @brief Return an object to the pool.
     * @param obj Object pointer to return. `nullptr` is ignored.
     *
     * The pool takes ownership of the pointer and will delete it on `Clear()` or
     * when the pool itself is destroyed.
     */
    void Put(pointer obj) {
        if (obj == nullptr) {
            return;
        }
        const std::size_t shard_index = CurrentShardIndex();
        Shard& shard = shards_[shard_index];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.objects.emplace_back(obj);
    }

    /**
     * @brief Clear the pool and delete all stored objects.
     *
     * @note This only affects objects currently stored inside the pool. Objects
     * checked out via `Get()` are owned by the caller and are not tracked.
     */
    void Clear() {
        for (Shard& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.objects.clear();
        }
    }

    /**
     * @brief Get the number of objects currently stored in the pool.
     * @return Current pool size.
     */
    std::size_t Size() const {
        std::size_t total = 0;
        for (const Shard& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            total += shard.objects.size();
        }
        return total;
    }

   private:
    struct Shard {
        mutable std::mutex mutex;
        std::vector<std::unique_ptr<T>> objects;
    };

    static std::size_t DefaultShardCount() {
        // std::thread::hardware_concurrency() is allowed to return 0.
        const unsigned int hc = std::thread::hardware_concurrency();
        const std::size_t base = std::max<std::size_t>(1, hc);
        return base * 2;
    }

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
        }
        return obj.release();
    }

    Factory factory_;
    std::vector<Shard> shards_;
};

}  // namespace lscq

#endif  // LSCQ_OBJECT_POOL_HPP_
