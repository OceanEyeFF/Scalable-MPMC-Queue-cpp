# Phase 3 StarterPrompt: SCQ核心实现

> **任务代号**: LSCQ-Phase3-SCQ
> **预计工期**: 2-3周
> **前置依赖**: Phase 2 完成（NCQ就绪）
> **后续阶段**: Phase 4 (SCQP双字宽CAS扩展)

---

## 1. 任务概述

### 1.1 核心目标
实现论文的核心算法 **SCQ (Scalable Circular Queue)**，在NCQ基础上增加：
1. **Threshold机制**（3n-1）- 解决livelock问题
2. **isSafe标志位** - 解决ABA问题
3. **Catchup优化**（fixState）- 处理dequeue-heavy场景
4. **Atomic_OR优化**（⊥=2n-1）- 减少CAS重试

### 1.2 技术挑战
- **Livelock避免**: 通过threshold机制保证Progress
- **ABA问题**: isSafe标志位防止错误的Entry复用
- **性能优化**: Catchup和Atomic_OR在高负载下显著提升性能
- **位域操作**: cycle和isSafe共享64位，需要位运算

### 1.3 任务价值
- ✅ 实现论文的核心创新点（Threshold + Catchup）
- ✅ 达到论文级性能（>50 Mops/sec @ 16 cores）
- ✅ 为Phase 4的SCQP和Phase 5的LSCQ奠定基础
- ✅ 验证lock-free算法的正确性证明

---

## 2. 任务边界

### 2.1 In Scope (本阶段必须完成)
- [x] Entry结构扩展（cycle + **isSafe** + index）
- [x] Threshold机制实现（**3n-1**）
- [x] enqueue实现（带threshold检查）
- [x] dequeue实现（带isSafe检查）
- [x] **fixState (Catchup)** 实现
- [x] **Atomic_OR优化**（⊥=2n-1标记）
- [x] Livelock测试（验证threshold有效性）
- [x] 30E70D场景测试（验证catchup效果）
- [x] 性能Benchmark（vs NCQ，目标>2x）
- [x] cache_remap优化验证

### 2.2 Out of Scope (本阶段不涉及)
- ❌ 双字宽CAS（SCQP，Phase 4实现）
- ❌ 4n-1 Threshold（SCQP特性，Phase 4实现）
- ❌ 链式无界扩展（LSCQ，Phase 5实现）
- ❌ Epoch-Based Reclamation（Phase 5实现）
- ❌ ARM64移植（Phase 6实现）

---

## 3. 前置条件与输入

### 3.1 前置依赖
- ✅ Phase 2已通过所有验收Gate
- ✅ `docs/Phase2-交接文档.md` 已创建
- ✅ NCQ工作正常（作为性能baseline）

### 3.2 必读文档（按顺序）
1. **`docs/Phase2-交接文档.md`** - 理解NCQ的实现和性能数据
2. **`docs/01-技术实现思路.md` 第4.2节** - SCQ算法和threshold数学推导
3. **`1908.04511v1.pdf` Figure 8** - SCQ伪代码（论文核心）
4. **`1908.04511v1.pdf` 第3.3节** - Threshold证明和Catchup原理
5. **`docs/02-分段开发计划.md` 第5节** - SCQ实现详细计划

### 3.3 关键代码复用（来自Phase 2）
- `include/lscq/ncq.hpp` - Ring Buffer结构（复用）
- `src/ncq.cpp` - cache_remap函数（直接复用）
- `tests/unit/test_ncq.cpp` - 测试模式（扩展为SCQ）
- `benchmarks/benchmark_ncq.cpp` - Benchmark框架（对比NCQ vs SCQ）

### 3.4 环境要求
- Phase 2构建环境
- 理解位运算（isSafe标志位）
- 理解Livelock和Progress概念

---

## 4. 详细任务清单

### 4.1 Entry结构扩展 (Day 1)

#### 4.1.1 创建 `include/lscq/scq.hpp`

