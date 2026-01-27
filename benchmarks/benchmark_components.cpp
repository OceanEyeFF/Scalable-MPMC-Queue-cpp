/**
 * @file benchmark_components.cpp
 * @brief Component overhead microbenchmarks for ObjectPool optimization Phase 0.
 *
 * Measures the overhead of individual components to inform optimization decisions:
 * - Map lookup with shared_mutex
 * - thread_local access
 * - Atomic operations
 * - Full hot path simulation (Map-based vs thread_local)
 *
 * Decision thresholds:
 * - BM_FullPath_MapBased < 25ns: Continue Per-Pool Map approach
 * - BM_FullPath_MapBased > 40ns: Switch to thread_local approach
 * - 25-40ns: Trade-off analysis needed
 *
 * @see docs/ObjectPool-Optimization-Plan.md (Phase 0)
 * @see docs/ObjectPool-Testing-Plan.md (Section 0.1)
 */

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <lscq/object_pool.hpp>
#include <lscq/object_pool_map.hpp>
#include <lscq/object_pool_tls.hpp>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

//=============================================================================
// 0.1.1 Map Lookup Overhead
//=============================================================================

/**
 * @brief Measure unordered_map + shared_mutex read overhead.
 *
 * This is the critical path in Per-Pool Map approach.
 * Expected: 30-50ns single-threaded, higher under contention.
 */
static void BM_MapLookup_SharedMutex(benchmark::State& state) {
    std::shared_mutex mutex;
    std::unordered_map<std::thread::id, int> map;
    auto tid = std::this_thread::get_id();
    map[tid] = 42;

    for (auto _ : state) {
        std::shared_lock lock(mutex);
        auto it = map.find(tid);
        benchmark::DoNotOptimize(it->second);
    }
}
BENCHMARK(BM_MapLookup_SharedMutex)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

/**
 * @brief Measure pure unordered_map lookup overhead (no lock).
 *
 * Baseline for understanding lock overhead contribution.
 */
static void BM_MapLookup_NoLock(benchmark::State& state) {
    std::unordered_map<std::thread::id, int> map;
    auto tid = std::this_thread::get_id();
    map[tid] = 42;

    for (auto _ : state) {
        auto it = map.find(tid);
        benchmark::DoNotOptimize(it->second);
    }
}
BENCHMARK(BM_MapLookup_NoLock);

/**
 * @brief Measure std::this_thread::get_id() overhead.
 *
 * Called on every Get/Put in Map-based approach.
 */
static void BM_GetThreadId(benchmark::State& state) {
    for (auto _ : state) {
        auto tid = std::this_thread::get_id();
        benchmark::DoNotOptimize(tid);
    }
}
BENCHMARK(BM_GetThreadId);

/**
 * @brief Measure std::hash<std::thread::id> overhead.
 *
 * Used internally by unordered_map for thread::id keys.
 */
static void BM_HashThreadId(benchmark::State& state) {
    auto tid = std::this_thread::get_id();
    std::hash<std::thread::id> hasher;

    for (auto _ : state) {
        auto h = hasher(tid);
        benchmark::DoNotOptimize(h);
    }
}
BENCHMARK(BM_HashThreadId);

//=============================================================================
// 0.1.2 Lock Overhead
//=============================================================================

/**
 * @brief Measure shared_mutex read lock overhead.
 *
 * Used in Map-based approach for Get/Put operations.
 */
