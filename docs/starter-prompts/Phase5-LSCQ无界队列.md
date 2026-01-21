# Phase 5 StarterPrompt: LSCQ无界队列

> **任务代号**: LSCQ-Phase5-LSCQ
> **预计工期**: 2周
> **前置依赖**: Phase 4 完成（SCQP就绪）
> **后续阶段**: Phase 6 (优化与多平台支持)

---

## 1. 任务概述

### 1.1 核心目标
实现论文的最终目标 **LSCQ (Linked Scalable Circular Queue)**，通过链接多个SCQ/SCQP节点实现无界队列。关键技术：
1. **链表结构** - SCQNode链表，动态扩展
2. **Finalize机制** - 标记满节点，触发下一个节点
3. **Epoch-Based Reclamation (EBR)** - 安全回收已空节点
4. **高并发性能** - 百万级元素无性能衰减

### 1.2 技术挑战
- **内存回收**: 无GC环境下安全回收节点（EBR实现）
- **链表并发**: head/tail指针的CAS操作
- **Finalize时机**: 避免过早或过晚finalize
- **性能权衡**: 链表开销 vs 无界能力

### 1.3 任务价值
- ✅ 实现完整的无界MPMC队列
- ✅ 解决有界队列的容量限制问题
- ✅ 验证lock-free内存回收机制
- ✅ 完成论文算法的完整复现

---

## 2. 任务边界

### 2.1 In Scope (本阶段必须完成)
- [x] SCQNode结构定义（包含SCQ/SCQP + next指针）
- [x] LSCQ类定义（head/tail节点指针）
- [x] Finalize机制实现（is_finalized标志位）
- [x] enqueue实现（动态扩展链表）
- [x] dequeue实现（跨节点出队）
- [x] **Epoch-Based Reclamation** 实现
- [x] 内存泄漏测试（百万级元素）
- [x] 压力测试（长时间运行）
- [x] 性能测试（vs 有界队列）

### 2.2 Out of Scope (本阶段不涉及)
- ❌ ARM64移植（Phase 6实现）
- ❌ 生产环境优化（Phase 6实现）
- ❌ 多平台Fallback（Phase 6实现）

---

## 3. 前置条件与输入

### 3.1 前置依赖
- ✅ Phase 4已通过所有验收Gate
- ✅ `docs/Phase4-交接文档.md` 已创建
- ✅ SCQP工作正常（作为LSCQ节点）

### 3.2 必读文档（按顺序）
1. **`docs/Phase4-交接文档.md`** - SCQP实现和队列满检测
2. **`docs/01-技术实现思路.md` 第4.4节** - LSCQ算法和EBR原理
3. **`1908.04511v1.pdf` Figure 9** - LSCQ伪代码（论文核心）
4. **`1908.04511v1.pdf` 第5节** - 内存回收机制
5. **`docs/02-分段开发计划.md` 第7节** - LSCQ实现计划

### 3.3 关键代码复用（来自Phase 4）
- `include/lscq/scqp.hpp` - 作为链表节点（复用）
- `src/scqp.cpp` - 节点操作（复用）
- `tests/unit/test_scqp.cpp` - 测试模式（扩展为无界测试）

### 3.4 环境要求
- Phase 4构建环境
- 理解Epoch-Based Reclamation原理
- 理解lock-free链表操作

---

## 4. 详细任务清单

### 4.1 Epoch-Based Reclamation (EBR) 实现 (Day 1-3)

#### 4.1.1 EBR原理（C++无GC环境）

**问题**: 如何安全回收SCQNode？
- **危险**: 线程A正在访问节点N，线程B回收N → Segfault
- **GC解决**: Go语言自动管理（论文实现）
- **C++方案**: Epoch-Based Reclamation（手动管理）

**EBR核心思想**:
1. **全局Epoch**: 单调递增计数器（例如：1, 2, 3, ...）
2. **线程局部Epoch**: 每个线程记录进入时的epoch
3. **延迟回收**: 节点不立即释放，加入"待回收列表"
4. **安全回收**: 所有线程离开旧epoch后，回收旧节点

**数学保证**:
```
设当前全局Epoch = E
线程T正在访问节点N，则T的局部epoch = E或E-1
只有当所有线程的局部epoch >= E+1时，epoch E-2的节点才能安全回收
```

#### 4.1.2 创建 `include/lscq/detail/epoch.hpp`

