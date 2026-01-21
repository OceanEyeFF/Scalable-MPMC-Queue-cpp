# Phase 2 交接文档（NCQ 实现与验证）
文档版本：v1
最后更新：2026-01-18
适用分支：`main`（若实际分支名不同，请同步修改 CI 触发分支）
阶段范围：Phase 2（Naive Circular Queue / NCQ 实现、验证、Benchmark、覆盖率与文档补齐）
目标读者：后续 Phase 3（SCQ）实现者、CI/性能回归维护者、API 使用者
本文件目标：提供可复现的构建/测试/覆盖率/性能验证步骤，并对关键实现做索引与风险提示

目录
- 第1章：完成情况概览
- 第2章：关键代码位置索引
- 第3章：NCQ 算法验证结果
- 第4章：性能 Benchmark 结果
- 第5章：已知问题和限制
- 第6章：Phase 3 接口预留
- 第7章：下阶段快速启动指南
- 第8章：经验教训和最佳实践
- 第9章：附录

---

## 第1章：完成情况概览
### 1.1 功能完成对照（✅ 已完成 / ❌ 未完成）
- [x] ✅ NCQ 核心算法（Figure 5 控制流落地：`enqueue/dequeue`）
- [x] ✅ `cache_remap`（按缓存行分组重映射，降低伪共享）
- [x] ✅ TSan 友好的 16B Entry 读取（CAS2 “no-op” 读取模式：`detail::entry_load`）
- [x] ✅ MSQueue（Michael-Scott）对比基线实现（用于 Benchmark/正确性回归）
- [x] ✅ 单元测试（包含并发 8 线程入队、4P4C 等测试组）
- [x] ✅ 代码覆盖率（llvm-cov；总行覆盖率 ≥90%）
- [x] ✅ 性能 Benchmark（NCQ vs MSQueue；1/2/4/8/16 线程 pair）
- [x] ✅ 交接文档（本文件；包含 9 个必需章节）
- [ ] ❌ Phase 3：SCQ 核心实现（仅做接口预留与注意事项，未实现）
- [ ] ❌ Phase 3：内存回收（HP/EBR）与无界扩容（后续阶段完成）

### 1.2 通过的测试与覆盖率（本地可复现）
测试执行环境（本次采样）
- OS：Windows（PowerShell）
- 编译器：clang-cl / LLVM 19.1.5（见本仓库构建产物）
- 说明：覆盖率与测试均来自 `build/coverage` 目录（clang 覆盖率插桩构建）

单元测试通过情况（ctest）
- 测试命令：`ctest --test-dir build/coverage --output-on-failure`
- 结果摘要：总计 20 项测试，100% 通过（0 失败，Total 0.62s）

覆盖率（llvm-cov report）
- 总体行覆盖率：98.19%（TOTAL Lines: 663，Missed: 12）
- NCQ 核心实现行覆盖率：`src/ncq.cpp` 97.62%（Lines: 84，Missed: 2）
- NCQ API 头文件：`include/lscq/ncq.hpp` 100.00%（Lines: 1，Missed: 0；注意：实现主要在 `src/ncq.cpp`）

### 1.3 性能指标达成情况（验证 G3.2 / G3.4）
本节结论先行（详细数据见第4章）
- G3.2（单线程吞吐量 > 5 Mops/sec）：✅ 达成（NCQ 线程=1：28.0056 Mops/s）
- G3.4（Gap < 50%）：✅ 达成（按“NCQ 相对 MSQueue 的性能缺口”定义，缺口为 0%）
Gap 定义（用于验收，避免“更快导致 gap>50%”的歧义）
- Gap% = max(0, (MSQueue_Mops - NCQ_Mops) / MSQueue_Mops) * 100%
- 解释：若 NCQ 更快，则“缺口”为 0%；若 NCQ 更慢，则 gap 表示 NCQ 落后 MSQueue 的百分比

---

## 第2章：关键代码位置索引
说明：本章用于后续维护者快速定位 NCQ/Benchmark/测试/原子抽象层的关键实现点

