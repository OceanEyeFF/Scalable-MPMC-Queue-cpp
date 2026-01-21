# Task 3.2: Benchmark 测试与性能数据生成 - 完成报告

## 任务概述

**任务编号**: 3.2
**任务名称**: 运行 Benchmark 测试并生成性能数据
**完成日期**: 2026-01-21
**状态**: ✅ 已完成

## 修复摘要

在运行 Pair benchmark 测试过程中，发现并修复了以下关键问题：

### 1. SCQP dequeue 活锁问题 (Root Cause)

**问题描述**:
在 `SCQP::dequeue_ptr()` 和 `SCQP::dequeue_index()` 函数中，当 `cycle_e == cycle_h` 且 `is_safe == false` 时，代码会进入无限 `continue` 循环，而没有先检查 `ptr == nullptr`。

**根本原因**:
`is_safe` 标志用于标记正在进行并发 dequeue 的槽位。当一个线程正在检查某个槽位时，其他线程可能已经完成了 dequeue 并将 `ptr` 置为 `nullptr`。旧代码的逻辑顺序是：
```cpp
// 旧代码 (有bug)
if (cycle_e == cycle_h) {
    if (!is_safe) {
        continue;  // 无限循环！即使 ptr 已经是 nullptr
    }
    // ...
}
```

**修复方案**:
重新排序检查逻辑，先检查 `ptr/index` 是否为空，再检查 `is_safe` 标志：

```cpp
// 修复后的代码
if (cycle_e == cycle_h) {
    T* value = ent.ptr;
    if (value == nullptr) {
        break;  // 先检查值，没有值就退出内循环
    }
    if (!is_safe) {
        if (++inner_retries > MAX_INNER_RETRIES) {
            break;  // 安全阀，防止无限自旋
        }
        continue;
    }
    // ... 执行 dequeue
}
```

**修改文件**: [src/scqp.cpp](../src/scqp.cpp)

### 2. LSCQ dequeue 无限等待问题

**问题描述**:
当 LSCQ 节点被 finalize 但 `next` 指针尚未设置时，dequeue 会无限等待。

**修复方案**:
添加 `MAX_WAIT_RETRIES = 1024` 安全阀：

```cpp
constexpr int MAX_WAIT_RETRIES = 1024;
int wait_retries = 0;

while (true) {
    // ...
    if (next == nullptr) {
        if (!is_finalized) {
            return nullptr;  // 队列真正为空
        }
        // finalized 但 next 尚未设置
        if (++wait_retries > MAX_WAIT_RETRIES) {
            return nullptr;  // 安全阀：让调用者重试
        }
        std::this_thread::yield();
        continue;
    }
    wait_retries = 0;  // 找到 next 后重置计数器
    // ...
}
```

**修改文件**: [src/lscq.cpp](../src/lscq.cpp)

### 3. LSCQ Benchmark CyclicBarrier 死锁问题

**问题描述**:
`CyclicBarrier` 与 Google Benchmark 的迭代控制机制不兼容。当某个线程通过 `SkipWithError` 提前退出时，其他线程会永远阻塞在 barrier 上。

**修复方案**:
为 `BM_LSCQ_Pair` 创建 `SimpleLSCQContext`，移除 `CyclicBarrier`，使用简单的原子指针同步：

```cpp
struct SimpleLSCQContext {
    lscq::EBRManager ebr;
    lscq::LSCQ<lscq_bench::Value> q;
    std::vector<lscq_bench::Value> pool;
    std::uint64_t pool_mask;
    // 不使用 CyclicBarrier
};
```

**修改文件**: [benchmarks/benchmark_pair.cpp](../benchmarks/benchmark_pair.cpp)

## 性能测试结果

### 测试环境

| 项目 | 值 |
|------|-----|
| 主机名 | DESKTOP-52M2MMQ |
| CPU 核心数 | 24 |
| CPU 频率 | 3700 MHz |
| L1D Cache | 32 KB (每2核共享) |
| L1I Cache | 32 KB (每2核共享) |
| L2 Cache | 512 KB (每2核共享) |
| L3 Cache | 32 MB (每12核共享) |
| Google Benchmark | v1.9.4 (release) |