```cpp
#ifndef LSCQ_EPOCH_HPP
#define LSCQ_EPOCH_HPP

#include <atomic>
#include <vector>
#include <mutex>
#include <cstdint>

namespace lscq {
namespace detail {

/// @brief Epoch-Based Reclamation管理器
template<typename T>
class EpochManager {
public:
    static EpochManager& instance() {
        static EpochManager mgr;
        return mgr;
    }

    /// @brief 线程进入临界区，返回guard
    class EpochGuard {
    public:
        EpochGuard(EpochManager& mgr) : mgr_(mgr) {
            local_epoch_ = mgr_.enter();
        }

        ~EpochGuard() {
            mgr_.exit(local_epoch_);
        }

        EpochGuard(const EpochGuard&) = delete;
        EpochGuard& operator=(const EpochGuard&) = delete;

    private:
        EpochManager& mgr_;
        uint64_t local_epoch_;
    };

    /// @brief 标记节点待回收
    void retire(T* node);

    /// @brief 尝试回收旧节点
    void try_reclaim();

private:
    EpochManager() : global_epoch_(0) {}

    uint64_t enter();
    void exit(uint64_t epoch);

    uint64_t min_epoch();

    std::atomic<uint64_t> global_epoch_;
    std::atomic<int> active_threads_{0};

    struct RetiredNode {
        T* ptr;
        uint64_t epoch;
    };

    std::mutex retired_mutex_;
    std::vector<RetiredNode> retired_nodes_;
};

// === 实现 ===

template<typename T>
uint64_t EpochManager<T>::enter() {
    active_threads_.fetch_add(1, std::memory_order_relaxed);
    return global_epoch_.load(std::memory_order_acquire);
}

template<typename T>
void EpochManager<T>::exit(uint64_t epoch) {
    active_threads_.fetch_sub(1, std::memory_order_relaxed);

    // 尝试推进全局epoch（如果没有活跃线程）
    if (active_threads_.load(std::memory_order_relaxed) == 0) {
        uint64_t current = global_epoch_.load(std::memory_order_relaxed);
        global_epoch_.compare_exchange_weak(
            current, current + 1,
            std::memory_order_release,
            std::memory_order_relaxed
        );
    }

    // 定期回收
    if (epoch % 100 == 0) {
        try_reclaim();
    }
}

template<typename T>
void EpochManager<T>::retire(T* node) {
    uint64_t current_epoch = global_epoch_.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock(retired_mutex_);
    retired_nodes_.push_back({node, current_epoch});

    // 定期回收
    if (retired_nodes_.size() > 1000) {
        try_reclaim();
    }
}

template<typename T>
void EpochManager<T>::try_reclaim() {
    uint64_t safe_epoch = min_epoch();

    std::lock_guard<std::mutex> lock(retired_mutex_);

    auto it = retired_nodes_.begin();
    while (it != retired_nodes_.end()) {
        // 如果节点的epoch比所有活跃线程都旧，可以安全回收
        if (it->epoch + 2 < safe_epoch) {
            delete it->ptr;
            it = retired_nodes_.erase(it);
        } else {
            ++it;
        }
    }
}

template<typename T>
uint64_t EpochManager<T>::min_epoch() {
    // 简化实现：返回 global_epoch - 2
    // 生产实现需要跟踪每个线程的局部epoch
    uint64_t ge = global_epoch_.load(std::memory_order_acquire);
    return (ge > 2) ? (ge - 2) : 0;
}

}  // namespace detail
}  // namespace lscq

#endif  // LSCQ_EPOCH_HPP
```

**关键设计点**:
1. **EpochGuard**: RAII管理线程进入/退出
2. **retire()**: 延迟回收节点
3. **try_reclaim()**: 定期清理旧节点
4. **min_epoch()**: 计算安全回收阈值

**验收点**: 头文件编译无错误

---

### 4.2 LSCQ链表结构 (Day 4-5)

#### 4.2.1 创建 `include/lscq/lscq.hpp`