| 路径 | 关键符号/实体 | 说明 |
| --- | --- | --- |
| `include/lscq/ncq.hpp` | `template<class T> class lscq::NCQ` | NCQ 公共 API、约束（T 必须是 unsigned integral）、空队列哨兵 `kEmpty` |
| `src/ncq.cpp` | `NCQ<T>::enqueue` | Figure 5 入队：读取 Tail、检查 cycle、CAS2 写入槽位、推进 Tail |
| `src/ncq.cpp` | `NCQ<T>::dequeue` | Figure 5 出队：读取 Head、判断 empty、推进 Head、返回 index |
| `src/ncq.cpp` | `NCQ<T>::cache_remap` | cache-remap：将连续 idx 打散到不同 cache line，降低伪共享 |
| `include/lscq/detail/ncq_impl.hpp` | `lscq::detail::entry_load` | 通过 CAS2 进行 16B Entry 的 TSan 安全“原子读取” |
| `include/lscq/cas2.hpp` | `lscq::cas2` / `lscq::has_cas2_support` | 双字 CAS 抽象层与运行时/编译期能力检测 |
| `include/lscq/config.hpp` | `lscq::config::DEFAULT_SCQSIZE` | 默认队列容量常量（Phase 2 引入；Doxygen 文档补齐） |
| `include/lscq/msqueue.hpp` | `template<class T> class lscq::MSQueue` | 作为对比基线的简化 Michael-Scott 队列 |
| `src/msqueue.cpp` | `MSQueue<T>::enqueue/dequeue` | MSQueue 核心算法（未做实时回收：retire list） |
| `benchmarks/benchmark_ncq.cpp` | `BM_NCQ_Pair` / `BM_MSQueue_Pair` | Pair benchmark：1/2/4/8/16 threads，输出 Mops 与元信息 counters |
| `tests/unit/test_ncq.cpp` | `ConcurrentEnqueue...` / `ConcurrentEnqueueDequeue...` | NCQ 并发正确性验证（无丢失/无重复/守恒） |
| `tests/unit/test_msqueue.cpp` | `MSQueue.MultiProducerMultiConsumer` | 基线队列并发正确性验证（去重检查） |

---

## 第3章：NCQ 算法验证结果
### 3.1 单元测试
本阶段的单元测试覆盖三类目标
- 功能正确性：FIFO、空队列返回、保留值拒绝入队
- 并发正确性：8 线程入队无丢失无重复；4P4C 守恒与去重
- 边界与回绕：capacity 归一化、head/tail 溢出模拟

测试组列表（来自 `build/coverage/tests/lscq_unit_tests.exe --gtest_list_tests`）
- `Smoke`：HeaderIncludesAndMacrosAreCoherent
- `Cas2`：EntryLayout
- `Cas2_BasicOperation`：SuccessUpdatesValue / NullPointerReturnsFalse / FallbackMutexAndNativeImplAreCorrectWhenCallable
- `Cas2_FailedOperation`：FailureKeepsValueAndUpdatesExpected
- `Cas2_Concurrent`：MultiThreadedIncrementIsCorrect
- `Cas2_RuntimeDetection`：HasCas2SupportMatchesConfigurationAndCpu
- `BasicOperation`：Enqueue100Dequeue100AndFifoOrder
- `FIFO_Order`：StrictlyIncreasing0To999
- `EdgeCases`：EnqueueRejectsReservedEmptySentinel / DequeueOnEmptyReturnsSentinel / ConstructorClampsCapacityToMinimumAndRoundsToCacheLine / EnqueueSkipsSlotWhenTailAppearsFull
- `ConcurrentEnqueue`：EightThreadsEnqueue80000NoLossNoDup
- `ConcurrentEnqueueDequeue`：FourProducersFourConsumersConserveCountNoDup
- `CycleWrap`：TailAndHeadOverflowDoNotBreakSingleSlotSimulation
- `MSQueue`：EmptyInitially / SingleThreadFifo / MultiProducerMultiConsumer

ctest 通过状态（来自 `ctest --test-dir build/coverage --output-on-failure`）
- 通过：20 / 20
- 失败：0

