# Phase 4 StarterPrompt: SCQP双字宽CAS扩展

> **任务代号**: LSCQ-Phase4-SCQP
> **预计工期**: 1周
> **前置依赖**: Phase 3 完成（SCQ就绪）
> **后续阶段**: Phase 5 (LSCQ无界队列)

---

## 1. 任务概述

### 1.1 核心目标
实现 **SCQP (SCQ with Pointers)** 变体，直接在Entry中存储指针而非索引，适用于对象队列场景。关键改进：
1. **Entry结构**: `{cycle, T*}` 替代 `{cycle, index}`
2. **Threshold调整**: 从3n-1提升为 **4n-1**
3. **队列满检测**: 增加满队列判断逻辑
4. **性能对比**: 验证SCQP相比SCQ的性能差异

### 1.2 技术挑战
- **指针存储**: Entry直接存储指针，要求CAS2硬件支持
- **4n-1 Threshold**: 数学证明和实现
- **队列满**: 与"队列空"的区分逻辑
- **Fallback策略**: 无CAS2硬件时降级为SCQ（索引模式）

### 1.3 任务价值
- ✅ 支持对象队列（无需外部索引数组）
- ✅ 为Phase 5的LSCQ链表操作奠定基础
- ✅ 验证CAS2在实际指针操作中的性能
- ✅ 提供两种队列模式供用户选择

---

## 2. 任务边界

### 2.1 In Scope (本阶段必须完成)
- [x] SCQP类模板定义（`template<typename T>`）
- [x] Entry结构调整（`{cycle, T*}`）
- [x] Threshold调整为 **4n-1**
- [x] enqueue实现（带队列满检测）
- [x] dequeue实现（复用SCQ逻辑）
- [x] 队列满检测逻辑
- [x] 平台检测（CAS2必需）
- [x] SCQP vs SCQ性能对比
- [x] 对象队列测试（真实对象入队出队）

### 2.2 Out of Scope (本阶段不涉及)
- ❌ 链式无界扩展（LSCQ，Phase 5实现）
- ❌ 内存回收机制（Phase 5实现）
- ❌ ARM64移植（Phase 6实现）
- ❌ 生产环境优化（Phase 6实现）

---

## 3. 前置条件与输入

### 3.1 前置依赖
- ✅ Phase 3已通过所有验收Gate
- ✅ `docs/Phase3-交接文档.md` 已创建
- ✅ SCQ性能达标（>50 Mops/sec @ 16 cores）

### 3.2 必读文档（按顺序）
1. **`docs/Phase3-交接文档.md`** - SCQ实现和性能数据
2. **`docs/01-技术实现思路.md` 第4.3节** - SCQP算法和4n-1推导
3. **`1908.04511v1.pdf` 第4节** - SCQP变体描述
4. **`docs/02-分段开发计划.md` 第6节** - SCQP实现计划

### 3.3 关键代码复用（来自Phase 3）
- `include/lscq/scq.hpp` - Threshold框架（复用）
- `src/scq.cpp` - fix_state函数（复用）
- `tests/unit/test_scq.cpp` - 测试模式（扩展为指针测试）

### 3.4 环境要求
- Phase 3构建环境
- CAS2硬件支持（x86-64 cmpxchg16b）
- 理解指针对齐和地址空间

---

## 4. 详细任务清单

### 4.1 平台检测和Entry结构 (Day 1)

#### 4.1.1 创建 `include/lscq/scqp.hpp`

**核心区别**: Entry存储指针而非索引