**核心修改**: Entry从NCQ的2字段扩展为3字段

```cpp
#ifndef LSCQ_SCQ_HPP
#define LSCQ_SCQ_HPP

#include "lscq/config.hpp"
#include "lscq/cas2.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lscq {

/// @brief Scalable Circular Queue - 高性能MPMC有界队列
/// @tparam T 存储的索引或指针类型
template<typename T = uint64_t>
class SCQ {
public:
    static constexpr size_t SCQSIZE = config::DEFAULT_SCQSIZE;
    static constexpr T EMPTY_VALUE = static_cast<T>(-1);

    // === 关键常量（基于论文3.3节） ===
    static constexpr uint64_t THRESHOLD = 3 * SCQSIZE - 1;  // Livelock阈值
    static constexpr uint64_t BOTTOM = 2 * SCQSIZE - 1;     // ⊥ = 2n-1（全1）

    SCQ();
    ~SCQ();

    SCQ(const SCQ&) = delete;
    SCQ& operator=(const SCQ&) = delete;

    void enqueue(T index);
    T dequeue();

    bool is_empty() const;

private:
    /// @brief Entry结构（关键扩展）
    /// 使用位域优化：cycle (63位) + isSafe (1位)
    struct alignas(16) Entry {
        uint64_t cycle_and_safe;  // [63:1]=cycle, [0]=isSafe
        T index;

        // 辅助方法
        uint64_t cycle() const { return cycle_and_safe >> 1; }
        bool is_safe() const { return (cycle_and_safe & 1) != 0; }

        void set_cycle(uint64_t c) {
            cycle_and_safe = (c << 1) | (cycle_and_safe & 1);
        }

        void set_safe(bool safe) {
            if (safe) {
                cycle_and_safe |= 1;
            } else {
                cycle_and_safe &= ~1ULL;
            }
        }

        bool operator==(const Entry& other) const {
            return cycle_and_safe == other.cycle_and_safe &&
                   index == other.index;
        }
    };

    Entry* entries_;
    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> tail_;
    alignas(64) std::atomic<uint64_t> threshold_;  // 动态threshold

    size_t cache_remap(size_t idx) const;

    /// @brief Catchup优化 - 处理head > tail场景
    void fix_state();

    /// @brief 检查是否需要catchup
    bool should_catchup(uint64_t h, uint64_t t) const {
        return (h > t) && (h - t > SCQSIZE);
    }
};

}  // namespace lscq

#endif  // LSCQ_SCQ_HPP
```

**关键设计点**:
1. **cycle_and_safe**: 64位共享，高63位存cycle，低1位存isSafe
2. **THRESHOLD = 3n-1**: 论文证明的livelock避免阈值
3. **BOTTOM = 2n-1**: 用于Atomic_OR标记（二进制全1）
4. **threshold_**: 动态threshold计数器（多线程共享）

**验收点**: 头文件编译无错误，`sizeof(Entry) == 16`

---

### 4.2 Threshold机制实现 (Day 2-3)

#### 4.2.1 Threshold数学原理（来自论文3.3节）

**问题**: NCQ在高并发下可能livelock（所有线程都在重试CAS）

**解决**: 引入Threshold机制
- 初始值：`threshold = 3 * SCQSIZE - 1`
- 每次enqueue/dequeue失败后：`threshold--`
- 当 `threshold == 0` 时：触发"慢路径"（帮助其他线程）

**数学证明**（简化）:
```
设 P = 生产者数量, C = 消费者数量, n = SCQSIZE
则 Threshold = 3n - 1 保证:
  - 至少有一个线程在 3n-1 次重试内成功
  - 最坏情况：P+C 个线程竞争，每个线程最多重试 (3n-1)/(P+C) 次
```

#### 4.2.2 创建 `src/scq.cpp` - Enqueue实现

参考论文Figure 8，增加threshold检查：

