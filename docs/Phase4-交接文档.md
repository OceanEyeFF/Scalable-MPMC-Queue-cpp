# Phase 4 交接文档（SCQP 实现与验证）

文档版本：v1.0  
最后更新：2026-01-19  
适用分支：`main`  
阶段范围：Phase 4（Scalable Circular Queue for Pointers / SCQP：指针语义、CAS2 运行时检测与 Fallback、测试与 Benchmark、覆盖率验证）  
目标读者：后续维护者、性能回归维护者、API 使用者、CI 维护者  
本文件目标：提供可复现的构建/测试/覆盖率/性能验证步骤，并对 SCQP 的关键实现、设计决策、Fallback 机制做完整索引与说明  

---

## 目录

- 第1章：完成情况概览
- 第2章：关键代码位置索引
- 第3章：架构设计与线程安全模型
- 第4章：核心实现（阈值 4n-1、满队列检测、consume 标记）
- 第5章：CAS2 运行时检测与自动降级（Fallback）
- 第6章：测试策略与用例清单（SCQP）
- 第7章：性能 Benchmark 设计与指标解读
- 第8章：API 使用指南与最佳实践
- 第9章：已知问题、限制与排障手册
- 第10章：未来优化方向与迁移检查清单

---

## 第1章：完成情况概览

### 1.1 功能完成对照（✅ 已完成 / ❌ 未完成）

**Phase 4 核心交付物**：
- [x] ✅ `SCQP<T>` 类模板与编译单元拆分（头文件声明 + `src/` 显式实例化）
- [x] ✅ `EntryP`：`alignas(16)` + 16B 结构布局（支持 CAS2 payload）
- [x] ✅ API：`bool enqueue(T* ptr)` / `T* dequeue()`（空队列返回 `nullptr`）
- [x] ✅ `nullptr` 入队拒绝（运行时检查）
- [x] ✅ 阈值初始化：`4 * qsize_ - 1`（区别于 SCQ 的 `3 * qsize_ - 1`）
- [x] ✅ 显式满队列检测：`(tail - head) >= scqsize_` 时直接返回 `false`
- [x] ✅ consume 标记：`dequeue()` 使用 `atomic_exchange` 将 entry 的 `ptr` 置为 `nullptr`
- [x] ✅ CAS2 运行时检测：构造阶段调用 `has_cas2_support()` 并固化当前模式
- [x] ✅ Fallback：CAS2 不可用时，降级为 “Entry 存索引 + ptr_array_ 间接存指针”
- [x] ✅ 测试：新增 `tests/unit/test_scqp.cpp`（镜像 SCQ 的 7 个核心用例并验证指针语义）
- [x] ✅ Benchmark：新增 `benchmarks/benchmark_scqp.cpp`（Pair/MultiEnqueue/MultiDequeue）
- [x] ✅ 文档：本交接文档（10 章节，≥200 行）

### 1.2 本地可复现构建/测试/覆盖率（Windows clang-cl）

**构建（Debug，目标 `lscq_impl`）**：
- `cmake --preset windows-clang-debug`
- `cmake --build build/debug --target lscq_impl --config Debug`

**运行 SCQP 单元测试**：
- `ctest --test-dir build/debug --output-on-failure -R SCQP --verbose`

**覆盖率（llvm-profdata + llvm-cov）**：
- `cmake --preset windows-clang-debug -DLSCQ_ENABLE_COVERAGE=ON`
- `cmake --build build/debug --config Debug`
- `ctest --test-dir build/debug --output-on-failure -R SCQP`
- `llvm-profdata merge -sparse build/debug/tests/coverage-*.profraw -o coverage.profdata`
- `llvm-cov report build/debug/tests/lscq_unit_tests.exe -instr-profile=coverage.profdata`

**性能测试（Release，Benchmark 过滤 SCQP）**：
- `cmake --preset windows-clang-release`
- `cmake --build build/release --config Release`
- `./build/release/benchmarks/lscq_benchmarks.exe --benchmark_filter="BM_SCQP_.*" --benchmark_min_time=2s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

---

## 第2章：关键代码位置索引

### 2.1 头文件与实现文件

- `include/lscq/scqp.hpp`：`SCQP<T>` 声明、`EntryP` 定义、对外 API 与配置
- `src/scqp.cpp`：核心算法实现（enqueue/dequeue/fixState）、CAS2P 与 Fallback 分支

### 2.2 聚合头与构建系统

- `include/lscq/lscq.hpp`：增加 `#include <lscq/scqp.hpp>`
- `CMakeLists.txt`：`lscq_impl` 目标新增 `src/scqp.cpp`

