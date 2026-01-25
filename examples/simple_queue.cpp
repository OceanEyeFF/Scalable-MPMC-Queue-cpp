#include <cstdint>
#include <iostream>
#include <lscq/cas2.hpp>
#include <lscq/scq.hpp>
#include <lscq/scqp.hpp>
#include <vector>

int main() {
    std::cout << "CAS2 supported: " << (lscq::has_cas2_support() ? "yes" : "no") << "\n";

    {
        lscq::SCQ<std::uint64_t> q(1024);
        std::cout << "[SCQ] enqueue 1..5\n";
        for (std::uint64_t v = 1; v <= 5; ++v) {
            if (!q.enqueue(v)) {
                std::cerr << "[SCQ] enqueue failed: " << v << "\n";
                return 1;
            }
        }

        std::cout << "[SCQ] dequeue until empty:\n";
        while (true) {
            const std::uint64_t v = q.dequeue();
            if (v == lscq::SCQ<std::uint64_t>::kEmpty) {
                break;
            }
            std::cout << "  " << v << "\n";
        }
    }

    {
        std::vector<std::uint64_t> values = {10, 20, 30, 40, 50};
        lscq::SCQP<std::uint64_t> q(1024);
        std::cout << "[SCQP] using fallback: " << (q.is_using_fallback() ? "yes" : "no") << "\n";

        std::cout << "[SCQP] enqueue pointers to {10,20,30,40,50}\n";
        for (auto& v : values) {
            if (!q.enqueue(&v)) {
                std::cerr << "[SCQP] enqueue failed\n";
                return 1;
            }
        }

        std::cout << "[SCQP] dequeue until empty:\n";
        while (true) {
            std::uint64_t* p = q.dequeue();
            if (p == nullptr) {
                break;
            }
            std::cout << "  " << *p << "\n";
        }
    }

    return 0;
}