```cpp
#ifndef LSCQ_SCQP_HPP
#define LSCQ_SCQP_HPP

#include "lscq/config.hpp"
#include "lscq/cas2.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace lscq {

/// @brief SCQP - SCQ with Pointers（直接存储指针）
/// @tparam T 对象类型（存储T*指针）
/// @note 要求CAS2硬件支持，否则无法实例化
template<typename T>
class SCQP {
public:
    static constexpr size_t SCQSIZE = config::DEFAULT_SCQSIZE;

    // === 关键常量变更 ===
    static constexpr uint64_t THRESHOLD = 4 * SCQSIZE - 1;  // ← 从3n-1提升为4n-1
    static constexpr uint64_t BOTTOM = 2 * SCQSIZE - 1;

    SCQP();
    ~SCQP();

    SCQP(const SCQP&) = delete;
    SCQP& operator=(const SCQP&) = delete;

    /// @brief 入队（指针模式）
    /// @param ptr 对象指针（调用者保证生命周期）
    /// @return true=成功, false=队列满
    bool enqueue(T* ptr);

    /// @brief 出队
    /// @return 对象指针，队列空时返回nullptr
    T* dequeue();

    bool is_empty() const;
    bool is_full() const;  // ← 新增：队列满检测

private:
    /// @brief Entry结构（关键变更）
    /// 直接存储指针，要求8字节对齐
    struct alignas(16) Entry {
        uint64_t cycle_and_safe;  // [63:1]=cycle, [0]=isSafe
        T* ptr;                   // ← 存储指针而非索引

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
                   ptr == other.ptr;
        }
    };

    Entry* entries_;
    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> tail_;
    alignas(64) std::atomic<uint64_t> threshold_;

    size_t cache_remap(size_t idx) const;
    void fix_state();

    /// @brief 检查队列是否满（新增）
    bool check_full(uint64_t t, uint64_t h) const {
        return (t - h) >= SCQSIZE;
    }

#if !LSCQ_HAS_CAS2_COMPILE_TIME
    static_assert(false, "SCQP requires CAS2 hardware support");
#endif
};

}  // namespace lscq

#endif  // LSCQ_SCQP_HPP
```

**关键设计点**:
1. **T* ptr**: Entry直接存储指针，要求`sizeof(T*) == 8`
2. **4n-1 Threshold**: 论文证明的指针队列阈值
3. **is_full()**: 检测 `tail - head >= SCQSIZE`
4. **static_assert**: 无CAS2硬件时编译报错

**验收点**: 头文件编译无错误，`sizeof(Entry) == 16`

---

### 4.2 4n-1 Threshold数学推导 (Day 2)

#### 4.2.1 为什么SCQP需要4n-1？

**SCQ（索引模式，3n-1）**:
- Entry结构：`{cycle, index}`
- 最坏情况：P个生产者 + C个消费者竞争
- 3n-1保证至少一个线程成功

**SCQP（指针模式，4n-1）**:
- Entry结构：`{cycle, T*}`
- **新增复杂性**: 队列满时需要返回失败（而非阻塞）
- **额外竞争**: enqueue可能因"队列满"失败，增加重试
- **4n-1保证**: 即使队列满，也能在4n-1次重试内检测到

**推导（简化）**:
```
设 n = SCQSIZE, P = 生产者数量, C = 消费者数量

SCQ: enqueue阻塞等待空位，最多 3n-1 次重试保证成功
SCQP: enqueue需要检测"队列满"并返回，额外增加 n 次重试余量
       → Threshold = 3n + n - 1 = 4n - 1
```

**论文引用**（`1908.04511v1.pdf` 第4节）:
> "For SCQP, we increase the threshold to 4n-1 to account for full-queue scenarios."

---

### 4.3 SCQP核心实现 (Day 3-4)

#### 4.3.1 创建 `src/scqp.cpp`

