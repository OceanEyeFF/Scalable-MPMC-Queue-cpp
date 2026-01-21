#include <lscq/scqp.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kOpsPerThread = 1'000'000;
constexpr std::size_t kMultiCapacity = 1u << 22;

class CyclicBarrier {
public:
    explicit CyclicBarrier(int parties) : parties_(parties), arrived_(0), generation_(0) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mu_);
        const std::size_t gen = generation_;
        if (++arrived_ == static_cast<std::size_t>(parties_)) {
            arrived_ = 0;
            ++generation_;
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [&] { return generation_ != gen; });
    }

private:
    int parties_;
    std::size_t arrived_;
    std::size_t generation_;
    std::mutex mu_;
    std::condition_variable cv_;
};

inline void add_common_counters(benchmark::State& state, int producers, int consumers,
                                std::uint64_t total_ops) {
    state.counters["threads"] =
        benchmark::Counter(static_cast<double>(state.threads()), benchmark::Counter::kAvgThreads);
    state.counters["producers"] =
        benchmark::Counter(static_cast<double>(producers), benchmark::Counter::kAvgThreads);
    state.counters["consumers"] =
        benchmark::Counter(static_cast<double>(consumers), benchmark::Counter::kAvgThreads);
    state.counters["ops_per_thread"] =
        benchmark::Counter(static_cast<double>(kOpsPerThread), benchmark::Counter::kAvgThreads);
    state.counters["total_ops"] =
        benchmark::Counter(static_cast<double>(total_ops), benchmark::Counter::kAvgThreads);

    state.counters["Mops"] =
        benchmark::Counter(static_cast<double>(total_ops) / 1e6, benchmark::Counter::kIsRate);
}

inline std::size_t scqsize_for_effective_capacity(std::size_t effective_capacity) {
    // Mirror SCQ: QSIZE = SCQSIZE / 2.
    return 2 * effective_capacity;
}

}  // namespace

static void BM_SCQP_Pair(benchmark::State& state) {
    using Value = std::uint64_t;

    const int threads = static_cast<int>(state.threads());
    const int producers = (threads == 1) ? 1 : (threads / 2);
    const int consumers = (threads == 1) ? 0 : (threads / 2);

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(kOpsPerThread);
    add_common_counters(state, producers, consumers, total_ops);
    state.counters["has_cas2_support"] = benchmark::Counter(lscq::has_cas2_support() ? 1.0 : 0.0,
                                                           benchmark::Counter::kAvgThreads);

    if (threads == 1) {
        for (auto _ : state) {
            state.PauseTiming();
            const std::size_t effective_capacity = kOpsPerThread + 1024;
            const std::size_t scqsize = scqsize_for_effective_capacity(effective_capacity);
            lscq::SCQP<Value> q(scqsize);
            state.counters["scqsize"] =
                benchmark::Counter(static_cast<double>(scqsize), benchmark::Counter::kAvgThreads);
            state.counters["qsize"] = benchmark::Counter(static_cast<double>(effective_capacity),
                                                        benchmark::Counter::kAvgThreads);
            state.counters["using_fallback"] = benchmark::Counter(q.is_using_fallback() ? 1.0 : 0.0,
                                                                 benchmark::Counter::kAvgThreads);
            std::vector<Value> pool(1u << 20);
            for (std::size_t i = 0; i < pool.size(); ++i) {
                pool[i] = static_cast<Value>(i);
            }
            const std::uint64_t mask = static_cast<std::uint64_t>(pool.size() - 1);
            state.ResumeTiming();

            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                auto* p = &pool[static_cast<std::size_t>(static_cast<std::uint64_t>(i) & mask)];
                while (!q.enqueue(p)) {
                }
            }

            state.PauseTiming();
        }
        return;
    }

    struct Context {
        explicit Context(int threads, std::size_t scqsize, std::size_t qsize)
            : q(std::make_unique<lscq::SCQP<Value>>(scqsize)),
              start(threads),
              finish(threads),
              scqsize(scqsize),
              qsize(qsize),
              pool(1u << 20) {
            for (std::size_t i = 0; i < pool.size(); ++i) {
                pool[i] = static_cast<Value>(i);
            }
        }

        std::unique_ptr<lscq::SCQP<Value>> q;
        CyclicBarrier start;
        CyclicBarrier finish;
        std::size_t scqsize;
        std::size_t qsize;
        std::vector<Value> pool;
    };

    static std::atomic<Context*> g_ctx{nullptr};

    if (state.thread_index() == 0) {
        const std::size_t qsize = static_cast<std::size_t>(producers) * kOpsPerThread + 1024;
        const std::size_t scqsize = scqsize_for_effective_capacity(qsize);
        auto* ctx = new Context(threads, scqsize, qsize);
        g_ctx.store(ctx, std::memory_order_release);
    }

    Context* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    state.counters["scqsize"] =
        benchmark::Counter(static_cast<double>(ctx->scqsize), benchmark::Counter::kAvgThreads);
    state.counters["qsize"] =
        benchmark::Counter(static_cast<double>(ctx->qsize), benchmark::Counter::kAvgThreads);
    state.counters["using_fallback"] = benchmark::Counter(ctx->q->is_using_fallback() ? 1.0 : 0.0,
                                                         benchmark::Counter::kAvgThreads);

    const std::uint64_t mask = static_cast<std::uint64_t>(ctx->pool.size() - 1);

    for (auto _ : state) {
        ctx->start.arrive_and_wait();

        if (state.thread_index() < producers) {
            const std::uint64_t base =
                static_cast<std::uint64_t>(state.thread_index()) * kOpsPerThread;
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                auto* p = &ctx->pool[static_cast<std::size_t>((base + i) & mask)];
                while (!ctx->q->enqueue(p)) {
                }
            }
        } else {
            std::size_t done = 0;
            while (done < kOpsPerThread) {
                auto* p = ctx->q->dequeue();
                if (p == nullptr) {
                    continue;
                }
                benchmark::DoNotOptimize(p);
                benchmark::DoNotOptimize(*p);
                ++done;
            }
        }

        ctx->finish.arrive_and_wait();
    }

    ctx->finish.arrive_and_wait();
    if (state.thread_index() == 0) {
        delete ctx;
        g_ctx.store(nullptr, std::memory_order_release);
    }
}