```cpp
#ifndef LSCQ_LSCQ_HPP
#define LSCQ_LSCQ_HPP

#include "lscq/scqp.hpp"
#include "lscq/detail/epoch.hpp"
#include <atomic>
#include <memory>

namespace lscq {

/// @brief LSCQ - 无界MPMC队列（链接多个SCQ节点）
/// @tparam T 对象类型
template<typename T>
class LSCQ {
public:
    LSCQ();
    ~LSCQ();

    LSCQ(const LSCQ&) = delete;
    LSCQ& operator=(const LSCQ&) = delete;

    void enqueue(T* ptr);
    T* dequeue();

private:
    /// @brief SCQ节点（链表节点）
    struct SCQNode {
        SCQP<T> queue;                       // 内嵌的有界队列
        std::atomic<SCQNode*> next;          // 下一个节点
        std::atomic<bool> is_finalized;      // 是否已满（finalized）

        SCQNode() : next(nullptr), is_finalized(false) {}
    };

    alignas(64) std::atomic<SCQNode*> head_;  // 出队节点
    alignas(64) std::atomic<SCQNode*> tail_;  // 入队节点

    using EpochMgr = detail::EpochManager<SCQNode>;

    /// @brief Finalize当前节点，创建新节点
    void finalize_and_advance(SCQNode* node);

    /// @brief 尝试回收空节点
    void try_reclaim_head();
};

}  // namespace lscq

#endif  // LSCQ_LSCQ_HPP
```

**关键设计点**:
1. **SCQNode**: 包含SCQP + next指针 + is_finalized标志
2. **head_/tail_**: 链表头尾指针（原子操作）
3. **EpochMgr**: EBR管理器（单例）

**验收点**: 头文件编译无错误

---

### 4.3 LSCQ核心实现 (Day 6-8)

#### 4.3.1 创建 `src/lscq.cpp`

```cpp
#include "lscq/lscq.hpp"

namespace lscq {

template<typename T>
LSCQ<T>::LSCQ() {
    // 创建初始节点
    SCQNode* initial = new SCQNode();
    head_.store(initial, std::memory_order_relaxed);
    tail_.store(initial, std::memory_order_relaxed);
}

template<typename T>
LSCQ<T>::~LSCQ() {
    // 清空队列
    SCQNode* node = head_.load(std::memory_order_relaxed);
    while (node != nullptr) {
        SCQNode* next = node->next.load(std::memory_order_relaxed);
        delete node;
        node = next;
    }
}

// === enqueue实现（动态扩展） ===
template<typename T>
void LSCQ<T>::enqueue(T* ptr) {
    auto guard = typename EpochMgr::EpochGuard(EpochMgr::instance());

    while (true) {
        SCQNode* tail = tail_.load(std::memory_order_acquire);

        // 尝试入队到当前tail节点
        if (tail->queue.enqueue(ptr)) {
            return;  // 成功
        }

        // 队列满，检查是否已finalized
        if (tail->is_finalized.load(std::memory_order_acquire)) {
            // 已有其他线程finalize，尝试tail的next
            SCQNode* next = tail->next.load(std::memory_order_acquire);
            if (next != nullptr) {
                // 帮助推进tail
                tail_.compare_exchange_weak(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed
                );
            }
            continue;
        }

        // 尝试finalize当前节点
        bool expected = false;
        if (tail->is_finalized.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // 成功finalize，创建新节点
            finalize_and_advance(tail);
        }
        // 其他线程finalize了，重试
    }
}

// === dequeue实现（跨节点） ===
template<typename T>
T* LSCQ<T>::dequeue() {
    auto guard = typename EpochMgr::EpochGuard(EpochMgr::instance());

    while (true) {
        SCQNode* head = head_.load(std::memory_order_acquire);

        // 尝试从当前head节点出队
        T* result = head->queue.dequeue();
        if (result != nullptr) {
            return result;  // 成功
        }

        // 当前节点空，检查是否有next
        SCQNode* next = head->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            // 整个队列为空
            return nullptr;
        }

        // 推进head到下一个节点
        if (head_.compare_exchange_weak(
                head, next,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            // 成功推进，延迟回收旧head
            EpochMgr::instance().retire(head);
        }
    }
}

// === Finalize机制 ===
template<typename T>
void LSCQ<T>::finalize_and_advance(SCQNode* node) {
    // 创建新节点
    SCQNode* new_node = new SCQNode();

    // 链接到当前节点
    SCQNode* expected_next = nullptr;
    if (!node->next.compare_exchange_strong(
            expected_next, new_node,
            std::memory_order_release,
            std::memory_order_relaxed)) {
        // 其他线程已创建，删除我们的节点
        delete new_node;
        new_node = node->next.load(std::memory_order_acquire);
    }

    // 推进tail
    tail_.compare_exchange_weak(
        node, new_node,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

// 显式实例化
template class LSCQ<int>;
template class LSCQ<uint64_t>;
template class LSCQ<void>;

}  // namespace lscq
```