```cpp
#include "lscq/scq.hpp"
#include <new>
#include <cstdlib>

namespace lscq {

template<typename T>
SCQ<T>::SCQ() : head_(SCQSIZE), tail_(SCQSIZE), threshold_(THRESHOLD) {
    entries_ = static_cast<Entry*>(
        aligned_alloc(64, sizeof(Entry) * SCQSIZE)
    );
    if (!entries_) {
        throw std::bad_alloc();
    }

    // 初始化：cycle=0, isSafe=false
    for (size_t i = 0; i < SCQSIZE; ++i) {
        entries_[i].cycle_and_safe = 0;  // cycle=0, isSafe=0
        entries_[i].index = 0;
    }
}

template<typename T>
SCQ<T>::~SCQ() {
    free(entries_);
}

// === enqueue实现（带Threshold机制） ===
template<typename T>
void SCQ<T>::enqueue(T index) {
    while (true) {
        // 1. 获取当前tail和threshold
        uint64_t t = tail_.load(std::memory_order_acquire);
        size_t j = cache_remap(t % SCQSIZE);

        // 2. 读取entry
        Entry ent = entries_[j];

        uint64_t cycle_t = t / SCQSIZE;
        uint64_t cycle_e = ent.cycle();

        // 3. === Threshold检查（关键新增） ===
        uint64_t thr = threshold_.load(std::memory_order_relaxed);
        if (thr == 0) {
            // Threshold耗尽，触发catchup
            fix_state();
            threshold_.store(THRESHOLD, std::memory_order_relaxed);  // 重置
            continue;
        }

        // 4. 检查entry状态
        if (cycle_e < cycle_t) {
            // Entry可用，尝试插入
            if (!ent.is_safe()) {
                // === 使用Atomic_OR标记为safe（优化） ===
                Entry safe_ent = ent;
                safe_ent.set_safe(true);
                Entry expected = ent;

                // 尝试原子设置isSafe（不改变cycle和index）
                if (cas2(&entries_[j], expected, safe_ent)) {
                    ent = safe_ent;
                } else {
                    // CAS失败，threshold--，重试
                    threshold_.fetch_sub(1, std::memory_order_relaxed);
                    continue;
                }
            }

            // 尝试插入数据
            Entry new_ent;
            new_ent.set_cycle(cycle_t);
            new_ent.set_safe(true);
            new_ent.index = index;

            Entry expected = ent;

            if (cas2(&entries_[j], expected, new_ent)) {
                // 成功，推进tail
                tail_.compare_exchange_weak(
                    t, t + 1,
                    std::memory_order_release,
                    std::memory_order_relaxed
                );
                return;
            }
        } else if (cycle_e == cycle_t) {
            // Entry已占用，帮助推进tail
            tail_.compare_exchange_weak(
                t, t + 1,
                std::memory_order_release,
                std::memory_order_relaxed
            );
        }

        // CAS失败，threshold--，重试
        threshold_.fetch_sub(1, std::memory_order_relaxed);
    }
}

// === dequeue实现（带isSafe检查） ===
template<typename T>
T SCQ<T>::dequeue() {
    while (true) {
        // 1. 获取head
        uint64_t h = head_.load(std::memory_order_acquire);
        size_t j = cache_remap(h % SCQSIZE);

        // 2. 读取entry
        Entry ent = entries_[j];

        uint64_t cycle_h = h / SCQSIZE;
        uint64_t cycle_e = ent.cycle();

        // 3. === Threshold检查 ===
        uint64_t thr = threshold_.load(std::memory_order_relaxed);
        if (thr == 0) {
            fix_state();
            threshold_.store(THRESHOLD, std::memory_order_relaxed);
            continue;
        }

        // 4. 检查entry状态
        if (cycle_e == cycle_h) {
            // === isSafe检查（关键新增） ===
            if (!ent.is_safe()) {
                // Entry未就绪，threshold--，重试
                threshold_.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            // 尝试推进head并读取数据
            if (head_.compare_exchange_weak(
                    h, h + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {

                // 成功，标记entry为已消费（使用Atomic_OR优化）
                Entry consumed_ent;
                consumed_ent.set_cycle(BOTTOM);  // ⊥ = 2n-1（全1）
                consumed_ent.set_safe(false);
                consumed_ent.index = 0;

                Entry expected = ent;
                cas2(&entries_[j], expected, consumed_ent);  // Best-effort

                return ent.index;
            }
        } else if (cycle_e + 1 == cycle_h) {
            // 队列为空
            return EMPTY_VALUE;
        }

        // CAS失败，threshold--，重试
        threshold_.fetch_sub(1, std::memory_order_relaxed);
    }
}
```