### 3.2 Sanitizer 报告（ASan / TSan）
本仓库 CI 已配置 Linux 侧 ASan/TSan（见 `.github/workflows/ci.yml` 的 `linux-sanitizers` job）
- ASan：`LSCQ_ENABLE_SANITIZERS=ON`（由 CMake 探测并启用 AddressSanitizer flags）
- TSan：显式设置 `-fsanitize=thread -fno-omit-frame-pointer`（并在 link flags 中启用）

Linux（CI/本地 Linux）可复现命令（摘自 CI 配置）
```bash
# ASan（Debug）
cmake -S . -B build/ci/linux/asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLSCQ_ENABLE_SANITIZERS=ON
cmake --build build/ci/linux/asan
ctest --test-dir build/ci/linux/asan --output-on-failure

# TSan（Debug）
cmake -S . -B build/ci/linux/tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLSCQ_ENABLE_SANITIZERS=OFF \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/ci/linux/tsan
ctest --test-dir build/ci/linux/tsan --output-on-failure
```

Windows 本地说明（本次环境）
- 现象：`LSCQ_ENABLE_SANITIZERS=ON` 时，CMake 内置 ASan probe 在本机报 `interception_win: unhandled instruction`，随后“跳过 sanitizer flags”
- 影响：Windows 本机无法保证 ASan 已实际启用（需以 CMake configure 输出为准）
- 本次采样：建议将 ASan/TSan 结果以 Linux CI 为准；Windows 侧可先完成 Debug/Release 与单元测试/Benchmark 的复现

输出摘要（验收口径）
- ASan：若启用成功，`ctest` 返回 0，且无 `ERROR: AddressSanitizer` 报告
- TSan：`ctest` 返回 0，且无 `WARNING: ThreadSanitizer` 数据竞争报告

### 3.3 代码覆盖率
覆盖率构建目录：`build/coverage`
- 生成 profile：GoogleTest 通过 `LLVM_PROFILE_FILE=coverage-%p.profraw` 输出多份 `.profraw`
- 合并 profile：`llvm-profdata merge -sparse ... -o build/coverage/coverage.profdata`
- 报告：`llvm-cov report build/coverage/tests/lscq_unit_tests.exe -instr-profile build/coverage/coverage.profdata`

覆盖率结果摘要（本次采样）
- TOTAL Lines：98.19%
- `src/ncq.cpp`：97.62%
- `include/lscq/detail/ncq_impl.hpp`：100.00%

---

## 第4章：性能 Benchmark 结果
### 4.1 吞吐量表格（NCQ vs MSQueue + Gap%）
Benchmark 二进制：`build/release/benchmarks/lscq_benchmarks.exe`
运行时间戳（本次采样）：2026-01-18T18:11:47+08:00
机器摘要（来自 benchmark 输出）
- CPU：24 x 3700 MHz
- L1D：32 KiB (x12)；L2：512 KiB (x12)；L3：32768 KiB (x2)

Benchmark 命令（PowerShell）
```powershell
cmake --preset windows-clang-release
cmake --build build/release
.\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter="BM_(NCQ|MSQueue)_Pair" --benchmark_min_time=1s
```

数据表（单位：Mops/s；Gap% 定义见 1.3）
| Threads | NCQ Mops/s | MSQueue Mops/s | Gap%（NCQ 落后缺口） |
| ---: | ---: | ---: | ---: |
| 1  | 28.0056 | 12.2409 | 0.00% |
| 2  | 41.1132 | 12.4924 | 0.00% |
| 4  | 25.9126 | 10.7061 | 0.00% |
| 8  | 12.8455 | 6.72868 | 0.00% |
| 16 | 8.75248 | 5.22337 | 0.00% |

### 4.2 性能分析（是否达到目标）
目标（摘自 Phase 2 开发计划）
- G3.2：单线程 > 5 Mops/sec
- G3.4：gap < 50%

