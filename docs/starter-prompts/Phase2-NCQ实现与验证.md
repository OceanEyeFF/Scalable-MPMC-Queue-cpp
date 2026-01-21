# Phase 2 StarterPrompt: NCQ实现与验证

> **任务代号**: LSCQ-Phase2-NCQ
> **预计工期**: 1周
> **前置依赖**: Phase 1 完成（基础设施就绪）
> **后续阶段**: Phase 3 (SCQ核心实现)

---

## 1. 任务概述

### 1.1 核心目标
实现论文中的 **NCQ (Naive Circular Queue)** 算法，作为后续SCQ的baseline和正确性验证参考。NCQ是简化版的MPMC队列，使用Ring Buffer + FAA + CAS2实现。

### 1.2 为什么先实现NCQ？
1. **复杂度递进**: NCQ比SCQ简单，无threshold/catchup机制，便于理解Ring Buffer基础
2. **正确性参考**: NCQ算法相对简单，更容易验证FIFO语义正确性
3. **性能基线**: 可与M&S Queue对比，验证测试框架和Benchmark准确性
4. **算法验证**: NCQ与SCQ的核心逻辑相似，先验证基础流程

### 1.3 任务价值
- ✅ 建立MPMC队列的基础实现模板
- ✅ 验证CAS2抽象层在实际场景中的可用性
- ✅ 为Phase 3的SCQ优化提供性能对比基准
- ✅ 积累并发测试和Benchmark经验

---

## 2. 任务边界

### 2.1 In Scope (本阶段必须完成)
- [x] NCQ类模板定义（支持泛型`T`）
- [x] Entry结构定义（cycle + index）
- [x] enqueue实现（基于论文Figure 5）
- [x] dequeue实现（基于论文Figure 5）
- [x] cache_remap函数（避免false sharing）
- [x] 单线程FIFO正确性测试
- [x] 多线程并发正确性测试
- [x] 性能Benchmark（1/2/4/8/16线程 Pair模式）
- [x] 与M&S Queue性能对比
- [x] API文档初稿

### 2.2 Out of Scope (本阶段不涉及)
- ❌ Threshold机制（SCQ特性，Phase 3实现）
- ❌ Catchup优化（SCQ特性，Phase 3实现）
- ❌ isSafe标志位（SCQ特性，Phase 3实现）
- ❌ Atomic_OR优化（SCQ特性，Phase 3实现）
- ❌ 链式无界扩展（LSCQ特性，Phase 5实现）
- ❌ 内存回收机制（Phase 5实现）
- ❌ ARM64移植（Phase 6实现）

---

## 3. 前置条件与输入

### 3.1 前置依赖
- ✅ Phase 1已通过所有验收Gate
- ✅ `docs/Phase1-交接文档.md` 已创建
- ✅ CAS2抽象层工作正常
- ✅ 测试框架和Benchmark框架就绪

### 3.2 必读文档（按顺序）
1. **`docs/Phase1-交接文档.md`** - 理解Phase 1的代码结构和接口
2. **`docs/01-技术实现思路.md` 第4.1节** - NCQ算法伪代码和原理
3. **`1908.04511v1.pdf` Figure 5** - 论文中的NCQ算法原始描述
4. **`docs/02-分段开发计划.md` 第4节** - NCQ实现详细计划

### 3.3 关键代码复用（来自Phase 1）
- `include/lscq/cas2.hpp` - CAS2接口（直接使用）
- `include/lscq/config.hpp` - DEFAULT_SCQSIZE常量
- `tests/unit/test_cas2.cpp` - 测试模式参考
- `CMakeLists.txt` - 添加新目标的模式

### 3.4 环境要求
- Phase 1构建环境（Clang++ 14+, CMake, Ninja）
- 理解Ring Buffer和取模运算
- 理解FAA（Fetch-And-Add）原子操作

---

## 4. 详细任务清单

### 4.1 数据结构定义 (Day 1)

#### 4.1.1 创建 `include/lscq/ncq.hpp`
参考 `docs/02-分段开发计划.md` 第4.3.1节：

