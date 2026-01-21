# LSCQ 无界队列实现 - 开发计划

## 概述
实现论文的最终目标 LSCQ (Linked Scalable Circular Queue) 无界队列，通过链接多个 SCQP 节点实现无界 MPMC 队列，集成简化版 Epoch-Based Reclamation (EBR) 内存回收机制，性能目标为 SCQP 的 85-95%，测试覆盖率 ≥90%，通过 AddressSanitizer 零泄漏验证。

## 任务分解

### 任务 1: EBR 内存回收机制实现
- **ID**: task-1
- **type**: default
- **描述**: 实现简化版 Epoch-Based Reclamation 管理器，包含全局 epoch 计数器（原子递增）、Thread-local epoch 跟踪（存储线程活跃 epoch）、三代 Retired 节点列表（epochs[3] 存储待回收节点）、RAII EpochGuard（构造时进入 epoch，析构时退出 epoch）、内存回收策略（当节点 epoch < global_epoch - 2 时安全回收）
- **文件范围**:
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\ebr.hpp`
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\src\ebr.cpp`
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\unit\test_ebr.cpp`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\CMakeLists.txt` (lscq_impl 添加 src/ebr.cpp)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\CMakeLists.txt` (添加 test_ebr.cpp)
- **依赖**: 无
- **测试命令**: `cmake --preset windows-clang-debug && cmake --build build/debug --target lscq_unit_tests --config Debug && ctest --test-dir build/debug --output-on-failure -R EBR --verbose`
- **测试重点**:
  - 基础功能: 全局 epoch 计数器从 0 开始递增，enter_epoch/exit_epoch 正确更新线程状态
  - 节点 Retire: retire_node 将节点加入当前 epoch 列表，不立即销毁
  - 内存回收: 调用 try_reclaim 后，满足 epoch < global_epoch - 2 的节点被释放
  - 多线程安全: 8 线程并发 retire/reclaim，验证无内存泄漏和 double-free
  - EpochGuard RAII: 作用域结束后自动调用 exit_epoch
  - 边界场景: 空列表调用 try_reclaim 不崩溃，单线程连续 retire 10000 节点后正确回收
  - ASan 验证: 无 use-after-free、memory leak、double-free 报告
  - 覆盖率: ≥90% (enter_epoch, exit_epoch, retire_node, try_reclaim 所有分支)

### 任务 2: LSCQ Node 结构与类框架
- **ID**: task-2
- **type**: default
- **描述**: 定义 LSCQ::Node 结构（包含 SCQP 实例、std::atomic<Node*> next 指针、std::atomic<bool> finalized 标志、64 字节对齐防止伪共享），实现 LSCQ 类框架（std::atomic<Node*> head_/tail_ 指针、64 字节对齐、构造函数初始化首个 Node、析构函数遍历链表释放所有 Node 并调用 EBR 回收）
- **文件范围**:
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\lscq.hpp` (已存在，添加 Node 结构和 LSCQ 类定义)
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\src\lscq.cpp`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\CMakeLists.txt` (lscq_impl 添加 src/lscq.cpp)
- **依赖**: 依赖 task-1 (需要 EBR 管理器)
- **测试命令**: `cmake --preset windows-clang-debug && cmake --build build/debug --target lscq_impl --config Debug`
- **测试重点**:
  - 编译成功: LSCQ 类模板实例化 (LSCQ<MyObject*>) 无语法错误
  - Node 结构: sizeof(Node) 验证 64 字节对齐，next 和 finalized 初始化为 nullptr 和 false
  - 构造函数: head_ 和 tail_ 均指向首个 Node，首个 Node 的 SCQP 初始化完成
  - 析构函数: 遍历链表释放所有 Node（通过 EBR retire_node），无内存泄漏
  - 内存布局: Node 成员 SCQP、next、finalized 顺序正确，64 字节对齐
  - API 声明: bool enqueue(T* ptr) 和 T* dequeue() 接口骨架存在

