/**
 * @file benchmark_object_pool_tls_v2.cpp
 * @brief Benchmarks for ObjectPoolTLS (v1) vs ObjectPoolTLSv2 (multi-slot TLS cache).
 */

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include <lscq/object_pool_tls.hpp>
#include <lscq/object_pool_tls_v2.hpp>

namespace {

struct Item {
    std::uint64_t payload = 0;
};

using Clock = std::chrono::steady_clock;

double Percentile(const std::vector<double>& sorted, double pct) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = (pct / 100.0) * static_cast<double>(sorted.size() - 1);
    const std::size_t index = static_cast<std::size_t>(position);
    const double fraction = position - static_cast<double>(index);
    if (index + 1 >= sorted.size()) {
        return sorted.back();
    }
    return sorted[index] * (1.0 - fraction) + sorted[index + 1] * fraction;
}

void PublishLatencyStats(benchmark::State& state, std::vector<double>& samples) {
    if (samples.empty()) {
        return;
    }
    std::sort(samples.begin(), samples.end());
    const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
                        static_cast<double>(samples.size());

    state.counters["latency_p50_ns"] = Percentile(samples, 50.0) * 1e9;
    state.counters["latency_p90_ns"] = Percentile(samples, 90.0) * 1e9;
    state.counters["latency_p99_ns"] = Percentile(samples, 99.0) * 1e9;
    state.counters["latency_mean_ns"] = mean * 1e9;
}

}  // namespace

static void BM_ObjectPoolTLSv1_Latency_FastSlot(benchmark::State& state) {
    lscq::ObjectPoolTLS<Item> pool([] { return new Item(); }, 1);

    Item* warm = pool.Get();
    pool.Put(warm);

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(state.iterations()));

    for (auto _ : state) {
        const auto start = Clock::now();
        Item* p = pool.Get();
        const auto end = Clock::now();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
        const double seconds = std::chrono::duration<double>(end - start).count();
        state.SetIterationTime(seconds);
        samples.push_back(seconds);
    }

    PublishLatencyStats(state, samples);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolTLSv1_Latency_FastSlot)->UseManualTime()->Iterations(5000)->Threads(1);

static void BM_ObjectPoolTLSv2_Latency_FastSlot(benchmark::State& state) {
    lscq::ObjectPoolTLSv2<Item> pool([] { return new Item(); });

    Item* warm = pool.Get();
    pool.Put(warm);

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(state.iterations()));

    for (auto _ : state) {
        const auto start = Clock::now();
        Item* p = pool.Get();
        const auto end = Clock::now();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
        const double seconds = std::chrono::duration<double>(end - start).count();
        state.SetIterationTime(seconds);
        samples.push_back(seconds);
    }

    PublishLatencyStats(state, samples);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolTLSv2_Latency_FastSlot)->UseManualTime()->Iterations(5000)->Threads(1);

static void BM_ObjectPoolTLSv2_Latency_BatchHit(benchmark::State& state) {
    lscq::ObjectPoolTLSv2<Item> pool([] { return new Item(); });

    Item* a = pool.Get();
    Item* b = pool.Get();
    pool.Put(a);
    pool.Put(b);

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(state.iterations()));

    for (auto _ : state) {
        Item* fast = pool.Get();
        const auto start = Clock::now();
        Item* p = pool.Get();
        const auto end = Clock::now();
        pool.Put(fast);
        pool.Put(p);

        const double seconds = std::chrono::duration<double>(end - start).count();
        state.SetIterationTime(seconds);
        samples.push_back(seconds);
    }

    PublishLatencyStats(state, samples);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolTLSv2_Latency_BatchHit)->UseManualTime()->Iterations(5000)->Threads(1);

static void BM_ObjectPoolTLSv1_Latency_SharedFallback(benchmark::State& state) {
    lscq::ObjectPoolTLS<Item> pool([] { return new Item(); }, 1);

    Item* warm = pool.Get();
    pool.Put(warm);

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(state.iterations()));

    for (auto _ : state) {
        Item* fast = pool.Get();
        const auto start = Clock::now();
        Item* p = pool.Get();
        const auto end = Clock::now();
        pool.Put(fast);
        pool.Put(p);

        const double seconds = std::chrono::duration<double>(end - start).count();
        state.SetIterationTime(seconds);
        samples.push_back(seconds);
    }

    PublishLatencyStats(state, samples);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolTLSv1_Latency_SharedFallback)->UseManualTime()->Iterations(5000)->Threads(1);

static void BM_ObjectPoolTLSv2_Latency_SharedFallback(benchmark::State& state) {
    lscq::ObjectPoolTLSv2<Item> pool([] { return new Item(); });

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(state.iterations()));

    for (auto _ : state) {
        state.PauseTiming();
        pool.Clear();
        state.ResumeTiming();

        const auto start = Clock::now();
        Item* p = pool.Get();
        const auto end = Clock::now();
        pool.Put(p);

        const double seconds = std::chrono::duration<double>(end - start).count();
        state.SetIterationTime(seconds);
        samples.push_back(seconds);
    }

    PublishLatencyStats(state, samples);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolTLSv2_Latency_SharedFallback)->UseManualTime()->Iterations(3000)->Threads(1);

