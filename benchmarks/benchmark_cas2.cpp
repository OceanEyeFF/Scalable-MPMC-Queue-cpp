#include <lscq/cas2.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>

namespace {

inline void cas2_increment(lscq::Entry* value, lscq::Entry& expected) {
  while (true) {
    const lscq::Entry desired{expected.cycle_flags + 1u, expected.index_or_ptr + 1u};
    if (lscq::cas2(value, expected, desired)) {
      expected = desired;
      return;
    }
  }
}

}  // namespace

static void bm_cas2_single_thread(benchmark::State& state) {
  lscq::Entry value{0u, 0u};
  lscq::Entry expected{0u, 0u};

  for (auto _ : state) {
    cas2_increment(&value, expected);
    benchmark::DoNotOptimize(value);
  }

  state.counters["has_cas2_support"] = lscq::has_cas2_support() ? 1.0 : 0.0;
}
BENCHMARK(bm_cas2_single_thread);

static void bm_cas2_contended(benchmark::State& state) {
  static lscq::Entry shared{0u, 0u};

  lscq::Entry expected{0u, 0u};
  for (auto _ : state) {
    cas2_increment(&shared, expected);
  }

  benchmark::DoNotOptimize(expected);
  state.counters["threads"] =
      benchmark::Counter(static_cast<double>(state.threads()), benchmark::Counter::kAvgThreads);
  state.counters["has_cas2_support"] =
      benchmark::Counter(lscq::has_cas2_support() ? 1.0 : 0.0, benchmark::Counter::kAvgThreads);
}
BENCHMARK(bm_cas2_contended)->Threads(2);
BENCHMARK(bm_cas2_contended)->Threads(4);
BENCHMARK(bm_cas2_contended)->Threads(8);