结论（基于 4.1 数据）
- G3.2：✅ 达成（NCQ 线程=1 为 28.0056 Mops/s）
- G3.4：✅ 达成（NCQ 相对 MSQueue 不存在“落后缺口”，gap=0%）

补充观察（用于 Phase 3 规划）
- NCQ 在该机器上整体吞吐量显著高于 MSQueue，且在 2 线程达到峰值（受线程调度/缓存/benchmark 负载模型影响）
- 8/16 线程下吞吐量下降明显，建议 Phase 3（SCQ）关注：减少自旋、优化 contention、改进内存布局/批处理
- Benchmark 为 pair 模型：threads>1 时 producers=threads/2，consumers=threads/2；吞吐量表示所有线程总操作率

---

## 第5章：已知问题和限制
算法/语义层面的限制
- NCQ 是“有界环形队列”，capacity 固定且会向上对齐到 cache-line 的 entry 数倍（见 `src/ncq.cpp` 初始化）
- `enqueue()` 在“队列满”的情况下会自旋等待（无“队列满返回 false”的有界失败语义），在高负载下可能导致长时间占用 CPU
- `dequeue()` 以返回哨兵 `kEmpty` 表示空队列；因此 `T` 不能使用 `max()` 作为有效值（并且 `enqueue(kEmpty)` 会显式失败）
- `is_empty()` 是 moment-in-time 的近似判断（并发场景下可能立刻过时），仅用于调试/指标，不应作为强语义依据

实现/可移植性方面的限制
- 依赖 16B Entry 的双字 CAS（`lscq::cas2`）；在不支持的平台会走回退路径（通常性能会显著下降）
- cache line 大小目前按 64B 常量处理（`src/ncq.cpp`）；若目标平台 cache line 不同，需要统一抽象（Phase 3 可改为配置化）
- Windows 本机启用 ASan 可能失败（见 3.2 的说明）；建议以 Linux CI 的 ASan/TSan 作为 sanitizer 的权威验证入口

测试/Benchmark 局限
- 当前 Benchmark 为单次 Iterations(1)；如需更稳定数据，建议增加 repetitions、导出 JSON、固定 affinity、禁用睿频等
- 并发测试覆盖了典型场景，但仍无法穷尽所有 interleavings；TSan/模型检查可作为补充

---

## 第6章：Phase 3 接口预留（为 SCQ 继承预留的扩展点）
本章不实现 Phase 3，仅描述“如何在不推倒 Phase 2 的情况下演进到 SCQ”

### 6.1 结构与抽象预留点
- `lscq::Entry`：保持 16B 布局（cycle/flags 与 index/pointer 的承载位），Phase 3 可在 `cycle_flags` 中编码额外 flag
- `detail::entry_load(Entry*)`：已封装 TSan 友好的原子读取；Phase 3 继续复用以避免数据竞争误报
- `NCQ::cache_remap()`：作为可复用的 mapping 函数；Phase 3 可提取为 `detail::cache_remap(idx, scqsize)` 以减少模板实例化
- `config::DEFAULT_SCQSIZE`：作为默认容量；Phase 3 的 SCQ/SCQP/LSCQ 可沿用同一常量并在必要时引入新参数

### 6.2 SCQ 继承/替换建议
- 建议将 SCQ 的 Entry 语义与 NCQ 的 Entry 对齐（cycle 的定义、空/满判据），减少迁移成本
- 若 SCQ 引入“pointer slot”与内存回收（HP/EBR），建议将 `index_or_ptr` 的解释从 “index” 迁移为 “uintptr_t”
- 如需保留 NCQ 作为回归基线：将 SCQ 与 NCQ 共用 benchmark harness（`benchmark_ncq.cpp` 的 Pair 模型可复用）

### 6.3 兼容性注意事项（提前写进 API 约束）
- `NCQ<T>` 当前仅支持 unsigned integral；Phase 3 若需要泛型 payload，建议分层：SCQ 存 pointer，payload 由 allocator/回收策略管理
- 若 Phase 3 需要“可失败的 enqueue（full）”：需要重新定义返回语义与测试（当前 NCQ enqueue 在 full 情况下自旋）