### 2.3 原子工具与辅助实现

- `include/lscq/detail/atomic_ptr.hpp`：
  - `atomic_exchange_ptr`：用于 consume 标记（将 `T*` 置空）
  - `atomic_compare_exchange_ptr`：用于 Fallback 模式发布/回滚 `ptr_array_`
  - `atomic_exchange_u64`：用于 Fallback 模式 consume 标记（写入空索引）
- `include/lscq/detail/ncq_impl.hpp`：沿用 “CAS2 no-op” 的 16B entry atomic load 思路（用于 SCQ）

---

## 第3章：架构设计与线程安全模型

### 3.1 对外语义

- `enqueue(T* ptr)`：
  - 入参必须非空；`nullptr` 直接返回 `false`
  - 满队列时直接返回 `false`（不在实现内部等待其他线程 dequeue）
- `dequeue()`：
  - 空队列返回 `nullptr`
  - 否则返回此前 `enqueue()` 过的同一指针地址（身份保持）

### 3.2 线程安全边界

- 构造/析构：要求外部保证对象尚未并发访问（与 SCQ 一致）
- 并发访问：多生产者/多消费者安全
- 指针生命周期：SCQP 不管理对象生命周期；调用方需保证指针在出队前有效

---

## 第4章：核心实现（阈值 4n-1、满队列检测、consume 标记）

### 4.1 阈值（Threshold）

- `threshold_` 初始值：`4 * qsize_ - 1`
- 目的：
  - 与 SCQ 一样，提供 “bounded retry / fast empty check” 的优化与活锁防护
  - SCQP 采用更保守的阈值初始化（4n-1），用于适配指针语义下的更激进空转场景

### 4.2 显式满队列检测

- enqueue 热路径前置检查：
  - 如果 `(tail - head) >= scqsize_`，认为队列已满，直接返回 `false`
  - 调用方可以选择等待、退避、或切换到备用队列/降级策略
- 设计理由：
  - 与 SCQ 的 “内部自旋直到成功” 形成对比
  - 更利于上层调度（尤其是 backpressure 或限定延迟的系统）

### 4.3 consume 标记：`atomic_exchange(ptr, nullptr)`

- dequeue 成功消费一个 entry 时：
  - 使用 `atomic_exchange_ptr(&entry.ptr, nullptr)` 将该 slot 标记为空
  - 相比 SCQ 的 `atomic_or(⊥)`：
    - 指针语义天然存在 “空值 sentinel”（nullptr）
    - 交换返回旧值可用于调试/一致性检查（实现中返回值可忽略）

---

## 第5章：CAS2 运行时检测与自动降级（Fallback）

### 5.1 为什么需要 Fallback

- CAS2（CMPXCHG16B）在 x86_64 上通常可用，但不是绝对保证
- SCQP 需要以 16B 原子方式读写 `cycle_flags + ptr/idx`，否则无法保持 SCQ 类算法的关键不变量
- 因此实现采用：
  - **CAS2 可用**：直接使用 `EntryP { cycle_flags, T* }`
  - **CAS2 不可用**：使用 `Entry { cycle_flags, index }` + `ptr_array_[index]`

### 5.2 运行时检测入口

- 构造函数中调用 `has_cas2_support()` 并缓存到 `using_fallback_`
- 对外可通过 `is_using_fallback()` 查询当前模式（用于日志/诊断/性能解释）

### 5.3 Fallback 的数据布局

- `entries_i_`：`lscq::Entry` 数组（16B/16-align）
  - 第二字段存储 `index`（0..SCQSIZE-1）
  - 空槽用 `kEmptyIndex = UINT64_MAX` 标记
- `ptr_array_`：`std::unique_ptr<T*[]>`，大小为 `SCQSIZE`
  - 通过原子 CAS/Exchange 发布和消费指针

---

## 第6章：测试策略与用例清单（SCQP）

### 6.1 新增测试文件

- `tests/unit/test_scqp.cpp`

### 6.2 用例覆盖（镜像 SCQ 的 7 个核心测试）