```cpp
#ifndef LSCQ_NCQ_HPP
#define LSCQ_NCQ_HPP

#include "lscq/config.hpp"
#include "lscq/cas2.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lscq {

/// @brief Naive Circular Queue - 简单的MPMC有界队列
/// @tparam T 存储的索引或指针类型（通常为uint64_t）
template<typename T = uint64_t>
class NCQ {
public:
    static constexpr size_t SCQSIZE = config::DEFAULT_SCQSIZE;  // 65536
    static constexpr T EMPTY_VALUE = static_cast<T>(-1);        // 空队列标记

    NCQ();
    ~NCQ();

    // 禁止拷贝和移动（原子变量不可拷贝）
    NCQ(const NCQ&) = delete;
    NCQ& operator=(const NCQ&) = delete;
    NCQ(NCQ&&) = delete;
    NCQ& operator=(NCQ&&) = delete;

    /// @brief 入队操作（阻塞直到成功）
    /// @param index 要插入的索引值
    void enqueue(T index);

    /// @brief 出队操作
    /// @return 返回索引值，队列为空时返回EMPTY_VALUE
    T dequeue();

    /// @brief 检查队列是否为空（非精确，仅用于调试）
    bool is_empty() const;

private:
    /// @brief Entry结构（16字节对齐）
    struct alignas(16) Entry {
        uint64_t cycle;  // Cycle计数器
        T index;         // 存储的索引值

        bool operator==(const Entry& other) const {
            return cycle == other.cycle && index == other.index;
        }
    };

    Entry* entries_;                             // Ring buffer数组
    alignas(64) std::atomic<uint64_t> head_;     // 出队指针（cacheline隔离）
    alignas(64) std::atomic<uint64_t> tail_;     // 入队指针（cacheline隔离）

    /// @brief Cache remap函数（避免false sharing）
    /// @param idx 逻辑索引
    /// @return 物理索引
    size_t cache_remap(size_t idx) const;
};

}  // namespace lscq

#endif  // LSCQ_NCQ_HPP
```

**关键设计点**:
- `Entry` 使用 `alignas(16)` 对齐，满足CAS2要求
- `head_` 和 `tail_` 使用 `alignas(64)` 避免false sharing
- `EMPTY_VALUE` 使用 `-1` 表示空队列

**验收点**: 头文件可被包含，无编译错误

---

### 4.2 核心算法实现 (Day 2-3)

#### 4.2.1 创建 `src/ncq.cpp`
参考 `docs/02-分段开发计划.md` 第4.3.2节和论文Figure 5：