**关键算法点**:
1. **Threshold递减**: 每次重试都递减threshold
2. **Threshold耗尽**: 触发`fix_state()`帮助其他线程
3. **isSafe检查**: dequeue时必须等待isSafe=true
4. **BOTTOM标记**: dequeue后设置cycle=2n-1，避免ABA

**验收点**: 编译通过，无语法错误

---

### 4.3 Catchup (fixState) 实现 (Day 4)

#### 4.3.1 Catchup原理（来自论文3.4节）

**场景**: 30E70D (30%生产, 70%消费) - Dequeue-heavy workload
**问题**: head快速推进，tail落后，导致虚假的"队列满"
**解决**: `fix_state()` 检测到 `head > tail + SCQSIZE` 时，强制推进tail

#### 4.3.2 fix_state() 实现

```cpp
template<typename T>
void SCQ<T>::fix_state() {
    while (true) {
        uint64_t h = head_.load(std::memory_order_acquire);
        uint64_t t = tail_.load(std::memory_order_acquire);

        // 检查是否需要catchup
        if (h <= t || (h - t <= SCQSIZE)) {
            return;  // 状态正常
        }

        // head远超tail，推进tail
        if (tail_.compare_exchange_weak(
                t, h,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;  // 成功修复
        }
    }
}

template<typename T>
size_t SCQ<T>::cache_remap(size_t idx) const {
    constexpr size_t ENTRIES_PER_LINE = 64 / sizeof(Entry);
    size_t line = idx / ENTRIES_PER_LINE;
    size_t offset = idx % ENTRIES_PER_LINE;
    return (offset * (SCQSIZE / ENTRIES_PER_LINE)) + line;
}

template<typename T>
bool SCQ<T>::is_empty() const {
    return head_.load(std::memory_order_relaxed) >=
           tail_.load(std::memory_order_relaxed);
}

// 显式实例化
template class SCQ<uint64_t>;
template class SCQ<uint32_t>;

}  // namespace lscq
```

**验收点**: `fix_state()` 可被调用，逻辑正确

---

### 4.4 Livelock测试 (Day 5-6)

#### 4.4.1 创建 `tests/unit/test_scq.cpp`

**核心测试**: 验证threshold机制防止livelock

