#include <lscq/lscq.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::size_t producers = 4;
    std::size_t consumers = 4;
    std::size_t tasks_per_producer = 20000;
    std::size_t scqsize = 1024;  // size of each SCQP node inside LSCQ
};

void print_help(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "Options:\n"
              << "  --producers=<N>           Producer threads (default: 4)\n"
              << "  --consumers=<N>           Consumer threads (default: 4)\n"
              << "  --tasks-per-producer=<N>  Tasks per producer (default: 20000)\n"
              << "  --scqsize=<N>             Per-node ring size (default: 1024)\n"
              << "  --help                    Show help\n";
}

bool parse_u64(const std::string& s, std::size_t& out) {
    unsigned long long v = 0;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    const auto res = std::from_chars(begin, end, v, 10);
    if (res.ec != std::errc() || res.ptr != end) {
        return false;
    }
    out = static_cast<std::size_t>(v);
    return true;
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            print_help(argv[0]);
            return false;
        }
        const auto eq = a.find('=');
        if (eq == std::string::npos) {
            std::cerr << "Unknown arg: " << a << "\n";
            return false;
        }
        const std::string key = a.substr(0, eq);
        const std::string val = a.substr(eq + 1);

        if (key == "--producers") {
            if (!parse_u64(val, opt.producers)) {
                return false;
            }
            continue;
        }
        if (key == "--consumers") {
            if (!parse_u64(val, opt.consumers)) {
                return false;
            }
            continue;
        }
        if (key == "--tasks-per-producer") {
            if (!parse_u64(val, opt.tasks_per_producer)) {
                return false;
            }
            continue;
        }
        if (key == "--scqsize") {
            if (!parse_u64(val, opt.scqsize)) {
                return false;
            }
            continue;
        }

        std::cerr << "Unknown arg: " << a << "\n";
        return false;
    }

    if (opt.producers == 0 || opt.consumers == 0 || opt.tasks_per_producer == 0) {
        std::cerr << "Invalid: producers/consumers/tasks-per-producer must be > 0\n";
        return false;
    }
    return true;
}

// Enqueue with cooperative spinning. LSCQ::enqueue() can return false under extreme contention
// (internal retry limit). Retrying is a reasonable policy for demo code.
void enqueue_spin(lscq::LSCQ<std::uint64_t>& q, std::uint64_t* ptr, std::atomic<bool>& cancel) {
    while (!cancel.load(std::memory_order_relaxed)) {
        if (q.enqueue(ptr)) {
            return;
        }
        std::this_thread::yield();
    }
}

