# Phase 6 StarterPrompt: 优化与多平台支持

> **任务代号**: LSCQ-Phase6-Optimization
> **预计工期**: 2-3周
> **前置依赖**: Phase 5 完成（LSCQ就绪）
> **后续阶段**: Phase 7 (持续维护)

---

## 1. 任务概述

### 1.1 核心目标
项目收尾阶段，完成性能调优、多平台支持和文档完善：
1. **性能Profiling** - 识别热点函数并优化
2. **ARM64移植** - 实现ARM64 CAS2支持
3. **Fallback方案** - 无CAS2硬件时的降级策略
4. **文档完善** - API文档、使用教程、性能报告
5. **CI/CD多平台** - Linux、macOS、Windows自动化测试

### 1.2 技术挑战
- **Profiling分析**: 使用perf/gprof识别瓶颈
- **ARM64 CAS2**: CASP指令的C++封装
- **跨平台兼容**: 统一接口，条件编译
- **文档质量**: 清晰、准确、易用

### 1.3 任务价值
- ✅ 发布production-ready版本
- ✅ 支持主流平台（x86/ARM/PPC）
- ✅ 完整的文档体系
- ✅ 为开源社区贡献高质量代码

---

## 2. 任务边界

### 2.1 In Scope (本阶段必须完成)
- [x] 性能Profiling（perf/VTune）
- [x] 热点函数优化（cache_remap、threshold检查）
- [x] ARM64 CAS2实现
- [x] Fallback到单字宽CAS（无CAS2硬件）
- [x] PowerPC平台验证
- [x] 完整API文档（Doxygen）
- [x] 使用教程和示例
- [x] 性能对比报告
- [x] CI/CD多平台（GitHub Actions）

### 2.2 Out of Scope (未来工作)
- ❌ NUMA优化（需要专用硬件）
- ❌ GPU加速（不适用）
- ❌ 商业支持（开源项目）

---

## 3. 前置条件与输入

### 3.1 前置依赖
- ✅ Phase 5已通过所有验收Gate
- ✅ `docs/Phase5-交接文档.md` 已创建
- ✅ LSCQ功能完整

### 3.2 必读文档（按顺序）
1. **`docs/Phase5-交接文档.md`** - LSCQ实现和性能数据
2. **`docs/01-技术实现思路.md` 第3.1节** - CAS2跨平台实现
3. **`docs/02-分段开发计划.md` 第8节** - Phase 6计划
4. **`1908.04511v1.pdf` 第6节** - 性能评估方法

### 3.3 关键代码复用（来自Phase 5）
- 所有已实现的队列（NCQ/SCQ/SCQP/LSCQ）
- CAS2抽象层（扩展为ARM64）
- Benchmark框架（跨平台验证）

### 3.4 环境要求
- Phase 5构建环境
- 性能分析工具（perf/VTune/Instruments）
- ARM64测试环境（可选：树莓派/AWS Graviton）

---

## 4. 详细任务清单

### 4.1 性能Profiling与优化 (Day 1-4)

#### 4.1.1 Profiling工具配置

**Linux (perf)**:
```bash
# 编译带调试符号的Release版本
cmake --preset windows-clang-release -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 运行Profiling
perf record -g ./build/release/benchmarks/lscq_benchmarks --benchmark_filter="LSCQ_Pair"
perf report

# 热点分析
perf annotate
```

**macOS (Instruments)**:
```bash
# 使用Xcode Instruments
instruments -t "Time Profiler" ./build/release/benchmarks/lscq_benchmarks
```

**Windows (VTune)**:
```bash
# Intel VTune Profiler
vtune -collect hotspots -- .\build\release\benchmarks\lscq_benchmarks.exe
```

#### 4.1.2 预期热点函数
1. **cache_remap** - 频繁调用（每次enqueue/dequeue）
2. **cas2** - 原子操作开销
3. **threshold检查** - 每次重试递减
4. **fix_state** - catchup触发时的开销