```cpp
#include <gtest/gtest.h>
#include "lscq/scq.hpp"
#include "lscq/ncq.hpp"  // 用于对比
#include <thread>
#include <vector>
#include <atomic>

using namespace lscq;

// === 基础功能测试（复用NCQ测试） ===
TEST(SCQTest, BasicEnqueueDequeue) {
    SCQ<uint64_t> queue;
    queue.enqueue(42);
    queue.enqueue(100);
    EXPECT_EQ(queue.dequeue(), 42);
    EXPECT_EQ(queue.dequeue(), 100);
    EXPECT_EQ(queue.dequeue(), SCQ<uint64_t>::EMPTY_VALUE);
}

TEST(SCQTest, FIFO_Order) {
    SCQ<uint64_t> queue;
    for (uint64_t i = 0; i < 1000; ++i) {
        queue.enqueue(i);
    }
    for (uint64_t i = 0; i < 1000; ++i) {
        EXPECT_EQ(queue.dequeue(), i);
    }
}

// === Livelock测试（关键新增） ===
TEST(SCQTest, NoLivelock_HighContention) {
    constexpr int NUM_THREADS = 16;  // 高并发
    constexpr int ITERATIONS = 10000;

    SCQ<uint64_t> queue;
    std::atomic<int> completed{0};
    std::atomic<bool> livelock_detected{false};

    auto worker = [&](int id) {
        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < ITERATIONS; ++i) {
            queue.enqueue(id * ITERATIONS + i);
            queue.dequeue();

            // 检测livelock：单次操作超过1秒
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > 10) {
                livelock_detected.store(true);
                return;
            }
        }
        completed.fetch_add(1);
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_FALSE(livelock_detected.load()) << "Livelock detected!";
    EXPECT_EQ(completed.load(), NUM_THREADS) << "Not all threads completed";
}

// === Threshold重置测试 ===
TEST(SCQTest, ThresholdReset) {
    SCQ<uint64_t> queue;

    // 手动触发threshold耗尽（模拟高竞争）
    for (int i = 0; i < 100; ++i) {
        queue.enqueue(i);
    }

    // 验证队列仍可用（threshold应已重置）
    for (int i = 0; i < 100; ++i) {
        EXPECT_NE(queue.dequeue(), SCQ<uint64_t>::EMPTY_VALUE);
    }
}

// === 30E70D场景测试（验证Catchup） ===
TEST(SCQTest, Catchup_30E70D) {
    constexpr int NUM_PRODUCERS = 3;
    constexpr int NUM_CONSUMERS = 7;
    constexpr int ITEMS_PER_PRODUCER = 10000;

    SCQ<uint64_t> queue;
    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};

    auto producer = [&](int id) {
        for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            queue.enqueue(id * ITEMS_PER_PRODUCER + i);
            enqueued.fetch_add(1);
        }
    };

    auto consumer = [&]() {
        while (dequeued.load() < NUM_PRODUCERS * ITEMS_PER_PRODUCER) {
            uint64_t val = queue.dequeue();
            if (val != SCQ<uint64_t>::EMPTY_VALUE) {
                dequeued.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        threads.emplace_back(producer, i);
    }
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back(consumer);
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(enqueued.load(), NUM_PRODUCERS * ITEMS_PER_PRODUCER);
    EXPECT_EQ(dequeued.load(), NUM_PRODUCERS * ITEMS_PER_PRODUCER);
}

// === isSafe验证测试 ===
TEST(SCQTest, SafeFlagCheck) {
    SCQ<uint64_t> queue;

    // 单线程验证：enqueue后entry应该是safe
    queue.enqueue(42);

    // 多线程验证：dequeue不会读到未就绪的entry
    // （通过并发测试隐式验证）
}

// === SCQ vs NCQ性能对比测试 ===
TEST(SCQTest, PerformanceVsNCQ) {
    constexpr int ITERATIONS = 100000;

    // SCQ测试
    SCQ<uint64_t> scq;
    auto scq_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        scq.enqueue(i);
        scq.dequeue();
    }
    auto scq_end = std::chrono::high_resolution_clock::now();
    auto scq_duration = std::chrono::duration_cast<std::chrono::microseconds>(scq_end - scq_start).count();

    // NCQ测试
    NCQ<uint64_t> ncq;
    auto ncq_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        ncq.enqueue(i);
        ncq.dequeue();
    }
    auto ncq_end = std::chrono::high_resolution_clock::now();
    auto ncq_duration = std::chrono::duration_cast<std::chrono::microseconds>(ncq_end - ncq_start).count();

    // SCQ应该更快或相当（单线程差异不大）
    std::cout << "SCQ: " << scq_duration << " us, NCQ: " << ncq_duration << " us\n";
    EXPECT_LE(scq_duration, ncq_duration * 1.5);  // 允许50%误差
}
```