1. `SCQP_Basic.SequentialEnqueueDequeueFifo`
2. `SCQP_EdgeCases.EnqueueRejectsNullptr`
3. `SCQP_EdgeCases.DequeueOnEmptyReturnsNullptr`
4. `SCQP_EdgeCases.EnqueueFailsWhenQueueIsFull`
5. `SCQP_Concurrent.ProducersConsumers16x16_1M_NoLossNoDup_Conservative`
6. `SCQP_Stress.ThresholdExhaustionThenBurstEnqueue_AllThreadsEnqueue`
7. `SCQP_Stress.Catchup_30Enq70Deq_QueueNonEmptyStillWorks`

### 6.3 关键断言点

- 指针身份验证：出队返回的地址必须来自入队地址集合
- 满队列：第 `SCQSIZE+1` 次入队失败（不阻塞）
- 空队列：`dequeue()` 返回 `nullptr`
- 并发：无丢失、无重复、无越界指针
- 修复机制：空 dequeue 风暴后 `fixState` 推进 tail 的可观测性

---

## 第7章：性能 Benchmark 设计与指标解读

### 7.1 新增 Benchmark 文件

- `benchmarks/benchmark_scqp.cpp`

### 7.2 Benchmark 项目

- `BM_SCQP_Pair`：1/2/4/8/16 线程（成对生产/消费）
- `BM_SCQP_MultiEnqueue`：2/4/8/16 enqueue + 1 dequeue（总线程数 3/5/9/17）
- `BM_SCQP_MultiDequeue`：1 enqueue + 2/4/8/16 dequeue（总线程数 3/5/9/17）

### 7.3 结果字段（Counters）

- `has_cas2_support`：平台 CAS2 支持情况（1/0）
- `using_fallback`：SCQP 当前是否处于降级模式（1/0）
- `Mops`：吞吐量（ops/s）

---

## 第8章：API 使用指南与最佳实践

### 8.1 正确使用方式

- 只入队非空指针：`q.enqueue(ptr)`，`ptr != nullptr`
- 出队为空：`if (auto* p = q.dequeue()) { ... }`
- 上层处理背压：
  - 若 `enqueue()` 返回 `false`，由上层决定 retry/backoff/丢弃/降级

### 8.2 生命周期与所有权

- SCQP 不管理 `T` 对象生命周期
- 推荐：
  - 使用对象池/稳定地址容器（如 `std::vector<T>` 预分配或自定义池）
  - 避免入队指向临时对象/栈对象的指针（除非确保消费前不离开作用域）

---

## 第9章：已知问题、限制与排障手册

### 9.1 已知限制

- 指针必须是 64-bit（当前实现 `static_assert(sizeof(T*) == 8)`）
- 满队列策略是 “失败返回”，对上层意味着需要自行 backpressure
- Fallback 模式依赖 `ptr_array_` 的原子发布/消费，性能会显著低于 CAS2 模式

### 9.2 常见排障

- **现象：并发测试卡住**
  - 检查调用侧是否在 `enqueue(false)` 时没有 retry
  - 检查是否错误入队了 `nullptr`（会被拒绝，导致总量不足）
- **现象：性能明显低**
  - 查看 `using_fallback` 是否为 1（CAS2 不可用导致）
  - 确认 CPU 支持 CX16，且构建开启 `LSCQ_ENABLE_CAS2=ON`

---

## 第10章：未来优化方向与迁移检查清单

### 10.1 未来优化方向（可选）

- 更强的满队列检测一致性（减少极端竞争下的误判）
- 在 Fallback 模式中减少 `ptr_array_` 的二阶段提交成本
- 指针 payload 支持更丰富的标记位（例如低位 tag，或 encode 状态）
- 为不同平台（非 x86_64）提供更高效的 16B 原子方案或替代算法

### 10.2 迁移检查清单

- [ ] 构建系统：`lscq_impl` 已包含 `src/scqp.cpp`
- [ ] 头文件：`include/lscq/lscq.hpp` 已包含 `scqp.hpp`
- [ ] 单测：`ctest -R SCQP` 通过
- [ ] 覆盖率：SCQP 相关路径覆盖率 ≥90%
- [ ] 性能：`BM_SCQP_*` 在 CAS2 平台上接近 `SCQ` 的 90%~100%
- [ ] 平台：确认 `has_cas2_support` 与 `using_fallback` 状态符合预期