### 任务 3: LSCQ Enqueue 操作与 Finalize 机制
- **ID**: task-3
- **type**: default
- **描述**: 实现 LSCQ 无界 enqueue 操作（EpochGuard 保护访问、循环尝试 tail_->scqp.enqueue(ptr)、失败时检查 finalized 标志、若未 finalized 则 CAS 设置为 true 并创建新 Node、CAS 更新 tail_->next 和 tail_ 指针、重试逻辑直到成功），Finalize 机制确保队列满时优雅扩展链表
- **文件范围**:
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\lscq.hpp` (enqueue 方法实现)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\src\lscq.cpp` (enqueue 实现)
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\unit\test_lscq.cpp`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\CMakeLists.txt` (添加 test_lscq.cpp)
- **依赖**: 依赖 task-2 (需要 LSCQ 类框架)
- **测试命令**: `cmake --preset windows-clang-debug && cmake --build build/debug --target lscq_unit_tests --config Debug && ctest --test-dir build/debug --output-on-failure -R LSCQ.Enqueue --verbose`
- **测试重点**:
  - 基础功能: 单线程 enqueue 100 个指针，所有返回 true
  - Finalize 触发: 单线程 enqueue 超过 SCQP capacity 个指针，验证创建新 Node (tail_->next != nullptr)
  - 链表扩展: 连续 enqueue 3 * SCQP_capacity 个指针，验证链表有 ≥3 个 Node
  - nullptr 拒绝: enqueue(nullptr) 返回 false 或触发断言
  - 多线程并发: 8 生产者线程并发 enqueue 10000 指针，所有成功，无丢失
  - Finalize 竞争: 多线程同时触发 Finalize，验证仅创建一个新 Node (无重复 next 指针)
  - EpochGuard 保护: enqueue 期间线程 epoch 正确注册和退出
  - 边界场景: SCQP capacity=8 时连续 enqueue 100 次，验证链表正确扩展

