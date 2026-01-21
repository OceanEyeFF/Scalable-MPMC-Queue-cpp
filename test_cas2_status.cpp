// Temporary test program to check CAS2 support status
#include <iostream>
#include <lscq/cas2.hpp>
#include <lscq/scqp.hpp>

int main() {
    std::cout << "=== CAS2 Support Status ===" << std::endl;
    std::cout << "kEnableCas2 (compile-time): " << (lscq::kEnableCas2 ? "YES" : "NO") << std::endl;
    std::cout << "has_cas2_support() (runtime): " << (lscq::has_cas2_support() ? "YES" : "NO") << std::endl;

    // Create a small SCQP instance to check fallback status
    lscq::SCQP<int> scqp(128);
    std::cout << "SCQP is_using_fallback(): " << (scqp.is_using_fallback() ? "YES (SCQ index mode)" : "NO (pointer mode)") << std::endl;

    if (!lscq::has_cas2_support()) {
        std::cout << "\n⚠️  WARNING: CAS2 not supported!" << std::endl;
        std::cout << "    All threads will serialize through ONE global mutex!" << std::endl;
        std::cout << "    This explains the Test #29 timeout (16P+16C@1M)!" << std::endl;
    } else {
        std::cout << "\n✓ CAS2 is supported - native 128-bit atomic operations available" << std::endl;
    }

    return 0;
}
