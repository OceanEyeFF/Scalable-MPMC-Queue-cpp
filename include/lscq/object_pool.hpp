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

#include <cstddef>
#include <functional>
#include <lscq/detail/object_pool_core.hpp>
#include <utility>

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
class ObjectPool : private detail::ObjectPoolCore<T> {
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
    explicit ObjectPool(Factory factory,
                        std::size_t shard_count = detail::ObjectPoolCore<T>::DefaultShardCount())
        : detail::ObjectPoolCore<T>(std::move(factory), shard_count) {}

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /** @brief Destroy the pool and release all objects currently stored in it. */
    ~ObjectPool() = default;

    /**
     * @brief Get an object from the pool.
     * @return A pointer to an object. If the pool is empty, the factory is used.
     *
     * @note The returned pointer is owned by the caller and should be returned
     * via `Put()` when no longer needed.
     */
    pointer Get() { return this->GetShared(); }

    /**
     * @brief Return an object to the pool.
     * @param obj Object pointer to return. `nullptr` is ignored.
     *
     * The pool takes ownership of the pointer and will delete it on `Clear()` or
     * when the pool itself is destroyed.
     */
    void Put(pointer obj) { this->PutShared(obj); }

    /**
     * @brief Clear the pool and delete all stored objects.
     *
     * @note This only affects objects currently stored inside the pool. Objects
     * checked out via `Get()` are owned by the caller and are not tracked.
     *
     * @note Phase 1 local-cache implementations keep the same Clear() API and must
     * remain safe to call concurrently with Get()/Put() (clearing both shared storage
     * and any per-thread caches with appropriate synchronization).
     */
    void Clear() { this->ClearShared(); }

    /**
     * @brief Get the number of objects currently stored in the pool.
     * @return Current pool size.
     *
     * @note Under concurrency this value is approximate.
     */
    std::size_t Size() const { return this->SizeApprox(); }

   private:
    std::size_t CurrentShardIndex() const { return detail::ObjectPoolCore<T>::CurrentShardIndex(); }
};

}  // namespace lscq

#endif  // LSCQ_OBJECT_POOL_HPP_