```cpp
#include "lscq/ncq.hpp"
#include <new>
#include <cstdlib>

namespace lscq {

template<typename T>
NCQ<T>::NCQ() : head_(SCQSIZE), tail_(SCQSIZE) {
    // 分配对齐的内存（64字节对齐以优化cacheline）
    entries_ = static_cast<Entry*>(
        aligned_alloc(64, sizeof(Entry) * SCQSIZE)
    );
    if (!entries_) {
        throw std::bad_alloc();
    }

    // 初始化所有entry（cycle=0表示空槽位）
    for (size_t i = 0; i < SCQSIZE; ++i) {
        entries_[i].cycle = 0;
        entries_[i].index = 0;
    }
}

template<typename T>
NCQ<T>::~NCQ() {
    free(entries_);
}

// === enqueue实现（基于论文Figure 5） ===
template<typename T>
void NCQ<T>::enqueue(T index) {
    while (true) {
        // 1. 获取当前tail
        uint64_t t = tail_.load(std::memory_order_acquire);
        size_t j = cache_remap(t % SCQSIZE);

        // 2. 读取目标entry（原子加载）
        Entry ent = entries_[j];

        // 3. 计算cycle
        uint64_t cycle_t = t / SCQSIZE;
        uint64_t cycle_e = ent.cycle;

        // 4. 检查entry状态
        if (cycle_e == cycle_t) {
            // Case 1: entry已被占用，帮助推进tail
            tail_.compare_exchange_weak(
                t, t + 1,
                std::memory_order_release,
                std::memory_order_relaxed
            );
            continue;
        }

        if (cycle_e + 1 != cycle_t) {
            // Case 2: tail已过期，重试
            continue;
        }

        // Case 3: entry空闲，尝试CAS插入
        Entry new_ent{cycle_t, index};
        Entry expected = ent;

        if (cas2(&entries_[j], expected, new_ent)) {
            // 成功插入，尝试推进tail
            tail_.compare_exchange_weak(
                t, t + 1,
                std::memory_order_release,
                std::memory_order_relaxed
            );
            return;
        }
        // CAS失败，重试
    }
}

// === dequeue实现（基于论文Figure 5） ===
template<typename T>
T NCQ<T>::dequeue() {
    while (true) {
        // 1. 获取当前head
        uint64_t h = head_.load(std::memory_order_acquire);
        size_t j = cache_remap(h % SCQSIZE);

        // 2. 读取目标entry
        Entry ent = entries_[j];

        // 3. 计算cycle
        uint64_t cycle_h = h / SCQSIZE;
        uint64_t cycle_e = ent.cycle;

        // 4. 检查entry状态
        if (cycle_e != cycle_h) {
            if (cycle_e + 1 == cycle_h) {
                // 队列为空
                return EMPTY_VALUE;
            }
            // Head已过期，重试
            continue;
        }

        // 5. Entry有效，尝试推进head
        if (head_.compare_exchange_weak(
                h, h + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return ent.index;
        }
        // CAS失败，重试
    }
}

// === cache_remap实现 ===
template<typename T>
size_t NCQ<T>::cache_remap(size_t idx) const {
    constexpr size_t ENTRIES_PER_LINE = 64 / sizeof(Entry);  // 64B cacheline
    size_t line = idx / ENTRIES_PER_LINE;
    size_t offset = idx % ENTRIES_PER_LINE;
    return (offset * (SCQSIZE / ENTRIES_PER_LINE)) + line;
}

template<typename T>
bool NCQ<T>::is_empty() const {
    return head_.load(std::memory_order_relaxed) >=
           tail_.load(std::memory_order_relaxed);
}

// === 显式实例化（避免链接错误） ===
template class NCQ<uint64_t>;
template class NCQ<uint32_t>;

}  // namespace lscq
```

**关键算法点**（对应论文）:
1. **Cycle计数**: `cycle = idx / SCQSIZE`，用于区分队列绕圈
2. **CAS2原子性**: Entry的cycle和index必须原子更新
3. **Tail推进**: 即使CAS失败也帮助推进tail（避免阻塞）
4. **空队列检测**: `cycle_e + 1 == cycle_h`

**验收点**:
```bash
cmake --build build/debug
nm build/debug/src/libncq.a | grep NCQ  # 确认符号导出
```

---

### 4.3 单元测试编写 (Day 4)

#### 4.3.1 创建 `tests/unit/test_ncq.cpp`
参考 `docs/02-分段开发计划.md` 第4.3.3节：

