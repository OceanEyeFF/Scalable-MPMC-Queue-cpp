# Phase 8 StarterPrompt: 文档补全和使用说明

> **任务代号**: LSCQ-Phase8-Documentation
> **预计工期**: 1-2周
> **前置依赖**: Phase 7 完成（性能测试完成）
> **后续阶段**: 项目发布（v1.0.0）

---

## 1. 任务概述

### 1.1 核心目标
完善项目的**完整文档体系**，确保用户可以轻松上手和使用：
1. **README.md** - 项目主文档，包含快速开始、特性介绍
2. **使用教程** - 详细的使用指南和最佳实践
3. **API文档** - 完整的Doxygen API参考
4. **示例代码** - 多个实用示例工程
5. **故障排查** - 常见问题和解决方案
6. **贡献指南** - 开源贡献流程

### 1.2 技术挑战
- **文档质量**: 清晰、准确、易懂
- **代码示例**: 实用、可运行、有注释
- **用户体验**: 降低学习曲线
- **国际化**: 中英文双语文档（可选）

### 1.3 任务价值
- ✅ 提升项目可用性
- ✅ 吸引开源社区贡献
- ✅ 降低用户学习成本
- ✅ 为项目发布做准备

---

## 2. 任务边界

### 2.1 In Scope (本阶段必须完成)
- [x] **README.md** 完善
  - 项目简介和特性
  - 快速开始（Quick Start）
  - 安装指南
  - 性能数据展示
  - License和贡献链接
- [x] **使用教程** (`docs/Tutorial.md`)
  - 基础用法
  - 高级特性
  - 最佳实践
  - 性能调优
- [x] **API文档** (Doxygen)
  - 所有公开类和方法的注释
  - 生成HTML文档
  - 发布到GitHub Pages（可选）
- [x] **示例代码** (`examples/`)
  - 基础示例（simple_usage.cpp）
  - 任务队列示例（task_queue.cpp）
  - 生产者-消费者示例（producer_consumer.cpp）
  - 性能对比示例（benchmark_demo.cpp）
- [x] **故障排查指南** (`docs/Troubleshooting.md`)
- [x] **贡献指南** (`CONTRIBUTING.md`)
- [x] **行为准则** (`CODE_OF_CONDUCT.md`)
- [x] **License文件** (`LICENSE`)

### 2.2 Out of Scope (本阶段不涉及)
- ❌ 新功能开发
- ❌ Bug修复（除非影响示例代码）
- ❌ 性能优化（Phase 6-7已完成）

---

## 3. 前置条件与输入

### 3.1 前置依赖
- ✅ Phase 7已通过所有验收Gate
- ✅ `docs/Phase7-交接文档.md` 已创建
- ✅ 性能报告和图表已生成

### 3.2 必读文档（按顺序）
1. **`docs/Phase7-交接文档.md`** - 性能数据和对比分析
2. **`docs/Phase7-性能报告.md`** - 完整性能报告
3. **`docs/01-技术实现思路.md`** - 技术细节（供API文档参考）
4. **优秀开源项目的文档** - 例如Folly、Boost的README

### 3.3 关键代码复用（来自Phase 7）
- 性能数据表格（嵌入README）
- 性能对比图表（嵌入使用指南）
- Benchmark示例代码

### 3.4 环境要求
- Phase 7构建环境
- Doxygen 1.9+
- Markdown编辑器
- （可选）Python + matplotlib（生成更多图表）

---

## 4. 详细任务清单

### 4.1 README.md 完善 (Day 1-2)

#### 4.1.1 项目主文档结构

**创建/修改 `README.md`**:

```markdown
# LSCQ - Scalable Lock-Free MPMC Queue for C++

[![CI](https://github.com/yourname/lscq-cpp/workflows/CI/badge.svg)](https://github.com/yourname/lscq-cpp/actions)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://en.cppreference.com/w/cpp/17)

高性能、无锁、多生产者多消费者（MPMC）队列，基于学术论文实现：

> Ruslan Nikolaev. "A Scalable, Portable, and Memory-Efficient Lock-Free FIFO Queue"
> PPoPP 2019

## ✨ 特性

- **🚀 高性能**: 16核达到 52+ Mops/sec，比传统MSQueue快3倍+
- **🔒 无锁**: 真正的lock-free算法，无互斥锁阻塞
- **👥 MPMC**: 支持多生产者多消费者并发
- **📦 无界扩展**: LSCQ变体支持动态容量扩展
- **🧠 内存高效**: Epoch-Based Reclamation，无GC依赖
- **🌍 跨平台**: 支持x86-64、ARM64、PowerPC

## 📊 性能数据

### 吞吐量对比（Pair场景）

![Performance](docs/Phase7-性能对比图.png)

| Queue Type | 1 Thread | 8 Threads | 16 Threads | vs MSQueue |
|------------|----------|-----------|------------|------------|
| **SCQ**    | 8.2 Mops/sec | 42.5 Mops/sec | **52.1 Mops/sec** | **+189%** |
| **LSCQ**   | 7.5 Mops/sec | 38.2 Mops/sec | 45.8 Mops/sec | +154% |
| MSQueue    | 6.1 Mops/sec | 15.8 Mops/sec | 18.0 Mops/sec | - |
| Mutex Queue| 5.2 Mops/sec | 8.3 Mops/sec  | 9.1 Mops/sec  | -50% |

*测试环境: Intel Xeon, Clang++ 14, -O3 -march=native*

详细性能报告: [Phase7-性能报告.md](docs/Phase7-性能报告.md)

## 🚀 快速开始

### 安装

**要求**:
- C++17 编译器（Clang++ 14+ 或 GCC 10+）
- CMake 3.20+
- Ninja（推荐）

**克隆并编译**:
```bash
git clone https://github.com/yourname/lscq-cpp.git
cd lscq-cpp