#### 4.1.3 优化策略

**优化1: cache_remap内联**
```cpp
// 修改 include/lscq/scq.hpp
__attribute__((always_inline))
inline size_t cache_remap(size_t idx) const {
    constexpr size_t ENTRIES_PER_LINE = 64 / sizeof(Entry);
    size_t line = idx / ENTRIES_PER_LINE;
    size_t offset = idx % ENTRIES_PER_LINE;
    return (offset * (SCQSIZE / ENTRIES_PER_LINE)) + line;
}
```

**优化2: Threshold快速路径**
```cpp
// 修改 src/scq.cpp enqueue
uint64_t thr = threshold_.load(std::memory_order_relaxed);
if (__builtin_expect(thr > 0, 1)) {  // likely分支预测
    // 快速路径
} else {
    // 慢路径：fix_state
}
```

**优化3: Entry对齐优化**
```cpp
// 确保Entry cache line对齐
struct alignas(64) Entry {  // 改为64字节对齐
    uint64_t cycle_and_safe;
    T ptr;
    char padding[64 - sizeof(uint64_t) - sizeof(T*)];
};
```

**验收点**: Profiling前后性能提升5-10%

---

### 4.2 ARM64移植 (Day 5-7)

#### 4.2.1 ARM64 CAS2实现

**ARM64特性**:
- **CASP指令** (Compare-And-Swap Pair): ARMv8.1+支持
- **128位原子操作**: 类似x86-64的cmpxchg16b

**修改 `include/lscq/cas2.hpp`**:

```cpp
#ifndef LSCQ_CAS2_HPP
#define LSCQ_CAS2_HPP

#include "lscq/config.hpp"
#include <cstdint>
#include <atomic>

namespace lscq {

struct alignas(16) Entry {
    uint64_t cycle_flags;
    uint64_t index_or_ptr;

    bool operator==(const Entry& other) const {
        return cycle_flags == other.cycle_flags &&
               index_or_ptr == other.index_or_ptr;
    }
};

// === x86-64 CAS2实现 ===
#if defined(LSCQ_ARCH_X86_64) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)

inline bool cas2(Entry* ptr, Entry& expected, Entry& desired) {
    __int128 exp = *reinterpret_cast<__int128*>(&expected);
    __int128 des = *reinterpret_cast<__int128*>(&desired);

    bool success = __sync_bool_compare_and_swap_16(
        reinterpret_cast<__int128*>(ptr), exp, des
    );

    if (!success) {
        expected = *ptr;
    }
    return success;
}

inline bool has_cas2_support() {
    return true;  // x86-64编译时已检测
}

// === ARM64 CAS2实现 ===
#elif defined(LSCQ_ARCH_ARM64) && (__ARM_ARCH >= 8)

inline bool cas2(Entry* ptr, Entry& expected, Entry& desired) {
    // 使用C++20 std::atomic (需要Clang 11+或GCC 10+)
    std::atomic<Entry>* atomic_ptr = reinterpret_cast<std::atomic<Entry>*>(ptr);

    return atomic_ptr->compare_exchange_strong(
        expected, desired,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

inline bool has_cas2_support() {
    // ARMv8.1+运行时检测（通过CPUID）
#if __ARM_ARCH >= 8
    return true;  // 简化实现：假设ARMv8+都支持
#else
    return false;
#endif
}

// === Fallback实现（使用std::mutex） ===
#else

#include <mutex>

class CAS2Fallback {
    mutable std::mutex mutex_;

public:
    bool cas2(Entry* ptr, Entry& expected, Entry& desired) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (*ptr == expected) {
            *ptr = desired;
            return true;
        }
        expected = *ptr;
        return false;
    }
};

extern CAS2Fallback g_cas2_fallback;

inline bool cas2(Entry* ptr, Entry& expected, Entry& desired) {
    return g_cas2_fallback.cas2(ptr, expected, desired);
}

inline bool has_cas2_support() {
    return false;  // Fallback模式
}

#endif

}  // namespace lscq

#endif  // LSCQ_CAS2_HPP
```

