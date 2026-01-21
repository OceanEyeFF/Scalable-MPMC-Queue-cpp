# SCQP 实现 - 开发计划

## 概述
实现指针模式的 Scalable Circular Queue (SCQP)，在 SCQ 架构基础上支持指针类型存储（nullptr 作为空标记），阈值从 3n-1 调整为 4n-1，增加满队列显式失败检测机制，并实现 CAS2 运行时检测与自动降级到 SCQ 的 fallback 策略。

## 任务分解

### 任务 1: SCQP 类型与指针 Entry 布局
- **ID**: task-1
- **type**: default
- **描述**: 创建 SCQP 类模板，定义指针专用 Entry 结构 (`EntryP: {uint64_t cycle_flags; T* ptr}`)，实现 nullptr 作为空标记的语义，声明 enqueue/dequeue 接口骨架，替换 SCQ 的整数索引模式为指针模式
- **文件范围**:
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\scqp.hpp`
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\src\scqp.cpp`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\lscq.hpp` (添加 scqp.hpp 包含)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\CMakeLists.txt` (lscq_impl 目标添加 src/scqp.cpp)
- **依赖**: 无
- **测试命令**: `cmake --preset windows-clang-debug && cmake --build build/debug --target lscq_impl --config Debug`
- **测试重点**:
  - 编译成功，无语法错误
  - scqp.hpp 可被 lscq.hpp 正确包含
  - SCQP 类模板实例化成功 (SCQP<MyObject*>)
  - EntryP 结构体 16B 对齐正确 (对齐 CAS2 要求)
  - API 签名: `bool enqueue(T* ptr)` 和 `T* dequeue()` (返回 nullptr 表示空队列)
  - nullptr 拒绝入队的静态断言或运行时检查

### 任务 2: 满队列检测与阈值调整 (4n-1) + 指针 consume 标记
- **ID**: task-2
- **type**: default
- **描述**: 实现阈值初始化为 `4 * qsize_ - 1` (区别于 SCQ 的 3n-1)，在 enqueue 中增加显式满队列检测 `(tail - head) >= SCQSIZE` 返回失败（不自旋），在 dequeue 的 consume 阶段使用 `atomic_exchange` 将 ptr 设置为 nullptr（替代 SCQ 的 atomic_or 索引标记），确保指针语义的正确性
- **文件范围**:
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\scqp.hpp`
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\src\scqp.cpp`
  - 可选新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\detail\atomic_ptr.hpp` (指针原子操作 helper)
- **依赖**: 依赖 task-1
- **测试命令**: `ctest --test-dir build/debug --output-on-failure -R SCQP`
- **测试重点**:
  - 阈值初始值正确 (4 * qsize_ - 1)
  - 满队列场景: enqueue 返回 false 而不是自旋等待
  - 空队列场景: dequeue 返回 nullptr
  - Consume 标记: dequeue 后 entry 的 ptr 字段变为 nullptr
  - 指针身份验证: enqueue(p) 后 dequeue() 返回相同指针地址 p
  - FIFO 顺序验证: 按入队顺序出队
  - 边界场景: 连续 QSIZE 次 enqueue 后第 QSIZE+1 次失败

### 任务 3: CAS2 运行时检测与自动降级 Fallback
- **ID**: task-3
- **type**: default
- **描述**: 实现 SCQP 构造函数中调用 `has_cas2_support()` 进行运行时 CPUID 检测，当 CAS2 不可用时自动降级到 SCQ 索引模式 (内部维护指针数组，enqueue/dequeue 通过索引间接访问)，在 fallback 模式下保持 API 兼容性，记录 fallback 状态供调试和基准测试报告
- **文件范围**:
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\scqp.hpp`
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\src\scqp.cpp`
  - 可能更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\cas2.hpp` (确保 has_cas2_support 导出)
- **依赖**: 依赖 task-1
- **测试命令**: `ctest --test-dir build/debug --output-on-failure -R SCQP`
- **测试重点**:
  - CAS2 可用平台: SCQP 直接使用指针 Entry 模式
  - CAS2 不可用平台: 自动降级到 SCQ 索引 + 指针数组模式
  - Fallback 模式功能正确性: enqueue/dequeue 返回正确指针
  - API 透明性: 外部调用者无需关心内部 fallback 实现
  - 调试信息: 构造函数或 is_using_fallback() 方法暴露当前模式
  - 混合测试: 同一测试在 CAS2 和 fallback 模式下均通过

