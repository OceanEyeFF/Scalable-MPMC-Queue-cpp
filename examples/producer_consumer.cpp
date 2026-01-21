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
    std::size_t items_per_producer = 200000;
    std::size_t batch_size = 64;  // "batch" is an application-level concept here
    std::size_t scqsize = 1024;
};

void print_help(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "Options:\n"
              << "  --producers=<N>           Producer threads (default: 4)\n"
              << "  --consumers=<N>           Consumer threads (default: 4)\n"
              << "  --items-per-producer=<N>  Items per producer (default: 200000)\n"
              << "  --batch-size=<N>          Batch size for produce/consume loops (default: 64)\n"
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
        if (key == "--items-per-producer") {
            if (!parse_u64(val, opt.items_per_producer)) {
                return false;
            }
            continue;
        }
        if (key == "--batch-size") {
            if (!parse_u64(val, opt.batch_size)) {
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

    if (opt.producers == 0 || opt.consumers == 0 || opt.items_per_producer == 0 || opt.batch_size == 0) {
        std::cerr << "Invalid: producers/consumers/items-per-producer/batch-size must be > 0\n";
        return false;
    }
    return true;
}

void enqueue_spin(lscq::LSCQ<std::uint64_t>& q, std::uint64_t* ptr, std::atomic<bool>& cancel) {
    while (!cancel.load(std::memory_order_relaxed)) {
        if (q.enqueue(ptr)) {
            return;
        }
        std::this_thread::yield();
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 0;
    }

    const std::size_t total_items = opt.producers * opt.items_per_producer;
    std::cout << "LSCQ examples - producer/consumer (MPMC, application batching)\n"
              << "Producers: " << opt.producers << "\n"
              << "Consumers: " << opt.consumers << "\n"
              << "Items per producer: " << opt.items_per_producer << "\n"
              << "Total items: " << total_items << "\n"
              << "Batch size: " << opt.batch_size << "\n"
              << "SCQSIZE: " << opt.scqsize << "\n\n";

    lscq::EBRManager ebr;
    lscq::LSCQ<std::uint64_t> q(ebr, opt.scqsize);

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

    // Poison pill to stop consumers (never produced by this demo's payload generator).
    constexpr std::uint64_t kStopValue = std::numeric_limits<std::uint64_t>::max();

    // Consumers collect items into batches and then "process" the batch.
    for (std::size_t c = 0; c < opt.consumers; ++c) {
        consumer_threads.emplace_back([&, c] {
            (void)c;
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::vector<std::unique_ptr<std::uint64_t>> batch;
            batch.reserve(opt.batch_size);

            std::uint64_t local_checksum = 0;
            std::size_t local_consumed = 0;

            for (;;) {
                std::uint64_t* raw = q.dequeue();
                if (raw == nullptr) {
                    std::this_thread::yield();
                    continue;
                }

                std::unique_ptr<std::uint64_t> item(raw);  // regain ownership, auto-delete
                if (*item == kStopValue) {
                    break;
                }

                batch.push_back(std::move(item));
                ++local_consumed;

                // Batch processing: aggregate the batch (as an example).
                if (batch.size() >= opt.batch_size) {
                    for (const auto& it : batch) {
                        local_checksum += *it;
                    }
                    batch.clear();  // frees items in the batch
                }
            }

            // Process any leftover items not forming a full batch.
            for (const auto& it : batch) {
                local_checksum += *it;
            }

            consumed.fetch_add(local_consumed, std::memory_order_relaxed);
            checksum.fetch_add(local_checksum, std::memory_order_relaxed);
        });
    }

    // Producers generate items in batches (application-level batching).
    for (std::size_t p = 0; p < opt.producers; ++p) {
        producer_threads.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const std::uint64_t base_id = static_cast<std::uint64_t>(p * opt.items_per_producer);
            std::size_t remaining = opt.items_per_producer;
            std::uint64_t next_id = base_id;

            while (remaining > 0 && !cancel.load(std::memory_order_relaxed)) {
                const std::size_t n = (remaining < opt.batch_size) ? remaining : opt.batch_size;

                // Create a small batch of heap items, then enqueue them one by one.
                // The queue API itself is single-item enqueue; batching is done at
                // the producer side to improve locality / reduce overhead elsewhere.
                for (std::size_t i = 0; i < n; ++i) {
                    const std::uint64_t id = next_id++;
                    const std::uint64_t payload = id ^ 0xA5A5A5A5A5A5A5A5ULL;

                    auto item = std::make_unique<std::uint64_t>(payload);

                    std::uint64_t* raw = item.get();
                    enqueue_spin(q, raw, cancel);
                    if (cancel.load(std::memory_order_relaxed)) {
                        return;  // item auto-freed
                    }
                    (void)item.release();  // transfer ownership to consumer
                    produced.fetch_add(1, std::memory_order_relaxed);
                }

                remaining -= n;
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

    // Signal consumers to stop once all production is complete.
    // These stop items are enqueued after all real items, so they will be consumed last.
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
    (void)ebr.try_reclaim();

    const std::size_t produced_n = produced.load(std::memory_order_relaxed);
    const std::size_t consumed_n = consumed.load(std::memory_order_relaxed);
    const auto checksum_n = checksum.load(std::memory_order_relaxed);

    std::cout << "Produced: " << produced_n << "\n"
              << "Consumed: " << consumed_n << "\n"
              << "Elapsed: " << dt.count() << " s\n"
              << "Throughput: " << (static_cast<double>(consumed_n) / dt.count() / 1e6) << " Mitems/s\n"
              << "Checksum (sum): " << checksum_n << "\n";

    if (produced_n != total_items || consumed_n != total_items) {
        std::cerr << "ERROR: unexpected item counts (produced/consumed mismatch)\n";
        return 1;
    }
    return 0;
}
