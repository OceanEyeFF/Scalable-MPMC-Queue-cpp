#include <lscq/lscq.hpp>

#include <benchmark/benchmark.h>

static void bm_smoke(benchmark::State& state) {
  for (auto _ : state) {
    auto value = lscq::kEnableCas2;
    benchmark::DoNotOptimize(value);
  }
}
BENCHMARK(bm_smoke);