```cpp
#include <gtest/gtest.h>
#include "lscq/ncq.hpp"
#include <thread>
#include <vector>
#include <algorithm>
#include <set>

using namespace lscq;

// === 基础功能测试 ===
TEST(NCQTest, BasicEnqueueDequeue) {
    NCQ<uint64_t> queue;

    queue.enqueue(42);
    queue.enqueue(100);

    EXPECT_EQ(queue.dequeue(), 42);
    EXPECT_EQ(queue.dequeue(), 100);
    EXPECT_EQ(queue.dequeue(), NCQ<uint64_t>::EMPTY_VALUE);
}

// === FIFO顺序测试 ===
TEST(NCQTest, FIFO_Order) {
    NCQ<uint64_t> queue;

    // 入队1000个元素
    for (uint64_t i = 0; i < 1000; ++i) {
        queue.enqueue(i);
    }

    // 验证FIFO顺序
    for (uint64_t i = 0; i < 1000; ++i) {
        EXPECT_EQ(queue.dequeue(), i) << "FIFO order violated at index " << i;
    }

    EXPECT_EQ(queue.dequeue(), NCQ<uint64_t>::EMPTY_VALUE);
}

// === 多线程生产者测试 ===
TEST(NCQTest, ConcurrentEnqueue) {
    constexpr int NUM_THREADS = 8;
    constexpr int ITEMS_PER_THREAD = 1000;

    NCQ<uint64_t> queue;

    auto worker = [&](int thread_id) {
        for (int i = 0; i < ITEMS_PER_THREAD; ++i) {
            uint64_t value = static_cast<uint64_t>(thread_id) * ITEMS_PER_THREAD + i;
            queue.enqueue(value);
        }
    };

    // 启动多个生产者
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    // 验证所有元素都被插入
    std::set<uint64_t> results;
    uint64_t val;
    while ((val = queue.dequeue()) != NCQ<uint64_t>::EMPTY_VALUE) {
        // 验证无重复
        EXPECT_TRUE(results.insert(val).second) << "Duplicate value: " << val;
    }

    EXPECT_EQ(results.size(), NUM_THREADS * ITEMS_PER_THREAD);
}

// === 多线程生产者-消费者测试（关键测试） ===
TEST(NCQTest, ConcurrentEnqueueDequeue) {
    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 10000;

    NCQ<uint64_t> queue;
    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};
    std::vector<uint64_t> dequeued_values;
    std::mutex dequeued_mutex;

    // 生产者线程
    auto producer = [&](int thread_id) {
        for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            uint64_t value = static_cast<uint64_t>(thread_id) * ITEMS_PER_PRODUCER + i;
            queue.enqueue(value);
            enqueued.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // 消费者线程
    auto consumer = [&]() {
        while (dequeued.load(std::memory_order_relaxed) <
               NUM_PRODUCERS * ITEMS_PER_PRODUCER) {
            uint64_t val = queue.dequeue();
            if (val != NCQ<uint64_t>::EMPTY_VALUE) {
                {
                    std::lock_guard<std::mutex> lock(dequeued_mutex);
                    dequeued_values.push_back(val);
                }
                dequeued.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> threads;

    // 启动生产者和消费者
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        threads.emplace_back(producer, i);
    }
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back(consumer);
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证
    EXPECT_EQ(dequeued.load(), NUM_PRODUCERS * ITEMS_PER_PRODUCER);

    // 验证无重复
    std::sort(dequeued_values.begin(), dequeued_values.end());
    auto it = std::unique(dequeued_values.begin(), dequeued_values.end());
    EXPECT_EQ(it, dequeued_values.end()) << "Duplicate values detected";
}

// === Cycle溢出测试（边界条件） ===
TEST(NCQTest, CycleWrap) {
    NCQ<uint64_t> queue;

    // 模拟cycle接近溢出（实际需要很长时间）
    // 这里只测试逻辑正确性
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 100; ++i) {
            queue.enqueue(i);
        }
        for (int i = 0; i < 100; ++i) {
            EXPECT_EQ(queue.dequeue(), static_cast<uint64_t>(i));
        }
    }
}
```

**测试覆盖点**:
- ✅ 基础功能（单线程入队出队）
- ✅ FIFO顺序保证
- ✅ 多生产者并发
- ✅ 多生产者+多消费者并发（最关键）
- ✅ 边界条件（cycle溢出）

**验收点**:
```bash
./build/debug/tests/lscq_tests --gtest_filter="NCQTest.*"
# 所有测试通过
```

---

### 4.4 性能Benchmark (Day 5)

#### 4.4.1 创建 `benchmarks/benchmark_ncq.cpp`
参考 `docs/03-性能验证方案.md` 第3.1节：

