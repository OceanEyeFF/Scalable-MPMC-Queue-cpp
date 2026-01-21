#include <lscq/lscq.hpp>
#include <lscq/msqueue.hpp>
#include <lscq/mutex_queue.hpp>
#include <lscq/ncq.hpp>
#include <lscq/scq.hpp>
#include <lscq/scqp.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

constexpr std::size_t kEntryBytes = sizeof(lscq::Entry);

constexpr bool is_power_of_two(std::size_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

constexpr std::size_t round_up_pow2(std::size_t v) noexcept {
    if (v <= 1) {
        return 1;
    }
    if (is_power_of_two(v)) {
        return v;
    }
    std::size_t out = 1;
    while (out < v) {
        out <<= 1;
    }
    return out;
}

constexpr std::size_t round_up(std::size_t v, std::size_t alignment) noexcept {
    return (v + (alignment - 1)) / alignment * alignment;
}

constexpr std::size_t scq_scqsize_for_effective(std::size_t effective_capacity) noexcept {
    std::size_t scqsize = effective_capacity * 2;
    if (scqsize < 4) {
        scqsize = 4;
    }
    return round_up_pow2(scqsize);
}

constexpr std::size_t scq_effective_for_scqsize(std::size_t scqsize) noexcept { return scqsize / 2; }

constexpr std::size_t ncq_capacity_rounded(std::size_t capacity) noexcept {
    constexpr std::size_t entries_per_line = 64 / sizeof(lscq::Entry);  // 4
    std::size_t cap = capacity;
    if (cap == 0) {
        cap = 1;
    }
    if (cap < entries_per_line) {
        cap = entries_per_line;
    }
    return round_up(cap, entries_per_line);
}

inline std::size_t scqp_bytes_for_scqsize(std::size_t scqsize, bool using_fallback) {
    const std::size_t s = round_up_pow2((scqsize < 4) ? 4 : scqsize);
    if (using_fallback) {
        return s * sizeof(lscq::Entry) + s * sizeof(void*);
    }
    return s * sizeof(lscq::SCQP<std::uint64_t>::EntryP);
}

static void BM_NCQ_MemoryEfficiency(benchmark::State& state) {
    const std::size_t capacity = static_cast<std::size_t>(state.range(0));
    const std::size_t rounded = ncq_capacity_rounded(capacity);
    std::size_t bytes = rounded * kEntryBytes;

    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
    }

    state.counters["capacity"] = static_cast<double>(capacity);
    state.counters["capacity_rounded"] = static_cast<double>(rounded);
    state.counters["estimated_bytes"] = static_cast<double>(bytes);
    state.counters["estimated_mb"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static void BM_SCQ_MemoryEfficiency(benchmark::State& state) {
    const std::size_t effective = static_cast<std::size_t>(state.range(0));
    const std::size_t scqsize = scq_scqsize_for_effective(effective);
    const std::size_t qsize = scq_effective_for_scqsize(scqsize);
    std::size_t bytes = scqsize * kEntryBytes;

    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
    }

    state.counters["qsize_req"] = static_cast<double>(effective);
    state.counters["qsize"] = static_cast<double>(qsize);
    state.counters["scqsize"] = static_cast<double>(scqsize);
    state.counters["estimated_bytes"] = static_cast<double>(bytes);
    state.counters["estimated_mb"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static void BM_SCQP_MemoryEfficiency(benchmark::State& state) {
    const std::size_t effective = static_cast<std::size_t>(state.range(0));
    const std::size_t scqsize = scq_scqsize_for_effective(effective);
    const std::size_t qsize = scq_effective_for_scqsize(scqsize);
    const bool using_fallback = !lscq::has_cas2_support();
    std::size_t bytes = scqp_bytes_for_scqsize(scqsize, using_fallback);

    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
    }

    state.counters["qsize_req"] = static_cast<double>(effective);
    state.counters["qsize"] = static_cast<double>(qsize);
    state.counters["scqsize"] = static_cast<double>(scqsize);
    state.counters["using_fallback"] = using_fallback ? 1.0 : 0.0;
    state.counters["estimated_bytes"] = static_cast<double>(bytes);
    state.counters["estimated_mb"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static void BM_LSCQ_MemoryEfficiency(benchmark::State& state) {
    // Interpret range(0) as target live elements.
    const std::size_t elements = static_cast<std::size_t>(state.range(0));

    constexpr std::size_t node_scqsize = 1u << 12;  // keep consistent with benchmark_pair/mixed
    const std::size_t node_qsize = node_scqsize / 2;
    const bool using_fallback = !lscq::has_cas2_support();
    const std::size_t per_node_bytes =
        sizeof(lscq::LSCQ<std::uint64_t>::Node) + scqp_bytes_for_scqsize(node_scqsize, using_fallback);

    const std::size_t nodes = (elements + node_qsize - 1) / node_qsize;
    std::size_t bytes = nodes * per_node_bytes;

    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
    }

    state.counters["elements"] = static_cast<double>(elements);
    state.counters["node_scqsize"] = static_cast<double>(node_scqsize);
    state.counters["node_qsize"] = static_cast<double>(node_qsize);
    state.counters["nodes"] = static_cast<double>(nodes);
    state.counters["using_fallback"] = using_fallback ? 1.0 : 0.0;
    state.counters["estimated_bytes"] = static_cast<double>(bytes);
    state.counters["estimated_mb"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static void BM_MSQueue_MemoryEfficiency(benchmark::State& state) {
    const std::size_t elements = static_cast<std::size_t>(state.range(0));
    const std::size_t node_size = lscq::MSQueue<std::uint64_t>::node_size_bytes();
    std::size_t bytes = (elements + 1) * node_size;

    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
    }

    state.counters["elements"] = static_cast<double>(elements);
    state.counters["node_size"] = static_cast<double>(node_size);
    state.counters["estimated_bytes"] = static_cast<double>(bytes);
    state.counters["estimated_mb"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static void BM_MutexQueue_MemoryEfficiency(benchmark::State& state) {
    const std::size_t elements = static_cast<std::size_t>(state.range(0));
    // Conservative estimate for std::queue<std::uint64_t> backing allocations.
    constexpr std::size_t per_element = sizeof(std::uint64_t) + 2 * sizeof(void*);
    std::size_t bytes = elements * per_element + sizeof(lscq::MutexQueue<std::uint64_t>);

    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
    }

    state.counters["elements"] = static_cast<double>(elements);
    state.counters["estimated_bytes"] = static_cast<double>(bytes);
    state.counters["estimated_mb"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void apply_memory_args(benchmark::internal::Benchmark* b) {
    const std::size_t sizes[] = {1u << 10, 1u << 12, 1u << 14, 1u << 16, 1u << 18, 1u << 20, 1u << 22};
    for (std::size_t s : sizes) {
        b->Arg(static_cast<std::int64_t>(s));
    }
    b->Iterations(1);
    b->UseRealTime();
}

}  // namespace

BENCHMARK(BM_NCQ_MemoryEfficiency)->Name("BM_NCQ_MemoryEfficiency")->Apply(apply_memory_args);
BENCHMARK(BM_SCQ_MemoryEfficiency)->Name("BM_SCQ_MemoryEfficiency")->Apply(apply_memory_args);
BENCHMARK(BM_SCQP_MemoryEfficiency)->Name("BM_SCQP_MemoryEfficiency")->Apply(apply_memory_args);
BENCHMARK(BM_LSCQ_MemoryEfficiency)->Name("BM_LSCQ_MemoryEfficiency")->Apply(apply_memory_args);
BENCHMARK(BM_MSQueue_MemoryEfficiency)->Name("BM_MSQueue_MemoryEfficiency")->Apply(apply_memory_args);
BENCHMARK(BM_MutexQueue_MemoryEfficiency)->Name("BM_MutexQueue_MemoryEfficiency")->Apply(apply_memory_args);
