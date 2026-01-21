// benchmark_stress.cpp - Independent stress test suite for high enqueue pressure scenarios
//
// This file contains 70E30D (70% enqueue, 30% dequeue) benchmark tests.
// Only unbounded/expandable queues are suitable for this extreme scenario:
// - MSQueue: Unbounded linked-list queue
// - LSCQ: Linked Scalable Circular Queue (can expand dynamically)
//
// Bounded queues (NCQ, SCQ, SCQP, MutexQueue) are NOT included here because
// they will fill up quickly and cause excessive spinning in the enqueue loop.

#include "benchmark_utils.hpp"

namespace {

// Stress test configuration
constexpr int kStressEnqueueRetryLimit = 10000;  // Max retries before giving up

// MSQueue stress test - truly unbounded, should handle 70E30D well
static void BM_MSQueue_70E30D_Stress(benchmark::State& state) {
    lscq_bench::pin_thread_index(state.thread_index());

    using Queue = lscq::MSQueue<lscq_bench::Value>;
    using ops = lscq_bench::QueueOps<Queue>;
    using ctx_t = lscq_bench::SharedContext<Queue>;
    using item_t = typename ops::item_type;

    static std::atomic<ctx_t*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new ctx_t(threads, lscq_bench::kSharedCapacity);

        // Larger prefill for stress test stability
        const std::size_t prefill = static_cast<std::size_t>(threads) * 500u;
        for (std::size_t i = 0; i < prefill; ++i) {
            const item_t it = ctx->make_item(0, static_cast<std::uint64_t>(i));
            (void)ops::enqueue(*ctx->q, it);
        }

        g_ctx.store(ctx, std::memory_order_release);
    }

    ctx_t* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    ctx->start.arrive_and_wait();

    lscq_bench::XorShift64Star rng(
        static_cast<std::uint64_t>(0x70E30DULL ^ (static_cast<std::uint64_t>(state.thread_index()) + 1u)));

    std::uint64_t seq = 0;
    std::uint64_t enqueue_failures = 0;

    for (auto _ : state) {
        const std::uint64_t r = rng.next();
        const int pct = static_cast<int>(r % 100u);
        if (pct < 70) {  // 70% enqueue
            const item_t it = ctx->make_item(state.thread_index(), seq++);
            int retries = 0;
            while (!ops::enqueue(*ctx->q, it)) {
                if (++retries > kStressEnqueueRetryLimit) {
                    ++enqueue_failures;
                    break;  // Safety valve
                }
                std::this_thread::yield();
            }
        } else {  // 30% dequeue
            item_t out{};
            (void)ops::dequeue(*ctx->q, out);
            benchmark::DoNotOptimize(out);
        }
    }

    ctx->finish.arrive_and_wait();

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(threads);
    lscq_bench::add_common_counters(state, threads, threads, total_ops);
    state.counters["enqueue_pct"] = benchmark::Counter(70.0, benchmark::Counter::kAvgThreads);
    state.counters["dequeue_pct"] = benchmark::Counter(30.0, benchmark::Counter::kAvgThreads);
    state.counters["enqueue_failures"] =
        benchmark::Counter(static_cast<double>(enqueue_failures), benchmark::Counter::kAvgThreads);

    ctx->finish.arrive_and_wait();
    if (state.thread_index() == 0) {
        delete ctx;
        g_ctx.store(nullptr, std::memory_order_release);
    }
}

// LSCQ stress test - linked structure, can expand dynamically
static void BM_LSCQ_70E30D_Stress(benchmark::State& state) {
    lscq_bench::pin_thread_index(state.thread_index());

    static std::atomic<lscq_bench::LSCQContext*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new lscq_bench::LSCQContext(threads, lscq_bench::kLSCQNodeScqsize);

        // Larger prefill for stress test stability
        const std::size_t prefill = static_cast<std::size_t>(threads) * 500u;
        for (std::size_t i = 0; i < prefill; ++i) {
            auto* p = &ctx->pool[i & ctx->pool_mask];
            (void)ctx->q.enqueue(p);
        }

        g_ctx.store(ctx, std::memory_order_release);
    }

    lscq_bench::LSCQContext* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    ctx->start.arrive_and_wait();

    lscq_bench::XorShift64Star rng(
        static_cast<std::uint64_t>(0x70E30DULL ^ (static_cast<std::uint64_t>(state.thread_index()) + 1u)));

    std::uint64_t enqueue_failures = 0;

    for (auto _ : state) {
        const std::uint64_t r = rng.next();
        const int pct = static_cast<int>(r % 100u);
        if (pct < 70) {  // 70% enqueue
            auto* p = &ctx->pool[static_cast<std::size_t>(r & ctx->pool_mask)];
            int retries = 0;
            while (!ctx->q.enqueue(p)) {
                if (++retries > kStressEnqueueRetryLimit) {
                    ++enqueue_failures;
                    break;  // Safety valve
                }
                std::this_thread::yield();
            }
        } else {  // 30% dequeue
            auto* out = ctx->q.dequeue();
            benchmark::DoNotOptimize(out);
            if (out != nullptr) {
                benchmark::DoNotOptimize(*out);
            }
        }
    }

    ctx->finish.arrive_and_wait();

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(threads);
    lscq_bench::add_common_counters(state, threads, threads, total_ops);
    state.counters["enqueue_pct"] = benchmark::Counter(70.0, benchmark::Counter::kAvgThreads);
    state.counters["dequeue_pct"] = benchmark::Counter(30.0, benchmark::Counter::kAvgThreads);
    state.counters["node_scqsize"] =
        benchmark::Counter(static_cast<double>(lscq_bench::kLSCQNodeScqsize), benchmark::Counter::kAvgThreads);
    state.counters["enqueue_failures"] =
        benchmark::Counter(static_cast<double>(enqueue_failures), benchmark::Counter::kAvgThreads);

    ctx->finish.arrive_and_wait();
    if (state.thread_index() == 0) {
        delete ctx;
        g_ctx.store(nullptr, std::memory_order_release);
    }
}

}  // namespace

static void apply_threads(benchmark::internal::Benchmark* b) {
    for (int t : lscq_bench::kThreadCounts) {
        b->Threads(t);
    }
    b->UseRealTime();
}

// Register stress test benchmarks
BENCHMARK(BM_MSQueue_70E30D_Stress)->Name("BM_MSQueue_70E30D_Stress")->Apply(apply_threads);
BENCHMARK(BM_LSCQ_70E30D_Stress)->Name("BM_LSCQ_70E30D_Stress")->Apply(apply_threads);