```cpp
#include <benchmark/benchmark.h>
#include "lscq/ncq.hpp"
#include <thread>
#include <vector>

using namespace lscq;

// === Pair模式Benchmark（最重要） ===
static void BM_NCQ_EnqueueDequeue_Pair(benchmark::State& state) {
    int thread_count = state.range(0);
    NCQ<uint64_t> queue;

    // 预填充队列（避免空队列）
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

    // 设置吞吐量（每个线程做2个操作：enqueue + dequeue）
    state.SetItemsProcessed(state.iterations() * thread_count * 2);
}

BENCHMARK(BM_NCQ_EnqueueDequeue_Pair)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// === 50E50D模式Benchmark ===
static void BM_NCQ_50Enqueue_50Dequeue(benchmark::State& state) {
    int thread_count = state.range(0);
    NCQ<uint64_t> queue;

    // 预填充
    for (int i = 0; i < thread_count * 100; ++i) {
        queue.enqueue(i);
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_ops{0};

    auto worker = [&](int id) {
        uint64_t local_ops = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (id % 2 == 0) {
                queue.enqueue(42);
            } else {
                benchmark::DoNotOptimize(queue.dequeue());
            }
            ++local_ops;
        }
        total_ops.fetch_add(local_ops, std::memory_order_relaxed);
    };

    for (auto _ : state) {
        stop.store(false);
        std::vector<std::thread> threads;
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back(worker, i);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop.store(true);

        for (auto& t : threads) {
            t.join();
        }
    }

    state.SetItemsProcessed(total_ops.load());
}

BENCHMARK(BM_NCQ_50Enqueue_50Dequeue)
    ->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// === 空队列率Benchmark ===
static void BM_NCQ_EmptyQueue(benchmark::State& state) {
    NCQ<uint64_t> queue;
    std::atomic<int> empty_count{0};
    std::atomic<int> total_count{0};

    for (auto _ : state) {
        uint64_t val = queue.dequeue();
        if (val == NCQ<uint64_t>::EMPTY_VALUE) {
            empty_count.fetch_add(1);
        }
        total_count.fetch_add(1);
        queue.enqueue(42);
    }

    double empty_rate = static_cast<double>(empty_count.load()) / total_count.load();
    state.counters["EmptyRate"] = empty_rate;
}

BENCHMARK(BM_NCQ_EmptyQueue)->UseRealTime();

BENCHMARK_MAIN();
```

**Benchmark目标**（Phase 2不强求达标）:
- Pair @ 1 thread: > 5 Mops/sec
- Pair @ 8 threads: > 20 Mops/sec
- Pair @ 16 threads: > 30 Mops/sec（论文SCQ达到50+）

**验收点**:
```bash
./build/release/benchmarks/benchmark_ncq --benchmark_min_time=1s
# Benchmark可运行，无崩溃
```

---

### 4.5 与M&S Queue对比 (Day 6)

#### 4.5.1 创建 `benchmarks/baseline/msqueue.hpp`
实现简单的Michael-Scott Queue（使用`std::atomic<Node*>`）:

```cpp
#ifndef LSCQ_MSQUEUE_HPP
#define LSCQ_MSQUEUE_HPP

#include <atomic>
#include <memory>

namespace lscq::baseline {

template<typename T>
class MSQueue {
public:
    MSQueue();
    ~MSQueue();

    void enqueue(T value);
    T dequeue();  // 返回-1表示空

private:
    struct Node {
        T data;
        std::atomic<Node*> next;
        Node(T val) : data(val), next(nullptr) {}
    };

    alignas(64) std::atomic<Node*> head_;
    alignas(64) std::atomic<Node*> tail_;
};

}  // namespace lscq::baseline

#endif
```

#### 4.5.2 对比Benchmark
```cpp
static void BM_MSQueue_Pair(benchmark::State& state) {
    // 实现与BM_NCQ_Pair相同的测试
    // ...
}

BENCHMARK(BM_MSQueue_Pair)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16);
```

**验收点**: NCQ性能应接近或优于MSQueue

---

### 4.6 CMake集成 (Day 7)

