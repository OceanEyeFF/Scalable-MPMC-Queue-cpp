/**
 * @file benchmark_object_pool_map.cpp
 * @brief Benchmarks for ObjectPoolMap (Phase 1 local-cache, map-based).
 */

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>

#include <lscq/object_pool_map.hpp>

namespace {

struct Trivial {
    std::size_t payload = 0;
};

}  // namespace

static void BM_ObjectPoolMap_GetPut_HotLocal(benchmark::State& state) {
    std::atomic<std::size_t> created{0};
    lscq::ObjectPoolMap<Trivial> pool(
        [&] {
            created.fetch_add(1, std::memory_order_relaxed);
            return new Trivial();
        },
        8);

    // Warm up: create the per-thread entry and ensure the local slot is populated.
    Trivial* warm = pool.Get();
    pool.Put(warm);

    for (auto _ : state) {
        Trivial* p = pool.Get();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
    }

    pool.Clear();
    benchmark::DoNotOptimize(created.load(std::memory_order_relaxed));
}
BENCHMARK(BM_ObjectPoolMap_GetPut_HotLocal)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

