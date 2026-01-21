#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <lscq/cas2.hpp>
#include <lscq/detail/platform.hpp>
#include <thread>
#include <vector>

TEST(Cas2, EntryLayout) {
    EXPECT_EQ(sizeof(lscq::Entry), 16u);
    EXPECT_EQ(alignof(lscq::Entry), 16u);
}

TEST(PlatformDetection, Arm64MacroMatchesCompilerDefines) {
#if defined(_M_ARM64) || defined(_M_ARM64EC) || defined(__aarch64__) || defined(__arm64__)
    EXPECT_EQ(LSCQ_ARCH_ARM64, 1);
#else
    EXPECT_EQ(LSCQ_ARCH_ARM64, 0);
#endif
}

TEST(Cas2_BasicOperation, SuccessUpdatesValue) {
    lscq::Entry value{1u, 2u};
    lscq::Entry expected = value;
    const lscq::Entry desired{3u, 4u};

    EXPECT_TRUE(lscq::cas2(&value, expected, desired));
    EXPECT_EQ(value, desired);
    EXPECT_EQ(expected, (lscq::Entry{1u, 2u}));
}

TEST(Cas2_FailedOperation, FailureKeepsValueAndUpdatesExpected) {
    lscq::Entry value{3u, 4u};
    lscq::Entry expected{9u, 9u};
    const lscq::Entry desired{5u, 6u};

    EXPECT_FALSE(lscq::cas2(&value, expected, desired));
    EXPECT_EQ(value, (lscq::Entry{3u, 4u}));
    EXPECT_EQ(expected, (lscq::Entry{3u, 4u}));
}

TEST(Cas2_BasicOperation, NullPointerReturnsFalse) {
    lscq::Entry expected{1u, 2u};
    const lscq::Entry desired{3u, 4u};

    EXPECT_FALSE(lscq::cas2(nullptr, expected, desired));
    EXPECT_EQ(expected, (lscq::Entry{1u, 2u}));
}

TEST(Cas2_Concurrent, MultiThreadedIncrementIsCorrect) {
    lscq::Entry shared{0u, 0u};

    const std::size_t hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    const std::size_t threads = (hw == 0u) ? 4u : (hw > 8u ? 8u : hw);
    const std::size_t iters_per_thread = 5000u;

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};

    auto worker = [&]() {
        ready.fetch_add(1, std::memory_order_relaxed);
        while (!start.load(std::memory_order_acquire)) {
            // spin
        }

        lscq::Entry expected{0u, 0u};
        for (std::size_t i = 0; i < iters_per_thread; ++i) {
            while (true) {
                const lscq::Entry desired{expected.cycle_flags + 1u, expected.index_or_ptr + 1u};
                if (lscq::cas2(&shared, expected, desired)) {
                    expected = desired;
                    break;
                }
            }
        }
    };

    std::vector<std::thread> ts;
    ts.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        ts.emplace_back(worker);
    }

    while (ready.load(std::memory_order_acquire) != threads) {
        // spin
    }
    start.store(true, std::memory_order_release);

    for (auto& t : ts) {
        t.join();
    }

    const std::uint64_t expected_total = static_cast<std::uint64_t>(threads * iters_per_thread);
    EXPECT_EQ(shared.cycle_flags, expected_total);
    EXPECT_EQ(shared.index_or_ptr, expected_total);
}

TEST(Cas2_RuntimeDetection, HasCas2SupportMatchesConfigurationAndCpu) {
    if (!lscq::kEnableCas2) {
        EXPECT_FALSE(lscq::has_cas2_support());
        return;
    }

#if LSCQ_ARCH_X86_64
    EXPECT_EQ(lscq::has_cas2_support(), lscq::detail::cpu_has_cmpxchg16b());
#elif LSCQ_ARCH_ARM64
    EXPECT_TRUE(lscq::has_cas2_support());
#else
    EXPECT_FALSE(lscq::has_cas2_support());
#endif
}

TEST(Cas2_BasicOperation, FallbackMutexAndNativeImplAreCorrectWhenCallable) {
    {
        lscq::Entry value{1u, 2u};
        lscq::Entry expected = value;
        const lscq::Entry desired{3u, 4u};
        EXPECT_TRUE(lscq::detail::cas2_mutex(&value, expected, desired));
        EXPECT_EQ(value, desired);
        EXPECT_EQ(expected, (lscq::Entry{1u, 2u}));
    }

    if (!lscq::kEnableCas2) {
        return;
    }

#if LSCQ_ARCH_X86_64
    if (!lscq::detail::cpu_has_cmpxchg16b()) {
        return;
    }
#elif !LSCQ_ARCH_ARM64
    return;
#endif

    {
        lscq::Entry value{10u, 20u};
        lscq::Entry expected = value;
        const lscq::Entry desired{30u, 40u};
        EXPECT_TRUE(lscq::detail::cas2_native(&value, expected, desired));
        EXPECT_EQ(value, desired);
        EXPECT_EQ(expected, (lscq::Entry{10u, 20u}));
    }
}