static void BM_SCQP_MultiEnqueue(benchmark::State& state) {
    using Value = std::uint64_t;

    const int threads = static_cast<int>(state.threads());
    const int producers = threads - 1;
    const int consumers = 1;
    const std::uint64_t total_enqueues =
        static_cast<std::uint64_t>(producers) * static_cast<std::uint64_t>(kOpsPerThread);
    const std::uint64_t total_ops = 2 * total_enqueues;

    add_common_counters(state, producers, consumers, total_ops);
    state.counters["has_cas2_support"] = benchmark::Counter(lscq::has_cas2_support() ? 1.0 : 0.0,
                                                           benchmark::Counter::kAvgThreads);

    struct Context {
        explicit Context(int threads, std::size_t scqsize, std::size_t qsize)
            : q(std::make_unique<lscq::SCQP<Value>>(scqsize)),
              start(threads),
              finish(threads),
              scqsize(scqsize),
              qsize(qsize),
              pool(1u << 20) {
            for (std::size_t i = 0; i < pool.size(); ++i) {
                pool[i] = static_cast<Value>(i);
            }
        }

        std::unique_ptr<lscq::SCQP<Value>> q;
        CyclicBarrier start;
        CyclicBarrier finish;
        std::size_t scqsize;
        std::size_t qsize;
        std::vector<Value> pool;
    };

    static std::atomic<Context*> g_ctx{nullptr};

    if (state.thread_index() == 0) {
        const std::size_t qsize = kMultiCapacity;
        const std::size_t scqsize = scqsize_for_effective_capacity(qsize);
        auto* ctx = new Context(threads, scqsize, qsize);
        g_ctx.store(ctx, std::memory_order_release);
    }

    Context* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    state.counters["scqsize"] =
        benchmark::Counter(static_cast<double>(ctx->scqsize), benchmark::Counter::kAvgThreads);
    state.counters["qsize"] =
        benchmark::Counter(static_cast<double>(ctx->qsize), benchmark::Counter::kAvgThreads);
    state.counters["using_fallback"] = benchmark::Counter(ctx->q->is_using_fallback() ? 1.0 : 0.0,
                                                         benchmark::Counter::kAvgThreads);

    const std::uint64_t mask = static_cast<std::uint64_t>(ctx->pool.size() - 1);

    for (auto _ : state) {
        ctx->start.arrive_and_wait();

        if (state.thread_index() < producers) {
            const std::uint64_t base =
                static_cast<std::uint64_t>(state.thread_index()) * kOpsPerThread;
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                auto* p = &ctx->pool[static_cast<std::size_t>((base + i) & mask)];
                while (!ctx->q->enqueue(p)) {
                }
            }
        } else {
            std::size_t done = 0;
            while (done < static_cast<std::size_t>(total_enqueues)) {
                auto* p = ctx->q->dequeue();
                if (p == nullptr) {
                    std::this_thread::yield();
                    continue;
                }
                benchmark::DoNotOptimize(p);
                benchmark::DoNotOptimize(*p);
                ++done;
            }
        }

        ctx->finish.arrive_and_wait();
    }

    ctx->finish.arrive_and_wait();
    if (state.thread_index() == 0) {
        delete ctx;
        g_ctx.store(nullptr, std::memory_order_release);
    }
}