#### 4.6.1 更新 `CMakeLists.txt`
```cmake
# 添加NCQ源文件
add_library(lscq_impl STATIC
    src/ncq.cpp
)
target_include_directories(lscq_impl PUBLIC include/)
target_link_libraries(lscq_impl PUBLIC lscq)

# 更新测试
add_executable(lscq_tests
    tests/unit/test_cas2.cpp
    tests/unit/test_ncq.cpp
)
target_link_libraries(lscq_tests lscq_impl GTest::gtest_main)

# 更新Benchmark
add_executable(lscq_benchmarks
    benchmarks/benchmark_cas2.cpp
    benchmarks/benchmark_ncq.cpp
)
target_link_libraries(lscq_benchmarks lscq_impl benchmark::benchmark_main)
```

**验收点**:
```bash
cmake --build build/debug
ctest --test-dir build/debug
./build/release/benchmarks/lscq_benchmarks
```

---

## 5. 交付物清单

### 5.1 代码文件
- [ ] `include/lscq/ncq.hpp` - NCQ类声明
- [ ] `src/ncq.cpp` - NCQ实现
- [ ] `tests/unit/test_ncq.cpp` - NCQ单元测试
- [ ] `benchmarks/benchmark_ncq.cpp` - NCQ性能测试
- [ ] `benchmarks/baseline/msqueue.hpp` - MSQueue参考实现
- [ ] 更新后的 `CMakeLists.txt`

### 5.2 测试结果
- [ ] 所有单元测试通过（至少5个测试用例）
- [ ] 并发测试无data race（ThreadSanitizer验证）
- [ ] Benchmark结果报告（保存为 `docs/Phase2-Benchmark结果.md`）

### 5.3 文档
- [ ] `docs/Phase2-交接文档.md` - **必须创建**（见第7节）
- [ ] `docs/Phase2-Benchmark结果.md` - 性能数据
- [ ] API文档注释（Doxygen风格）

---

## 6. 验收标准 (Gate Conditions)

### 6.1 编译验收
- ✅ **G1.1**: NCQ相关代码零警告编译通过
- ✅ **G1.2**: 模板实例化正常（uint64_t和uint32_t）

### 6.2 功能验收
- ✅ **G2.1**: BasicEnqueueDequeue测试通过
- ✅ **G2.2**: FIFO_Order测试通过（1000元素）
- ✅ **G2.3**: ConcurrentEnqueue测试通过（8线程×1000元素）
- ✅ **G2.4**: ConcurrentEnqueueDequeue测试通过（4P+4C×10000元素）
- ✅ **G2.5**: 无内存泄漏（AddressSanitizer）
- ✅ **G2.6**: 无data race（ThreadSanitizer）

### 6.3 性能验收
- ✅ **G3.1**: Pair Benchmark可运行，无崩溃
- ✅ **G3.2**: 单线程吞吐量 > 5 Mops/sec
- ✅ **G3.3**: 16线程吞吐量 > 20 Mops/sec（Phase 2不强求）
- ✅ **G3.4**: 与MSQueue对比，性能差距 < 50%

### 6.4 代码质量验收
- ✅ **G4.1**: 测试覆盖率 > 85%
- ✅ **G4.2**: clang-tidy无critical警告
- ✅ **G4.3**: 代码符合.clang-format规范

### 6.5 文档验收
- ✅ **G5.1**: `docs/Phase2-交接文档.md` 已创建
- ✅ **G5.2**: 代码包含Doxygen注释（至少类和public方法）

---

## 7. 下一阶段预览

### 7.1 Phase 3概述
**阶段名称**: SCQ核心实现
**核心任务**: 在NCQ基础上增加threshold、catchup、Atomic_OR优化
**关键技术**: Threshold机制（3n-1）、fixState优化、isSafe标志位
**预计工期**: 2-3周

### 7.2 Phase 3依赖本阶段的关键产出
1. **NCQ作为baseline** - SCQ的性能提升以NCQ为基准
2. **Ring Buffer框架** - SCQ复用NCQ的ring buffer结构
3. **测试模式** - SCQ复用NCQ的测试用例，增加livelock测试
4. **Benchmark框架** - 直接对比NCQ vs SCQ

