#pragma once

#include <lscq/ebr.hpp>
#include <lscq/lscq.hpp>
#include <lscq/msqueue.hpp>
#include <lscq/mutex_queue.hpp>
#include <lscq/ncq.hpp>
#include <lscq/scq.hpp>
#include <lscq/scqp.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace lscq_bench {

constexpr int kThreadCounts[] = {1, 2, 4, 8, 12, 16, 24};

constexpr std::size_t kSharedCapacity = 1u << 18;   // 262,144 (effective capacity)
constexpr std::size_t kPointerPoolSize = 1u << 20;  // 1,048,576
constexpr std::size_t kLSCQNodeScqsize = 1u << 12;  // 4,096 ring size (2n)

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

struct XorShift64Star {
    std::uint64_t s;

    explicit XorShift64Star(std::uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ULL) {}

    std::uint64_t next() noexcept {
        std::uint64_t x = s;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        s = x;
        return x * 2685821657736338717ULL;
    }
};

inline void pin_current_thread(unsigned cpu) noexcept {
#if defined(_WIN32)
    const unsigned c = cpu % 64u;
    const DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << c);
    (void)SetThreadAffinityMask(GetCurrentThread(), mask);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpu), &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

inline void pin_thread_index(int thread_index) noexcept {
    const unsigned hc = std::thread::hardware_concurrency();
    const unsigned cpu = (hc == 0) ? static_cast<unsigned>(thread_index)
                                   : (static_cast<unsigned>(thread_index) % hc);
    pin_current_thread(cpu);
}

inline void add_common_counters(benchmark::State& state, int producers, int consumers,
                                std::uint64_t total_ops) {
    state.counters["threads"] =
        benchmark::Counter(static_cast<double>(state.threads()), benchmark::Counter::kAvgThreads);
    state.counters["producers"] =
        benchmark::Counter(static_cast<double>(producers), benchmark::Counter::kAvgThreads);
    state.counters["consumers"] =
        benchmark::Counter(static_cast<double>(consumers), benchmark::Counter::kAvgThreads);
    state.counters["total_ops"] =
        benchmark::Counter(static_cast<double>(total_ops), benchmark::Counter::kAvgThreads);
    state.counters["Mops"] =
        benchmark::Counter(static_cast<double>(total_ops) / 1e6, benchmark::Counter::kIsRate);
}

template <class Queue>
struct QueueOps;

using Value = std::uint64_t;

template <>
struct QueueOps<lscq::NCQ<Value>> {
    using queue_type = lscq::NCQ<Value>;
    using item_type = Value;
    static constexpr bool kPointerQueue = false;

    static constexpr const char* name() { return "NCQ"; }

    static std::unique_ptr<queue_type> make_queue(std::size_t effective_capacity) {
        return std::make_unique<queue_type>(effective_capacity);
    }

    static bool enqueue(queue_type& q, item_type v) { return q.enqueue(v); }

    static bool dequeue(queue_type& q, item_type& out) {
        const item_type v = q.dequeue();
        if (v == queue_type::kEmpty) {
            return false;
        }
        out = v;
        return true;
    }
};

template <>
struct QueueOps<lscq::SCQ<Value>> {
    using queue_type = lscq::SCQ<Value>;
    using item_type = Value;
    static constexpr bool kPointerQueue = false;

    static constexpr const char* name() { return "SCQ"; }

    static std::unique_ptr<queue_type> make_queue(std::size_t effective_capacity) {
        // SCQ effective usable capacity is QSIZE = SCQSIZE / 2.
        const std::size_t scqsize = effective_capacity * 2;
        return std::make_unique<queue_type>(scqsize);
    }

    static std::uint64_t value_mask(const queue_type& q) noexcept {
        // SCQ requires value < (SCQSIZE - 1). Round size to power-of-two, so SCQSIZE-2 is a mask.
        const std::size_t scqsize = q.scqsize();
        return (scqsize >= 2) ? static_cast<std::uint64_t>(scqsize - 2) : 0;
    }

    static bool enqueue(queue_type& q, item_type v) { return q.enqueue(v); }

    static bool dequeue(queue_type& q, item_type& out) {
        const item_type v = q.dequeue();
        if (v == queue_type::kEmpty) {
            return false;
        }
        out = v;
        return true;
    }
};

template <>
struct QueueOps<lscq::MSQueue<Value>> {
    using queue_type = lscq::MSQueue<Value>;
    using item_type = Value;
    static constexpr bool kPointerQueue = false;