static void BM_SCQP_MultiDequeue(benchmark::State& state) {
    using Value = std::uint64_t;

    const int threads = static_cast<int>(state.threads());
    const int producers = 1;
    const int consumers = threads - 1;
    const std::uint64_t total_dequeues =
        static_cast<std::uint64_t>(consumers) * static_cast<std::uint64_t>(kOpsPerThread);
    const std::uint64_t total_ops = 2 * total_dequeues;

    add_common_counters(state, producers, consumers, total_ops);
    state.counters["has_cas2_support"] = benchmark::Counter(lscq::has_cas2_support() ? 1.0 : 0.0,
                                                           benchmark::Counter::kAvgThreads);

    struct Context {
        explicit Context(int threads, std::size_t scqsize, std::size_t qsize)
            : q(std::make_unique<lscq::SCQP<Value>>(scqsize)),
              start(threads),
              finish(threads),
              scqsize(scqsize),
              qsize(qsize),
              pool(1u << 20) {
            for (std::size_t i = 0; i < pool.size(); ++i) {
                pool[i] = static_cast<Value>(i);
            }
        }

        std::unique_ptr<lscq::SCQP<Value>> q;
        CyclicBarrier start;
        CyclicBarrier finish;
        std::size_t scqsize;
        std::size_t qsize;
        std::vector<Value> pool;
    };

    static std::atomic<Context*> g_ctx{nullptr};

    if (state.thread_index() == 0) {
        const std::size_t qsize = kMultiCapacity;
        const std::size_t scqsize = scqsize_for_effective_capacity(qsize);
        auto* ctx = new Context(threads, scqsize, qsize);
        g_ctx.store(ctx, std::memory_order_release);
    }

    Context* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    state.counters["scqsize"] =
        benchmark::Counter(static_cast<double>(ctx->scqsize), benchmark::Counter::kAvgThreads);
    state.counters["qsize"] =
        benchmark::Counter(static_cast<double>(ctx->qsize), benchmark::Counter::kAvgThreads);
    state.counters["using_fallback"] = benchmark::Counter(ctx->q->is_using_fallback() ? 1.0 : 0.0,
                                                         benchmark::Counter::kAvgThreads);

    const std::uint64_t mask = static_cast<std::uint64_t>(ctx->pool.size() - 1);

    for (auto _ : state) {
        ctx->start.arrive_and_wait();

        if (state.thread_index() == 0) {
            for (std::uint64_t i = 0; i < total_dequeues; ++i) {
                auto* p = &ctx->pool[static_cast<std::size_t>(i & mask)];
                while (!ctx->q->enqueue(p)) {
                }
            }
        } else {
            std::size_t done = 0;
            while (done < kOpsPerThread) {
                auto* p = ctx->q->dequeue();
                if (p == nullptr) {
                    std::this_thread::yield();
                    continue;
                }
                benchmark::DoNotOptimize(p);
                benchmark::DoNotOptimize(*p);
                ++done;
            }
        }

        ctx->finish.arrive_and_wait();
    }

    ctx->finish.arrive_and_wait();
    if (state.thread_index() == 0) {
        delete ctx;
        g_ctx.store(nullptr, std::memory_order_release);
    }
}

BENCHMARK(BM_SCQP_Pair)->Threads(1)->UseRealTime();
BENCHMARK(BM_SCQP_Pair)->Threads(2)->UseRealTime();
BENCHMARK(BM_SCQP_Pair)->Threads(4)->UseRealTime();
BENCHMARK(BM_SCQP_Pair)->Threads(8)->UseRealTime();
BENCHMARK(BM_SCQP_Pair)->Threads(16)->UseRealTime();

BENCHMARK(BM_SCQP_MultiEnqueue)->Threads(3)->UseRealTime();
BENCHMARK(BM_SCQP_MultiEnqueue)->Threads(5)->UseRealTime();
BENCHMARK(BM_SCQP_MultiEnqueue)->Threads(9)->UseRealTime();
BENCHMARK(BM_SCQP_MultiEnqueue)->Threads(17)->UseRealTime();

BENCHMARK(BM_SCQP_MultiDequeue)->Threads(3)->UseRealTime();
BENCHMARK(BM_SCQP_MultiDequeue)->Threads(5)->UseRealTime();
BENCHMARK(BM_SCQP_MultiDequeue)->Threads(9)->UseRealTime();
BENCHMARK(BM_SCQP_MultiDequeue)->Threads(17)->UseRealTime();