**关键算法点**:
1. **enqueue扩展**: 队列满时finalize并创建新节点
2. **dequeue跨节点**: head节点空时推进到next
3. **EpochGuard**: RAII保护临界区
4. **retire()**: 延迟回收旧head节点

**验收点**: 编译通过，无语法错误

---

### 4.4 内存泄漏测试 (Day 9-10)

#### 4.4.1 创建 `tests/unit/test_lscq.cpp`

**核心测试**: 验证内存回收和无界能力

```cpp
#include <gtest/gtest.h>
#include "lscq/lscq.hpp"
#include <thread>
#include <vector>
#include <memory>

using namespace lscq;

// === 基础无界队列测试 ===
TEST(LSCQTest, BasicUnbounded) {
    LSCQ<int> queue;

    // 插入远超单个SCQ容量的元素
    constexpr int COUNT = 100000;  // 100k元素
    std::vector<int> objects(COUNT);

    for (int i = 0; i < COUNT; ++i) {
        objects[i] = i;
        queue.enqueue(&objects[i]);
    }

    // 全部出队
    for (int i = 0; i < COUNT; ++i) {
        int* ptr = queue.dequeue();
        ASSERT_NE(ptr, nullptr) << "Dequeue failed at " << i;
        EXPECT_EQ(*ptr, i);
    }

    EXPECT_EQ(queue.dequeue(), nullptr);  // 空队列
}

// === 内存泄漏测试（关键） ===
TEST(LSCQTest, NoMemoryLeak) {
    LSCQ<int> queue;

    // 百万级入队出队
    constexpr int ITERATIONS = 1000000;

    std::vector<int> pool(100);
    for (int i = 0; i < 100; ++i) {
        pool[i] = i;
    }

    for (int iter = 0; iter < ITERATIONS; ++iter) {
        int idx = iter % 100;
        queue.enqueue(&pool[idx]);
    }

    for (int iter = 0; iter < ITERATIONS; ++iter) {
        int* ptr = queue.dequeue();
        ASSERT_NE(ptr, nullptr);
    }

    // 检查：使用Valgrind或AddressSanitizer
    // 验证无内存泄漏
}

// === 并发无界队列测试 ===
TEST(LSCQTest, ConcurrentUnbounded) {
    constexpr int NUM_THREADS = 8;
    constexpr int ITEMS_PER_THREAD = 10000;

    LSCQ<int> queue;

    std::vector<std::vector<int>> objects(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; ++i) {
        objects[i].resize(ITEMS_PER_THREAD);
        for (int j = 0; j < ITEMS_PER_THREAD; ++j) {
            objects[i][j] = i * ITEMS_PER_THREAD + j;
        }
    }

    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};

    auto producer = [&](int id) {
        for (int i = 0; i < ITEMS_PER_THREAD; ++i) {
            queue.enqueue(&objects[id][i]);
            enqueued.fetch_add(1);
        }
    };

    auto consumer = [&]() {
        while (dequeued.load() < NUM_THREADS * ITEMS_PER_THREAD) {
            int* ptr = queue.dequeue();
            if (ptr != nullptr) {
                dequeued.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS / 2; ++i) {
        threads.emplace_back(producer, i);
    }
    for (int i = 0; i < NUM_THREADS / 2; ++i) {
        threads.emplace_back(consumer);
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(enqueued.load(), NUM_THREADS / 2 * ITEMS_PER_THREAD);
    EXPECT_EQ(dequeued.load(), NUM_THREADS / 2 * ITEMS_PER_THREAD);
}

// === 压力测试（长时间运行） ===
TEST(LSCQTest, StressTest) {
    LSCQ<int> queue;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_ops{0};

    auto worker = [&]() {
        int local_val = 42;
        while (!stop.load()) {
            queue.enqueue(&local_val);
            queue.dequeue();
            total_ops.fetch_add(2);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(worker);
    }

    // 运行10秒
    std::this_thread::sleep_for(std::chrono::seconds(10));
    stop.store(true);

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Total ops: " << total_ops.load() << "\n";
    EXPECT_GT(total_ops.load(), 1000000);  // 至少100万次操作
}

// === EBR回收测试 ===
TEST(LSCQTest, EBR_Reclamation) {
    LSCQ<int> queue;

    // 创建多个节点（触发finalize）
    constexpr int NODES = 10;
    constexpr int PER_NODE = 100000;  // 超过SCQSIZE

    std::vector<int> objects(NODES * PER_NODE);
    for (int i = 0; i < NODES * PER_NODE; ++i) {
        objects[i] = i;
        queue.enqueue(&objects[i]);
    }

    // 出队触发head推进和节点回收
    for (int i = 0; i < NODES * PER_NODE; ++i) {
        queue.dequeue();
    }

    // 验证无内存泄漏（Valgrind）
}
```

