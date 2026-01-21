/**
 * @file benchmark_lscq_simple.cpp
 * @brief Simplified LSCQ benchmark to isolate the hanging issue
 */

#include <benchmark/benchmark.h>

#include <lscq/ebr.hpp>
#include <lscq/lscq.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kPoolSize = 1u << 20;  // 1M elements
constexpr std::size_t kNodeScqsize = 4096;

struct TestContext {
    lscq::EBRManager ebr;
    lscq::LSCQ<std::uint64_t> q;
    std::vector<std::uint64_t> pool;
    std::uint64_t pool_mask;

    explicit TestContext(std::size_t node_scqsize)
        : ebr(), q(ebr, node_scqsize), pool(kPoolSize), pool_mask(kPoolSize - 1) {
        for (std::size_t i = 0; i < pool.size(); ++i) {
            pool[i] = i;
        }
    }
};

static void BM_LSCQ_Simple(benchmark::State& state) {
    // Each thread creates its own context (no shared static variable)
    static std::atomic<TestContext*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new TestContext(kNodeScqsize);

        // Prefill
        const std::size_t prefill = static_cast<std::size_t>(threads) * 100u;
        for (std::size_t i = 0; i < prefill; ++i) {
            auto* p = &ctx->pool[i & ctx->pool_mask];
            (void)ctx->q.enqueue(p);
        }

        g_ctx.store(ctx, std::memory_order_release);
    }

    TestContext* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    // Simple counter-based iteration instead of pool random access
    std::uint64_t local_idx = static_cast<std::uint64_t>(state.thread_index()) * 10000000;

    for (auto _ : state) {
        auto* item = &ctx->pool[(local_idx++) & ctx->pool_mask];

        // Enqueue with retry limit
        int enq_retries = 0;
        while (!ctx->q.enqueue(item)) {
            if (++enq_retries > 100000) {
                state.SkipWithError("Enqueue stuck");
                return;
            }
            std::this_thread::yield();
        }

        // Dequeue with retry limit
        std::uint64_t* out = nullptr;
        int deq_retries = 0;
        while ((out = ctx->q.dequeue()) == nullptr) {
            if (++deq_retries > 100000) {
                state.SkipWithError("Dequeue stuck");
                return;
            }
            std::this_thread::yield();
        }
        benchmark::DoNotOptimize(out);
    }

    // Cleanup
    if (state.thread_index() == 0) {
        // Wait a bit for other threads to finish
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        delete g_ctx.exchange(nullptr, std::memory_order_acq_rel);
    }
}

}  // namespace

BENCHMARK(BM_LSCQ_Simple)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

BENCHMARK_MAIN();