### 7.3 Phase 3的关键区别
| 特性 | NCQ | SCQ (Phase 3) |
|------|-----|---------------|
| Threshold | 无 | **3n-1** (避免livelock) |
| Entry结构 | cycle + index | cycle + **isSafe** + index |
| Catchup | 无 | **fixState**优化 (head > tail) |
| Atomic_OR | 无 | **⊥=2n-1**标记已消费 |
| 性能 | Baseline | **>2x NCQ** @ 16 cores |

### 7.4 Phase 3需要的预先知识
- Livelock现象和threshold数学证明
- isSafe标志位的作用（避免ABA问题）
- Atomic_OR的位运算技巧
- fixState的应用场景（30E70D workload）

---

## 8. 阶段完成交接文档要求

### 8.1 文档结构
在完成本阶段所有任务后，**必须创建** `docs/Phase2-交接文档.md`，包含以下章节：

```markdown
# Phase 2 交接文档

## 1. 完成情况概览
- 所有验收Gate通过情况
- 实际工期 vs 预估工期
- 遇到的主要问题和解决方案

## 2. 关键代码位置索引
- NCQ类定义：`include/lscq/ncq.hpp:L18-L65`
- enqueue实现：`src/ncq.cpp:L30-L70`
- dequeue实现：`src/ncq.cpp:L75-L105`
- cache_remap实现：`src/ncq.cpp:L110-L118`
- 并发测试：`tests/unit/test_ncq.cpp:L75-L150`

## 3. NCQ算法验证结果
- FIFO顺序：✅ 1000元素无乱序
- 并发正确性：✅ 4P+4C×10000元素无重复/丢失
- Data race：✅ ThreadSanitizer无警告
- 内存泄漏：✅ AddressSanitizer无泄漏

## 4. 性能Benchmark结果
### 4.1 Pair模式
| 线程数 | NCQ (Mops/sec) | MSQueue (Mops/sec) | 提升 |
|--------|----------------|-------------------|------|
| 1      | 6.2            | 5.8               | +7%  |
| 2      | 10.1           | 8.9               | +13% |
| 4      | 15.3           | 12.4              | +23% |
| 8      | 22.7           | 17.2              | +32% |
| 16     | 28.5           | 20.1              | +42% |

### 4.2 50E50D模式
（填写实际测试结果）

### 4.3 关键发现
- cache_remap有效减少false sharing（对比测试提升15%）
- 多线程下NCQ明显优于MSQueue
- 16线程未达到论文SCQ的50 Mops/sec（预期，Phase 3优化）

## 5. 已知问题和限制
- [ ] 问题1: cache_remap在某些CPU上效果不明显（Skylake vs Zen3）
  - **影响**: 性能提升有限
  - **解决方案**: Phase 3可尝试不同remap策略
- [ ] 问题2: 队列满时enqueue会永久阻塞
  - **影响**: 生产环境需要超时机制
  - **解决方案**: Phase 5的LSCQ解决此问题（无界队列）

## 6. Phase 3接口预留
### 6.1 Entry结构需要扩展
NCQ的Entry需要增加`isSafe`字段：
```cpp
// NCQ (Phase 2)
struct Entry {
    uint64_t cycle;
    T index;
};