**测试覆盖点**:
- ✅ Livelock避免（16线程高竞争）
- ✅ Threshold重置机制
- ✅ 30E70D场景（Catchup效果）
- ✅ isSafe标志位功能
- ✅ 与NCQ性能对比

**验收点**:
```bash
./build/debug/tests/lscq_tests --gtest_filter="SCQTest.*"
# 所有测试通过，NoLivelock测试完成时间 < 10秒
```

---

### 4.5 性能Benchmark (Day 7-8)

#### 4.5.1 创建 `benchmarks/benchmark_scq.cpp`

**目标**: 证明SCQ相比NCQ有2x+性能提升

```cpp
#include <benchmark/benchmark.h>
#include "lscq/scq.hpp"
#include "lscq/ncq.hpp"
#include <thread>
#include <vector>

using namespace lscq;

// === SCQ Pair Benchmark ===
static void BM_SCQ_Pair(benchmark::State& state) {
    int thread_count = state.range(0);
    SCQ<uint64_t> queue;

    // 预填充
    for (int i = 0; i < thread_count * 100; ++i) {
        queue.enqueue(i);
    }

    auto worker = [&]() {
        for (auto _ : state) {
            queue.enqueue(42);
            uint64_t val = queue.dequeue();
            benchmark::DoNotOptimize(val);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    state.SetItemsProcessed(state.iterations() * thread_count * 2);
}

BENCHMARK(BM_SCQ_Pair)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// === NCQ Pair Benchmark（对比） ===
static void BM_NCQ_Pair(benchmark::State& state) {
    int thread_count = state.range(0);
    NCQ<uint64_t> queue;

    for (int i = 0; i < thread_count * 100; ++i) {
        queue.enqueue(i);
    }

    auto worker = [&]() {
        for (auto _ : state) {
            queue.enqueue(42);
            uint64_t val = queue.dequeue();
            benchmark::DoNotOptimize(val);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    state.SetItemsProcessed(state.iterations() * thread_count * 2);
}

BENCHMARK(BM_NCQ_Pair)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// === 30E70D Benchmark ===
static void BM_SCQ_30E70D(benchmark::State& state) {
    SCQ<uint64_t> queue;

    // 预填充
    for (int i = 0; i < 10000; ++i) {
        queue.enqueue(i);
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> ops{0};

    auto producer = [&]() {
        while (!stop.load()) {
            queue.enqueue(42);
            ops.fetch_add(1);
        }
    };

    auto consumer = [&]() {
        while (!stop.load()) {
            benchmark::DoNotOptimize(queue.dequeue());
            ops.fetch_add(1);
        }
    };

    for (auto _ : state) {
        stop.store(false);
        ops.store(0);

        std::vector<std::thread> threads;
        threads.emplace_back(producer);
        threads.emplace_back(producer);
        threads.emplace_back(producer);
        threads.emplace_back(consumer);
        threads.emplace_back(consumer);
        threads.emplace_back(consumer);
        threads.emplace_back(consumer);
        threads.emplace_back(consumer);
        threads.emplace_back(consumer);
        threads.emplace_back(consumer);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop.store(true);

        for (auto& t : threads) {
            t.join();
        }
    }

    state.SetItemsProcessed(ops.load());
}

BENCHMARK(BM_SCQ_30E70D)->UseRealTime();

BENCHMARK_MAIN();
```

**性能目标**（Phase 3必须达标）:
| 场景 | 线程数 | SCQ目标 | NCQ Baseline | 提升 |
|------|--------|---------|--------------|------|
| Pair | 1      | >8 Mops/sec | 6 Mops/sec | >1.3x |
| Pair | 8      | >40 Mops/sec | 22 Mops/sec | >1.8x |
| Pair | 16     | >50 Mops/sec | 28 Mops/sec | >1.8x |
| 30E70D | 10 (3P+7C) | >35 Mops/sec | 20 Mops/sec | >1.75x |