**关键点**:
1. **ARM64 CASP**: 使用`std::atomic<Entry>`（C++20）
2. **运行时检测**: `has_cas2_support()`返回平台能力
3. **Fallback**: 无硬件支持时使用`std::mutex`

**验收点**: ARM64平台编译通过，测试通过

---

### 4.3 PowerPC平台验证 (Day 8)

#### 4.3.1 PowerPC CAS2检测

**PowerPC特性**:
- **lqarx/stqcx**: 128位LL/SC指令（PowerPC 64位）
- **编译器支持**: GCC 8+

**修改 `include/lscq/config.hpp`**:

```cpp
// 检测CPU架构
#if defined(__x86_64__) || defined(_M_X64)
    #define LSCQ_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define LSCQ_ARCH_ARM64 1
#elif defined(__powerpc64__) || defined(__ppc64__)
    #define LSCQ_ARCH_PPC64 1
#else
    #define LSCQ_ARCH_UNKNOWN 1
#endif

// 检测CAS2支持
#if defined(LSCQ_ARCH_X86_64) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
    #define LSCQ_HAS_CAS2_COMPILE_TIME 1
#elif defined(LSCQ_ARCH_ARM64) && (__ARM_ARCH >= 8)
    #define LSCQ_HAS_CAS2_COMPILE_TIME 1
#elif defined(LSCQ_ARCH_PPC64) && (__GNUC__ >= 8)
    #define LSCQ_HAS_CAS2_COMPILE_TIME 1
#else
    #define LSCQ_HAS_CAS2_COMPILE_TIME 0
#endif
```

**验收点**: PowerPC平台编译通过（使用Fallback）

---

### 4.4 Fallback性能测试 (Day 9)

#### 4.4.1 创建 `tests/fallback/test_fallback.cpp`

```cpp
#include <gtest/gtest.h>
#include "lscq/scq.hpp"

// 强制Fallback模式
#undef LSCQ_HAS_CAS2_COMPILE_TIME
#define LSCQ_HAS_CAS2_COMPILE_TIME 0

#include "lscq/cas2.hpp"

TEST(FallbackTest, BasicFunctionality) {
    EXPECT_FALSE(lscq::has_cas2_support());

    // 验证Fallback下队列仍正常工作
    lscq::SCQ<uint64_t> queue;
    queue.enqueue(42);
    EXPECT_EQ(queue.dequeue(), 42);
}

TEST(FallbackTest, ConcurrentCorrectness) {
    lscq::SCQ<uint64_t> queue;

    // 简单并发测试
    std::thread t1([&]() {
        for (int i = 0; i < 1000; ++i) {
            queue.enqueue(i);
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < 1000; ++i) {
            while (queue.dequeue() == lscq::SCQ<uint64_t>::EMPTY_VALUE) {
                std::this_thread::yield();
            }
        }
    });

    t1.join();
    t2.join();
}
```

**性能预期**: Fallback性能约为CAS2的30-50%（但保证功能正确）

---

### 4.5 API文档完善 (Day 10-11)

#### 4.5.1 Doxygen配置

**创建 `Doxyfile`**:
```bash
PROJECT_NAME           = "LSCQ - Scalable MPMC Queue"
PROJECT_BRIEF          = "Lock-free MPMC queue based on academic research"
INPUT                  = include/ src/
RECURSIVE              = YES
GENERATE_HTML          = YES
GENERATE_LATEX         = NO
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
```

**运行Doxygen**:
```bash
doxygen Doxyfile
# 生成 html/index.html
```

#### 4.5.2 补充Doxygen注释

