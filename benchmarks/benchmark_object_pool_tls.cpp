/**
 * @file benchmark_object_pool_tls.cpp
 * @brief Microbenchmarks for ObjectPoolTLS (thread_local fast slot).
 */

#include <benchmark/benchmark.h>

#include <cstdint>

#include <lscq/object_pool.hpp>
#include <lscq/object_pool_tls.hpp>

namespace {

struct Item {
    std::uint64_t payload = 0;
};

}  // namespace

static void BM_ObjectPoolTLS_GetPut_Hit(benchmark::State& state) {
    lscq::ObjectPoolTLS<Item> pool([] { return new Item(); }, 1);

    // Warm: ensure TLS fast slot holds an object.
    Item* warm = pool.Get();
    pool.Put(warm);

    for (auto _ : state) {
        Item* p = pool.Get();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
    }

    pool.Clear();
}
BENCHMARK(BM_ObjectPoolTLS_GetPut_Hit)->Threads(1);

static void BM_ObjectPool_GetPut(benchmark::State& state) {
    lscq::ObjectPool<Item> pool([] { return new Item(); }, 1);

    Item* warm = pool.Get();
    pool.Put(warm);

    for (auto _ : state) {
        Item* p = pool.Get();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
    }

    pool.Clear();
}
BENCHMARK(BM_ObjectPool_GetPut)->Threads(1);

BENCHMARK_MAIN();