static void BM_ObjectPoolTLSv1_Throughput(benchmark::State& state) {
    static lscq::ObjectPoolTLS<Item> pool([] { return new Item(); }, 1);

    for (auto _ : state) {
        Item* p = pool.Get();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
    }

    state.SetItemsProcessed(state.iterations() * state.threads() * 2);
}
BENCHMARK(BM_ObjectPoolTLSv1_Throughput)->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(12)->Threads(16)->Threads(24);

static void BM_ObjectPoolTLSv2_Throughput(benchmark::State& state) {
    static lscq::ObjectPoolTLSv2<Item> pool([] { return new Item(); });

    for (auto _ : state) {
        Item* p = pool.Get();
        benchmark::DoNotOptimize(p);
        p->payload++;
        pool.Put(p);
    }

    state.SetItemsProcessed(state.iterations() * state.threads() * 2);
}
BENCHMARK(BM_ObjectPoolTLSv2_Throughput)->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(12)->Threads(16)->Threads(24);

static void BM_ObjectPoolTLSv1_BatchGetPut(benchmark::State& state) {
    const std::size_t batch = static_cast<std::size_t>(state.range(0));
    lscq::ObjectPoolTLS<Item> pool([] { return new Item(); }, 1);
    std::vector<Item*> local(batch);

    for (auto _ : state) {
        for (std::size_t i = 0; i < batch; ++i) {
            local[i] = pool.Get();
        }
        for (std::size_t i = 0; i < batch; ++i) {
            pool.Put(local[i]);
        }
    }

    state.SetItemsProcessed(state.iterations() * batch * state.threads() * 2);
}
BENCHMARK(BM_ObjectPoolTLSv1_BatchGetPut)->Arg(8)->Arg(32)->Arg(128)->Threads(1);

static void BM_ObjectPoolTLSv2_BatchGetPut(benchmark::State& state) {
    const std::size_t batch = static_cast<std::size_t>(state.range(0));
    lscq::ObjectPoolTLSv2<Item> pool([] { return new Item(); });
    std::vector<Item*> local(batch);

    for (auto _ : state) {
        for (std::size_t i = 0; i < batch; ++i) {
            local[i] = pool.Get();
        }
        for (std::size_t i = 0; i < batch; ++i) {
            pool.Put(local[i]);
        }
    }

    state.SetItemsProcessed(state.iterations() * batch * state.threads() * 2);
}
BENCHMARK(BM_ObjectPoolTLSv2_BatchGetPut)->Arg(8)->Arg(32)->Arg(128)->Threads(1);

static void BM_ObjectPoolTLSv1_Burst(benchmark::State& state) {
    const std::size_t burst = static_cast<std::size_t>(state.range(0));
    lscq::ObjectPoolTLS<Item> pool([] { return new Item(); }, 1);
    std::vector<Item*> local(burst);

    for (auto _ : state) {
        for (std::size_t i = 0; i < burst; ++i) {
            local[i] = pool.Get();
        }
        for (std::size_t i = 0; i < burst; ++i) {
            pool.Put(local[i]);
        }
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * burst * state.threads() * 2);
}
BENCHMARK(BM_ObjectPoolTLSv1_Burst)->Arg(64)->Arg(256)->Threads(1);

static void BM_ObjectPoolTLSv2_Burst(benchmark::State& state) {
    const std::size_t burst = static_cast<std::size_t>(state.range(0));
    lscq::ObjectPoolTLSv2<Item> pool([] { return new Item(); });
    std::vector<Item*> local(burst);

    for (auto _ : state) {
        for (std::size_t i = 0; i < burst; ++i) {
            local[i] = pool.Get();
        }
        for (std::size_t i = 0; i < burst; ++i) {
            pool.Put(local[i]);
        }
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * burst * state.threads() * 2);
}
BENCHMARK(BM_ObjectPoolTLSv2_Burst)->Arg(64)->Arg(256)->Threads(1);

static void BM_ObjectPoolTLSv1_Clear(benchmark::State& state) {
    const std::size_t prefill = static_cast<std::size_t>(state.range(0));
    lscq::ObjectPoolTLS<Item> pool([] { return new Item(); }, 1);
    std::vector<Item*> local;
    local.reserve(prefill);

    for (auto _ : state) {
        state.PauseTiming();
        local.clear();
        for (std::size_t i = 0; i < prefill; ++i) {
            local.push_back(pool.Get());
        }
        for (Item* p : local) {
            pool.Put(p);
        }
        state.ResumeTiming();

        const auto start = Clock::now();
        pool.Clear();
        const auto end = Clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolTLSv1_Clear)->UseManualTime()->Iterations(200)->Arg(1024)->Threads(1);

static void BM_ObjectPoolTLSv2_Clear(benchmark::State& state) {
    const std::size_t prefill = static_cast<std::size_t>(state.range(0));
    lscq::ObjectPoolTLSv2<Item> pool([] { return new Item(); });
    std::vector<Item*> local;
    local.reserve(prefill);

    for (auto _ : state) {
        state.PauseTiming();
        local.clear();
        for (std::size_t i = 0; i < prefill; ++i) {
            local.push_back(pool.Get());
        }
        for (Item* p : local) {
            pool.Put(p);
        }
        state.ResumeTiming();

        const auto start = Clock::now();
        pool.Clear();
        const auto end = Clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolTLSv2_Clear)->UseManualTime()->Iterations(200)->Arg(1024)->Threads(1);

BENCHMARK_MAIN();
