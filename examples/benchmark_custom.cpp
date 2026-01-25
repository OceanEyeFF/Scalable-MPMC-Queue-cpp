#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <lscq/cas2.hpp>
#include <lscq/ebr.hpp>
#include <lscq/lscq.hpp>
#include <lscq/scqp.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string queue = "lscq";  // lscq | scqp
    std::size_t scqsize = 1024;  // ring size (2n)
    std::size_t producers = 4;
    std::size_t consumers = 4;
    std::size_t ops_per_producer = 200000;
};

void print_help(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "Options:\n"
              << "  --queue=<lscq|scqp>        Queue type (default: lscq)\n"
              << "  --scqsize=<N>              SCQ/SCQP ring size (default: 1024)\n"
              << "  --producers=<N>            Producer threads (default: 4)\n"
              << "  --consumers=<N>            Consumer threads (default: 4)\n"
              << "  --ops-per-producer=<N>     Operations per producer (default: 200000)\n"
              << "  --help                     Show help\n";
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

        if (key == "--queue") {
            opt.queue = val;
            continue;
        }
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
    if (opt.queue != "lscq" && opt.queue != "scqp") {
        std::cerr << "Invalid: --queue must be lscq or scqp\n";
        return false;
    }
    return true;
}

template <class Queue>
double run_mpmc_benchmark(Queue& q, std::size_t producers, std::size_t consumers,
                          std::vector<std::uint64_t>& storage, std::size_t ops_per_producer) {
    const std::size_t total_ops = producers * ops_per_producer;

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<std::size_t> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

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
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

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

    while (ready.load(std::memory_order_acquire) != producers + consumers) {
        std::this_thread::yield();
    }

    const auto t0 = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }
    const auto t1 = std::chrono::steady_clock::now();

    const std::chrono::duration<double> dt = t1 - t0;
    return static_cast<double>(total_ops) / dt.count();
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 0;
    }

    std::cout << "Queue: " << opt.queue << "\n"
              << "CAS2 supported: " << (lscq::has_cas2_support() ? "yes" : "no") << "\n"
              << "Threads: P=" << opt.producers << " C=" << opt.consumers << "\n"
              << "Ops: " << opt.ops_per_producer << " per producer\n"
              << "SCQSIZE: " << opt.scqsize << "\n";

    const std::size_t total = opt.producers * opt.ops_per_producer;
    std::vector<std::uint64_t> storage(total);
    for (std::size_t i = 0; i < storage.size(); ++i) {
        storage[i] = static_cast<std::uint64_t>(i + 1);
    }

    double ops_per_sec = 0.0;
    if (opt.queue == "scqp") {
        lscq::SCQP<std::uint64_t> q(opt.scqsize);
        std::cout << "SCQP fallback: " << (q.is_using_fallback() ? "yes" : "no") << "\n";
        ops_per_sec =
            run_mpmc_benchmark(q, opt.producers, opt.consumers, storage, opt.ops_per_producer);
    } else {
        lscq::EBRManager ebr;
        lscq::LSCQ<std::uint64_t> q(ebr, opt.scqsize);
        ops_per_sec =
            run_mpmc_benchmark(q, opt.producers, opt.consumers, storage, opt.ops_per_producer);
        (void)ebr.try_reclaim();
    }

    std::cout << "Throughput: " << (ops_per_sec / 1e6) << " Mops/s\n";
    return 0;
}