### 任务 4: LSCQ Dequeue 操作与 Head 移动
- **ID**: task-4
- **type**: default
- **描述**: 实现 LSCQ dequeue 操作（EpochGuard 保护访问、循环尝试 head_->scqp.dequeue()、成功则返回指针、失败时检查 head_->next 是否存在、若存在则 CAS 更新 head_ 指向下一个 Node 并 retire 旧 head_、继续循环直到返回指针或确认队列为空返回 nullptr），Head 移动机制确保消费完旧 Node 后前进
- **文件范围**:
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\lscq.hpp` (dequeue 方法实现)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\src\lscq.cpp` (dequeue 实现)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\unit\test_lscq.cpp` (dequeue 测试用例)
- **依赖**: 依赖 task-3 (需要 enqueue 操作)
- **测试命令**: `cmake --preset windows-clang-debug && cmake --build build/debug --target lscq_unit_tests --config Debug && ctest --test-dir build/debug --output-on-failure -R LSCQ.Dequeue --verbose`
- **测试重点**:
  - 基础功能: enqueue 100 个指针后 dequeue 100 次，返回所有原始指针
  - 空队列: 新建 LSCQ 直接 dequeue 返回 nullptr
  - FIFO 顺序: enqueue 指针 [p1, p2, p3]，dequeue 顺序为 [p1, p2, p3]
  - Head 移动: enqueue 超过 SCQP capacity 个指针，dequeue 消费完第一个 Node 后 head_ 移动到下一个 Node
  - 跨节点 Dequeue: enqueue 3 * capacity 个指针，dequeue 全部，验证 head_ 最终指向最后一个 Node
  - 多线程并发: 8 消费者线程并发 dequeue，验证所有 enqueue 的指针都被取出，无重复
  - 生产者-消费者: 8 生产者 + 8 消费者并发 100K 操作，验证无丢失、无重复、无崩溃
  - EBR 回收验证: 旧 Node 被 retire 后通过 EBR 最终释放，无内存泄漏 (ASan 检查)
  - 边界场景: 单 Node 场景下 dequeue 空队列返回 nullptr，多 Node 场景下连续 dequeue 直到空

### 任务 5: 并发测试与 ASan 内存验证
- **ID**: task-5
- **type**: default
- **描述**: 完善 LSCQ 并发测试套件（多生产者多消费者压力测试、16+16 线程 100K 操作、验证无丢失无重复无崩溃），运行 AddressSanitizer 构建验证无内存泄漏、无 use-after-free、无 double-free，运行覆盖率分析验证 ≥90% 代码覆盖率，生成覆盖率报告
- **文件范围**:
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\unit\test_lscq.cpp` (并发测试用例)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\unit\test_ebr.cpp` (EBR 并发测试)
- **依赖**: 依赖 task-4 (需要完整 enqueue/dequeue 实现)
- **测试命令**:
  - **ASan 构建**: `cmake --preset windows-clang-debug -DLSCQ_ENABLE_SANITIZERS=ON && cmake --build build/debug --target lscq_unit_tests --config Debug && ctest --test-dir build/debug --output-on-failure -R LSCQ --verbose`
  - **覆盖率分析**: `cmake --preset windows-clang-debug -DLSCQ_ENABLE_COVERAGE=ON && cmake --build build/debug --target lscq_unit_tests --config Debug && ctest --test-dir build/debug --output-on-failure -R LSCQ && llvm-profdata merge -sparse build/debug/tests/coverage-*.profraw -o coverage.profdata && llvm-cov report build/debug/tests/lscq_unit_tests.exe -instr-profile=coverage.profdata -use-color`
- **测试重点**:
  - **并发正确性测试**:
    - LSCQ_MultiProducerMultiConsumer: 16 生产者 + 16 消费者，每线程 10000 操作，验证总数匹配
    - LSCQ_StressTest: 8+8 线程 100K 操作，验证无丢失、无重复、无死锁
    - LSCQ_ConcurrentFinalize: 多线程同时触发 Finalize，验证链表一致性
    - LSCQ_ConcurrentHeadMove: 多消费者同时触发 head 移动，验证无重复 retire
  - **ASan 内存验证**:
    - 无内存泄漏: 所有 Node 最终通过 EBR 释放
    - 无 use-after-free: EBR 回收策略正确，epoch < global_epoch - 2 的节点才释放
    - 无 double-free: 每个 Node 仅 retire 一次
    - 无数据竞争: head_/tail_ 原子操作正确，finalized 标志无竞争
  - **覆盖率验证**:
    - 代码覆盖率 ≥90%: llvm-cov report 输出总覆盖率
    - 关键路径覆盖: enqueue 成功/Finalize/重试, dequeue 成功/空队列/head 移动, EBR enter/exit/retire/reclaim
    - 分支覆盖: CAS 成功/失败分支, finalized 检查分支, next 存在/不存在分支
  - **性能基线测试**: 单线程 enqueue/dequeue 1M 操作耗时 < 2s (确保无性能回退)

### 任务 6: LSCQ 性能基准测试与 SCQP 对比
- **ID**: task-6
- **type**: default
- **描述**: 实现 LSCQ 性能基准测试（BM_LSCQ_Pair, BM_LSCQ_MultiEnqueue, BM_LSCQ_MultiDequeue），对比 SCQP 性能（同配置下运行 SCQP 基准），验证 LSCQ 性能达到 SCQP 的 85-95%，生成性能报告（包含吞吐量对比图表、多线程扩展性分析、EBR 开销分析）
- **文件范围**:
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\benchmark_lscq.cpp`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\CMakeLists.txt` (添加 benchmark_lscq.cpp)
- **依赖**: 依赖 task-4 (需要完整 LSCQ 实现)
- **测试命令**: `cmake --preset windows-clang-release && cmake --build build/release --target lscq_benchmarks --config Release && .\build\release\bin\lscq_benchmarks.exe --benchmark_filter="BM_LSCQ_.*|BM_SCQP_.*" --benchmark_min_time=3s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`
- **测试重点**:
  - **基准测试场景**:
    - BM_LSCQ_Pair: 1/2/4/8/16 线程对 (生产者-消费者配对)，测量吞吐量 (ops/s)
    - BM_LSCQ_MultiEnqueue: 2/4/8/16 个 enqueue 线程，1 个 dequeue 线程
    - BM_LSCQ_MultiDequeue: 1 个 enqueue 线程，2/4/8/16 个 dequeue 线程
    - BM_SCQP_Pair: 同配置运行 SCQP 基准作为对比基线
  - **性能目标**:
    - LSCQ@16 线程 >35 Mops/s (SCQP 约 40-45 Mops/s)
    - LSCQ 性能 85-95% of SCQP (EBR 和链表开销在合理范围)
    - 多线程扩展性: 16 线程吞吐量 ≥ 8 线程吞吐量 * 1.5 (良好扩展)
  - **性能分析**:
    - EBR 开销: 对比 SCQP 性能差异，确认 EBR epoch 管理开销 < 15%
    - Finalize 开销: 测量 Finalize 触发频率和延迟，确认链表扩展不影响稳态性能
    - Head 移动开销: 测量 head CAS 更新延迟，确认 Node retire 不阻塞 dequeue
  - **报告内容**:
    - 吞吐量对比表: LSCQ vs SCQP 各线程配置的 ops/s
    - 性能比率: LSCQ / SCQP 百分比，验证 ≥85%
    - 扩展性曲线: 线程数 vs 吞吐量，展示 LSCQ 线性扩展

### 任务 7: Phase5 交接文档编写
- **ID**: task-7
- **type**: default
- **描述**: 编写 Phase5-交接文档.md (≥200 行，10 章节)，涵盖 LSCQ 架构设计（Node 结构、链表拓扑、Finalize 机制）、EBR 实现细节（epoch 管理、三代列表、回收策略）、核心操作流程图（enqueue/dequeue/Finalize/Head 移动）、测试策略（单元测试、并发测试、ASan 验证、覆盖率分析）、性能分析（LSCQ vs SCQP 对比、EBR 开销、多线程扩展性）、API 使用指南、已知问题、未来优化方向、迁移检查清单
- **文件范围**:
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\docs\Phase5-交接文档.md`
- **依赖**: 依赖 task-5, task-6 (需要测试和性能数据)
- **测试命令**: 手动审阅文档内容，验证以下检查清单:
  - 文档总行数 ≥200 行 (使用 `wc -l` 或手动统计)
  - 包含 10 个章节: 概述、架构设计、EBR 实现、核心操作、测试策略、性能分析、API 使用、已知问题、未来优化、迁移清单
  - 包含代码示例: LSCQ 使用示例 (实例化、enqueue、dequeue)
  - 包含性能图表: LSCQ vs SCQP 吞吐量对比表格
  - 包含流程图: enqueue/dequeue/Finalize 流程的文本描述或 Markdown 图表