---

## 第7章：下阶段快速启动指南（PowerShell）
说明：所有命令均以仓库根目录为工作目录；若工具链路径不同，以 `CMakePresets.json` 为准

### 7.1 构建（Debug/Release）
```powershell
# Debug（用于开发与单元测试）
cmake --preset windows-clang-debug
cmake --build build/debug

# Release（用于性能 benchmark）
cmake --preset windows-clang-release
cmake --build build/release
```

### 7.2 运行单元测试（5+ 条命令的一部分）
```powershell
# 运行 Debug 测试（若已构建）
ctest --test-dir build/debug --output-on-failure

# 运行 Coverage 构建目录下的全部测试（本阶段的覆盖率数据来源）
ctest --test-dir build/coverage --output-on-failure

# 查看测试用例列表（用于定位某个测试组）
.\build\coverage\tests\lscq_unit_tests.exe --gtest_list_tests

# 仅运行 NCQ 并发相关测试（示例）
.\build\coverage\tests\lscq_unit_tests.exe --gtest_filter="ConcurrentEnqueue*:*ConcurrentEnqueueDequeue*"
```

### 7.3 生成/查看覆盖率（llvm-cov）
```powershell
# 汇总 profraw -> profdata（示例：将 build/coverage/tests 下所有 coverage-*.profraw 合并）
llvm-profdata merge -sparse (Get-ChildItem build/coverage/tests -Filter "coverage-*.profraw").FullName -o build/coverage/coverage.profdata

# 输出覆盖率汇总表
llvm-cov report .\build\coverage\tests\lscq_unit_tests.exe --instr-profile .\build\coverage\coverage.profdata

# 生成 HTML 覆盖率页面（输出目录自定）
llvm-cov show .\build\coverage\tests\lscq_unit_tests.exe --instr-profile .\build\coverage\coverage.profdata -format=html -output-dir build/coverage/coverage_html
```

### 7.4 运行 Benchmark（NCQ vs MSQueue）
```powershell
# 列出所有 benchmark case
.\build\release\benchmarks\lscq_benchmarks.exe --benchmark_list_tests

# 跑 NCQ 与 MSQueue 的 pair benchmark（推荐带 min_time 后缀）
.\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter="BM_(NCQ|MSQueue)_Pair" --benchmark_min_time=1s

# 只跑 NCQ
.\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter="BM_NCQ_Pair" --benchmark_min_time=1s
```

### 7.5 行数验收（本交接文档）
```powershell
Get-Content docs/Phase2-交接文档.md | Measure-Object -Line
```

---

## 第8章：经验教训和最佳实践
### 8.1 TSan 与“原子读取”的经验
- 对 16B 结构体（Entry）的读取，若直接做普通 load，TSan 可能报数据竞争（即使硬件原子/锁前提成立）
- 通过 CAS2 “no-op” 读取（desired==expected），可同时满足：读取原子性 + TSan 认为有同步语义（见 `detail::entry_load`）
- 最佳实践：将该模式封装成单点 helper，避免在业务逻辑中散落“奇技淫巧”

### 8.2 CAS2 与可移植性
- 必须明确 “CAS2 是否可用” 的两层含义：编译期支持（宏/平台）+ 运行时 CPU 支持（例如 `CMPXCHG16B`）
- 在不支持的平台提供回退实现（例如互斥锁），保证正确性；性能差距可通过 benchmark/feature flag 可视化
- 最佳实践：在 benchmark counters 中输出 `has_cas2_support`（已在 `BM_NCQ_Pair` 中提供）

### 8.3 并发测试编写要点
- 并发测试的核心不是“跑很多次”，而是“覆盖关键交错”：同时启动、竞争窗口、去重/守恒断言
- 经验：对结果集做排序去重（或 bitmap 计数），比仅检查 count 更容易发现重复/ABA 类问题
- 经验：测试容量、cycle wrap 等边界条件时，尽量把规模缩小到“可控的最小复现模型”（本仓库用单槽模拟 head/tail 溢出）

