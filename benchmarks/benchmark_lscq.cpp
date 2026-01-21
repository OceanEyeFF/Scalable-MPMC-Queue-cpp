#include <lscq/ebr.hpp>
#include <lscq/lscq.hpp>
#include <lscq/scqp.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kOpsPerThread = 1'000'000;
constexpr std::size_t kLSCQNodeSize = 1u << 12;  // 4K per node

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

}  // namespace

// ============================================================================
// LSCQ Benchmarks
// ============================================================================

static void BM_LSCQ_Pair(benchmark::State& state) {
    using Value = std::uint64_t;

    const int threads = static_cast<int>(state.threads());
    const int producers = (threads == 1) ? 1 : (threads / 2);
    const int consumers = (threads == 1) ? 0 : (threads / 2);

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(kOpsPerThread);
    add_common_counters(state, producers, consumers, total_ops);

    if (threads == 1) {
        // Single-threaded benchmark
        lscq::EBRManager ebr;
        lscq::LSCQ<Value> queue(ebr, kLSCQNodeSize);
        std::vector<Value> values(kOpsPerThread);

        for (auto _ : state) {
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                values[i] = i;
                queue.enqueue(&values[i]);
            }
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                benchmark::DoNotOptimize(queue.dequeue());
            }
        }
    } else {
        // Multi-threaded producer-consumer benchmark
        lscq::EBRManager ebr;
        lscq::LSCQ<Value> queue(ebr, kLSCQNodeSize);

        std::vector<std::vector<Value>> per_thread_values(static_cast<std::size_t>(producers));
        for (int p = 0; p < producers; ++p) {
            per_thread_values[static_cast<std::size_t>(p)].resize(kOpsPerThread);
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                per_thread_values[static_cast<std::size_t>(p)][i] =
                    static_cast<std::uint64_t>(p) * kOpsPerThread + i;
            }
        }

        CyclicBarrier start_barrier(threads + 1);
        std::atomic<bool> stop{false};

        auto producer_fn = [&](int tid) {
            auto& values = per_thread_values[static_cast<std::size_t>(tid)];
            start_barrier.arrive_and_wait();

            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                while (!queue.enqueue(&values[i])) {
                    std::this_thread::yield();
                }
            }
        };

        auto consumer_fn = [&]() {
            start_barrier.arrive_and_wait();

            std::size_t count = 0;
            while (!stop.load(std::memory_order_acquire) || count < kOpsPerThread) {
                Value* p = queue.dequeue();
                if (p != nullptr) {
                    benchmark::DoNotOptimize(p);
                    ++count;
                } else {
                    std::this_thread::yield();
                }
            }
        };

        for (auto _ : state) {
            stop.store(false, std::memory_order_release);

            std::vector<std::thread> worker_threads;
            worker_threads.reserve(static_cast<std::size_t>(threads));

            for (int p = 0; p < producers; ++p) {
                worker_threads.emplace_back(producer_fn, p);
            }
            for (int c = 0; c < consumers; ++c) {
                worker_threads.emplace_back(consumer_fn);
            }

            start_barrier.arrive_and_wait();

            for (auto& t : worker_threads) {
                t.join();
            }

            stop.store(true, std::memory_order_release);
        }
    }
}

// ============================================================================
// SCQP Comparison Benchmarks
// ============================================================================

static void BM_SCQP_Pair_Comparison(benchmark::State& state) {
    using Value = std::uint64_t;

    const int threads = static_cast<int>(state.threads());
    const int producers = (threads == 1) ? 1 : (threads / 2);
    const int consumers = (threads == 1) ? 0 : (threads / 2);

    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(kOpsPerThread);
    add_common_counters(state, producers, consumers, total_ops);
    state.counters["has_cas2_support"] = benchmark::Counter(lscq::has_cas2_support() ? 1.0 : 0.0,
                                                           benchmark::Counter::kAvgThreads);

    constexpr std::size_t queue_capacity = 1u << 22;  // 4M capacity

    if (threads == 1) {
        lscq::SCQP<Value> queue(queue_capacity);
        std::vector<Value> values(kOpsPerThread);

        for (auto _ : state) {
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                values[i] = i;
                queue.enqueue(&values[i]);
            }
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                benchmark::DoNotOptimize(queue.dequeue());
            }
        }
    } else {
        lscq::SCQP<Value> queue(queue_capacity);

        std::vector<std::vector<Value>> per_thread_values(static_cast<std::size_t>(producers));
        for (int p = 0; p < producers; ++p) {
            per_thread_values[static_cast<std::size_t>(p)].resize(kOpsPerThread);
            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                per_thread_values[static_cast<std::size_t>(p)][i] =
                    static_cast<std::uint64_t>(p) * kOpsPerThread + i;
            }
        }

        CyclicBarrier start_barrier(threads + 1);
        std::atomic<bool> stop{false};

        auto producer_fn = [&](int tid) {
            auto& values = per_thread_values[static_cast<std::size_t>(tid)];
            start_barrier.arrive_and_wait();

            for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                while (!queue.enqueue(&values[i])) {
                    std::this_thread::yield();
                }
            }
        };

        auto consumer_fn = [&]() {
            start_barrier.arrive_and_wait();

            std::size_t count = 0;
            while (!stop.load(std::memory_order_acquire) || count < kOpsPerThread) {
                Value* p = queue.dequeue();
                if (p != nullptr) {
                    benchmark::DoNotOptimize(p);
                    ++count;
                } else {
                    std::this_thread::yield();
                }
            }
        };

        for (auto _ : state) {
            stop.store(false, std::memory_order_release);

            std::vector<std::thread> worker_threads;
            worker_threads.reserve(static_cast<std::size_t>(threads));

            for (int p = 0; p < producers; ++p) {
                worker_threads.emplace_back(producer_fn, p);
            }
            for (int c = 0; c < consumers; ++c) {
                worker_threads.emplace_back(consumer_fn);
            }

            start_barrier.arrive_and_wait();

            for (auto& t : worker_threads) {
                t.join();
            }

            stop.store(true, std::memory_order_release);
        }
    }
}

// ============================================================================
// Benchmark Registration
// ============================================================================

BENCHMARK(BM_LSCQ_Pair)->Threads(1)->UseRealTime();
BENCHMARK(BM_LSCQ_Pair)->Threads(2)->UseRealTime();
BENCHMARK(BM_LSCQ_Pair)->Threads(4)->UseRealTime();
BENCHMARK(BM_LSCQ_Pair)->Threads(8)->UseRealTime();
BENCHMARK(BM_LSCQ_Pair)->Threads(16)->UseRealTime();

BENCHMARK(BM_SCQP_Pair_Comparison)->Threads(1)->UseRealTime();
BENCHMARK(BM_SCQP_Pair_Comparison)->Threads(2)->UseRealTime();
BENCHMARK(BM_SCQP_Pair_Comparison)->Threads(4)->UseRealTime();
BENCHMARK(BM_SCQP_Pair_Comparison)->Threads(8)->UseRealTime();
BENCHMARK(BM_SCQP_Pair_Comparison)->Threads(16)->UseRealTime();