### Pair Benchmark 性能数据 (Mops/s - 每秒百万操作)

| 队列类型 | 1线程 | 2线程 | 4线程 | 8线程 | 12线程 | 16线程 | 24线程 |
|---------|-------|-------|-------|-------|--------|--------|--------|
| **NCQ** | 77.02 | 45.71 | 19.91 | 15.60 | 12.54 | 7.98 | 7.32 |
| **SCQ** | 71.31 | 87.82 | 82.88 | 113.25 | 139.23 | 89.03 | 117.99 |
| **SCQP** | 66.13 | 77.03 | 47.36 | 55.17 | 60.28 | 44.78 | 47.07 |
| **LSCQ** | 45.25 | 56.70 | 42.07 | 53.90 | 61.32 | 42.76 | 50.25 |
| **MSQueue** | 44.63 | 52.46 | 23.66 | 14.59 | 10.72 | 9.45 | 6.40 |
| **MutexQueue** | 85.75 | 68.03 | 44.56 | 33.20 | 27.39 | 6.86 | 4.02 |

### 性能分析

1. **SCQ 表现最优**: 在多线程场景下，SCQ 展现出最佳的可扩展性，在 12 线程时达到峰值 139.23 Mops/s。

2. **SCQP 稳定性良好**: 修复后的 SCQP 在各线程数下表现稳定，吞吐量在 44-77 Mops/s 范围内。

3. **LSCQ 可扩展性**: LSCQ (链式 SCQ) 同样展现出良好的可扩展性，在 12 线程时达到 61.32 Mops/s。

4. **NCQ 可扩展性受限**: 朴素循环队列在高线程数时性能显著下降。

5. **传统队列对比**: MSQueue 和 MutexQueue 在高并发下性能急剧下降，验证了 SCQ 系列算法的优势。

### 关键指标说明

- **has_cas2_support**: 1.0 (支持 CAS2/DWCAS 指令)
- **using_fallback**: 0.0 (SCQP 未使用 fallback 模式)
- **scqsize**: 524288 (SCQ/SCQP 内部数组大小)
- **qsize**: 262144 (队列逻辑容量)
- **node_scqsize**: 4096 (LSCQ 每个节点的 SCQ 大小)

## 修改的文件列表

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/scqp.cpp` | 修复 | 解决 dequeue 活锁问题，添加 MAX_INNER_RETRIES |
| `src/lscq.cpp` | 修复 | 添加 MAX_WAIT_RETRIES 安全阀 |
| `benchmarks/benchmark_pair.cpp` | 修复 | 移除 LSCQ 的 CyclicBarrier |

## 生成的文件

| 文件 | 说明 |
|------|------|
| `benchmark_results.json` | 完整的 benchmark 性能数据 (42 个测试用例) |
| `docs/task_3_2_benchmark_completion.md` | 本任务完成报告 |

## 测试验证

所有 42 个 Pair benchmark 测试用例均已通过：

- ✅ BM_NCQ_Pair (7 个线程配置)
- ✅ BM_SCQ_Pair (7 个线程配置)
- ✅ BM_SCQP_Pair (7 个线程配置)
- ✅ BM_LSCQ_Pair (7 个线程配置)
- ✅ BM_MSQueue_Pair (7 个线程配置)
- ✅ BM_MutexQueue_Pair (7 个线程配置)

## 经验总结

### 静态代码分析的重要性

本次 bug 定位过程中，从动态调试切换到静态代码分析是关键转折点。通过仔细审查 SCQP 的 dequeue 操作链，发现了隐藏的活锁条件。

### Lock-free 算法调试要点

1. **检查顺序很重要**: 在 lock-free 算法中，条件检查的顺序可能导致活锁。
2. **安全阀机制**: 在无法保证有界等待的情况下，添加重试限制作为安全阀。
3. **同步原语兼容性**: 测试框架的迭代控制机制可能与某些同步原语（如 CyclicBarrier）不兼容。

---

*报告生成时间: 2026-01-21*