// SCQ (Phase 3)
struct Entry {
    uint64_t cycle : 63;
    uint64_t isSafe : 1;  // 新增
    T index;
};
```

### 6.2 Threshold机制接口
```cpp
// Phase 3需要增加
class SCQ {
    static constexpr uint64_t THRESHOLD = 3 * SCQSIZE - 1;
    bool is_safe(uint64_t idx);
    void fix_state();  // Catchup优化
};
```

### 6.3 测试框架扩展
- 需要增加**livelock测试**（验证threshold有效性）
- 需要增加**30E70D场景测试**（验证catchup优化）

## 7. 下阶段快速启动指南
**Phase 3开发者应该首先阅读**:
1. 本文档第2节（NCQ代码位置索引）
2. 本文档第4节（性能baseline数据）
3. `src/ncq.cpp` - 理解基础enqueue/dequeue流程
4. `docs/01-技术实现思路.md` 第4.2节 - SCQ算法和threshold推导
5. 论文 `1908.04511v1.pdf` Figure 8 - SCQ伪代码

**Phase 3开发应该复用**:
- NCQ的Ring Buffer结构（SCQSIZE、head_、tail_）
- cache_remap函数（直接复用）
- 测试框架模式（TEST宏、并发测试）
- Benchmark框架（对比NCQ vs SCQ）

**Phase 3开发需要新建**:
- `include/lscq/scq.hpp` - SCQ类声明（扩展Entry结构）
- `src/scq.cpp` - SCQ实现（增加threshold逻辑）
- `tests/unit/test_scq.cpp` - SCQ单元测试（增加livelock测试）
- `benchmarks/benchmark_scq.cpp` - SCQ vs NCQ对比

**Phase 3开发需要修改**:
- Entry结构：增加`isSafe`标志位
- enqueue：增加threshold检查
- dequeue：增加catchup调用

## 8. 经验教训和最佳实践
### 8.1 遇到的问题
1. **问题**: CAS2在Entry上失败（对齐问题）
   - **原因**: `alignas(16)` 未正确应用
   - **解决**: 检查 `sizeof(Entry)` 和 `alignof(Entry)`

2. **问题**: 并发测试偶现失败（dequeued_values重复）
   - **原因**: 多线程写入`dequeued_values`需要加锁
   - **解决**: 增加`std::mutex`保护

### 8.2 最佳实践
- ✅ 先写单线程测试，再写并发测试
- ✅ 使用`benchmark::DoNotOptimize`防止编译器优化
- ✅ Benchmark前预填充队列，避免空队列影响
- ✅ 使用`std::atomic`的`memory_order_relaxed`优化非关键路径

## 9. 附录：调试技巧
### 9.1 打印Entry状态
```cpp
void print_entry(const Entry& e) {
    std::cout << "Entry{cycle=" << e.cycle
              << ", index=" << e.index << "}\n";
}
```

### 9.2 检查head/tail状态
```cpp
void debug_queue_state() {
    uint64_t h = head_.load();
    uint64_t t = tail_.load();
    std::cout << "head=" << h << ", tail=" << t
              << ", size=" << (t - h) << "\n";
}
```
```

### 8.2 交接文档验收标准
- ✅ 所有章节完整填写（不少于300行）
- ✅ 性能数据真实可靠（来自实际测试）
- ✅ 至少记录2个实际问题和解决方案
- ✅ Phase 3接口预留清晰（Entry扩展、Threshold接口）

---

## 9. 常见问题（FAQ）

### Q1: NCQ的性能是否必须达到50 Mops/sec？
**A**: **不需要**。Phase 2的目标是建立baseline，NCQ性能通常在20-30 Mops/sec @ 16核。SCQ（Phase 3）才能达到50+。

### Q2: cache_remap是否必须实现？
**A**: **必须**。这是避免false sharing的关键，对多核性能有30%+提升。

### Q3: 如果队列满了怎么办？
**A**: Phase 2的NCQ会永久阻塞在enqueue。Phase 5的LSCQ通过链式扩展解决此问题。

### Q4: 为什么Entry要16字节对齐？
**A**: CAS2（128位CAS）要求地址16字节对齐，否则会崩溃或使用fallback。

### Q5: dequeue返回-1是否合理？
**A**: 对于索引队列合理。Phase 3/5实现对象队列时需要用`std::optional<T>`或特殊指针值。

---

## 10. 参考资料

- `docs/Phase1-交接文档.md` - CAS2接口和测试框架
- `docs/01-技术实现思路.md` 第4.1节 - NCQ算法伪代码
- `docs/02-分段开发计划.md` 第4节 - NCQ实现计划
- `1908.04511v1.pdf` Figure 5 - NCQ算法原始描述
- [Ring Buffer原理](https://en.wikipedia.org/wiki/Circular_buffer)
- [CAS原子操作](https://en.cppreference.com/w/cpp/atomic/atomic_compare_exchange)

---

**StarterPrompt版本**: v1.0
**创建日期**: 2026-01-18
**适用范围**: LSCQ项目 Phase 2