```cpp
#include "lscq/scqp.hpp"
#include <new>
#include <cstdlib>

namespace lscq {

template<typename T>
SCQP<T>::SCQP() : head_(SCQSIZE), tail_(SCQSIZE), threshold_(THRESHOLD) {
    entries_ = static_cast<Entry*>(
        aligned_alloc(64, sizeof(Entry) * SCQSIZE)
    );
    if (!entries_) {
        throw std::bad_alloc();
    }

    // 初始化：cycle=0, ptr=nullptr
    for (size_t i = 0; i < SCQSIZE; ++i) {
        entries_[i].cycle_and_safe = 0;
        entries_[i].ptr = nullptr;
    }
}

template<typename T>
SCQP<T>::~SCQP() {
    free(entries_);
}

// === enqueue实现（带队列满检测） ===
template<typename T>
bool SCQP<T>::enqueue(T* ptr) {
    if (ptr == nullptr) {
        return false;  // 不允许插入nullptr
    }

    while (true) {
        uint64_t t = tail_.load(std::memory_order_acquire);
        uint64_t h = head_.load(std::memory_order_acquire);

        // === 关键新增：队列满检测 ===
        if (check_full(t, h)) {
            return false;  // 队列满，立即返回
        }

        size_t j = cache_remap(t % SCQSIZE);
        Entry ent = entries_[j];

        uint64_t cycle_t = t / SCQSIZE;
        uint64_t cycle_e = ent.cycle();

        // Threshold检查（与SCQ相同）
        uint64_t thr = threshold_.load(std::memory_order_relaxed);
        if (thr == 0) {
            fix_state();
            threshold_.store(THRESHOLD, std::memory_order_relaxed);
            continue;
        }

        if (cycle_e < cycle_t) {
            // Entry可用
            if (!ent.is_safe()) {
                Entry safe_ent = ent;
                safe_ent.set_safe(true);
                Entry expected = ent;

                if (cas2(&entries_[j], expected, safe_ent)) {
                    ent = safe_ent;
                } else {
                    threshold_.fetch_sub(1, std::memory_order_relaxed);
                    continue;
                }
            }

            // 插入指针
            Entry new_ent;
            new_ent.set_cycle(cycle_t);
            new_ent.set_safe(true);
            new_ent.ptr = ptr;  // ← 存储指针

            Entry expected = ent;

            if (cas2(&entries_[j], expected, new_ent)) {
                tail_.compare_exchange_weak(
                    t, t + 1,
                    std::memory_order_release,
                    std::memory_order_relaxed
                );
                return true;  // 成功
            }
        } else if (cycle_e == cycle_t) {
            tail_.compare_exchange_weak(
                t, t + 1,
                std::memory_order_release,
                std::memory_order_relaxed
            );
        }

        threshold_.fetch_sub(1, std::memory_order_relaxed);
    }
}

// === dequeue实现（与SCQ几乎相同） ===
template<typename T>
T* SCQP<T>::dequeue() {
    while (true) {
        uint64_t h = head_.load(std::memory_order_acquire);
        size_t j = cache_remap(h % SCQSIZE);

        Entry ent = entries_[j];

        uint64_t cycle_h = h / SCQSIZE;
        uint64_t cycle_e = ent.cycle();

        uint64_t thr = threshold_.load(std::memory_order_relaxed);
        if (thr == 0) {
            fix_state();
            threshold_.store(THRESHOLD, std::memory_order_relaxed);
            continue;
        }

        if (cycle_e == cycle_h) {
            if (!ent.is_safe()) {
                threshold_.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            if (head_.compare_exchange_weak(
                    h, h + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {

                // 标记已消费
                Entry consumed_ent;
                consumed_ent.set_cycle(BOTTOM);
                consumed_ent.set_safe(false);
                consumed_ent.ptr = nullptr;

                Entry expected = ent;
                cas2(&entries_[j], expected, consumed_ent);

                return ent.ptr;  // ← 返回指针
            }
        } else if (cycle_e + 1 == cycle_h) {
            return nullptr;  // 队列空
        }

        threshold_.fetch_sub(1, std::memory_order_relaxed);
    }
}

template<typename T>
void SCQP<T>::fix_state() {
    // 与SCQ相同
    while (true) {
        uint64_t h = head_.load(std::memory_order_acquire);
        uint64_t t = tail_.load(std::memory_order_acquire);

        if (h <= t || (h - t <= SCQSIZE)) {
            return;
        }

        if (tail_.compare_exchange_weak(
                t, h,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

template<typename T>
size_t SCQP<T>::cache_remap(size_t idx) const {
    constexpr size_t ENTRIES_PER_LINE = 64 / sizeof(Entry);
    size_t line = idx / ENTRIES_PER_LINE;
    size_t offset = idx % ENTRIES_PER_LINE;
    return (offset * (SCQSIZE / ENTRIES_PER_LINE)) + line;
}

template<typename T>
bool SCQP<T>::is_empty() const {
    return head_.load(std::memory_order_relaxed) >=
           tail_.load(std::memory_order_relaxed);
}

template<typename T>
bool SCQP<T>::is_full() const {
    uint64_t t = tail_.load(std::memory_order_relaxed);
    uint64_t h = head_.load(std::memory_order_relaxed);
    return (t - h) >= SCQSIZE;
}

// 显式实例化（常见类型）
template class SCQP<int>;
template class SCQP<uint64_t>;
template class SCQP<void>;  // void* 通用指针

}  // namespace lscq
```

