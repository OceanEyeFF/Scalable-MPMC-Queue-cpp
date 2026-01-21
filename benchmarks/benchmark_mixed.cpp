#include "benchmark_utils.hpp"

namespace {

template <class Queue, int EnqueuePct>
static void BM_Mixed(benchmark::State& state) {
    lscq_bench::pin_thread_index(state.thread_index());

    static_assert(EnqueuePct >= 0 && EnqueuePct <= 100);

    using ops = lscq_bench::QueueOps<Queue>;
    using ctx_t = lscq_bench::SharedContext<Queue>;
    using item_t = typename ops::item_type;

    static std::atomic<ctx_t*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new ctx_t(threads, lscq_bench::kSharedCapacity);

        const std::size_t prefill = static_cast<std::size_t>(threads) * 100u;
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
        static_cast<std::uint64_t>(0x9e3779b97f4a7c15ULL ^ (static_cast<std::uint64_t>(state.thread_index()) + 1u) ^
                                   (static_cast<std::uint64_t>(EnqueuePct) << 32u)));

    std::uint64_t seq = 0;
    for (auto _ : state) {
        const std::uint64_t r = rng.next();
        const int pct = static_cast<int>(r % 100u);
        if (pct < EnqueuePct) {
            const item_t it = ctx->make_item(state.thread_index(), seq++);
            while (!ops::enqueue(*ctx->q, it)) {
                std::this_thread::yield();
            }
        } else {
            item_t out{};
            (void)ops::dequeue(*ctx->q, out);
            benchmark::DoNotOptimize(out);
            if constexpr (ops::kPointerQueue) {
                if (out != nullptr) {
                    benchmark::DoNotOptimize(*out);
                }
            }
        }
    }

    ctx->finish.arrive_and_wait();

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(threads);
    lscq_bench::add_common_counters(state, threads, threads, total_ops);
    state.counters["enqueue_pct"] =
        benchmark::Counter(static_cast<double>(EnqueuePct), benchmark::Counter::kAvgThreads);
    state.counters["dequeue_pct"] =
        benchmark::Counter(static_cast<double>(100 - EnqueuePct), benchmark::Counter::kAvgThreads);

    if constexpr (std::is_same_v<Queue, lscq::NCQ<lscq_bench::Value>>) {
        state.counters["capacity"] =
            benchmark::Counter(static_cast<double>(lscq_bench::kSharedCapacity),
                               benchmark::Counter::kAvgThreads);
    } else if constexpr (std::is_same_v<Queue, lscq::SCQ<lscq_bench::Value>>) {
        state.counters["scqsize"] =
            benchmark::Counter(static_cast<double>(ctx->q->scqsize()), benchmark::Counter::kAvgThreads);
        state.counters["qsize"] =
            benchmark::Counter(static_cast<double>(ctx->q->qsize()), benchmark::Counter::kAvgThreads);
        state.counters["has_cas2_support"] =
            benchmark::Counter(lscq::has_cas2_support() ? 1.0 : 0.0, benchmark::Counter::kAvgThreads);
    } else if constexpr (std::is_same_v<Queue, lscq::SCQP<lscq_bench::Value>>) {
        state.counters["scqsize"] =
            benchmark::Counter(static_cast<double>(ctx->q->scqsize()), benchmark::Counter::kAvgThreads);
        state.counters["qsize"] =
            benchmark::Counter(static_cast<double>(ctx->q->qsize()), benchmark::Counter::kAvgThreads);
        state.counters["using_fallback"] =
            benchmark::Counter(ctx->q->is_using_fallback() ? 1.0 : 0.0, benchmark::Counter::kAvgThreads);
        state.counters["has_cas2_support"] =
            benchmark::Counter(lscq::has_cas2_support() ? 1.0 : 0.0, benchmark::Counter::kAvgThreads);
    }

    ctx->finish.arrive_and_wait();
    if (state.thread_index() == 0) {
        delete ctx;
        g_ctx.store(nullptr, std::memory_order_release);
    }
}

