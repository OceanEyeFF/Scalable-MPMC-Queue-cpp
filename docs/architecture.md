# 架构概览

本仓库实现了一组 MPMC 队列家族，整体分为三层：

1. **原子原语层**：CAS2（128-bit compare-exchange）与跨平台回退实现
2. **有界队列层**：`NCQ`、`SCQ`、`SCQP`
3. **无界队列层**：`LSCQ`（链式拼接多个 `SCQP` 节点）+ `EBR` 内存回收

## 架构图（逻辑）

```mermaid
flowchart TB
  subgraph API[Public API]
    NCQ[lscq::NCQ<T>\n(bounded)]
    SCQ[lscq::SCQ<T>\n(bounded, scalable)]
    SCQP[lscq::SCQP<T>\n(bounded, pointers)]
    LSCQ[lscq::LSCQ<T>\n(unbounded, pointers)]
  end

  subgraph Core[Core Components]
    CAS2[lscq::cas2 / has_cas2_support\n(CAS2 or mutex fallback)]
    AO[detail::atomic_or_u64\n(consume mark)]
    EBR[lscq::EBRManager\n+ EpochGuard]
  end

  SCQ --> CAS2
  SCQ --> AO
  SCQP --> CAS2
  LSCQ --> SCQP
  LSCQ --> EBR
```

## 核心组件说明

### 1) CAS2（128-bit CAS）与回退

- 代码位置：`include/lscq/cas2.hpp`、`src/cas2.cpp`
- 对外 API：
  - `bool lscq::has_cas2_support()`
  - `bool lscq::cas2(Entry* ptr, Entry& expected, const Entry& desired)`
- 行为：
  - 在 `LSCQ_ENABLE_CAS2=ON` 且平台/CPU 支持时使用原生 128-bit CAS（x86_64 的 CMPXCHG16B）
  - 否则自动回退到互斥实现（保证正确性，性能较低）

### 2) SCQ（Scalable Circular Queue）

- 代码位置：`include/lscq/scq.hpp`、`src/scq.cpp`
- 特点：
  - ring 大小 `SCQSIZE = 2n`，**有效容量 `QSIZE = n`**
  - entry 中打包 `cycle + isSafe`，并使用 `⊥`（bottom 标记）减少竞争
  - `dequeue()` 空时返回 `SCQ<T>::kEmpty`
- 约束：
  - 该算法存储的是“索引值”，入队值必须 `< (SCQSIZE - 1)` 且不等于 `kEmpty`

### 3) SCQP（SCQ Pointer）

- 代码位置：`include/lscq/scqp.hpp`、`src/scqp.cpp`
- 语义：
  - `enqueue(T*)`：`nullptr` 被拒绝（返回 `false`）
  - `dequeue()`：空队列返回 `nullptr`
- 两种实现路径：
  - **CAS2 可用**：slot 直接存 `T*`（`EntryP`）
  - **回退模式**：slot 存 index，指针存放在 side pointer array 中（`is_using_fallback()` 可观测）

### 4) LSCQ（Linked SCQP）

- 代码位置：`include/lscq/lscq.hpp`、`src/lscq.cpp`
- 机制：
  - `Node` 内嵌一个 `SCQP`，当 tail 节点满时触发 `Finalize` 并链接新节点
  - head 节点消费完后向后推进，并通过 `EBR` 延迟回收旧节点（避免并发悬垂指针）

### 5) EBR（Epoch-Based Reclamation）

- 代码位置：`include/lscq/ebr.hpp`、`src/ebr.cpp`
- 对外 API：
  - `EBRManager::enter_critical()/exit_critical()`
  - `EpochGuard`（RAII）
  - `EBRManager::retire(T*)` / `try_reclaim()`

