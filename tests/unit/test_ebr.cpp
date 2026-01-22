#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <lscq/ebr.hpp>
#include <thread>
#include <vector>

namespace {

// Test node structure for retirement
struct TestNode {
    std::atomic<int> value{0};
    static std::atomic<int> delete_count;

    TestNode(int v = 0) : value(v) {}
    ~TestNode() { delete_count.fetch_add(1, std::memory_order_relaxed); }
};

std::atomic<int> TestNode::delete_count{0};

// Helper to wait for a condition with timeout
template <class Pred>
bool wait_for(Pred&& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return pred();
}

}  // namespace

// Basic functionality tests
TEST(EBR_Basic, ConstructDestruct) {
    lscq::EBRManager ebr;
    EXPECT_EQ(ebr.current_epoch(), 0u);
    EXPECT_FALSE(ebr.has_pending());
}

TEST(EBR_Basic, EnterExitCritical) {
    lscq::EBRManager ebr;

    ebr.enter_critical();
    ebr.exit_critical();

    // Should not crash
    SUCCEED();
}

TEST(EBR_Basic, EpochGuardRAII) {
    lscq::EBRManager ebr;

    {
        lscq::EpochGuard guard(ebr);
        // Inside critical section
    }  // Guard should auto-exit

    // Should not crash
    SUCCEED();
}

TEST(EBR_Basic, RetireSingleNode) {
    lscq::EBRManager ebr;
    TestNode::delete_count.store(0, std::memory_order_relaxed);

    TestNode* node = new TestNode(42);
    ebr.retire(node);

    EXPECT_TRUE(ebr.has_pending());
    EXPECT_EQ(ebr.pending_count(), 1u);
}

TEST(EBR_Basic, ReclaimAfterEpochAdvance) {
    lscq::EBRManager ebr;
    TestNode::delete_count.store(0, std::memory_order_relaxed);

    TestNode* node = new TestNode(42);
    ebr.retire(node);  // Retired at epoch 0

    EXPECT_TRUE(ebr.has_pending());

    // Advance epoch multiple times to trigger reclamation
    // Nodes can be reclaimed when current_epoch >= retire_epoch + 2
    ebr.try_reclaim();               // Epoch 0 -> 1, safe_epoch would be -1, cannot reclaim
    EXPECT_TRUE(ebr.has_pending());  // Still pending

    ebr.try_reclaim();  // Epoch 1 -> 2, safe_epoch = 0, can reclaim epoch 0 nodes
    // Should be reclaimed now
    EXPECT_FALSE(ebr.has_pending());
    EXPECT_EQ(TestNode::delete_count.load(std::memory_order_relaxed), 1);
}

TEST(EBR_Basic, ReclaimMultipleNodes) {
    lscq::EBRManager ebr;
    TestNode::delete_count.store(0, std::memory_order_relaxed);

    const int N = 10;
    for (int i = 0; i < N; ++i) {
        ebr.retire(new TestNode(i));
    }

    EXPECT_EQ(ebr.pending_count(), static_cast<std::size_t>(N));

    // Advance epochs
    for (int i = 0; i < 3; ++i) {
        ebr.try_reclaim();
    }

    // All should be reclaimed
    EXPECT_FALSE(ebr.has_pending());
    EXPECT_EQ(TestNode::delete_count.load(std::memory_order_relaxed), N);
}

TEST(EBR_EdgeCases, RetireNullptr) {
    lscq::EBRManager ebr;

    ebr.retire<TestNode>(nullptr);  // Should not crash
    EXPECT_FALSE(ebr.has_pending());
}

TEST(EBR_EdgeCases, ReclaimWithoutRetire) {
    lscq::EBRManager ebr;

    std::size_t reclaimed = ebr.try_reclaim();
    EXPECT_EQ(reclaimed, 0u);
    EXPECT_FALSE(ebr.has_pending());
}

TEST(EBR_EdgeCases, DestructorReclaims) {
    TestNode::delete_count.store(0, std::memory_order_relaxed);

    {
        lscq::EBRManager ebr;
        ebr.retire(new TestNode(1));
        ebr.retire(new TestNode(2));
        ebr.retire(new TestNode(3));
    }  // Destructor should reclaim all

    EXPECT_EQ(TestNode::delete_count.load(std::memory_order_relaxed), 3);
}

// Concurrent tests
TEST(EBR_Concurrent, MultipleThreadsEnterExit) {
    lscq::EBRManager ebr;
    constexpr int kNumThreads = 8;
    constexpr int kIterations = 1000;

    std::atomic<int> ready_count{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < kIterations; ++i) {
                lscq::EpochGuard guard(ebr);
                // Inside critical section
                std::this_thread::yield();
            }
        });
    }

    // Wait for all threads to be ready
    while (ready_count.load(std::memory_order_acquire) < kNumThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    SUCCEED();  // Should not deadlock or crash
}