**关键算法点**:
1. **check_full()**: 每次enqueue前检查队列是否满
2. **enqueue返回bool**: 满队列时返回false（而非阻塞）
3. **4n-1 Threshold**: 使用THRESHOLD常量
4. **指针存储**: Entry.ptr直接存储对象指针

**验收点**: 编译通过，无语法错误

---

### 4.4 对象队列测试 (Day 5)

#### 4.4.1 创建 `tests/unit/test_scqp.cpp`

**核心测试**: 验证指针队列和队列满检测

```cpp
#include <gtest/gtest.h>
#include "lscq/scqp.hpp"
#include <thread>
#include <vector>
#include <memory>

using namespace lscq;

// === 测试用对象 ===
struct TestObject {
    int id;
    std::string data;

    TestObject(int i, const std::string& d) : id(i), data(d) {}
};

// === 基础指针队列测试 ===
TEST(SCQPTest, BasicEnqueueDequeue) {
    SCQP<TestObject> queue;

    TestObject obj1(1, "hello");
    TestObject obj2(2, "world");

    EXPECT_TRUE(queue.enqueue(&obj1));
    EXPECT_TRUE(queue.enqueue(&obj2));

    TestObject* p1 = queue.dequeue();
    EXPECT_NE(p1, nullptr);
    EXPECT_EQ(p1->id, 1);
    EXPECT_EQ(p1->data, "hello");

    TestObject* p2 = queue.dequeue();
    EXPECT_NE(p2, nullptr);
    EXPECT_EQ(p2->id, 2);

    EXPECT_EQ(queue.dequeue(), nullptr);  // 空队列
}

// === 队列满检测测试（关键） ===
TEST(SCQPTest, QueueFull) {
    constexpr size_t QSIZE = 1024;  // 小队列便于测试
    SCQP<int> queue;

    std::vector<int> objects(QSIZE);
    for (size_t i = 0; i < QSIZE; ++i) {
        objects[i] = i;
    }

    // 填满队列
    for (size_t i = 0; i < QSIZE; ++i) {
        EXPECT_TRUE(queue.enqueue(&objects[i])) << "Failed at " << i;
    }

    // 再次入队应该失败（队列满）
    int extra = 999;
    EXPECT_FALSE(queue.enqueue(&extra)) << "Queue should be full";

    // 出队一个元素后可继续入队
    EXPECT_NE(queue.dequeue(), nullptr);
    EXPECT_TRUE(queue.enqueue(&extra)) << "Should succeed after dequeue";
}

// === nullptr拒绝测试 ===
TEST(SCQPTest, RejectNullptr) {
    SCQP<int> queue;
    EXPECT_FALSE(queue.enqueue(nullptr));
}

// === 并发对象队列测试 ===
TEST(SCQPTest, ConcurrentObjectQueue) {
    constexpr int NUM_THREADS = 8;
    constexpr int ITEMS_PER_THREAD = 1000;

    SCQP<TestObject> queue;

    // 预分配对象（避免多线程new）
    std::vector<std::vector<TestObject>> objects(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; ++i) {
        objects[i].reserve(ITEMS_PER_THREAD);
        for (int j = 0; j < ITEMS_PER_THREAD; ++j) {
            objects[i].emplace_back(i * ITEMS_PER_THREAD + j, "data");
        }
    }

    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};

    auto producer = [&](int id) {
        for (int i = 0; i < ITEMS_PER_THREAD; ++i) {
            while (!queue.enqueue(&objects[id][i])) {
                std::this_thread::yield();  // 队列满，等待
            }
            enqueued.fetch_add(1);
        }
    };

    auto consumer = [&]() {
        while (dequeued.load() < NUM_THREADS * ITEMS_PER_THREAD) {
            TestObject* obj = queue.dequeue();
            if (obj != nullptr) {
                EXPECT_GE(obj->id, 0);
                EXPECT_LT(obj->id, NUM_THREADS * ITEMS_PER_THREAD);
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

// === SCQP vs SCQ功能对比 ===
TEST(SCQPTest, CompareWithSCQ) {
    // SCQP支持指针，SCQ支持索引
    // 两者性能应接近（SCQP可能略慢，因为检测队列满）
    SCQP<int> scqp;
    SCQ<uint64_t> scq;

    int value = 42;
    scqp.enqueue(&value);
    scq.enqueue(42);

    EXPECT_EQ(scqp.dequeue()->id, 42);
    EXPECT_EQ(scq.dequeue(), 42);
}

// === 4n-1 Threshold验证 ===
TEST(SCQPTest, ThresholdValue) {
    SCQP<int> queue;
    EXPECT_EQ(queue.THRESHOLD, 4 * queue.SCQSIZE - 1);
}
```