- **测试重点**:
  - **章节 1: 概述**: LSCQ 设计目标（无界 MPMC 队列）、与 SCQP 的关系（链接多个 SCQP 节点）、核心特性（EBR 内存回收、Finalize 扩展、85-95% SCQP 性能）
  - **章节 2: 架构设计**: Node 结构（SCQP + next + finalized）、head_/tail_ 指针、64 字节对齐、链表拓扑（单向链表、tail 端扩展）、Finalize 机制（SCQP 满时创建新 Node）
  - **章节 3: EBR 实现**: 全局 epoch 计数器、Thread-local epoch 跟踪、三代 Retired 列表、EpochGuard RAII、回收策略（epoch < global_epoch - 2）、内存安全保证
  - **章节 4: 核心操作**: enqueue 流程图（EpochGuard → 尝试 SCQP enqueue → 失败检查 finalized → Finalize → 重试）、dequeue 流程图（EpochGuard → 尝试 SCQP dequeue → 成功返回 → 失败检查 next → Head 移动 → 重试）、Finalize 详细步骤（CAS finalized → 创建 Node → CAS next → CAS tail_）、Head 移动详细步骤（CAS head_ → retire 旧 Node）
  - **章节 5: 测试策略**: 单元测试覆盖（基础、并发、Finalize、Head 移动、EBR）、覆盖率分析（≥90%）、ASan 验证（零泄漏、零 UAF、零 double-free）、边界场景（空队列、满队列、单 Node、多 Node）
  - **章节 6: 性能分析**: LSCQ vs SCQP 吞吐量对比表、性能比率验证（85-95%）、多线程扩展性曲线、EBR 开销分析（epoch 管理 < 15%）、Finalize 开销分析、Head 移动开销分析
  - **章节 7: API 使用指南**: LSCQ<T*> 实例化示例、enqueue 用法（返回 true 表示成功）、dequeue 用法（返回 nullptr 表示空队列）、nullptr 处理（禁止 enqueue nullptr）、生命周期管理（析构自动释放所有 Node）
  - **章节 8: 已知问题**: EBR 回收延迟（最多保留 3 代节点）、内存峰值（Finalize 扩展链表）、Windows MSVC CAS2 支持状态（影响 SCQP 性能进而影响 LSCQ）、Thread-local 存储限制（最大线程数）
  - **章节 9: 未来优化**: 更激进的 EBR 策略（动态调整回收阈值）、Node 池化（避免频繁 new/delete）、SCQP capacity 自适应（根据负载动态调整）、Lock-free Head 移动（减少 CAS 竞争）
  - **章节 10: 迁移检查清单**: 从 SCQP 迁移到 LSCQ 的代码修改点（替换 SCQP<T*> 为 LSCQ<T*>）、测试验证步骤（运行单元测试、ASan 验证、性能基准）、配置调整（SCQP capacity 影响 LSCQ 节点大小）、依赖检查（确保 EBR 编译）