**测试覆盖点**:
- ✅ 无界能力（10万+元素）
- ✅ 内存泄漏（百万级操作）
- ✅ 并发正确性
- ✅ 压力测试（长时间运行）
- ✅ EBR回收机制

**验收点**:
```bash
valgrind --leak-check=full ./build/debug/tests/lscq_tests --gtest_filter="LSCQTest.*"
# 无内存泄漏报告
```

---

### 4.5 性能Benchmark (Day 11-12)

#### 4.5.1 创建 `benchmarks/benchmark_lscq.cpp`

```cpp
#include <benchmark/benchmark.h>
#include "lscq/lscq.hpp"
#include "lscq/scqp.hpp"
#include <thread>
#include <vector>

using namespace lscq;

// === LSCQ Pair Benchmark ===
static void BM_LSCQ_Pair(benchmark::State& state) {
    int thread_count = state.range(0);
    LSCQ<int> queue;

    std::vector<int> pool(thread_count * 1000);
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i] = i;
    }

    auto worker = [&](int id) {
        for (auto _ : state) {
            queue.enqueue(&pool[id]);
            int* ptr = queue.dequeue();
            benchmark::DoNotOptimize(ptr);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    state.SetItemsProcessed(state.iterations() * thread_count * 2);
}

BENCHMARK(BM_LSCQ_Pair)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// === SCQP Pair Benchmark（对比） ===
static void BM_SCQP_Pair(benchmark::State& state) {
    int thread_count = state.range(0);
    SCQP<int> queue;

    std::vector<int> pool(thread_count * 1000);
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i] = i;
    }

    // 预填充
    for (int i = 0; i < thread_count * 10; ++i) {
        queue.enqueue(&pool[i]);
    }

    auto worker = [&](int id) {
        for (auto _ : state) {
            while (!queue.enqueue(&pool[id])) {
                // 队列满
            }
            int* ptr = queue.dequeue();
            benchmark::DoNotOptimize(ptr);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    state.SetItemsProcessed(state.iterations() * thread_count * 2);
}

BENCHMARK(BM_SCQP_Pair)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
```

**性能预期**（Phase 5）:
| 场景 | LSCQ | SCQP | 差异 |
|------|------|------|------|
| Pair @ 1 thread | ~7 Mops/sec | ~8 Mops/sec | -12% |
| Pair @ 16 threads | ~40 Mops/sec | ~45 Mops/sec | -11% |

**解释**: LSCQ因链表开销，性能略低于SCQP（10-15%），但提供无界能力

**验收点**:
```bash
./build/release/benchmarks/lscq_benchmarks --benchmark_filter="LSCQ|SCQP"
```

---

### 4.6 CMake集成 (Day 13-14)

更新 `CMakeLists.txt`:
```cmake
add_library(lscq_impl STATIC
    src/ncq.cpp
    src/scq.cpp
    src/scqp.cpp
    src/lscq.cpp  # 新增
)

add_executable(lscq_tests
    tests/unit/test_cas2.cpp
    tests/unit/test_ncq.cpp
    tests/unit/test_scq.cpp
    tests/unit/test_scqp.cpp
    tests/unit/test_lscq.cpp  # 新增
)

add_executable(lscq_benchmarks
    benchmarks/benchmark_cas2.cpp
    benchmarks/benchmark_ncq.cpp
    benchmarks/benchmark_scq.cpp
    benchmarks/benchmark_scqp.cpp
    benchmarks/benchmark_lscq.cpp  # 新增
)
```

---

## 5. 交付物清单

### 5.1 代码文件
- [ ] `include/lscq/detail/epoch.hpp` - EBR管理器
- [ ] `include/lscq/lscq.hpp` - LSCQ类声明
- [ ] `src/lscq.cpp` - LSCQ实现
- [ ] `tests/unit/test_lscq.cpp` - LSCQ单元测试
- [ ] `benchmarks/benchmark_lscq.cpp` - LSCQ vs SCQP对比
- [ ] 更新后的 `CMakeLists.txt`