static void BM_SharedMutex_ReadLock(benchmark::State& state) {
    std::shared_mutex mutex;

    for (auto _ : state) {
        std::shared_lock lock(mutex);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_SharedMutex_ReadLock)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

/**
 * @brief Measure shared_mutex write lock overhead.
 *
 * Used in Map-based approach for thread registration.
 */
static void BM_SharedMutex_WriteLock(benchmark::State& state) {
    std::shared_mutex mutex;

    for (auto _ : state) {
        std::unique_lock lock(mutex);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_SharedMutex_WriteLock)->Threads(1);

/**
 * @brief Measure std::mutex lock overhead (baseline comparison).
 */
static void BM_Mutex_Lock(benchmark::State& state) {
    std::mutex mutex;

    for (auto _ : state) {
        std::lock_guard lock(mutex);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_Mutex_Lock)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

//=============================================================================
// 0.1.3 thread_local Overhead
//=============================================================================

namespace {

struct LocalCache {
    int* private_obj = nullptr;
    void* owner = nullptr;
};

// Thread-local cache for TLS approach benchmarks
thread_local LocalCache tls_cache;

}  // namespace

/**
 * @brief Measure thread_local access with pointer check.
 *
 * This is the fast path in thread_local approach.
 * Expected: ~3-5ns.
 */
static void BM_ThreadLocal_Access(benchmark::State& state) {
    tls_cache.private_obj = new int(42);
    tls_cache.owner = reinterpret_cast<void*>(0x12345678);

    for (auto _ : state) {
        if (tls_cache.owner == reinterpret_cast<void*>(0x12345678) && tls_cache.private_obj) {
            benchmark::DoNotOptimize(*tls_cache.private_obj);
        }
    }

    delete tls_cache.private_obj;
    tls_cache.private_obj = nullptr;
}
BENCHMARK(BM_ThreadLocal_Access);

/**
 * @brief Measure thread_local owner pointer check overhead.
 *
 * Isolates the cost of pool ownership verification.
 */
static void BM_ThreadLocal_OwnerCheck(benchmark::State& state) {
    void* pool_addr = reinterpret_cast<void*>(0x12345678);
    tls_cache.owner = pool_addr;

    for (auto _ : state) {
        bool match = (tls_cache.owner == pool_addr);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK(BM_ThreadLocal_OwnerCheck);

//=============================================================================
// 0.1.4 Atomic Operations Overhead
//=============================================================================

/**
 * @brief Measure atomic<bool> load with acquire semantics.
 *
 * Used for closing flag check.
 */
static void BM_AtomicBool_Load(benchmark::State& state) {
    std::atomic<bool> flag{false};

    for (auto _ : state) {
        bool v = flag.load(std::memory_order_acquire);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_AtomicBool_Load);

/**
 * @brief Measure atomic<int> fetch_add/sub pair.
 *
 * Used for active operation counting in OpGuard.
 */
static void BM_AtomicInt_FetchAdd(benchmark::State& state) {
    std::atomic<int> counter{0};

    for (auto _ : state) {
        counter.fetch_add(1, std::memory_order_acquire);
        counter.fetch_sub(1, std::memory_order_release);
    }
}
BENCHMARK(BM_AtomicInt_FetchAdd)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

//=============================================================================
// 0.1.5 Full Hot Path Simulation
//=============================================================================

/**
 * @brief Simulate Per-Pool Map approach full hot path.
 *
 * Includes: closing check + atomic inc + map lookup + atomic dec
 * This is the critical benchmark for optimization decisions.
 */
static void BM_FullPath_MapBased(benchmark::State& state) {
    std::shared_mutex mutex;
    std::unordered_map<std::thread::id, LocalCache> map;
    std::atomic<bool> closing{false};
    std::atomic<int> active_ops{0};

    auto tid = std::this_thread::get_id();
    map[tid] = LocalCache{new int(42), reinterpret_cast<void*>(0x12345678)};

    for (auto _ : state) {
        // Closing check (OpGuard entry)
        if (closing.load(std::memory_order_acquire)) {
            continue;
        }
        active_ops.fetch_add(1, std::memory_order_acquire);

        // Map lookup (critical section)
        {
            std::shared_lock lock(mutex);
            auto it = map.find(tid);
            if (it != map.end() && it->second.private_obj) {
                benchmark::DoNotOptimize(*it->second.private_obj);
            }
        }

        // OpGuard exit
        active_ops.fetch_sub(1, std::memory_order_release);
    }

    delete map[tid].private_obj;
}
BENCHMARK(BM_FullPath_MapBased)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

/**
 * @brief Simulate thread_local approach full hot path.
 *
 * Includes: closing check + atomic inc + TLS access + atomic dec
 * Compare with BM_FullPath_MapBased to measure optimization potential.
 */
static void BM_FullPath_ThreadLocal(benchmark::State& state) {
    static thread_local LocalCache fast_cache;
    std::atomic<bool> closing{false};
    std::atomic<int> active_ops{0};

    void* pool_addr = reinterpret_cast<void*>(0x12345678);
    fast_cache.owner = pool_addr;
    fast_cache.private_obj = new int(42);

    for (auto _ : state) {
        // Closing check (OpGuard entry)
        if (closing.load(std::memory_order_acquire)) {
            continue;
        }
        active_ops.fetch_add(1, std::memory_order_acquire);

        // thread_local fast path
        if (fast_cache.owner == pool_addr && fast_cache.private_obj) {
            benchmark::DoNotOptimize(*fast_cache.private_obj);
        }

        // OpGuard exit
        active_ops.fetch_sub(1, std::memory_order_release);
    }

    delete fast_cache.private_obj;
    fast_cache.private_obj = nullptr;
}
BENCHMARK(BM_FullPath_ThreadLocal)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

//=============================================================================
// 0.2 ObjectPool Get/Put End-to-End Benchmarks (Phase 1)
//=============================================================================

namespace {

struct PoolItem {
    std::uint64_t payload = 0;
};

constexpr std::size_t kPoolShardCount = 8;
constexpr std::size_t kClearFillCount = 1024;

template <typename PoolType>
void FillPool(PoolType& pool, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        pool.Put(new PoolItem());
    }
}

template <typename PoolType>
PoolType MakePool() {
    return PoolType([] { return new PoolItem(); }, kPoolShardCount);
}

}  // namespace

template <typename PoolType>
static void BM_ObjectPool_HotPath(benchmark::State& state) {
    PoolType pool = MakePool<PoolType>();

    PoolItem* warm = pool.Get();
    pool.Put(warm);

    for (auto _ : state) {
        PoolItem* p = pool.Get();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
    }

    pool.Clear();
}

template <typename PoolType>
static void BM_ObjectPool_ColdPath(benchmark::State& state) {
    PoolType pool = MakePool<PoolType>();

    for (auto _ : state) {
        state.PauseTiming();
        pool.Clear();
        state.ResumeTiming();

        PoolItem* p = pool.Get();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
    }

    pool.Clear();
}

template <typename PoolType>
static void BM_ObjectPool_Throughput(benchmark::State& state) {
    static PoolType pool = MakePool<PoolType>();

    if (state.thread_index() == 0) {
        pool.Clear();
    }

    for (auto _ : state) {
        PoolItem* p = pool.Get();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
    }

    state.SetItemsProcessed(state.iterations() * state.threads());
}

template <typename PoolType>
static void BM_ObjectPool_Clear(benchmark::State& state) {
    PoolType pool = MakePool<PoolType>();

    for (auto _ : state) {
        state.PauseTiming();
        FillPool(pool, kClearFillCount);
        state.ResumeTiming();

        pool.Clear();
    }
}

static void BM_ObjectPool_Baseline_HotPath(benchmark::State& state) {
    BM_ObjectPool_HotPath<lscq::ObjectPool<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPool_Baseline_HotPath)->Threads(1);

static void BM_ObjectPoolTLS_HotPath(benchmark::State& state) {
    BM_ObjectPool_HotPath<lscq::ObjectPoolTLS<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPoolTLS_HotPath)->Threads(1);

static void BM_ObjectPoolMap_HotPath(benchmark::State& state) {
    BM_ObjectPool_HotPath<lscq::ObjectPoolMap<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPoolMap_HotPath)->Threads(1);

static void BM_ObjectPool_Baseline_ColdPath(benchmark::State& state) {
    BM_ObjectPool_ColdPath<lscq::ObjectPool<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPool_Baseline_ColdPath)->Threads(1);

static void BM_ObjectPoolTLS_ColdPath(benchmark::State& state) {
    BM_ObjectPool_ColdPath<lscq::ObjectPoolTLS<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPoolTLS_ColdPath)->Threads(1);

static void BM_ObjectPoolMap_ColdPath(benchmark::State& state) {
    BM_ObjectPool_ColdPath<lscq::ObjectPoolMap<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPoolMap_ColdPath)->Threads(1);

static void BM_ObjectPool_Baseline_Throughput(benchmark::State& state) {
    BM_ObjectPool_Throughput<lscq::ObjectPool<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPool_Baseline_Throughput)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(12)
    ->Threads(16)
    ->Threads(24);

static void BM_ObjectPoolTLS_Throughput(benchmark::State& state) {
    BM_ObjectPool_Throughput<lscq::ObjectPoolTLS<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPoolTLS_Throughput)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(12)
    ->Threads(16)
    ->Threads(24);

static void BM_ObjectPoolMap_Throughput(benchmark::State& state) {
    BM_ObjectPool_Throughput<lscq::ObjectPoolMap<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPoolMap_Throughput)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(12)
    ->Threads(16)
    ->Threads(24);

static void BM_ObjectPool_Baseline_Clear(benchmark::State& state) {
    BM_ObjectPool_Clear<lscq::ObjectPool<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPool_Baseline_Clear)->Threads(1);

static void BM_ObjectPoolTLS_Clear(benchmark::State& state) {
    BM_ObjectPool_Clear<lscq::ObjectPoolTLS<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPoolTLS_Clear)->Threads(1);

static void BM_ObjectPoolMap_Clear(benchmark::State& state) {
    BM_ObjectPool_Clear<lscq::ObjectPoolMap<PoolItem>>(state);
}
BENCHMARK(BM_ObjectPoolMap_Clear)->Threads(1);

//=============================================================================
// Main
//=============================================================================

BENCHMARK_MAIN();