### 任务 4: 测试套件 + 基准测试 + Phase4 交接文档 + 覆盖率验证
- **ID**: task-4
- **type**: default
- **描述**: 创建 SCQP 完整单元测试 (test_scqp.cpp，镜像 SCQ 测试结构但验证指针语义)，增加 benchmark_scqp.cpp 性能基准 (BM_SCQP_Pair, BM_SCQP_MultiEnqueue, BM_SCQP_MultiDequeue)，编写 Phase4-交接文档.md (>200 行，10 章节，涵盖架构、实现细节、测试策略、性能结果、迁移指南、已知问题、未来优化)，运行覆盖率分析验证 ≥90% 代码覆盖率
- **文件范围**:
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\unit\test_scqp.cpp`
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\benchmark_scqp.cpp`
  - 新建 `e:\gitee\Scaleable-MPMC-Queue-cpp\docs\Phase4-交接文档.md`
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\CMakeLists.txt` (添加 test_scqp.cpp)
  - 更新 `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\CMakeLists.txt` (添加 benchmark_scqp.cpp)
- **依赖**: 依赖 task-2, task-3
- **测试命令**:
  - 单元测试: `ctest --test-dir build/debug --output-on-failure -R SCQP --verbose`
  - 覆盖率: `cmake --preset windows-clang-debug -DLSCQ_ENABLE_COVERAGE=ON && cmake --build build/debug && ctest --test-dir build/debug --output-on-failure -R SCQP && llvm-profdata merge -sparse build/debug/tests/coverage-*.profraw -o coverage.profdata && llvm-cov report build/debug/tests/lscq_unit_tests.exe -instr-profile=coverage.profdata`
  - 基准测试: `cmake --preset windows-clang-release && cmake --build build/release --config Release && .\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter="BM_SCQP_.*" --benchmark_min_time=2s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`
- **测试重点**:
  - **单元测试场景**:
    - 基础功能: 单线程 enqueue/dequeue 指针，验证指针身份 (地址相等)
    - 并发正确性: 16 生产者 + 16 消费者，100K 指针操作，验证无丢失
    - 满队列场景: enqueue 返回 false (不自旋)，区别于 SCQ 的自旋行为
    - 空队列场景: dequeue 返回 nullptr
    - nullptr 拒绝: enqueue(nullptr) 返回 false 或触发断言
    - Consume 验证: dequeue 后 entry ptr 字段为 nullptr
    - Fallback 模式: 强制 fallback 测试，验证指针数组模式正确性
    - FIFO 顺序: 验证多线程下指针出队顺序符合 FIFO
  - **基准测试场景**:
    - BM_SCQP_Pair: 1/2/4/8/16 线程对 (生产者-消费者配对)，测量吞吐量
    - BM_SCQP_MultiEnqueue: 2/4/8/16 个 enqueue 线程，1 个 dequeue 线程
    - BM_SCQP_MultiDequeue: 1 个 enqueue 线程，2/4/8/16 个 dequeue 线程
    - 性能目标: SCQP@16 线程 >40 Mops/s
    - SCQ 对比: SCQP 性能在 SCQ 的 90-100% 范围内 (指针模式开销)
    - CAS2 vs Fallback: 报告 has_cas2_support() 结果，避免混淆性能数据
  - **覆盖率验证**:
    - 代码覆盖率 ≥90% (llvm-cov report 输出)
    - 关键路径覆盖: enqueue 成功/失败, dequeue 成功/空队列, fixState, fallback 分支
  - **交接文档 (Phase4-交接文档.md)** (≥200 行，10 章节):
    1. **概述**: SCQP 设计目标，与 SCQ 的区别 (指针模式 vs 索引模式)
    2. **架构设计**: EntryP 结构, 4n-1 阈值, 满队列检测, nullptr 语义
    3. **核心实现**: enqueue/dequeue 流程图, consume 标记机制 (atomic_exchange)
    4. **CAS2 Fallback 策略**: 运行时检测, 自动降级到索引模式, 性能影响
    5. **测试策略**: 单元测试设计, 覆盖率分析, 边界场景验证
    6. **性能分析**: 基准测试结果, SCQP vs SCQ 对比, 多线程扩展性
    7. **API 使用指南**: SCQP<T*> 实例化示例, nullptr 处理, 满队列重试策略
    8. **已知问题**: Fallback 模式性能损失, Windows MSVC CAS2 支持状态
    9. **未来优化**: EntryP 内存布局优化, 更激进的 threshold 策略
    10. **迁移检查清单**: 从 SCQ 迁移到 SCQP 的代码修改点, 测试验证步骤

## 验收标准
- [ ] SCQP 类编译成功，API 符合 `bool enqueue(T* ptr)` 和 `T* dequeue()` 规范
- [ ] 阈值机制 (4n-1)、满队列显式失败、nullptr consume 标记全部实现
- [ ] CAS2 运行时检测与 fallback 到 SCQ 索引模式自动工作
- [ ] 所有单元测试通过 (基础、并发、满队列、空队列、nullptr 拒绝、fallback 模式)
- [ ] 代码覆盖率 ≥90% (使用 `llvm-cov` + `LSCQ_ENABLE_COVERAGE=ON`)
- [ ] SCQP@16 线程吞吐量 >40 Mops/s
- [ ] SCQP 性能在 SCQ 的 90-100% 范围内 (指针模式合理开销)
- [ ] 基准测试报告包含 has_cas2_support() 状态，区分 CAS2 vs Fallback 结果
- [ ] Phase4-交接文档.md 完成 (≥200 行，10 章节，涵盖架构、实现、测试、性能、已知问题、未来优化)
- [ ] 无 TSan 警告 (entry_load 使用 CAS2 no-op 或 fallback mutex)

## 技术要点
- **Entry 模型**: `struct EntryP { uint64_t cycle_flags; T* ptr; }` (16B/16-align, ptr=nullptr 表示空槽)
- **阈值常量**: `THRESHOLD = 4 * QSIZE - 1` (区别于 SCQ 的 3n-1)
- **满队列检测**: `(tail - head) >= SCQSIZE` 返回 false，不自旋 (区别于 SCQ 行为)
- **Consume 标记**: dequeue 时使用 `atomic_exchange(entry.ptr, nullptr)` 替代 atomic_or 索引标记
- **CAS2 Fallback**:
  - 运行时检测: `has_cas2_support()` 缓存 CPUID CX16 标志
  - Fallback 策略: 内部维护 `std::unique_ptr<T*[]> ptr_array_` + SCQ 索引模式
  - API 透明性: enqueue/dequeue 外部接口不变，内部自动路由
- **性能目标**:
  - SCQP@16 线程 >40 Mops/s (G3.1 目标)
  - SCQP 性能 90-100% of SCQ (G3.2 目标，指针模式合理开销)
- **覆盖率要求**: ≥90% (严格匹配 Phase 3 标准)
- **TSan 兼容性**: entry_load 使用 CAS2 no-op 或 fallback mutex，避免 data race 误报
- **C++17 约束**: 不使用 C++20 特性，仅使用 CAS2 (128 位原子操作) 或 mutex fallback
- **文档质量**: Phase4-交接文档.md 遵循 Phase 3 格式 (>200 行，10 章节，包含代码示例、性能图表、迁移指南)
- **测试框架**: GoogleTest (单元测试) + GoogleBenchmark (性能测试)
- **构建系统**: CMake Presets (windows-clang-debug/release)
- **关键不同点 (vs SCQ)**:
  - Entry 第二字段: `T* ptr` (SCQP) vs `uint64_t index` (SCQ)
  - 阈值: 4n-1 (SCQP) vs 3n-1 (SCQ)
  - 满队列: 显式失败 (SCQP) vs 自旋等待 (SCQ, tests/unit/test_scq.cpp:122-150)
  - Consume: nullptr exchange (SCQP) vs atomic_or index (SCQ)
  - Fallback: 自动降级到索引+指针数组 (SCQP) vs 直接使用 mutex (SCQ CAS2 不可用时)