# 配置Release构建
cmake --preset release-performance

# 编译
cmake --build build/release-perf

# 运行测试
ctest --test-dir build/release-perf
```

### 基础用法

**有界队列（SCQ）**:
```cpp
#include "lscq/scq.hpp"

lscq::SCQ<uint64_t> queue;

// 生产者
queue.enqueue(42);

// 消费者
uint64_t value = queue.dequeue();
if (value != lscq::SCQ<uint64_t>::EMPTY_VALUE) {
    std::cout << "Got: " << value << "\n";
}
```

**无界队列（LSCQ）**:
```cpp
#include "lscq/lscq.hpp"

lscq::LSCQ<int> queue;

// 生产者
int value = 42;
queue.enqueue(&value);

// 消费者
int* ptr = queue.dequeue();
if (ptr != nullptr) {
    std::cout << "Got: " << *ptr << "\n";
}
```

更多示例: [examples/](examples/)

## 📖 文档

- **[使用教程](docs/Tutorial.md)** - 详细使用指南
- **[API文档](https://yourname.github.io/lscq-cpp/)** - Doxygen生成的API参考
- **[性能报告](docs/Phase7-性能报告.md)** - 完整性能测试数据
- **[技术实现](docs/01-技术实现思路.md)** - 算法原理和设计
- **[故障排查](docs/Troubleshooting.md)** - 常见问题解决

## 🧩 队列类型选择

| 队列类型 | 容量 | 存储 | 性能 | 适用场景 |
|---------|------|------|------|---------|
| **NCQ** | 有界 | 索引 | Baseline | 性能对比基准 |
| **SCQ** | 有界 | 索引 | ⭐⭐⭐⭐⭐ | 高性能、已知容量上限 |
| **SCQP** | 有界 | 指针 | ⭐⭐⭐⭐ | 对象队列、已知容量上限 |
| **LSCQ** | 无界 | 指针 | ⭐⭐⭐⭐ | 对象队列、容量动态扩展 |

**推荐**:
- **高性能固定容量**: 使用 `SCQ`
- **对象队列无界扩展**: 使用 `LSCQ`
- **学习和对比**: 从 `NCQ` 开始

## 🏗️ 项目结构

```
lscq-cpp/
├── include/lscq/           # 头文件
│   ├── ncq.hpp            # NCQ (Baseline)
│   ├── scq.hpp            # SCQ (高性能有界)
│   ├── scqp.hpp           # SCQP (指针有界)
│   ├── lscq.hpp           # LSCQ (无界)
│   ├── cas2.hpp           # CAS2抽象层
│   └── detail/
│       └── epoch.hpp      # EBR内存回收
├── src/                   # 源文件
├── tests/                 # 单元测试
├── benchmarks/            # 性能测试
├── examples/              # 示例代码
└── docs/                  # 文档
```

## 🧪 测试与Benchmark

**运行测试**:
```bash
# 单元测试
ctest --test-dir build/release-perf

# 带Sanitizers的测试
cmake --preset windows-clang-debug -DLSCQ_ENABLE_SANITIZERS=ON
cmake --build build/debug
./build/debug/tests/lscq_tests
```

**运行Benchmark**:
```bash
./build/release-perf/benchmarks/comprehensive_benchmark \
    --benchmark_min_time=5s
```

## 🤝 贡献

欢迎贡献！请阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 了解贡献流程。

**贡献方式**:
- 🐛 报告Bug: [GitHub Issues](https://github.com/yourname/lscq-cpp/issues)
- 💡 功能建议: [GitHub Discussions](https://github.com/yourname/lscq-cpp/discussions)
- 🔧 提交PR: Fork → 开发 → 测试 → PR

## 📄 License

本项目采用 [MIT License](LICENSE)。

## 🙏 致谢

- 基于Ruslan Nikolaev的论文实现
- 感谢所有贡献者

## 📧 联系方式

- **作者**: [Your Name]
- **Email**: your.email@example.com
- **GitHub**: [@yourname](https://github.com/yourname)

---

⭐ 如果这个项目对你有帮助，欢迎Star支持！
```