TEST(EBR_Concurrent, ConcurrentRetireAndReclaim) {
    lscq::EBRManager ebr;
    TestNode::delete_count.store(0, std::memory_order_relaxed);

    constexpr int kNumProducers = 4;
    constexpr int kNumReclaimers = 2;
    constexpr int kNodesPerProducer = 100;

    std::atomic<int> ready_count{0};
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;

    // Producer threads: retire nodes
    for (int t = 0; t < kNumProducers; ++t) {
        threads.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < kNodesPerProducer; ++i) {
                lscq::EpochGuard guard(ebr);
                ebr.retire(new TestNode(i));
                std::this_thread::yield();
            }
        });
    }

    // Reclaimer threads: try to reclaim nodes
    for (int t = 0; t < kNumReclaimers; ++t) {
        threads.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (!stop.load(std::memory_order_acquire)) {
                ebr.try_reclaim();
                std::this_thread::yield();
            }
        });
    }

    // Wait for all threads to be ready
    while (ready_count.load(std::memory_order_acquire) < (kNumProducers + kNumReclaimers)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    // Wait for producers to finish
    for (int i = 0; i < kNumProducers; ++i) {
        threads[i].join();
    }

    // Stop reclaimers
    stop.store(true, std::memory_order_release);
    for (int i = kNumProducers; i < kNumProducers + kNumReclaimers; ++i) {
        threads[i].join();
    }

    // Final reclamation passes
    for (int i = 0; i < 5; ++i) {
        ebr.try_reclaim();
    }

    // Eventually all nodes should be reclaimed
    const int total_nodes = kNumProducers * kNodesPerProducer;
    EXPECT_EQ(TestNode::delete_count.load(std::memory_order_relaxed), total_nodes);
}

TEST(EBR_Concurrent, StressTestManyNodesManThreads) {
    lscq::EBRManager ebr;
    TestNode::delete_count.store(0, std::memory_order_relaxed);

#ifdef LSCQ_CI_LIGHTWEIGHT_TESTS
    // CI environment: lightweight test parameters (4 threads × 250 = 1K nodes)
    constexpr int kNumThreads = 4;
    constexpr int kNodesPerThread = 250;
#else
    // Local/full test environment (16 threads × 500 = 8K nodes)
    constexpr int kNumThreads = 16;
    constexpr int kNodesPerThread = 500;
#endif

    std::atomic<int> ready_count{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, tid = t]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < kNodesPerThread; ++i) {
                lscq::EpochGuard guard(ebr);
                ebr.retire(new TestNode(tid * 10000 + i));

                // Periodically try to reclaim
                if (i % 10 == 0) {
                    ebr.try_reclaim();
                }

                std::this_thread::yield();
            }
        });
    }

    // Wait for all threads to be ready
    while (ready_count.load(std::memory_order_acquire) < kNumThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    // Final reclamation passes
    for (int i = 0; i < 10; ++i) {
        ebr.try_reclaim();
        std::this_thread::yield();
    }

    // All nodes should eventually be reclaimed
    const int total_nodes = kNumThreads * kNodesPerThread;
    EXPECT_EQ(TestNode::delete_count.load(std::memory_order_relaxed), total_nodes);
}

// Safety tests: Ensure nodes aren't reclaimed while in use
TEST(EBR_Safety, NodeNotReclaimedWhileInCriticalSection) {
    lscq::EBRManager ebr;
    TestNode::delete_count.store(0, std::memory_order_relaxed);

    TestNode* node = new TestNode(42);

    std::atomic<bool> start{false};
    std::atomic<bool> can_exit{false};

    // Thread 1: Enter critical section and hold it
    std::thread t1([&]() {
        ebr.enter_critical();
        start.store(true, std::memory_order_release);

        // Wait for signal to exit
        while (!can_exit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        ebr.exit_critical();
    });

    // Wait for t1 to enter critical section
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Thread 2: Retire node and try to reclaim
    ebr.retire(node);

    // Try to reclaim multiple times
    for (int i = 0; i < 5; ++i) {
        ebr.try_reclaim();
    }

    // Node should NOT be deleted yet (t1 is still in critical section)
    EXPECT_EQ(TestNode::delete_count.load(std::memory_order_relaxed), 0);
    EXPECT_TRUE(ebr.has_pending());

    // Allow t1 to exit
    can_exit.store(true, std::memory_order_release);
    t1.join();

    // Now reclamation should be possible
    for (int i = 0; i < 3; ++i) {
        ebr.try_reclaim();
    }

    EXPECT_EQ(TestNode::delete_count.load(std::memory_order_relaxed), 1);
}

// Performance/scalability hint test
TEST(EBR_Performance, EpochAdvancesMonotonically) {
    lscq::EBRManager ebr;

    std::uint64_t prev_epoch = ebr.current_epoch();
    for (int i = 0; i < 100; ++i) {
        ebr.try_reclaim();
        std::uint64_t curr_epoch = ebr.current_epoch();
        EXPECT_GE(curr_epoch, prev_epoch);  // Should never decrease
        prev_epoch = curr_epoch;
    }
}