std::uint64_t do_work(std::uint64_t task_id, std::uint32_t producer, std::uint32_t work_units) noexcept {
    // Simple pseudo-work: a few integer mixes.
    std::uint64_t x = task_id ^ (static_cast<std::uint64_t>(producer) << 32);
    for (std::uint32_t i = 0; i < work_units; ++i) {
        x ^= (x << 7);
        x ^= (x >> 9);
        x *= 0x9E3779B97F4A7C15ULL;
    }
    return x;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 0;
    }

    const std::size_t total_tasks = opt.producers * opt.tasks_per_producer;
    std::cout << "LSCQ examples - task queue (MPMC work distribution)\n"
              << "Producers: " << opt.producers << "\n"
              << "Consumers: " << opt.consumers << "\n"
              << "Tasks per producer: " << opt.tasks_per_producer << "\n"
              << "Total tasks: " << total_tasks << "\n"
              << "SCQSIZE: " << opt.scqsize << "\n\n";

    // LSCQ now uses ObjectPool internally for node management (no EBRManager needed).
    lscq::LSCQ<std::uint64_t> q(opt.scqsize);

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> cancel{false};

    std::atomic<std::size_t> produced{0};
    std::atomic<std::size_t> consumed{0};
    std::atomic<std::uint64_t> checksum{0};

    std::vector<std::thread> producer_threads;
    std::vector<std::thread> consumer_threads;
    producer_threads.reserve(opt.producers);
    consumer_threads.reserve(opt.consumers);

    // We use a special value as a "poison pill" to stop consumers.
    // This value is never produced as a normal task id in this demo.
    constexpr std::uint64_t kStopValue = std::numeric_limits<std::uint64_t>::max();

    // Consumers: pop tasks and execute them until they receive a stop task.
    for (std::size_t c = 0; c < opt.consumers; ++c) {
        consumer_threads.emplace_back([&, c] {
            (void)c;
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::uint64_t local_checksum = 0;
            std::size_t local_consumed = 0;

            for (;;) {
                std::uint64_t* raw = q.dequeue();
                if (raw == nullptr) {
                    // Queue is empty right now; this is non-blocking, so we back off.
                    std::this_thread::yield();
                    continue;
                }

                // Regain ownership (ensures delete). Producers transfer ownership by release().
                std::unique_ptr<std::uint64_t> task(raw);
                if (*task == kStopValue) {
                    break;
                }

                const std::uint64_t task_id = *task;
                const std::uint32_t producer = static_cast<std::uint32_t>(task_id >> 32);
                const std::uint32_t work_units =
                    static_cast<std::uint32_t>(5 + (task_id % 8));  // derived from id for demo

                local_checksum ^= do_work(task_id, producer, work_units);
                ++local_consumed;
            }

            consumed.fetch_add(local_consumed, std::memory_order_relaxed);
            checksum.fetch_xor(local_checksum, std::memory_order_relaxed);
        });
    }

    // Producers: create tasks and enqueue them.
    for (std::size_t p = 0; p < opt.producers; ++p) {
        producer_threads.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t i = 0; i < opt.tasks_per_producer; ++i) {
                // Encode the producer id and sequence into one 64-bit task id.
                const std::uint64_t task_id =
                    (static_cast<std::uint64_t>(p) << 32) | static_cast<std::uint64_t>(i);

                auto task = std::make_unique<std::uint64_t>(task_id);
                std::uint64_t* raw = task.get();
                enqueue_spin(q, raw, cancel);
                if (cancel.load(std::memory_order_relaxed)) {
                    return;  // task auto-freed by unique_ptr
                }

                // Transfer ownership to the consumer.
                (void)task.release();
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != opt.producers + opt.consumers) {
        std::this_thread::yield();
    }

    const auto t0 = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    for (auto& t : producer_threads) {
        t.join();
    }

    // Once all producers have finished, push one "poison pill" per consumer.
    // Because the queue is FIFO, consumers will only see these after all real tasks.
    for (std::size_t i = 0; i < opt.consumers; ++i) {
        auto stop = std::make_unique<std::uint64_t>(kStopValue);
        std::uint64_t* raw = stop.get();
        enqueue_spin(q, raw, cancel);
        if (cancel.load(std::memory_order_relaxed)) {
            break;
        }
        (void)stop.release();
    }

    for (auto& t : consumer_threads) {
        t.join();
    }
    const auto t1 = std::chrono::steady_clock::now();

    const std::chrono::duration<double> dt = t1 - t0;
    // Note: LSCQ manages node lifecycle internally via ObjectPool.

    const std::size_t produced_n = produced.load(std::memory_order_relaxed);
    const std::size_t consumed_n = consumed.load(std::memory_order_relaxed);

    std::cout << "Produced: " << produced_n << "\n"
              << "Consumed: " << consumed_n << "\n"
              << "Elapsed: " << dt.count() << " s\n"
              << "Throughput: " << (static_cast<double>(consumed_n) / dt.count() / 1e6) << " Mtasks/s\n"
              << "Checksum (xor): " << checksum.load(std::memory_order_relaxed) << "\n";

    if (produced_n != total_tasks || consumed_n != total_tasks) {
        std::cerr << "ERROR: unexpected task counts (produced/consumed mismatch)\n";
        return 1;
    }
    return 0;
}
