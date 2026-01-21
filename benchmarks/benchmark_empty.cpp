#include "benchmark_utils.hpp"

namespace {

template <class Queue>
static void BM_EmptyQueue(benchmark::State& state) {
    lscq_bench::pin_thread_index(state.thread_index());

    using ops = lscq_bench::QueueOps<Queue>;
    using ctx_t = lscq_bench::SharedContext<Queue>;
    using item_t = typename ops::item_type;

    static std::atomic<ctx_t*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new ctx_t(threads, lscq_bench::kSharedCapacity);
        g_ctx.store(ctx, std::memory_order_release);
    }

    ctx_t* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    ctx->start.arrive_and_wait();

    for (auto _ : state) {
        item_t out{};
        (void)ops::dequeue(*ctx->q, out);
        benchmark::DoNotOptimize(out);
        if constexpr (ops::kPointerQueue) {
            if (out != nullptr) {
                benchmark::DoNotOptimize(*out);
            }
        }
    }

    ctx->finish.arrive_and_wait();

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(threads);
    lscq_bench::add_common_counters(state, 0, threads, total_ops);

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

static void BM_LSCQ_EmptyQueue(benchmark::State& state) {
    lscq_bench::pin_thread_index(state.thread_index());

    static std::atomic<lscq_bench::LSCQContext*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new lscq_bench::LSCQContext(threads, lscq_bench::kLSCQNodeScqsize);
        g_ctx.store(ctx, std::memory_order_release);
    }

    lscq_bench::LSCQContext* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    ctx->start.arrive_and_wait();

    for (auto _ : state) {
        auto* out = ctx->q.dequeue();
        benchmark::DoNotOptimize(out);
        if (out != nullptr) {
            benchmark::DoNotOptimize(*out);
        }
    }

    ctx->finish.arrive_and_wait();

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(threads);
    lscq_bench::add_common_counters(state, 0, threads, total_ops);
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

BENCHMARK(BM_EmptyQueue<lscq::NCQ<lscq_bench::Value>>)->Name("BM_NCQ_EmptyQueue")->Apply(apply_threads);
BENCHMARK(BM_EmptyQueue<lscq::SCQ<lscq_bench::Value>>)->Name("BM_SCQ_EmptyQueue")->Apply(apply_threads);
BENCHMARK(BM_EmptyQueue<lscq::SCQP<lscq_bench::Value>>)->Name("BM_SCQP_EmptyQueue")->Apply(apply_threads);
BENCHMARK(BM_LSCQ_EmptyQueue)->Name("BM_LSCQ_EmptyQueue")->Apply(apply_threads);
BENCHMARK(BM_EmptyQueue<lscq::MSQueue<lscq_bench::Value>>)->Name("BM_MSQueue_EmptyQueue")->Apply(apply_threads);
BENCHMARK(BM_EmptyQueue<lscq::MutexQueue<lscq_bench::Value>>)->Name("BM_MutexQueue_EmptyQueue")->Apply(apply_threads);