TEST(Cas2_FallbackStripedLocks, StripeIndexMatchesFormula) {
    constexpr std::size_t stripe_count = lscq::detail::kCas2FallbackStripeCount;
    static_assert((stripe_count & (stripe_count - 1u)) == 0u, "stripe_count must be power-of-two");

    std::vector<lscq::Entry> values(stripe_count * 2u, lscq::Entry{0u, 0u});
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(&values[i]);
        const std::size_t expected_idx = static_cast<std::size_t>((p >> 4u) & (stripe_count - 1u));
        EXPECT_EQ(lscq::detail::cas2_fallback_stripe_index(&values[i]), expected_idx);
    }
}

TEST(Cas2_FallbackStripedLocks, ConcurrentStressSameStripeAndAllStripes) {
    constexpr std::size_t stripe_count = lscq::detail::kCas2FallbackStripeCount;
    constexpr std::size_t slots_per_stripe = 4u;
    static_assert(slots_per_stripe > 0u, "slots_per_stripe must be positive");

    const std::size_t hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    const std::size_t threads = (hw == 0u) ? 8u : (hw > 16u ? 16u : hw);
    const std::size_t iters_per_thread = 20000u;

    std::vector<lscq::Entry> values(stripe_count * slots_per_stripe, lscq::Entry{0u, 0u});

    auto cas2_increment_once = [](lscq::Entry* ptr) {
        lscq::Entry expected{0u, 0u};
        while (true) {
            const lscq::Entry desired{expected.cycle_flags + 1u, expected.index_or_ptr + 1u};
            if (lscq::detail::cas2_mutex(ptr, expected, desired)) {
                return;
            }
        }
    };

    auto run = [&](const std::vector<lscq::Entry*>& slots) {
        std::atomic<std::size_t> ready{0};
        std::atomic<bool> start{false};

        auto worker = [&](std::uint64_t seed) {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                // spin
            }

            auto xorshift64 = [&seed]() {
                seed ^= seed << 13u;
                seed ^= seed >> 7u;
                seed ^= seed << 17u;
                return seed;
            };

            for (std::size_t i = 0; i < iters_per_thread; ++i) {
                const std::size_t idx = static_cast<std::size_t>(xorshift64() % slots.size());
                cas2_increment_once(slots[idx]);
            }
        };

        std::vector<std::thread> ts;
        ts.reserve(threads);
        for (std::size_t t = 0; t < threads; ++t) {
            ts.emplace_back(worker, 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(t));
        }

        while (ready.load(std::memory_order_acquire) != threads) {
            // spin
        }
        start.store(true, std::memory_order_release);

        for (auto& t : ts) {
            t.join();
        }

        std::uint64_t sum_cycle = 0;
        std::uint64_t sum_ptr = 0;
        for (auto* p : slots) {
            sum_cycle += p->cycle_flags;
            sum_ptr += p->index_or_ptr;
        }

        const std::uint64_t expected_total = static_cast<std::uint64_t>(threads * iters_per_thread);
        EXPECT_EQ(sum_cycle, expected_total);
        EXPECT_EQ(sum_ptr, expected_total);
    };

    // Phase 1: all selected slots hash to the same stripe -> worst-case contention (single stripe).
    std::vector<lscq::Entry*> same_stripe_slots;
    same_stripe_slots.reserve(slots_per_stripe);
    for (std::size_t i = 0; i < slots_per_stripe; ++i) {
        same_stripe_slots.push_back(&values[i * stripe_count]);
    }
    const std::size_t stripe0 = lscq::detail::cas2_fallback_stripe_index(same_stripe_slots[0]);
    for (auto* p : same_stripe_slots) {
        EXPECT_EQ(lscq::detail::cas2_fallback_stripe_index(p), stripe0);
    }
    run(same_stripe_slots);

    for (auto& v : values) {
        v = lscq::Entry{0u, 0u};
    }

    // Phase 2: spread across all stripes -> exercises striped lock selection under load.
    std::vector<lscq::Entry*> all_slots;
    all_slots.reserve(values.size());
    for (auto& v : values) {
        all_slots.push_back(&v);
    }
    run(all_slots);
}