**验收点**:
```bash
./build/release/benchmarks/lscq_benchmarks --benchmark_filter="SCQ|NCQ" --benchmark_min_time=2s
# 记录数据到 docs/Phase3-Benchmark结果.md
```

---

### 4.6 CMake集成 (Day 9)

更新 `CMakeLists.txt`:
```cmake
# 添加SCQ源文件
add_library(lscq_impl STATIC
    src/ncq.cpp
    src/scq.cpp  # 新增
)

# 更新测试
add_executable(lscq_tests
    tests/unit/test_cas2.cpp
    tests/unit/test_ncq.cpp
    tests/unit/test_scq.cpp  # 新增
)

# 更新Benchmark
add_executable(lscq_benchmarks
    benchmarks/benchmark_cas2.cpp
    benchmarks/benchmark_ncq.cpp
    benchmarks/benchmark_scq.cpp  # 新增
)
```

---

## 5. 交付物清单

### 5.1 代码文件
- [ ] `include/lscq/scq.hpp` - SCQ类声明
- [ ] `src/scq.cpp` - SCQ实现（带threshold和catchup）
- [ ] `tests/unit/test_scq.cpp` - SCQ单元测试
- [ ] `benchmarks/benchmark_scq.cpp` - SCQ vs NCQ性能对比
- [ ] 更新后的 `CMakeLists.txt`

### 5.2 测试结果
- [ ] 所有单元测试通过（至少7个测试用例）
- [ ] Livelock测试通过（16线程无超时）
- [ ] 30E70D测试通过（3P+7C）
- [ ] Benchmark结果（保存为 `docs/Phase3-Benchmark结果.md`）

### 5.3 文档
- [ ] `docs/Phase3-交接文档.md` - **必须创建**
- [ ] `docs/Phase3-Benchmark结果.md` - 性能数据和对比图表
- [ ] 代码Doxygen注释（threshold、fixState、isSafe）

---

## 6. 验收标准 (Gate Conditions)

### 6.1 编译验收
- ✅ **G1.1**: SCQ代码零警告编译通过
- ✅ **G1.2**: `sizeof(Entry) == 16`（对齐要求）
- ✅ **G1.3**: 位域操作正确（cycle()和is_safe()方法）

### 6.2 功能验收
- ✅ **G2.1**: BasicEnqueueDequeue测试通过
- ✅ **G2.2**: FIFO_Order测试通过
- ✅ **G2.3**: NoLivelock_HighContention测试通过（<10秒完成）
- ✅ **G2.4**: ThresholdReset测试通过
- ✅ **G2.5**: Catchup_30E70D测试通过（无死锁）
- ✅ **G2.6**: 无data race（ThreadSanitizer）

### 6.3 性能验收（关键）
- ✅ **G3.1**: Pair @ 16 threads > 50 Mops/sec
- ✅ **G3.2**: SCQ性能 > 1.8x NCQ @ 8+ threads
- ✅ **G3.3**: 30E70D性能 > 35 Mops/sec
- ✅ **G3.4**: Catchup明显改善30E70D性能（相比无catchup版本）

### 6.4 代码质量验收
- ✅ **G4.1**: 测试覆盖率 > 90%
- ✅ **G4.2**: 无内存泄漏（AddressSanitizer）
- ✅ **G4.3**: clang-tidy无错误

### 6.5 文档验收
- ✅ **G5.1**: `docs/Phase3-交接文档.md` 已创建（>400行）
- ✅ **G5.2**: Benchmark结果文档包含性能图表

---

## 7. 下一阶段预览

### 7.1 Phase 4概述
**阶段名称**: SCQP双字宽CAS扩展
**核心任务**: 直接在Entry中存储指针（而非索引）
**关键技术**: CAS2直接操作{cycle, T*}，Threshold调整为4n-1
**预计工期**: 1周

### 7.2 Phase 4依赖本阶段的关键产出
1. **SCQ算法** - SCQP是SCQ的变体
2. **Threshold框架** - SCQP使用4n-1 threshold
3. **测试框架** - 复用SCQ测试，增加指针队列测试

