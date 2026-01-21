#include "benchmark_utils.hpp"

namespace {

template <class Queue>
static void BM_Pair(benchmark::State& state) {
    lscq_bench::pin_thread_index(state.thread_index());

    using ops = lscq_bench::QueueOps<Queue>;
    using ctx_t = lscq_bench::SharedContext<Queue>;
    using item_t = typename ops::item_type;

    static std::atomic<ctx_t*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new ctx_t(threads, lscq_bench::kSharedCapacity);

        // Prefill to reduce initial empty effects (paper-like warm start).
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

    std::uint64_t seq = 0;
    for (auto _ : state) {
        const item_t it = ctx->make_item(state.thread_index(), seq++);
        while (!ops::enqueue(*ctx->q, it)) {
            std::this_thread::yield();
        }

        item_t out{};
        while (!ops::dequeue(*ctx->q, out)) {
            std::this_thread::yield();
        }
        benchmark::DoNotOptimize(out);
        if constexpr (ops::kPointerQueue) {
            benchmark::DoNotOptimize(*out);
        }
    }

    ctx->finish.arrive_and_wait();

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(threads) * 2u;
    lscq_bench::add_common_counters(state, threads, threads, total_ops);

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

static void BM_LSCQ_Pair(benchmark::State& state) {
    lscq_bench::pin_thread_index(state.thread_index());

    using item_t = lscq_bench::Value*;

    // Use a simpler context without CyclicBarrier for LSCQ
    // CyclicBarrier is incompatible with Google Benchmark's iteration control
    struct SimpleLSCQContext {
        lscq::EBRManager ebr;
        lscq::LSCQ<lscq_bench::Value> q;
        std::vector<lscq_bench::Value> pool;
        std::uint64_t pool_mask;

        explicit SimpleLSCQContext(std::size_t node_scqsize)
            : ebr(),
              q(ebr, node_scqsize),
              pool(lscq_bench::kPointerPoolSize),
              pool_mask(static_cast<std::uint64_t>(pool.size() - 1)) {
            for (std::size_t i = 0; i < pool.size(); ++i) {
                pool[i] = static_cast<lscq_bench::Value>(i);
            }
        }
    };

    static std::atomic<SimpleLSCQContext*> g_ctx{nullptr};

    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* ctx = new SimpleLSCQContext(lscq_bench::kLSCQNodeScqsize);

        const std::size_t prefill = static_cast<std::size_t>(threads) * 100u;
        for (std::size_t i = 0; i < prefill; ++i) {
            auto* p = &ctx->pool[i & ctx->pool_mask];
            (void)ctx->q.enqueue(p);
        }

        g_ctx.store(ctx, std::memory_order_release);
    }

    SimpleLSCQContext* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    // Simple counter-based iteration instead of random access
    std::uint64_t local_idx = static_cast<std::uint64_t>(state.thread_index()) * 10000000;

    for (auto _ : state) {
        item_t it = &ctx->pool[(local_idx++) & ctx->pool_mask];

        // Enqueue with retry limit
        int enq_retries = 0;
        while (!ctx->q.enqueue(it)) {
            if (++enq_retries > 100000) {
                state.SkipWithError("LSCQ enqueue stuck");
                return;
            }
            std::this_thread::yield();
        }

        // Dequeue with retry limit
        item_t out = nullptr;
        int deq_retries = 0;
        while ((out = ctx->q.dequeue()) == nullptr) {
            if (++deq_retries > 100000) {
                state.SkipWithError("LSCQ dequeue stuck");
                return;
            }
            std::this_thread::yield();
        }
        benchmark::DoNotOptimize(out);
        benchmark::DoNotOptimize(*out);
    }

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(state.iterations()) * static_cast<std::uint64_t>(threads) * 2u;
    lscq_bench::add_common_counters(state, threads, threads, total_ops);
    state.counters["node_scqsize"] =
        benchmark::Counter(static_cast<double>(lscq_bench::kLSCQNodeScqsize),
                           benchmark::Counter::kAvgThreads);

    if (state.thread_index() == 0) {
        // Wait a bit for other threads to finish their iterations
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        delete g_ctx.exchange(nullptr, std::memory_order_acq_rel);
    }
}

}  // namespace

static void apply_threads(benchmark::internal::Benchmark* b) {
    for (int t : lscq_bench::kThreadCounts) {
        b->Threads(t);
    }
    b->UseRealTime();
}

BENCHMARK(BM_Pair<lscq::NCQ<lscq_bench::Value>>)->Name("BM_NCQ_Pair")->Apply(apply_threads);
BENCHMARK(BM_Pair<lscq::SCQ<lscq_bench::Value>>)->Name("BM_SCQ_Pair")->Apply(apply_threads);
BENCHMARK(BM_Pair<lscq::SCQP<lscq_bench::Value>>)->Name("BM_SCQP_Pair")->Apply(apply_threads);
BENCHMARK(BM_LSCQ_Pair)->Name("BM_LSCQ_Pair")->Apply(apply_threads);
BENCHMARK(BM_Pair<lscq::MSQueue<lscq_bench::Value>>)->Name("BM_MSQueue_Pair")->Apply(apply_threads);
BENCHMARK(BM_Pair<lscq::MutexQueue<lscq_bench::Value>>)->Name("BM_MutexQueue_Pair")->Apply(apply_threads);