**验收点**: README清晰、完整、有吸引力

---

### 4.2 使用教程 (Day 3-4)

#### 4.2.1 创建 `docs/Tutorial.md`

```markdown
# LSCQ 使用教程

本教程将引导你从零开始使用LSCQ队列库。

## 目录

1. [基础概念](#基础概念)
2. [选择合适的队列](#选择合适的队列)
3. [基础用法](#基础用法)
4. [高级特性](#高级特性)
5. [最佳实践](#最佳实践)
6. [性能调优](#性能调优)

---

## 基础概念

### 什么是MPMC队列？

MPMC（Multi-Producer Multi-Consumer）队列允许：
- **多个生产者线程**同时入队元素
- **多个消费者线程**同时出队元素
- **无锁设计**，无需互斥锁

### LSCQ的特点

- **Lock-free**: 使用CAS原子操作，无阻塞
- **Scalable**: 线程数增加时性能线性增长
- **Memory-efficient**: 使用EBR回收内存

---

## 选择合适的队列

### 决策树

```
需要无界队列？
├─ 是 → LSCQ
└─ 否 → 需要存储指针？
    ├─ 是 → SCQP
    └─ 否 → SCQ
```

### 详细对比

| 需求 | 推荐队列 | 原因 |
|------|---------|------|
| 任务队列（容量未知） | LSCQ | 无界扩展 |
| 环形缓冲区（固定大小） | SCQ | 最高性能 |
| 对象池 | SCQP | 直接存储指针 |
| 索引队列 | SCQ | 内存效率高 |

---

## 基础用法

### 示例1: 简单任务队列

```cpp
#include "lscq/lscq.hpp"
#include <thread>
#include <iostream>

struct Task {
    int id;
    std::string name;
};

int main() {
    lscq::LSCQ<Task> task_queue;

    // 生产者线程
    std::thread producer([&]() {
        for (int i = 0; i < 100; ++i) {
            Task* task = new Task{i, "Task " + std::to_string(i)};
            task_queue.enqueue(task);
            std::cout << "Produced: " << task->name << "\n";
        }
    });

    // 消费者线程
    std::thread consumer([&]() {
        for (int i = 0; i < 100; ++i) {
            Task* task = task_queue.dequeue();
            while (task == nullptr) {
                task = task_queue.dequeue();
                std::this_thread::yield();  // 队列空，让出CPU
            }
            std::cout << "Consumed: " << task->name << "\n";
            delete task;
        }
    });

    producer.join();
    consumer.join();

    return 0;
}
```

### 示例2: 多生产者多消费者

```cpp
#include "lscq/scq.hpp"
#include <thread>
#include <vector>