**示例（SCQ类）**:
```cpp
/// @file scq.hpp
/// @brief Scalable Circular Queue implementation
/// @author LSCQ Team
/// @version 1.0

namespace lscq {

/// @class SCQ
/// @brief Bounded lock-free MPMC queue with threshold mechanism
///
/// SCQ uses Ring Buffer + FAA + CAS2 to achieve high scalability.
/// Key features:
/// - Threshold mechanism (3n-1) to avoid livelock
/// - Cache remap to reduce false sharing
/// - Catchup optimization for dequeue-heavy workloads
///
/// @tparam T Element type (usually uint64_t index)
///
/// @code
/// SCQ<uint64_t> queue;
/// queue.enqueue(42);
/// uint64_t val = queue.dequeue();
/// @endcode
///
/// @note Requires CAS2 hardware support (x86-64 cmpxchg16b or ARM64 CASP)
template<typename T = uint64_t>
class SCQ {
public:
    /// @brief Constructor - initializes ring buffer
    /// @throws std::bad_alloc if memory allocation fails
    SCQ();

    /// @brief Enqueue an element (blocking until success)
    /// @param index Element to enqueue
    void enqueue(T index);

    /// @brief Dequeue an element
    /// @return Element or EMPTY_VALUE if queue is empty
    T dequeue();

    // ... 其他方法
};

}  // namespace lscq
```

**验收点**: `doxygen Doxyfile` 无警告，文档覆盖率100%

---

### 4.6 使用教程和示例 (Day 12)

#### 4.6.1 创建 `examples/simple_usage.cpp`

```cpp
#include "lscq/lscq.hpp"
#include <iostream>
#include <thread>
#include <vector>

// 示例：多生产者多消费者任务队列

struct Task {
    int id;
    std::string description;
};

int main() {
    lscq::LSCQ<Task> task_queue;

    // 生产者线程
    auto producer = [&](int producer_id) {
        for (int i = 0; i < 100; ++i) {
            Task* task = new Task{producer_id * 100 + i, "Task from producer " + std::to_string(producer_id)};
            task_queue.enqueue(task);
            std::cout << "Produced: Task " << task->id << "\n";
        }
    };

    // 消费者线程
    auto consumer = [&](int consumer_id) {
        while (true) {
            Task* task = task_queue.dequeue();
            if (task == nullptr) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::cout << "Consumer " << consumer_id << " processing: " << task->description << "\n";
            delete task;  // 处理完后释放
        }
    };

    std::vector<std::thread> threads;

    // 启动3个生产者
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back(producer, i);
    }

    // 启动2个消费者
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(consumer, i);
    }

    // 等待生产者完成
    for (int i = 0; i < 3; ++i) {
        threads[i].join();
    }

    // 停止消费者（实际应用需要优雅关闭机制）
    for (int i = 3; i < 5; ++i) {
        threads[i].detach();
    }

    return 0;
}
```

#### 4.6.2 创建 `README.md`

```markdown
# LSCQ - Scalable MPMC Queue

High-performance lock-free Multi-Producer Multi-Consumer queue based on the paper:
**"A Scalable, Portable, and Memory-Efficient Lock-Free FIFO Queue"**

## Features

- ✅ **Lock-free**: No mutexes, high scalability
- ✅ **MPMC**: Multi-producer multi-consumer
- ✅ **Unbounded**: LSCQ variant supports unlimited size
- ✅ **High performance**: >50 Mops/sec @ 16 cores
- ✅ **Cross-platform**: x86-64, ARM64, PowerPC

## Quick Start

### Installation

```bash
git clone https://github.com/yourname/lscq-cpp.git
cd lscq-cpp
cmake --preset windows-clang-release
cmake --build build/release
ctest --test-dir build/release
```

### Usage

```cpp
#include "lscq/lscq.hpp"

lscq::LSCQ<int> queue;

// Producer
int value = 42;
queue.enqueue(&value);

// Consumer
int* ptr = queue.dequeue();
if (ptr != nullptr) {
    std::cout << *ptr << "\n";
}
```

## Performance

| Queue Type | Threads | Throughput |
|------------|---------|------------|
| LSCQ       | 16      | 48 Mops/sec |
| SCQ        | 16      | 52 Mops/sec |
| MSQueue    | 16      | 18 Mops/sec |

## Documentation

- [API Reference](docs/API.md)
- [Implementation Details](docs/01-技术实现思路.md)
- [Benchmark Results](docs/Phase6-性能报告.md)

## License

MIT License - see LICENSE file
```