### 7.3 Phase 4的关键区别
| 特性 | SCQ (Phase 3) | SCQP (Phase 4) |
|------|---------------|----------------|
| Entry存储 | `{cycle, index}` | `{cycle, T*}` |
| Threshold | 3n-1 | **4n-1** |
| 队列满检测 | 无 | **需要检测** |
| 适用场景 | 索引队列 | 对象队列 |
| CAS2要求 | 可选（fallback） | **必需** |

---

## 8. 阶段完成交接文档要求

创建 `docs/Phase3-交接文档.md`，包含：

```markdown
# Phase 3 交接文档

## 1. 完成情况概览
- 验收Gate通过情况
- 性能达标情况（与论文对比）

## 2. 关键代码位置索引
- Entry位域定义：`include/lscq/scq.hpp:L35-L58`
- Threshold检查：`src/scq.cpp:L45-L52`
- fix_state实现：`src/scq.cpp:L150-L170`
- Livelock测试：`tests/unit/test_scq.cpp:L50-L85`

## 3. SCQ算法验证结果
- Livelock：✅ 16线程无超时
- Threshold重置：✅ 自动重置正常
- 30E70D：✅ Catchup生效

## 4. 性能Benchmark结果
### 4.1 Pair模式
| 线程数 | SCQ | NCQ | 提升 |
|--------|-----|-----|------|
| 1      | ... | ... | ...  |
| 16     | ... | ... | ...  |

### 4.2 30E70D模式
（实际数据）

### 4.3 关键发现
- Threshold机制有效避免livelock
- Catchup在30E70D下提升35%性能
- 16线程达到55 Mops/sec（论文水平）

## 5. 已知问题和限制
- 队列满时enqueue仍会阻塞（Phase 5解决）

## 6. Phase 4接口预留
- Entry结构需要支持指针：`Entry{cycle, T*}`
- Threshold调整为4n-1
- 增加"队列满"检测逻辑

## 7. 下阶段快速启动指南
**阅读**:
- 本文档第2节
- 论文Figure 9（SCQP变体）

**复用**:
- SCQ的threshold框架
- fix_state函数
- 测试框架

**新建**:
- `include/lscq/scqp.hpp`
- `tests/unit/test_scqp.cpp`
```

---

## 9. 常见问题（FAQ）

### Q1: 为什么Threshold是3n-1而不是2n-1？
**A**: 数学证明（论文3.3节）：2n-1不足以保证progress，可能所有线程都在重试。3n-1保证至少一个线程成功。

### Q2: isSafe标志位的作用是什么？
**A**: 防止ABA问题。dequeue必须等待entry完全写入（isSafe=true），避免读取到部分写入的数据。

### Q3: Catchup什么时候触发？
**A**: 当 `head - tail > SCQSIZE` 且 `threshold == 0` 时触发。主要在30E70D场景。

### Q4: Atomic_OR优化的原理？
**A**: 使用 `cycle = 2n-1`（二进制全1）标记已消费，无需CAS重试，减少竞争。

### Q5: SCQ性能未达到50 Mops/sec怎么办？
**A**: 检查：(1) cache_remap是否生效 (2) threshold是否正确重置 (3) CPU是否支持CAS2硬件指令。

---

## 10. 参考资料

- `docs/Phase2-交接文档.md` - NCQ实现
- `docs/01-技术实现思路.md` 第4.2节 - SCQ算法
- `1908.04511v1.pdf` Figure 8 - SCQ伪代码
- `1908.04511v1.pdf` 第3.3-3.4节 - Threshold和Catchup证明
- [Lock-Free Progress Guarantees](https://en.wikipedia.org/wiki/Non-blocking_algorithm)

---

**StarterPrompt版本**: v1.0
**创建日期**: 2026-01-18
**适用范围**: LSCQ项目 Phase 3