int main() {
    lscq::SCQ<uint64_t> queue;

    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 1000;

    std::vector<std::thread> threads;

    // 启动生产者
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < ITEMS_PER_PRODUCER; ++j) {
                queue.enqueue(i * ITEMS_PER_PRODUCER + j);
            }
        });
    }

    // 启动消费者
    std::atomic<int> total_consumed{0};
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back([&]() {
            while (total_consumed.load() < NUM_PRODUCERS * ITEMS_PER_PRODUCER) {
                uint64_t value = queue.dequeue();
                if (value != lscq::SCQ<uint64_t>::EMPTY_VALUE) {
                    total_consumed.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    return 0;
}
```

---

## 高级特性

### 1. CAS2硬件检测

```cpp
#include "lscq/cas2.hpp"

if (lscq::has_cas2_support()) {
    std::cout << "CAS2 hardware support available\n";
} else {
    std::cout << "Using fallback implementation\n";
}
```

### 2. 自定义配置

```cpp
// 修改默认队列大小
namespace lscq::config {
    constexpr size_t DEFAULT_SCQSIZE = 1 << 20;  // 1M elements
}
```

### 3. 性能监控

```cpp
lscq::SCQ<uint64_t> queue;

// 获取队列状态（非精确）
bool is_empty = queue.is_empty();
```

---

## 最佳实践

### ✅ 推荐做法

1. **预分配对象**（LSCQ/SCQP）
   ```cpp
   // 好：预分配
   std::vector<Task> task_pool(1000);
   for (auto& task : task_pool) {
       queue.enqueue(&task);
   }
   ```

2. **使用对象池**避免频繁new/delete
   ```cpp
   // 使用对象池
   ObjectPool<Task> pool;
   queue.enqueue(pool.allocate());
   ```

3. **队列空时让出CPU**
   ```cpp
   while (true) {
       auto* item = queue.dequeue();
       if (item == nullptr) {
           std::this_thread::yield();  // 避免空转
           continue;
       }
       process(item);
   }
   ```

### ❌ 避免做法

1. **不要在队列操作中持有锁**
   ```cpp
   // 错误：死锁风险
   std::lock_guard<std::mutex> lock(mutex_);
   queue.enqueue(item);  // 可能长时间阻塞
   ```

2. **不要假设FIFO严格顺序**
   ```cpp
   // 警告：并发环境下FIFO是概率性的
   queue.enqueue(1);
   queue.enqueue(2);
   // dequeue可能先得到2（如果有并发操作）
   ```

---

## 性能调优

### 1. 编译器优化

```cmake
# CMakeLists.txt
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto")
```

### 2. CPU亲和性（可选）

```cpp
#include <pthread.h>

// 绑定线程到特定CPU核心
void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

std::thread producer([&]() {
    pin_to_core(0);  // 绑定到核心0
    // 生产逻辑...
});
```

### 3. 预填充队列（减少空队列）

```cpp
lscq::SCQ<uint64_t> queue;

// 预填充，减少空队列率
for (int i = 0; i < 1000; ++i) {
    queue.enqueue(i);
}
```

### 4. 批量操作（减少原子操作）

```cpp
// 好：批量入队
std::vector<int> batch(100);
for (auto& item : batch) {
    queue.enqueue(item);
}

// 而不是：
for (int i = 0; i < 100; ++i) {
    queue.enqueue(i);  // 每次都有原子操作开销
}
```

---

## 故障排查

参见 [Troubleshooting.md](Troubleshooting.md)

---

## 下一步

- 阅读 [API文档](https://yourname.github.io/lscq-cpp/)
- 查看 [性能报告](Phase7-性能报告.md)
- 尝试 [示例代码](../examples/)
```

**验收点**: 教程清晰、示例可运行、覆盖常见场景

---

### 4.3 API文档补全 (Day 5-6)

#### 4.3.1 补充Doxygen注释

**为所有公开类和方法添加Doxygen注释**:

```cpp
/// @file scq.hpp
/// @brief Scalable Circular Queue - 高性能有界MPMC队列
/// @author LSCQ Team
/// @version 1.0.0
/// @date 2026-01-20

namespace lscq {

/// @class SCQ
/// @brief 有界lock-free MPMC队列，基于Ring Buffer + Threshold机制
///
/// SCQ（Scalable Circular Queue）是一个高性能的有界队列，使用：
/// - **Ring Buffer**: 循环数组存储
/// - **Threshold机制**: 避免livelock（3n-1阈值）
/// - **Cache Remap**: 减少false sharing
/// - **Catchup优化**: 处理dequeue-heavy场景
///
/// @tparam T 元素类型（通常为uint64_t索引）
///
/// @par 性能特点
/// - 16核达到 52+ Mops/sec
/// - 比MSQueue快3倍+
/// - 单线程 8+ Mops/sec
///
/// @par 使用示例
/// @code
/// lscq::SCQ<uint64_t> queue;
///
/// // 生产者线程
/// queue.enqueue(42);
///
/// // 消费者线程
/// uint64_t value = queue.dequeue();
/// if (value != lscq::SCQ<uint64_t>::EMPTY_VALUE) {
///     std::cout << "Got: " << value << "\n";
/// }
/// @endcode
///
/// @note 要求CAS2硬件支持（x86-64 cmpxchg16b或ARM64 CASP）
///
/// @see https://github.com/yourname/lscq-cpp
/// @see LSCQ 无界队列版本
template<typename T = uint64_t>
class SCQ {
public:
    /// @brief 队列容量（编译时常量）
    static constexpr size_t SCQSIZE = config::DEFAULT_SCQSIZE;

    /// @brief 空队列标记值
    static constexpr T EMPTY_VALUE = static_cast<T>(-1);

    /// @brief Threshold阈值（3n-1）
    static constexpr uint64_t THRESHOLD = 3 * SCQSIZE - 1;

    /// @brief 构造函数 - 初始化ring buffer和原子变量
    /// @throws std::bad_alloc 如果内存分配失败
    ///
    /// @par 时间复杂度
    /// O(n)，n为SCQSIZE
    SCQ();

    /// @brief 析构函数 - 释放ring buffer内存
    ~SCQ();

    /// @brief 禁止拷贝构造（原子变量不可拷贝）
    SCQ(const SCQ&) = delete;

    /// @brief 禁止拷贝赋值
    SCQ& operator=(const SCQ&) = delete;

    /// @brief 入队操作（阻塞直到成功）
    /// @param index 要插入的索引值
    ///
    /// @par 线程安全
    /// 完全线程安全，支持多生产者并发
    ///
    /// @par 性能
    /// 单线程: ~8 Mops/sec
    /// 16线程: ~52 Mops/sec
    ///
    /// @warning 队列满时会阻塞等待，考虑使用LSCQ无界队列
    void enqueue(T index);

    /// @brief 出队操作
    /// @return 元素值，队列空时返回EMPTY_VALUE
    ///
    /// @par 线程安全
    /// 完全线程安全，支持多消费者并发
    T dequeue();

    /// @brief 检查队列是否为空（非精确）
    /// @return true表示队列可能为空
    ///
    /// @warning 并发环境下此方法仅供参考，不保证精确性
    bool is_empty() const;

private:
    // ... 私有成员和方法的注释
};

}  // namespace lscq
```

#### 4.3.2 生成Doxygen文档

**运行Doxygen**:
```bash
doxygen Doxyfile
```

**发布到GitHub Pages**（可选）:
```bash
# 创建gh-pages分支
git checkout --orphan gh-pages
cp -r html/* .
git add .
git commit -m "Generate API documentation"
git push origin gh-pages
```

**验收点**: Doxygen文档覆盖率100%，无警告

---

### 4.4 示例代码 (Day 7-8)

#### 4.4.1 创建多个示例工程

**`examples/CMakeLists.txt`**:
```cmake
# 示例1: 简单用法
add_executable(simple_usage simple_usage.cpp)
target_link_libraries(simple_usage lscq_impl)

# 示例2: 任务队列
add_executable(task_queue task_queue.cpp)
target_link_libraries(task_queue lscq_impl)

# 示例3: 生产者-消费者
add_executable(producer_consumer producer_consumer.cpp)
target_link_libraries(producer_consumer lscq_impl)

# 示例4: 性能对比Demo
add_executable(benchmark_demo benchmark_demo.cpp)
target_link_libraries(benchmark_demo lscq_impl)
```

**`examples/task_queue.cpp`** (完整示例):
```cpp
/// @file task_queue.cpp
/// @brief 使用LSCQ实现任务队列的完整示例

#include "lscq/lscq.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>

/// @brief 任务结构
struct Task {
    int id;
    std::string description;
    std::chrono::milliseconds duration;

    Task(int i, const std::string& desc, int ms)
        : id(i), description(desc), duration(ms) {}
};

/// @brief 任务队列管理器
class TaskQueue {
public:
    TaskQueue(int num_workers) : stop_(false) {
        // 启动工作线程
        for (int i = 0; i < num_workers; ++i) {
            workers_.emplace_back(&TaskQueue::worker_loop, this, i);
        }
    }

    ~TaskQueue() {
        stop();
    }

    /// @brief 提交任务
    void submit(Task* task) {
        queue_.enqueue(task);
        tasks_submitted_.fetch_add(1);
    }

    /// @brief 停止队列
    void stop() {
        stop_.store(true);
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    /// @brief 获取统计信息
    void print_stats() const {
        std::cout << "Tasks submitted: " << tasks_submitted_.load() << "\n";
        std::cout << "Tasks completed: " << tasks_completed_.load() << "\n";
    }

private:
    void worker_loop(int worker_id) {
        std::cout << "Worker " << worker_id << " started\n";

        while (!stop_.load()) {
            Task* task = queue_.dequeue();

            if (task == nullptr) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // 执行任务
            std::cout << "[Worker " << worker_id << "] Processing task "
                      << task->id << ": " << task->description << "\n";

            std::this_thread::sleep_for(task->duration);

            tasks_completed_.fetch_add(1);
            delete task;
        }

        std::cout << "Worker " << worker_id << " stopped\n";
    }

    lscq::LSCQ<Task> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_;
    std::atomic<int> tasks_submitted_{0};
    std::atomic<int> tasks_completed_{0};
};

int main() {
    std::cout << "=== Task Queue Demo ===\n\n";

    // 创建任务队列（4个工作线程）
    TaskQueue task_queue(4);

    // 提交100个任务
    for (int i = 0; i < 100; ++i) {
        Task* task = new Task(
            i,
            "Task " + std::to_string(i),
            std::chrono::milliseconds(10 + (i % 50))
        );
        task_queue.submit(task);
    }

    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // 停止队列
    task_queue.stop();

    // 打印统计
    task_queue.print_stats();

    return 0;
}
```

**验收点**: 所有示例可编译运行，有详细注释

---

### 4.5 故障排查指南 (Day 9)

#### 4.5.1 创建 `docs/Troubleshooting.md`

```markdown
# LSCQ 故障排查指南

本指南帮助你解决使用LSCQ时遇到的常见问题。

---

## 编译问题

### 问题1: "CAS2 not supported" 编译错误

**症状**:
```
error: static_assert failed "SCQP requires CAS2 hardware support"
```

**原因**: 你的CPU或编译器不支持128位CAS2指令

**解决方案**:
1. 使用SCQ（索引队列）代替SCQP
2. 或者使用Fallback模式（性能较低）

---

### 问题2: 链接错误 "undefined reference to lscq::SCQ"

**症状**:
```
undefined reference to `lscq::SCQ<unsigned long>::enqueue(unsigned long)'
```

**原因**: 缺少模板实例化或未链接库

**解决方案**:
```cmake
# CMakeLists.txt中添加
target_link_libraries(your_target lscq_impl)
```

---

## 运行时问题

### 问题3: Segmentation Fault (Segfault)

**可能原因**:
1. Entry未对齐到16字节
2. 访问已释放的对象（LSCQ/SCQP）
3. EBR回收了仍在使用的节点

**调试步骤**:
```bash
# 使用AddressSanitizer
cmake --preset windows-clang-debug -DLSCQ_ENABLE_SANITIZERS=ON
cmake --build build/debug
./build/debug/tests/lscq_tests
```

---

### 问题4: Data Race (ThreadSanitizer报错)

**症状**:
```
WARNING: ThreadSanitizer: data race
```

**原因**: 可能错误使用了队列API

**检查**:
- 是否在队列操作外有共享变量访问？
- 是否正确使用了atomic操作？

---

## 性能问题

### 问题5: 性能远低于预期

**症状**: Benchmark显示 < 10 Mops/sec

**可能原因**:
1. 未使用Release编译
2. 未启用优化选项
3. CPU不支持CAS2硬件
4. 有其他进程干扰

**解决方案**:
```bash
# 1. 检查编译配置
cmake --preset release-performance

# 2. 检查CAS2支持
./build/release-perf/tests/test_cas2

# 3. 关闭其他进程，绑定CPU核心
```

---

### 问题6: 多线程性能不佳

**症状**: 16线程性能低于8线程

**可能原因**:
- False sharing（伪共享）
- NUMA效应
- 超线程干扰

**解决方案**:
```bash
# 禁用超线程测试
# 使用CPU亲和性绑定
taskset -c 0-15 ./benchmark
```

---

## 内存问题

### 问题7: 内存泄漏（Valgrind报错）

**症状**:
```
definitely lost: 1,024 bytes in 1 blocks
```

**原因**: LSCQ/SCQP中的对象未正确释放

**检查**:
```cpp
// 确保所有enqueue的对象最终都被dequeue并释放
Task* task = new Task();
queue.enqueue(task);

// 稍后...
Task* result = queue.dequeue();
delete result;  // 必须释放！
```

---

### 问题8: EBR延迟回收导致内存占用高

**症状**: 内存占用持续增长

**原因**: EBR机制延迟回收节点

**解决方案**: 正常现象，内存会在epoch推进后回收。可调整epoch推进频率。

---

## 算法问题

### 问题9: 队列满时enqueue阻塞

**症状**: 生产者线程卡住

**原因**: SCQ/SCQP是有界队列

**解决方案**:
```cpp
// 使用LSCQ无界队列
lscq::LSCQ<Task> queue;  // 而非SCQP
```

---

### 问题10: FIFO顺序不严格

**症状**: dequeue顺序与enqueue不一致

**原因**: 并发环境下FIFO是概率性的

**解决方案**: 这是正常的。如果需要严格顺序，使用单生产者单消费者队列。

---

## 获取帮助

如果以上方法无法解决问题：

1. **检查文档**: [Tutorial.md](Tutorial.md)
2. **搜索Issues**: [GitHub Issues](https://github.com/yourname/lscq-cpp/issues)
3. **提交Bug报告**: 包含最小复现示例和环境信息

---

## 调试技巧

### 启用详细日志

```cpp
#define LSCQ_DEBUG 1
#include "lscq/scq.hpp"
```

### 打印队列状态

```cpp
void debug_queue(lscq::SCQ<uint64_t>& queue) {
    std::cout << "Queue empty: " << queue.is_empty() << "\n";
}
```

### 使用GDB调试

```bash
gdb ./your_program
(gdb) break lscq::SCQ::enqueue
(gdb) run
(gdb) print *this
```
```

**验收点**: 覆盖常见问题，解决方案清晰

---

### 4.6 贡献指南和License (Day 10)

#### 4.6.1 创建 `CONTRIBUTING.md`

```markdown
# 贡献指南

感谢你对LSCQ项目的关注！我们欢迎各种形式的贡献。

## 贡献方式

### 🐛 报告Bug

1. 在[GitHub Issues](https://github.com/yourname/lscq-cpp/issues)中搜索是否已存在
2. 如果没有，创建新Issue
3. 使用Bug报告模板，包含：
   - 问题描述
   - 复现步骤
   - 预期行为 vs 实际行为
   - 环境信息（OS、编译器、CPU）
   - 最小复现示例（MRE）

### 💡 功能建议

1. 在[GitHub Discussions](https://github.com/yourname/lscq-cpp/discussions)中讨论
2. 描述问题场景和期望的解决方案
3. 获得维护者反馈后，创建Issue

### 🔧 提交代码

#### 开发流程

1. **Fork仓库**
   ```bash
   # 在GitHub上Fork项目
   git clone https://github.com/your-username/lscq-cpp.git
   cd lscq-cpp
   ```

2. **创建分支**
   ```bash
   git checkout -b feature/your-feature-name
   # 或
   git checkout -b fix/issue-number-description
   ```

3. **开发和测试**
   ```bash
   # 编译
   cmake --preset windows-clang-debug
   cmake --build build/debug

   # 运行测试
   ctest --test-dir build/debug

   # 运行Sanitizers
   cmake --preset windows-clang-debug -DLSCQ_ENABLE_SANITIZERS=ON
   cmake --build build/debug
   ./build/debug/tests/lscq_tests
   ```

4. **代码风格**
   ```bash
   # 格式化代码
   clang-format -i include/**/*.hpp src/**/*.cpp tests/**/*.cpp

   # 静态检查
   clang-tidy include/**/*.hpp
   ```

5. **提交**
   ```bash
   git add .
   git commit -m "feat: add new feature"
   # 或
   git commit -m "fix: resolve issue #123"
   ```

   **Commit Message规范**（Conventional Commits）:
   - `feat:` 新功能
   - `fix:` Bug修复
   - `docs:` 文档更新
   - `test:` 测试相关
   - `perf:` 性能优化
   - `refactor:` 代码重构

6. **推送并创建PR**
   ```bash
   git push origin feature/your-feature-name
   ```
   在GitHub上创建Pull Request

#### PR检查清单

- [ ] 所有测试通过
- [ ] 代码覆盖率无降低
- [ ] 无Sanitizer错误
- [ ] 代码符合clang-format
- [ ] 添加了必要的测试
- [ ] 更新了文档（如果需要）
- [ ] Commit message符合规范

### 📖 文档贡献

- 修正错别字、语法错误
- 补充示例代码
- 翻译文档（中英互译）

---

## 代码规范

### C++编码风格

- **命名**:
  - 类名: `PascalCase` (例如 `SCQueue`)
  - 函数/变量: `snake_case` (例如 `enqueue`, `head_`)
  - 常量: `UPPER_CASE` (例如 `SCQSIZE`)
  - 私有成员后缀下划线 (例如 `head_`)

- **格式**: 使用 `.clang-format` 配置

- **注释**: 使用Doxygen风格

### 测试要求

- 新功能必须包含单元测试
- 测试覆盖率 > 90%
- 包含正确性测试和并发测试

### 性能要求

- 不得显著降低性能（< 5%）
- 性能优化需提供Benchmark数据

---

## 审核流程

1. **自动检查**: CI自动运行测试
2. **代码审查**: 至少1位维护者审查
3. **讨论修改**: 根据反馈修改代码
4. **合并**: 通过后合并到main分支

---

## 社区准则

请遵守 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)

---

## 许可协议

贡献的代码将采用项目的 [MIT License](LICENSE)
```

#### 4.6.2 创建 `LICENSE`

```
MIT License

Copyright (c) 2026 [Your Name]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

#### 4.6.3 创建 `CODE_OF_CONDUCT.md`

```markdown
# 行为准则

## 我们的承诺

为了营造开放和友好的环境，我们承诺让参与项目和社区的每个人都拥有无骚扰的体验。

## 我们的标准

积极行为示例：
- 使用友好和包容的语言
- 尊重不同的观点和经验
- 优雅地接受建设性批评
- 关注对社区最有利的事情
- 对其他社区成员表示同理心

不可接受的行为示例：
- 使用性化的语言或图像
- 骚扰性评论
- 发布他人的私人信息
- 其他不道德或不专业的行为

## 我们的责任

项目维护者有责任澄清可接受行为的标准，并对任何不可接受的行为采取适当和公平的纠正措施。

## 适用范围

本行为准则适用于所有项目空间，包括但不限于：
- 代码仓库
- Issue和PR讨论
- 社区论坛

## 执行

可向 [your.email@example.com] 报告滥用、骚扰或其他不可接受的行为。

## 归属

本行为准则改编自 [Contributor Covenant](https://www.contributor-covenant.org/), version 2.1。
```

**验收点**: 所有治理文档完整

---

## 5. 交付物清单

### 5.1 文档文件
- [ ] `README.md` - 项目主文档
- [ ] `docs/Tutorial.md` - 使用教程
- [ ] `docs/Troubleshooting.md` - 故障排查
- [ ] `CONTRIBUTING.md` - 贡献指南
- [ ] `CODE_OF_CONDUCT.md` - 行为准则
- [ ] `LICENSE` - MIT许可证

### 5.2 示例代码
- [ ] `examples/simple_usage.cpp`
- [ ] `examples/task_queue.cpp`
- [ ] `examples/producer_consumer.cpp`
- [ ] `examples/benchmark_demo.cpp`

### 5.3 API文档
- [ ] Doxygen生成的HTML文档
- [ ] (可选) GitHub Pages发布

### 5.4 交接文档
- [ ] `docs/Phase8-交接文档.md` - **必须创建**

---

## 6. 验收标准 (Gate Conditions)

### 6.1 文档质量验收
- ✅ **G1.1**: README清晰、完整、有吸引力
- ✅ **G1.2**: Tutorial覆盖常见场景，示例可运行
- ✅ **G1.3**: 故障排查指南实用
- ✅ **G1.4**: 无语法和拼写错误

### 6.2 示例代码验收
- ✅ **G2.1**: 所有示例可编译运行
- ✅ **G2.2**: 示例有详细注释
- ✅ **G2.3**: 至少4个示例覆盖不同场景

### 6.3 API文档验收
- ✅ **G3.1**: Doxygen覆盖率100%
- ✅ **G3.2**: 所有公开API有注释
- ✅ **G3.3**: 生成的文档可访问

### 6.4 开源治理验收
- ✅ **G4.1**: LICENSE文件存在
- ✅ **G4.2**: CONTRIBUTING.md完整
- ✅ **G4.3**: CODE_OF_CONDUCT.md存在

---

## 7. 项目发布准备

### 7.1 发布检查清单

- [ ] 所有Phase 1-8完成
- [ ] 所有测试通过
- [ ] 性能达标
- [ ] 文档完整
- [ ] License正确

### 7.2 GitHub Release v1.0.0

**创建Release**:
```bash
# 打标签
git tag -a v1.0.0 -m "Release version 1.0.0"
git push origin v1.0.0

# 在GitHub上创建Release
# 标题: LSCQ v1.0.0 - Initial Release
# 描述: 包含特性列表、性能数据、安装指南
```

**Release Notes示例**:
```markdown
# LSCQ v1.0.0 - Initial Release

🎉 首次正式发布！

## ✨ 特性

- ✅ 4种队列类型（NCQ/SCQ/SCQP/LSCQ）
- ✅ 16核达到52+ Mops/sec
- ✅ 支持x86-64、ARM64、PowerPC
- ✅ 完整文档和示例

## 📊 性能

- SCQ @ 16 cores: 52.1 Mops/sec (vs MSQueue +189%)
- LSCQ @ 16 cores: 45.8 Mops/sec

## 📖 文档

- [快速开始](README.md)
- [使用教程](docs/Tutorial.md)
- [API文档](https://yourname.github.io/lscq-cpp/)

## 🙏 致谢

感谢所有贡献者！
```

---

## 8. 阶段完成交接文档要求

创建 `docs/Phase8-交接文档.md`，包含：

```markdown
# Phase 8 交接文档

## 1. 完成情况概览
- README: ✅ 完整、清晰
- Tutorial: ✅ 覆盖常见场景
- API文档: ✅ 100%覆盖
- 示例代码: ✅ 4个示例
- 开源治理: ✅ 完整

## 2. 文档清单
| 文档类型 | 文件名 | 行数 | 状态 |
|---------|--------|------|------|
| 主文档 | README.md | 250 | ✅ |
| 教程 | docs/Tutorial.md | 500 | ✅ |
| 故障排查 | docs/Troubleshooting.md | 300 | ✅ |
| 贡献指南 | CONTRIBUTING.md | 200 | ✅ |
| 行为准则 | CODE_OF_CONDUCT.md | 50 | ✅ |

## 3. 示例代码清单
- simple_usage.cpp: 基础用法
- task_queue.cpp: 任务队列完整示例
- producer_consumer.cpp: 多生产者多消费者
- benchmark_demo.cpp: 性能对比演示

## 4. API文档
- Doxygen覆盖率: 100%
- GitHub Pages: https://yourname.github.io/lscq-cpp/

## 5. 项目发布准备
- ✅ License: MIT
- ✅ 版本: v1.0.0
- ✅ Release Notes已准备
- ✅ GitHub Release可发布

## 6. 后续维护计划
- Bug修复流程
- 功能请求评审
- 社区PR审核
- 文档持续更新
```

---

## 9. 常见问题（FAQ）

### Q1: 文档需要中英文双语吗？
**A**: 可选。建议至少有英文版README，方便国际用户。

### Q2: API文档需要发布到哪里？
**A**: 可选GitHub Pages，或者包含在Release附件中。

### Q3: 示例代码需要多少个？
**A**: 至少4个，覆盖基础用法、任务队列、多线程、性能对比。

### Q4: 如何吸引开源贡献者？
**A**: 清晰的文档、友好的社区、及时的反馈、Good First Issue标签。

---

## 10. 参考资料

- `docs/Phase7-交接文档.md` - 性能数据
- [Doxygen Manual](https://www.doxygen.nl/manual/)
- [GitHub README Best Practices](https://github.com/matiassingers/awesome-readme)
- [Contributor Covenant](https://www.contributor-covenant.org/)

---

**StarterPrompt版本**: v1.0
**创建日期**: 2026-01-20
**适用范围**: LSCQ项目 Phase 8

---

**下列文档可能对你开展工作有帮助**

Phase 7 交接文档: @docs/Phase7-交接文档.md
Phase 7 性能报告: @docs/Phase7-性能报告.md