template <int EnqueuePct>
static void BM_LSCQ_Mixed(benchmark::State& state) {
    lscq_bench::pin_thread_index(state.thread_index());

    static_assert(EnqueuePct >= 0 && EnqueuePct <= 100);

    static std::atomic<lscq_bench::LSCQContext*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new lscq_bench::LSCQContext(threads, lscq_bench::kLSCQNodeScqsize);

        const std::size_t prefill = static_cast<std::size_t>(threads) * 100u;
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
        static_cast<std::uint64_t>(0xfeedbeefULL ^ (static_cast<std::uint64_t>(state.thread_index()) + 1u) ^
                                   (static_cast<std::uint64_t>(EnqueuePct) << 32u)));

    for (auto _ : state) {
        const std::uint64_t r = rng.next();
        const int pct = static_cast<int>(r % 100u);
        if (pct < EnqueuePct) {
            auto* p = &ctx->pool[static_cast<std::size_t>(r & ctx->pool_mask)];
            while (!ctx->q.enqueue(p)) {
                std::this_thread::yield();
            }
        } else {
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
    state.counters["enqueue_pct"] =
        benchmark::Counter(static_cast<double>(EnqueuePct), benchmark::Counter::kAvgThreads);
    state.counters["dequeue_pct"] =
        benchmark::Counter(static_cast<double>(100 - EnqueuePct), benchmark::Counter::kAvgThreads);
    state.counters["node_scqsize"] =
        benchmark::Counter(static_cast<double>(lscq_bench::kLSCQNodeScqsize), benchmark::Counter::kAvgThreads);

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

// 50E50D
BENCHMARK(BM_Mixed<lscq::NCQ<lscq_bench::Value>, 50>)->Name("BM_NCQ_50E50D")->Apply(apply_threads);
BENCHMARK(BM_Mixed<lscq::SCQ<lscq_bench::Value>, 50>)->Name("BM_SCQ_50E50D")->Apply(apply_threads);
BENCHMARK(BM_Mixed<lscq::SCQP<lscq_bench::Value>, 50>)->Name("BM_SCQP_50E50D")->Apply(apply_threads);
BENCHMARK(BM_LSCQ_Mixed<50>)->Name("BM_LSCQ_50E50D")->Apply(apply_threads);
BENCHMARK(BM_Mixed<lscq::MSQueue<lscq_bench::Value>, 50>)->Name("BM_MSQueue_50E50D")->Apply(apply_threads);
BENCHMARK(BM_Mixed<lscq::MutexQueue<lscq_bench::Value>, 50>)->Name("BM_MutexQueue_50E50D")->Apply(apply_threads);

// 30E70D
BENCHMARK(BM_Mixed<lscq::NCQ<lscq_bench::Value>, 30>)->Name("BM_NCQ_30E70D")->Apply(apply_threads);
BENCHMARK(BM_Mixed<lscq::SCQ<lscq_bench::Value>, 30>)->Name("BM_SCQ_30E70D")->Apply(apply_threads);
BENCHMARK(BM_Mixed<lscq::SCQP<lscq_bench::Value>, 30>)->Name("BM_SCQP_30E70D")->Apply(apply_threads);
BENCHMARK(BM_LSCQ_Mixed<30>)->Name("BM_LSCQ_30E70D")->Apply(apply_threads);
BENCHMARK(BM_Mixed<lscq::MSQueue<lscq_bench::Value>, 30>)->Name("BM_MSQueue_30E70D")->Apply(apply_threads);
BENCHMARK(BM_Mixed<lscq::MutexQueue<lscq_bench::Value>, 30>)->Name("BM_MutexQueue_30E70D")->Apply(apply_threads);

// 70E30D - Moved to benchmark_stress.cpp (independent stress test suite)
// Only unbounded queues (MSQueue, LSCQ) are suitable for high enqueue pressure scenarios