**测试覆盖点**:
- ✅ 基础指针队列功能
- ✅ 队列满检测（关键）
- ✅ nullptr拒绝
- ✅ 并发对象队列
- ✅ 4n-1 Threshold验证

**验收点**:
```bash
./build/debug/tests/lscq_tests --gtest_filter="SCQPTest.*"
# 所有测试通过
```

---

### 4.5 性能Benchmark (Day 6)

#### 4.5.1 创建 `benchmarks/benchmark_scqp.cpp`

**目标**: 对比SCQP vs SCQ性能

```cpp
#include <benchmark/benchmark.h>
#include "lscq/scqp.hpp"
#include "lscq/scq.hpp"
#include <thread>
#include <vector>

using namespace lscq;

// === SCQP Pair Benchmark ===
static void BM_SCQP_Pair(benchmark::State& state) {
    int thread_count = state.range(0);
    SCQP<int> queue;

    // 预分配对象池
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
            int local_val = id;
            while (!queue.enqueue(&local_val)) {
                // 队列满，重试
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

// === SCQ Pair Benchmark（对比） ===
static void BM_SCQ_Pair(benchmark::State& state) {
    int thread_count = state.range(0);
    SCQ<uint64_t> queue;

    for (int i = 0; i < thread_count * 10; ++i) {
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

BENCHMARK_MAIN();
```

**性能预期**（Phase 4）:
| 场景 | SCQP | SCQ | 差异 |
|------|------|-----|------|
| Pair @ 1 thread | ~8 Mops/sec | ~8 Mops/sec | 相当 |
| Pair @ 16 threads | ~45 Mops/sec | ~50 Mops/sec | -10% |

**解释**: SCQP因队列满检测，性能略低于SCQ（5-10%）

**验收点**:
```bash
./build/release/benchmarks/lscq_benchmarks --benchmark_filter="SCQP|SCQ"
```

---

### 4.6 CMake集成 (Day 7)

更新 `CMakeLists.txt`:
```cmake
add_library(lscq_impl STATIC
    src/ncq.cpp
    src/scq.cpp
    src/scqp.cpp  # 新增
)

add_executable(lscq_tests
    tests/unit/test_cas2.cpp
    tests/unit/test_ncq.cpp
    tests/unit/test_scq.cpp
    tests/unit/test_scqp.cpp  # 新增
)

add_executable(lscq_benchmarks
    benchmarks/benchmark_cas2.cpp
    benchmarks/benchmark_ncq.cpp
    benchmarks/benchmark_scq.cpp
    benchmarks/benchmark_scqp.cpp  # 新增
)
```

---

## 5. 交付物清单

