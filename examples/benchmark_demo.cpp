#include <lscq/lscq.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::size_t scqsize = 1024;
    std::size_t producers = 4;
    std::size_t consumers = 4;
    std::size_t ops_per_producer = 200000;
};

void print_help(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "Options:\n"
              << "  --scqsize=<N>                      LSCQ per-node ring size (default: 1024)\n"
              << "  --producers=<N>                    Producer threads (default: 4)\n"
              << "  --consumers=<N>                    Consumer threads (default: 4)\n"
              << "  --ops-per-producer=<N>             Operations per producer (default: 200000)\n"
              << "  --help                             Show help\n\n"
              << "Notes:\n"
              << "  - This is a *demo* of throughput measurement (ops/s) using a steady clock.\n"
              << "  - For LSCQ we enqueue pointers into a pre-allocated vector to avoid measuring\n"
              << "    allocation overhead. Real programs often transfer ownership (new/delete),\n"
              << "    which will lower throughput.\n";
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

        if (key == "--scqsize") {
            if (!parse_u64(val, opt.scqsize)) {
                return false;
            }
            continue;
        }
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
        if (key == "--ops-per-producer") {
            if (!parse_u64(val, opt.ops_per_producer)) {
                return false;
            }
            continue;
        }

        std::cerr << "Unknown arg: " << a << "\n";
        return false;
    }

    if (opt.producers == 0 || opt.consumers == 0 || opt.ops_per_producer == 0) {
        std::cerr << "Invalid: producers/consumers/ops-per-producer must be > 0\n";
        return false;
    }
    return true;
}

// A small "gate" so all threads start their work at roughly the same time.
// This is not perfect synchronization, but good enough for a demo.
struct StartGate {
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};

    void arrive_and_wait(std::size_t total_threads) {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        (void)total_threads;
    }

    void wait_until_ready(std::size_t total_threads) {
        while (ready.load(std::memory_order_acquire) != total_threads) {
            std::this_thread::yield();
        }
    }

    void go() { start.store(true, std::memory_order_release); }
};

double bench_lscq(std::size_t scqsize, std::size_t producers, std::size_t consumers,
                  std::size_t ops_per_producer) {
    const std::size_t total_ops = producers * ops_per_producer;

    // Pre-allocate payload storage so the benchmark measures queue operations
    // rather than allocation/deallocation.
    std::vector<std::uint64_t> storage(total_ops);
    for (std::size_t i = 0; i < storage.size(); ++i) {
        storage[i] = static_cast<std::uint64_t>(i + 1);
    }

    lscq::EBRManager ebr;
    lscq::LSCQ<std::uint64_t> q(ebr, scqsize);

    StartGate gate;
    std::atomic<std::size_t> consumed{0};
    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            gate.arrive_and_wait(producers + consumers);

            const std::size_t begin = p * ops_per_producer;
            const std::size_t end = begin + ops_per_producer;
            for (std::size_t i = begin; i < end; ++i) {
                auto* ptr = &storage[i];
                while (!q.enqueue(ptr)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (std::size_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&] {
            gate.arrive_and_wait(producers + consumers);

            while (consumed.load(std::memory_order_relaxed) < total_ops) {
                auto* ptr = q.dequeue();
                if (ptr == nullptr) {
                    std::this_thread::yield();
                    continue;
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    gate.wait_until_ready(producers + consumers);
    const auto t0 = std::chrono::steady_clock::now();
    gate.go();

    for (auto& t : threads) {
        t.join();
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)ebr.try_reclaim();

    const std::chrono::duration<double> dt = t1 - t0;
    return static_cast<double>(total_ops) / dt.count();
}

void print_result(const char* name, double ops_per_sec) {
    std::cout << name << ": " << (ops_per_sec / 1e6) << " Mops/s (" << ops_per_sec << " ops/s)\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 0;
    }

    std::cout << "LSCQ examples - benchmark demo (simple timer)\n"
              << "Threads: P=" << opt.producers << " C=" << opt.consumers << "\n"
              << "Ops per producer: " << opt.ops_per_producer << "\n"
              << "Total ops: " << (opt.producers * opt.ops_per_producer) << "\n"
              << "SCQSIZE (LSCQ): " << opt.scqsize << "\n\n";

    const double ops = bench_lscq(opt.scqsize, opt.producers, opt.consumers, opt.ops_per_producer);
    print_result("LSCQ (pointers, pre-allocated)", ops);

    return 0;
}
