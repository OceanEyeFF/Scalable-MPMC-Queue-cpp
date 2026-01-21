# SCQ 核心实现 - 开发计划

## 概述
实现 Scalable Circular Queue (SCQ) 核心算法，在 NCQ 基础上增加阈值机制、isSafe 标志、Catchup/fixState 和 Atomic_OR 优化，实现 >1.8x 性能提升（>50 Mops/s @ 16 线程）。

## 任务分解

### 任务 1: SCQ API 与数据模型
- **ID**: task-1
- **type**: default
- **描述**: 创建 SCQ 类模板及其数据模型，定义环形容量 (2n)、可用容量 (n)、阈值 (3n-1)、⊥ 标记 (2n-1)、bitfield 打包 (cycle 63位 + isSafe 1位)，声明 enqueue/dequeue/fixState 接口
- **文件范围**:
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\scq.hpp`
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\src\scq.cpp`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\lscq.hpp` (添加 scq.hpp 包含)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\CMakeLists.txt` (lscq_impl 目标添加 src/scq.cpp)
- **依赖**: 无
- **测试命令**: `cmake --preset windows-clang-debug && cmake --build build/debug --target lscq_impl --config Debug`
- **测试重点**:
  - 编译成功，无语法错误
  - scq.hpp 可被 lscq.hpp 正确包含
  - SCQ 类模板实例化成功 (SCQ<uint64_t>)
  - 数据成员对齐正确 (Entry 16B 对齐)

### 任务 2: SCQ 核心算法实现
- **ID**: task-2
- **type**: default
- **描述**: 在 src/scq.cpp 实现 enqueue/dequeue/fixState 核心逻辑，包括阈值检查 (THRESHOLD = 3*QSIZE-1)、isSafe 标志打包/解包、Catchup (head-tail > SCQSIZE 时触发 fixState)、Atomic_OR 优化 (⊥=SCQSIZE-1 标记减少 CAS 重试)、TSan 兼容的 entry_load (CAS2 no-op)
- **文件范围**:
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\src\scq.cpp`
  - 可选新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\detail\atomic_or.hpp` (Atomic_OR helper)
- **依赖**: 依赖 task-1
- **测试命令**: `ctest --test-dir build/debug --output-on-failure -R SCQ`
- **测试重点**:
  - Enqueue/dequeue 基础功能正常 (单线程、多线程)
  - Threshold 机制正确触发 (3n-1 条件)
  - isSafe 位正确设置和检查 (防止 ABA 问题)
  - Catchup/fixState 在 30E70D 负载下正确工作 (dequeue-heavy)
  - Atomic_OR 优化减少 CAS 冲突 (⊥=2n-1 标记)
  - 无 TSan 警告 (entry_load 使用 CAS2 no-op)

### 任务 3: SCQ 单元测试
- **ID**: task-3
- **type**: default
- **描述**: 创建 SCQ 单元测试套件，覆盖基础功能 (enqueue/dequeue)、并发正确性 (多生产者多消费者)、边界场景 (满队列、空队列)、保守性测试 (入队数 = 出队数 + 队内数)、活锁压力测试 (阈值耗尽验证)、30E70D Catchup 场景 (head-tail > SCQSIZE)
- **文件范围**:
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\unit\test_scq.cpp`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\CMakeLists.txt` (lscq_unit_tests 目标添加 unit/test_scq.cpp)
- **依赖**: 依赖 task-2
- **测试命令**: `ctest --test-dir build/debug --output-on-failure -R SCQ --verbose`
- **测试重点**:
  - 基础场景: 单线程顺序入队出队
  - 并发场景: 16 生产者 + 16 消费者，1M 操作
  - 边界场景: 满队列 enqueue 自旋、空队列 dequeue 返回 kEmpty
  - 保守性: 入队总数 = 出队总数 + 队列剩余 (原子计数验证)
  - 活锁压力: 阈值耗尽场景 (所有线程同时 enqueue)
  - Catchup: 30 个 enqueue 线程 + 70 个 dequeue 线程，验证 fixState 触发和队列不为空时正常工作