### 5.1 代码文件
- [ ] `include/lscq/scqp.hpp` - SCQP类声明
- [ ] `src/scqp.cpp` - SCQP实现
- [ ] `tests/unit/test_scqp.cpp` - SCQP单元测试
- [ ] `benchmarks/benchmark_scqp.cpp` - SCQP vs SCQ对比
- [ ] 更新后的 `CMakeLists.txt`

### 5.2 测试结果
- [ ] 所有单元测试通过（至少5个测试用例）
- [ ] 队列满测试通过
- [ ] 并发对象队列测试通过
- [ ] Benchmark结果（保存为 `docs/Phase4-Benchmark结果.md`）

### 5.3 文档
- [ ] `docs/Phase4-交接文档.md` - **必须创建**
- [ ] `docs/Phase4-Benchmark结果.md` - SCQP vs SCQ性能数据

---

## 6. 验收标准 (Gate Conditions)

### 6.1 编译验收
- ✅ **G1.1**: SCQP代码零警告编译通过
- ✅ **G1.2**: 无CAS2硬件时编译报错（static_assert生效）

### 6.2 功能验收
- ✅ **G2.1**: BasicEnqueueDequeue测试通过
- ✅ **G2.2**: QueueFull测试通过（队列满正确检测）
- ✅ **G2.3**: RejectNullptr测试通过
- ✅ **G2.4**: ConcurrentObjectQueue测试通过
- ✅ **G2.5**: 无data race（ThreadSanitizer）

### 6.3 性能验收
- ✅ **G3.1**: SCQP @ 16 threads > 40 Mops/sec
- ✅ **G3.2**: SCQP性能在SCQ的90-100%范围内
- ✅ **G3.3**: 队列满场景无死锁

### 6.4 文档验收
- ✅ **G4.1**: `docs/Phase4-交接文档.md` 已创建（>200行）

---

## 7. 下一阶段预览

### 7.1 Phase 5概述
**阶段名称**: LSCQ无界队列
**核心任务**: 链接多个SCQ/SCQP构成无界队列
**关键技术**: Epoch-Based Reclamation、Finalize机制、链表操作
**预计工期**: 2周

### 7.2 Phase 5依赖本阶段的关键产出
1. **SCQP作为节点** - LSCQ链表节点使用SCQP
2. **指针队列** - LSCQ内部存储SCQNode*
3. **队列满检测** - LSCQ通过is_full()触发链表扩展

---

## 8. 阶段完成交接文档要求

创建 `docs/Phase4-交接文档.md`，包含：

```markdown
# Phase 4 交接文档

## 1. 完成情况概览
## 2. 关键代码位置索引
- Entry定义：`include/lscq/scqp.hpp:L42-L68`
- 队列满检测：`src/scqp.cpp:L35-L40`
- 4n-1 Threshold：`include/lscq/scqp.hpp:L20`

## 3. SCQP算法验证结果
- 队列满检测：✅ 正常工作
- 4n-1 Threshold：✅ 无livelock
- 指针队列：✅ 对象正确入队出队

## 4. 性能Benchmark结果
| 场景 | SCQP | SCQ | 差异 |
|------|------|-----|------|
| ...  | ...  | ... | ...  |

## 5. Phase 5接口预留
- SCQP可用作LSCQ节点
- is_full()触发链表扩展
```

---

## 9. 常见问题（FAQ）

### Q1: 为什么enqueue返回bool而非阻塞？
**A**: SCQP是有界队列，队列满时必须返回失败供LSCQ扩展链表。

### Q2: SCQP性能为什么比SCQ略低？
**A**: 每次enqueue需要检测队列满（额外的head/tail读取），增加开销。

### Q3: 无CAS2硬件怎么办？
**A**: SCQP强制要求CAS2，无硬件支持时编译报错。用户应使用SCQ（索引模式）。

---

## 10. 参考资料

- `docs/Phase3-交接文档.md` - SCQ实现
- `docs/01-技术实现思路.md` 第4.3节 - SCQP算法
- `1908.04511v1.pdf` 第4节 - SCQP变体

---

**StarterPrompt版本**: v1.0
**创建日期**: 2026-01-18
**适用范围**: LSCQ项目 Phase 4

---

Phase3交接文档： @docs\Phase3-交接文档.md