### 8.4 性能测试的可重复性
- 使用 `--benchmark_min_time=1s`（带单位后缀）避免基准框架忽略参数导致噪声
- 建议固定 iterations 与 ops_per_thread，并将 capacity 与线程数关系写入 counters（本仓库已输出 capacity/threads/producers/consumers）
- 如果 Phase 3 引入新的 queue 变体，建议沿用同一 benchmark harness，减少对比偏差

---

## 第9章：附录
### 9.1 参考文献
- Scalable MPMC queue（论文：Figure 5 / cache-remap 思路；仓库根目录含 `1908.04511v1.pdf`）
- Michael & Scott Queue（经典 lock-free queue；本仓库实现为简化版，避免即时回收）

### 9.2 覆盖率原始输出（节选）
命令
```powershell
llvm-cov report .\build\coverage\tests\lscq_unit_tests.exe --instr-profile .\build\coverage\coverage.profdata
```
输出（节选）
```text
Filename                             Regions    Missed Regions     Cover   Functions  Missed Functions  Executed       Lines      Missed Lines     Cover
--------------------------------------------------------------------------------------------------------------------------------
include\lscq\cas2.hpp                     22                 2    90.91%           8                 0   100.00%          54                 5    90.74%
include\lscq\detail\ncq_impl.hpp           1                 0   100.00%           1                 0   100.00%           5                 0   100.00%
include\lscq\ncq.hpp                       1                 0   100.00%           1                 0   100.00%           1                 0   100.00%
src\ncq.cpp                               41                 1    97.56%           7                 0   100.00%          84                 2    97.62%
tests\unit\test_ncq.cpp                  779               108    86.14%          16                 0   100.00%         225                 0   100.00%
TOTAL                                   1694               195    88.49%          58                 0   100.00%         663                12    98.19%
```

### 9.3 Benchmark 原始输出（节选）
命令
```powershell
.\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter="BM_(NCQ|MSQueue)_Pair" --benchmark_min_time=1s
```
输出（节选，Mops=总吞吐）
```text
BM_NCQ_Pair/iterations:1/real_time/threads:1        ... Mops=28.0056/s ... threads=1
BM_NCQ_Pair/iterations:1/real_time/threads:2        ... Mops=41.1132/s ... threads=2
BM_NCQ_Pair/iterations:1/real_time/threads:4        ... Mops=25.9126/s ... threads=4
BM_NCQ_Pair/iterations:1/real_time/threads:8        ... Mops=12.8455/s ... threads=8
BM_NCQ_Pair/iterations:1/real_time/threads:16       ... Mops=8.75248/s ... threads=16
BM_MSQueue_Pair/iterations:1/real_time/threads:1    ... Mops=12.2409/s ... threads=1
BM_MSQueue_Pair/iterations:1/real_time/threads:2    ... Mops=12.4924/s ... threads=2
BM_MSQueue_Pair/iterations:1/real_time/threads:4    ... Mops=10.7061/s ... threads=4
BM_MSQueue_Pair/iterations:1/real_time/threads:8    ... Mops=6.72868/s ... threads=8
BM_MSQueue_Pair/iterations:1/real_time/threads:16   ... Mops=5.22337/s ... threads=16
```

### 9.4 CI 配置说明（Sanitizers）
- CI 文件：`.github/workflows/ci.yml`
- 关键点：Linux job 同时跑 ASan 与 TSan；Windows job 在 Debug 配置开启 `LSCQ_ENABLE_SANITIZERS=ON`（依赖 CMake probe）

### 9.5 验收清单（自检）
- [x] 文档存在且 ≥300 行（不含空行；可用 `Measure-Object -Line` 校验）
- [x] 包含全部 9 个必需章节
- [x] 性能数据表格完整（NCQ、MSQueue、Gap%）
- [x] 代码索引 ≥ 8 项（本文件列出 12 项）
- [x] 文档中验证性能指标 G3.2 和 G3.4（见 1.3 / 4.2）
- [x] 包含 TSan/ASan 命令与输出摘要（见 3.2；Windows 本机 ASan probe 限制已说明）