**验收点**: 示例可编译运行，README清晰易懂

---

### 4.7 CI/CD多平台配置 (Day 13-14)

#### 4.7.1 更新 `.github/workflows/ci.yml`

```yaml
name: CI

on: [push, pull_request]

jobs:
  test-linux:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        compiler: [gcc-10, clang-14]
    steps:
      - uses: actions/checkout@v3

      - name: Install Dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake ninja-build

      - name: Configure
        run: cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=${{ matrix.compiler }}

      - name: Build
        run: cmake --build build

      - name: Test
        run: ctest --test-dir build --output-on-failure

      - name: Sanitizer Test
        run: |
          cmake -B build-san -G Ninja -DCMAKE_CXX_COMPILER=${{ matrix.compiler }} -DLSCQ_ENABLE_SANITIZERS=ON
          cmake --build build-san
          ctest --test-dir build-san --output-on-failure

  test-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v3

      - name: Install Dependencies
        run: brew install cmake ninja

      - name: Configure
        run: cmake -B build -G Ninja

      - name: Build
        run: cmake --build build

      - name: Test
        run: ctest --test-dir build --output-on-failure

  test-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3

      - name: Setup Clang
        run: choco install llvm --version=14.0.0

      - name: Configure
        run: cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++

      - name: Build
        run: cmake --build build

      - name: Test
        run: ctest --test-dir build --output-on-failure

  benchmark:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - name: Install Dependencies
        run: sudo apt-get install -y cmake ninja-build

      - name: Configure Release
        run: cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build build

      - name: Run Benchmarks
        run: ./build/benchmarks/lscq_benchmarks --benchmark_min_time=1s

      - name: Upload Results
        uses: actions/upload-artifact@v3
        with:
          name: benchmark-results
          path: benchmark_results.txt
```

**验收点**: CI在Linux/macOS/Windows三平台全部通过

---

## 5. 交付物清单

### 5.1 代码文件
- [ ] 优化后的 `src/scq.cpp`（内联、分支预测）
- [ ] 扩展后的 `include/lscq/cas2.hpp`（ARM64支持）
- [ ] `tests/fallback/test_fallback.cpp` - Fallback测试
- [ ] `examples/simple_usage.cpp` - 使用示例
- [ ] `Doxyfile` - Doxygen配置

### 5.2 文档
- [ ] `README.md` - 项目主文档
- [ ] `docs/API.md` - API参考手册
- [ ] `docs/Phase6-性能报告.md` - 最终性能数据
- [ ] `docs/Phase6-交接文档.md` - **必须创建**
- [ ] Doxygen生成的HTML文档

### 5.3 CI/CD
- [ ] `.github/workflows/ci.yml` - 多平台CI
- [ ] 所有平台测试通过

---

## 6. 验收标准 (Gate Conditions)

### 6.1 性能验收
- ✅ **G1.1**: Profiling优化后性能提升5-10%
- ✅ **G1.2**: SCQ @ 16 threads > 55 Mops/sec
- ✅ **G1.3**: LSCQ @ 16 threads > 45 Mops/sec

### 6.2 平台验收
- ✅ **G2.1**: x86-64平台所有测试通过
- ✅ **G2.2**: ARM64平台编译通过，测试通过（如有设备）
- ✅ **G2.3**: Fallback模式功能正确

### 6.3 文档验收
- ✅ **G3.1**: Doxygen文档覆盖率100%
- ✅ **G3.2**: README清晰完整
- ✅ **G3.3**: 至少2个使用示例

### 6.4 CI/CD验收
- ✅ **G4.1**: CI在Linux/macOS/Windows全部通过
- ✅ **G4.2**: Benchmark自动运行并上传结果

---

