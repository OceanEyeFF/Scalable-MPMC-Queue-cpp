#include <lscq/mutex_queue.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace {

constexpr std::size_t kOpsPerThread = 1'000'000;

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

inline void add_common_counters(benchmark::State& state, int producers, int consumers) {
    state.counters["threads"] =
        benchmark::Counter(static_cast<double>(state.threads()), benchmark::Counter::kAvgThreads);
    state.counters["producers"] =
        benchmark::Counter(static_cast<double>(producers), benchmark::Counter::kAvgThreads);
    state.counters["consumers"] =
        benchmark::Counter(static_cast<double>(consumers), benchmark::Counter::kAvgThreads);
    state.counters["ops_per_thread"] =
        benchmark::Counter(static_cast<double>(kOpsPerThread), benchmark::Counter::kAvgThreads);

    state.counters["Mops"] = benchmark::Counter(
        static_cast<double>(kOpsPerThread) * static_cast<double>(state.threads()) / 1e6,
        benchmark::Counter::kIsRate);
}

}  // namespace

static void BM_MutexQueue_Pair(benchmark::State& state) {
    using Value = std::uint64_t;

    const int threads = static_cast<int>(state.threads());
    const int producers = (threads == 1) ? 1 : (threads / 2);
    const int consumers = (threads == 1) ? 0 : (threads / 2);
    add_common_counters(state, producers, consumers);

    if (threads == 1) {
        for (auto _ : state) {
            state.PauseTiming();
            lscq::MutexQueue<Value> q;
            state.ResumeTiming();

            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                (void)q.enqueue(static_cast<Value>(i));
            }

            state.PauseTiming();
        }
        return;
    }

    struct Context {
        explicit Context(int threads)
            : q(std::make_unique<lscq::MutexQueue<Value>>()), start(threads), finish(threads) {}

        std::unique_ptr<lscq::MutexQueue<Value>> q;
        CyclicBarrier start;
        CyclicBarrier finish;
    };

    static std::atomic<Context*> g_ctx{nullptr};

    if (state.thread_index() == 0) {
        auto* ctx = new Context(threads);
        g_ctx.store(ctx, std::memory_order_release);
    }

    Context* ctx = nullptr;
    while ((ctx = g_ctx.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }

    for (auto _ : state) {
        ctx->start.arrive_and_wait();

        if (state.thread_index() < producers) {
            const std::uint64_t base = static_cast<std::uint64_t>(state.thread_index()) << 32;
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                (void)ctx->q->enqueue(static_cast<Value>(base + i));
            }
        } else {
            std::size_t done = 0;
            Value out{};
            while (done < kOpsPerThread) {
                if (!ctx->q->dequeue(out)) {
                    continue;
                }
                benchmark::DoNotOptimize(out);
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

BENCHMARK(BM_MutexQueue_Pair)->Threads(1)->Iterations(1)->UseRealTime();
BENCHMARK(BM_MutexQueue_Pair)->Threads(2)->Iterations(1)->UseRealTime();
BENCHMARK(BM_MutexQueue_Pair)->Threads(4)->Iterations(1)->UseRealTime();
BENCHMARK(BM_MutexQueue_Pair)->Threads(8)->Iterations(1)->UseRealTime();
BENCHMARK(BM_MutexQueue_Pair)->Threads(16)->Iterations(1)->UseRealTime();