### 5.2 测试结果
- [ ] 所有单元测试通过
- [ ] 内存泄漏测试通过（Valgrind零泄漏）
- [ ] 压力测试通过（10秒无崩溃）
- [ ] Benchmark结果（保存为 `docs/Phase5-Benchmark结果.md`）

### 5.3 文档
- [ ] `docs/Phase5-交接文档.md` - **必须创建**
- [ ] `docs/Phase5-Benchmark结果.md` - LSCQ vs SCQP性能数据

---

## 6. 验收标准 (Gate Conditions)

### 6.1 编译验收
- ✅ **G1.1**: LSCQ代码零警告编译通过
- ✅ **G1.2**: EBR模块编译通过

### 6.2 功能验收
- ✅ **G2.1**: BasicUnbounded测试通过（10万元素）
- ✅ **G2.2**: NoMemoryLeak测试通过（Valgrind零泄漏）
- ✅ **G2.3**: ConcurrentUnbounded测试通过
- ✅ **G2.4**: StressTest测试通过（10秒 > 100万ops）
- ✅ **G2.5**: 无data race（ThreadSanitizer）

### 6.3 性能验收
- ✅ **G3.1**: LSCQ @ 16 threads > 35 Mops/sec
- ✅ **G3.2**: LSCQ性能在SCQP的85-95%范围内
- ✅ **G3.3**: 百万级元素无性能衰减

### 6.4 文档验收
- ✅ **G4.1**: `docs/Phase5-交接文档.md` 已创建（>300行）

---

## 7. 下一阶段预览

### 7.1 Phase 6概述
**阶段名称**: 优化与多平台支持
**核心任务**: 性能调优、ARM64移植、文档完善
**关键技术**: Profiling优化、平台适配、CI多平台
**预计工期**: 2-3周

### 7.2 Phase 6依赖本阶段的关键产出
1. **完整的LSCQ实现** - Phase 6优化的基础
2. **Benchmark框架** - 验证优化效果
3. **EBR机制** - 跨平台内存回收

---

## 8. 阶段完成交接文档要求

创建 `docs/Phase5-交接文档.md`，包含：

```markdown
# Phase 5 交接文档

## 1. 完成情况概览
## 2. 关键代码位置索引
- EBR管理器：`include/lscq/detail/epoch.hpp:L15-L120`
- LSCQ enqueue：`src/lscq.cpp:L30-L70`
- Finalize机制：`src/lscq.cpp:L100-L130`

## 3. LSCQ算法验证结果
- 无界能力：✅ 10万+元素无问题
- 内存回收：✅ Valgrind零泄漏
- 并发正确性：✅ 8线程×10000元素

## 4. 性能Benchmark结果
| 场景 | LSCQ | SCQP | 差异 |
|------|------|------|------|
| ...  | ...  | ...  | ...  |

## 5. EBR机制说明
- Epoch推进策略
- 节点回收时机
- 内存占用分析

## 6. Phase 6优化方向
- profiling热点函数
- 减少EBR开销
- ARM64 CAS2移植
```

---

## 9. 常见问题（FAQ）

### Q1: LSCQ性能为什么比SCQP低？
**A**: 链表开销（next指针访问、节点创建/回收）增加10-15%开销，但换来无界能力。

### Q2: EBR会导致内存延迟释放吗？
**A**: 是的，节点可能延迟2-3个epoch释放（通常毫秒级），但保证安全性。

### Q3: 如何验证无内存泄漏？
**A**: 使用Valgrind或AddressSanitizer运行测试，确认"definitely lost: 0 bytes"。

### Q4: LSCQ适合什么场景？
**A**: 需要无界队列且可容忍10-15%性能损失的场景（如消息队列、任务池）。

---

## 10. 参考资料

- `docs/Phase4-交接文档.md` - SCQP实现
- `docs/01-技术实现思路.md` 第4.4节 - LSCQ算法
- `1908.04511v1.pdf` Figure 9 - LSCQ伪代码
- `1908.04511v1.pdf` 第5节 - 内存回收
- [Epoch-Based Reclamation](https://www.cs.toronto.edu/~tomhart/papers/tomhart_thesis.pdf)

---

**StarterPrompt版本**: v1.0
**创建日期**: 2026-01-18
**适用范围**: LSCQ项目 Phase 5

---

Phase4交接文档： @docs\Phase4-交接文档.md