## 7. 项目总结

### 7.1 完成情况
- **Phase 1-6**: 全部完成
- **代码行数**: 预计~10000行（含测试和文档）
- **测试覆盖率**: >90%
- **性能达标**: 论文水平的80-100%

### 7.2 技术亮点
1. ✅ 完整实现了论文的4种队列（NCQ/SCQ/SCQP/LSCQ）
2. ✅ Epoch-Based Reclamation内存回收
3. ✅ 跨平台支持（x86/ARM/PPC）
4. ✅ 完善的测试和文档

### 7.3 未来工作（Phase 7）
- Bug修复
- 性能监控
- 社区反馈响应
- 新平台支持（RISC-V）

---

## 8. 阶段完成交接文档要求

创建 `docs/Phase6-交接文档.md`，包含：

```markdown
# Phase 6 交接文档

## 1. 完成情况概览
- 性能优化：✅ 提升8%
- ARM64移植：✅ 编译通过
- 文档完善：✅ 100%覆盖
- CI/CD：✅ 三平台通过

## 2. 最终性能数据
| Queue | Platform | Threads | Throughput |
|-------|----------|---------|------------|
| SCQ   | x86-64   | 16      | 56 Mops/sec |
| LSCQ  | x86-64   | 16      | 47 Mops/sec |
| SCQ   | ARM64    | 16      | 42 Mops/sec |

## 3. 平台支持矩阵
| Platform | CAS2 | Status |
|----------|------|--------|
| x86-64   | ✅   | 完全支持 |
| ARM64    | ✅   | 完全支持 |
| PowerPC  | ❌   | Fallback |
| RISC-V   | ❌   | 未测试 |

## 4. 文档清单
- API参考：`docs/API.md`
- 性能报告：`docs/Phase6-性能报告.md`
- 使用教程：`examples/`
- Doxygen：`html/index.html`

## 5. 开源发布清单
- [ ] LICENSE文件
- [ ] CONTRIBUTING.md
- [ ] CODE_OF_CONDUCT.md
- [ ] GitHub Release v1.0.0

## 6. 持续维护计划（Phase 7）
- Bug修复流程
- 性能回归测试
- 社区PR审核标准
```

---

## 9. 常见问题（FAQ）

### Q1: 如何选择队列类型？
**A**:
- **SCQ**: 有界队列，最高性能
- **SCQP**: 有界队列，支持指针存储
- **LSCQ**: 无界队列，稍低性能但无容量限制
- **NCQ**: 仅用于性能对比，不推荐生产使用

### Q2: 无CAS2硬件怎么办？
**A**: 自动Fallback到`std::mutex`实现，性能约为CAS2的30-50%。

### Q3: 如何贡献代码？
**A**: 参见`CONTRIBUTING.md`，提交PR前确保CI通过。

### Q4: 性能未达到论文水平？
**A**: 检查：(1) CPU型号 (2) 编译优化选项 (3) 是否开启CAS2硬件支持

---

## 10. 参考资料

- `docs/Phase5-交接文档.md` - LSCQ实现
- `docs/01-技术实现思路.md` - 完整技术文档
- `1908.04511v1.pdf` - 原始论文
- [Intel VTune Profiler](https://software.intel.com/content/www/us/en/develop/tools/oneapi/components/vtune-profiler.html)
- [ARM CASP Instruction](https://developer.arm.com/documentation/ddi0595/2021-06/Base-Instructions/CASP--CASPA--CASPAL--CASPL--Compare-and-Swap-Pair-of-words-or-doublewords-in-memory-)

---

**StarterPrompt版本**: v1.0
**创建日期**: 2026-01-18
**适用范围**: LSCQ项目 Phase 6
**项目状态**: Production-Ready

---

**下列文档可能对你开展工作有帮助**

Phase5 交接文档: @docs/Phase5-交接文档.md

Phase4 完成总结: @docs/Phase4-完成报告.md
Phase5 完成总结: @docs/Phase5-完成总结.md