### 任务 4: SCQ 基准测试与性能对比
- **ID**: task-4
- **type**: default
- **描述**: 创建 SCQ 基准测试，实现 Pair (生产者-消费者配对)、MultiEnqueue、MultiDequeue 基准，对比 NCQ 和 SCQ 性能 (1/2/4/8/16 线程)，验证 >1.8x 提升目标 (>50 Mops/s @ 16 线程)，扩展 compare_gap.py 支持 SCQ vs NCQ 数据可视化
- **文件范围**:
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\benchmark_scq.cpp`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\CMakeLists.txt` (lscq_benchmarks 目标添加 benchmark_scq.cpp)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\compare_gap.py` (添加 SCQ 数据解析和对比图表)
- **依赖**: 依赖 task-2
- **测试命令**: `cmake --preset windows-clang-release && cmake --build build/release --config Release && .\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter="BM_(NCQ|SCQ)_Pair" --benchmark_min_time=1s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true`
- **测试重点**:
  - Pair 基准: 1-16 线程对 (生产者-消费者配对)，测量吞吐量 (Mops/s)
  - MultiEnqueue: 2-16 个 enqueue 线程，1 个 dequeue 线程
  - MultiDequeue: 1 个 enqueue 线程，2-16 个 dequeue 线程
  - 性能验证: SCQ @ 16 线程 >50 Mops/s，相比 NCQ >1.8x 提升
  - 对比报告: compare_gap.py 生成 NCQ vs SCQ 吞吐量、延迟对比图表 (PNG/SVG)

## 验收标准
- [ ] SCQ 类编译成功，API 符合 NCQ 接口规范 (enqueue/dequeue)
- [ ] 阈值机制 (3n-1)、isSafe 标志、Catchup/fixState、Atomic_OR 优化全部实现
- [ ] 所有单元测试通过 (基础、并发、边界、保守性、活锁、30E70D Catchup)
- [ ] 代码覆盖率 ≥90% (使用 `llvm-cov` + `LSCQ_ENABLE_COVERAGE=ON`)
- [ ] SCQ @ 16 线程吞吐量 >50 Mops/s，相比 NCQ >1.8x 提升
- [ ] 无 TSan 警告 (entry_load 使用 CAS2 no-op 模式)
- [ ] 基准测试报告生成 (compare_gap.py 输出 NCQ vs SCQ 对比图表)

## 技术要点
- **容量模型**: 环形大小 SCQSIZE = 2n，可用容量 QSIZE = n，用户可配置 n (默认 config::DEFAULT_SCQSIZE/2)
- **Bitfield 打包**: `cycle_flags = (cycle << 1) | isSafe`，cycle 63 位，isSafe 1 位 (LSB)
- **阈值常量**: `THRESHOLD = 3 * QSIZE - 1` (atomic<int64_t>，初始值 3n-1)
- **⊥ 标记**: `⊥ = SCQSIZE - 1` (全 1 掩码，Atomic_OR 用于标记空槽减少 CAS 冲突)
- **fixState 触发条件**: `(head - tail > SCQSIZE) && (threshold <= 0)`，重置 threshold 为 3n-1
- **TSan 兼容性**: entry_load 使用 `cas2_compare_exchange_weak(old, old)` no-op 模式，避免 data race 误报
- **性能优化**: Atomic_OR (⊥ 标记) 降低 enqueue CAS 重试次数，isSafe 标志防止 ABA 问题
- **保守性验证**: 测试中使用原子计数器验证 `enqueued_count = dequeued_count + queue_size`
- **C++17 约束**: 不使用 C++20 特性 (std::atomic<T>::wait/notify)，仅使用 CAS2 (128 位原子操作)