    static constexpr const char* name() { return "MSQueue"; }

    static std::unique_ptr<queue_type> make_queue(std::size_t /*effective_capacity*/) {
        return std::make_unique<queue_type>();
    }

    static bool enqueue(queue_type& q, item_type v) { return q.enqueue(v); }

    static bool dequeue(queue_type& q, item_type& out) { return q.dequeue(out); }
};

template <>
struct QueueOps<lscq::MutexQueue<Value>> {
    using queue_type = lscq::MutexQueue<Value>;
    using item_type = Value;
    static constexpr bool kPointerQueue = false;

    static constexpr const char* name() { return "MutexQueue"; }

    static std::unique_ptr<queue_type> make_queue(std::size_t /*effective_capacity*/) {
        return std::make_unique<queue_type>();
    }

    static bool enqueue(queue_type& q, item_type v) { return q.enqueue(v); }

    static bool dequeue(queue_type& q, item_type& out) { return q.dequeue(out); }
};

template <>
struct QueueOps<lscq::SCQP<Value>> {
    using queue_type = lscq::SCQP<Value>;
    using item_type = Value*;
    static constexpr bool kPointerQueue = true;

    static constexpr const char* name() { return "SCQP"; }

    static std::unique_ptr<queue_type> make_queue(std::size_t effective_capacity) {
        const std::size_t scqsize = effective_capacity * 2;
        return std::make_unique<queue_type>(scqsize);
    }

    static bool enqueue(queue_type& q, item_type p) { return q.enqueue(p); }

    static bool dequeue(queue_type& q, item_type& out) {
        item_type p = q.dequeue();
        if (p == nullptr) {
            return false;
        }
        out = p;
        return true;
    }
};

struct LSCQContext {
    lscq::EBRManager ebr;
    lscq::LSCQ<Value> q;
    CyclicBarrier start;
    CyclicBarrier finish;
    std::vector<Value> pool;
    std::uint64_t pool_mask;

    explicit LSCQContext(int threads, std::size_t node_scqsize)
        : ebr(),
          q(ebr, node_scqsize),
          start(threads),
          finish(threads),
          pool(kPointerPoolSize),
          pool_mask(static_cast<std::uint64_t>(pool.size() - 1)) {
        for (std::size_t i = 0; i < pool.size(); ++i) {
            pool[i] = static_cast<Value>(i);
        }
    }
};

template <>
struct QueueOps<lscq::LSCQ<Value>> {
    using queue_type = lscq::LSCQ<Value>;
    using item_type = Value*;
    static constexpr bool kPointerQueue = true;
    using context_type = LSCQContext;

    static constexpr const char* name() { return "LSCQ"; }
};

template <class Queue>
struct SharedContext {
    using ops = QueueOps<Queue>;
    using queue_type = typename ops::queue_type;
    using item_type = typename ops::item_type;

    std::unique_ptr<queue_type> q;
    CyclicBarrier start;
    CyclicBarrier finish;
    std::vector<Value> pool;
    std::uint64_t pool_mask;
    std::uint64_t scq_value_mask;

    SharedContext(int threads, std::size_t effective_capacity)
        : q(ops::make_queue(effective_capacity)),
          start(threads),
          finish(threads),
          pool(ops::kPointerQueue ? kPointerPoolSize : 0),
          pool_mask(pool.empty() ? 0 : static_cast<std::uint64_t>(pool.size() - 1)),
          scq_value_mask(0) {
        if (!pool.empty()) {
            for (std::size_t i = 0; i < pool.size(); ++i) {
                pool[i] = static_cast<Value>(i);
            }
        }
        if constexpr (std::is_same_v<queue_type, lscq::SCQ<Value>>) {
            scq_value_mask = ops::value_mask(*q);
        }
    }

    item_type make_item(int thread_index, std::uint64_t seq) {
        if constexpr (ops::kPointerQueue) {
            const std::uint64_t idx =
                (static_cast<std::uint64_t>(thread_index) * 0x9e3779b97f4a7c15ULL + seq) &
                pool_mask;
            return &pool[static_cast<std::size_t>(idx)];
        } else {
            Value v = (static_cast<std::uint64_t>(thread_index) << 32u) + seq;
            if constexpr (std::is_same_v<queue_type, lscq::SCQ<Value>>) {
                v &= scq_value_mask;
            }
            if (v == (std::numeric_limits<Value>::max)()) {
                v -= 1;
            }
            return v;
        }
    }
};

}  // namespace lscq_bench