## 验收标准
- [ ] EBR 管理器实现完成，所有 EBR 单元测试通过，覆盖率 ≥90%
- [ ] LSCQ Node 结构和类框架实现，编译成功，64 字节对齐验证通过
- [ ] LSCQ enqueue 操作实现，Finalize 机制正常工作，链表正确扩展
- [ ] LSCQ dequeue 操作实现，Head 移动机制正常工作，旧 Node 正确 retire
- [ ] 所有 LSCQ 单元测试通过（基础、并发、Finalize、Head 移动、空队列）
- [ ] 代码覆盖率 ≥90% (使用 `llvm-cov` + `LSCQ_ENABLE_COVERAGE=ON`)
- [ ] ASan 验证通过（零内存泄漏、零 use-after-free、零 double-free）
- [ ] LSCQ 性能达到 SCQP 的 85-95% (16 线程 >35 Mops/s)
- [ ] 性能基准测试报告生成（LSCQ vs SCQP 对比表、扩展性曲线、开销分析）
- [ ] Phase5-交接文档.md 完成 (≥200 行，10 章节，包含流程图、性能图表、代码示例、迁移指南)

## 技术要点
- **EBR 设计**:
  - 全局 epoch 计数器: `std::atomic<uint64_t> global_epoch_` (仅递增，不回绕)
  - Thread-local epoch: `thread_local uint64_t thread_epoch_` (活跃时存储当前 epoch)
  - Retired 列表: `std::vector<Node*> retired_[3]` (三代列表，epoch % 3 索引)
  - 回收策略: `if (node_epoch < global_epoch - 2) delete node` (保证无活跃访问)
  - EpochGuard: `struct EpochGuard { EpochGuard() { enter_epoch(); } ~EpochGuard() { exit_epoch(); } }`
- **LSCQ 架构**:
  - Node 结构: `struct Node { SCQP<T> scqp; std::atomic<Node*> next{nullptr}; std::atomic<bool> finalized{false}; } alignas(64);`
  - Head/Tail 指针: `alignas(64) std::atomic<Node*> head_; alignas(64) std::atomic<Node*> tail_;`
  - Finalize 机制: SCQP enqueue 失败时检查 finalized 标志，CAS 设为 true 后创建新 Node，CAS 更新 next 和 tail_
  - Head 移动: dequeue 失败时检查 head_->next，存在则 CAS head_ 指向 next 并 retire 旧 head_
- **并发安全**:
  - 所有 head_/tail_/next/finalized 访问使用原子操作
  - EpochGuard 保护 enqueue/dequeue 期间的 Node 访问
  - CAS 失败时循环重试，避免 ABA 问题（epoch 保护）
- **性能目标**:
  - LSCQ@16 线程 >35 Mops/s (SCQP 约 40-45 Mops/s)
  - LSCQ 性能 85-95% of SCQP (EBR 和链表开销合理)
  - EBR 开销 < 15% (epoch 管理和回收延迟)
  - 多线程扩展性: 16 线程吞吐量 ≥ 8 线程吞吐量 * 1.5
- **测试要求**:
  - 覆盖率 ≥90% (严格匹配 Phase 4 标准)
  - ASan 零泄漏、零 UAF、零 double-free
  - 并发测试: 16+16 线程 100K 操作无丢失无重复
  - 边界测试: 空队列、满队列、单 Node、多 Node、并发 Finalize、并发 Head 移动
- **C++17 约束**: 不使用 C++20 特性，仅使用 std::atomic、thread_local、alignas
- **构建系统**: CMake Presets (windows-clang-debug/release)，GoogleTest (单元测试)，Google Benchmark (性能测试)
- **文档质量**: Phase5-交接文档.md 遵循 Phase 4 格式 (≥200 行，10 章节，流程图、性能图表、代码示例、迁移指南)
- **关键差异 (vs SCQP)**:
  - 队列容量: 无界 (LSCQ) vs 有界 (SCQP)
  - 内存管理: EBR 回收 (LSCQ) vs 固定数组 (SCQP)
  - 扩展策略: Finalize 创建新 Node (LSCQ) vs enqueue 失败返回 (SCQP)
  - Head 移动: 动态前进 (LSCQ) vs 固定不变 (SCQP)
  - 性能开销: EBR + 链表遍历 (LSCQ) vs 直接数组访问 (SCQP)
