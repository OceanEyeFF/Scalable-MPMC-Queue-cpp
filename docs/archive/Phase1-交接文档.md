# Phase 1 交接文档（基础设施搭建）

文档版本：v1  
最后更新：2026-01-18  
适用分支：`main`（若实际分支名不同，请同步修改 CI 触发分支）  

本交接文档覆盖 Phase 1（基础设施搭建）在代码库中的落地结果，并提供可重复的本地/CI 构建与测试指引。

---

## 1. 阶段目标与完成情况

### 1.1 阶段目标

Phase 1 的目标是把项目从“零散代码”提升为“可持续开发的工程化项目”，核心包括：

- 目录结构固定化（`include/`、`src/`、`tests/`、`benchmarks/`、`docs/`、`.github/workflows/`）
- CMake 构建系统（Clang/Windows 优先），支持 Debug/Release
- Sanitizers（以 AddressSanitizer 为主，ThreadSanitizer 在 Linux 侧验证）
- 单元测试 + 基准测试（GoogleTest / Google Benchmark）
- 代码格式化（clang-format）与 CI 自动化（GitHub Actions）

### 1.2 完成情况（对照任务列表）

- task-1（项目骨架与 CMake）：已完成
  - CMakeLists 主配置、测试/基准测试子工程可构建
  - CMake Presets 提供 Windows/Clang 的本地构建入口
- task-2（CAS2 抽象层）：已完成
  - 提供 `lscq::Entry`、`lscq::cas2()`、`lscq::has_cas2_support()` 等 API
  - x86-64/Windows/clang-cl 路径使用 `_InterlockedCompareExchange128`
  - 非对齐/非支持平台回退到互斥锁路径
- task-3（单元测试与基准测试）：已完成
  - `tests/unit` 覆盖基础功能、失败路径、并发与运行时检测
  - `benchmarks/` 提供基准测试入口（用于性能回归）
- task-4（CI、格式化、交接文档）：已完成
  - GitHub Actions CI：Windows clang-cl（Debug/Release）+ Linux ASan/TSan
  - clang-format：Google Style 派生（`IndentWidth=4`）
  - 本交接文档：包含 7 个必需章节，并给出可复现的操作步骤

### 1.3 关键产物清单

- CI 工作流：`.github/workflows/ci.yml`
- 代码格式：`.clang-format`
- 交接文档：`docs/Phase1-交接文档.md`

---

## 2. 技术实现总结

### 2.1 工程结构与模块边界

- `include/lscq/`：公共头文件（以 header-only 的方式对外暴露能力）
  - `include/lscq/config.hpp`：平台/编译器/功能开关宏与 constexpr 视图
  - `include/lscq/cas2.hpp`：CAS2 API、回退实现与（在 Windows x86-64）原生实现
  - `include/lscq/detail/platform.hpp`：对齐判断、CPU 特性检测等底层工具
- `src/`：实现文件（目前 `src/cas2.cpp` 为占位 TU，便于未来做运行时检测/平台实现扩展）
- `tests/`：GoogleTest 单元测试（通过 `gtest_discover_tests` 注册）
- `benchmarks/`：Google Benchmark 基准测试（默认关闭 benchmark 自带测试与 Werror）

### 2.2 构建系统设计要点（CMake）

- 目标库拆分：
  - `lscq`：`INTERFACE` 库，向外暴露 include path、编译定义、警告/消毒器等“使用要求”
  - `lscq_impl`：`STATIC` 库，目前仅包含 `src/cas2.cpp`，为未来扩展留接口
- 警告策略：
  - MSVC：`/W4 /permissive-`
  - 非 MSVC：`-Wall -Wextra -Wpedantic`
- Sanitizers：
  - `LSCQ_ENABLE_SANITIZERS=ON` 时，优先启用 AddressSanitizer（ASan）
  - Windows clang-cl 下包含“探测编译+运行”的校验逻辑，并自动定位/复制 ASan runtime DLL
  - ThreadSanitizer（TSan）不通过该开关统一启用：在 CI 的 Linux job 以编译/链接旗标单独验证
- clang-cl + 16-byte 原子：
  - 为避免 Release 下出现 `__atomic_compare_exchange_16` 这类无法解析的 libcall，CMake 在
    `MSVC + Clang` 且 `LSCQ_ENABLE_CAS2=ON` 时为依赖方追加 `/clang:-mcx16`。

### 2.3 CI 总览（GitHub Actions）

CI 目标是“格式一致 + 多平台构建测试 + Sanitizers 兜底”，当前配置：

- `format` job（Ubuntu）：
  - 运行 `clang-format --dry-run --Werror`，对 `include/**/*.hpp`、`src/**/*.cpp`、`tests/**/*.cpp` 强制风格一致
- `windows-clang` job（Windows clang-cl）：
  - Debug：启用 `LSCQ_ENABLE_SANITIZERS=ON`（ASan），编译 + 单测
  - Release：关闭 sanitizers（更贴近发布形态），编译 + 单测
- `linux-sanitizers` job（Ubuntu clang++）：
  - ASan：`LSCQ_ENABLE_SANITIZERS=ON`
  - TSan：通过 `-fsanitize=thread` 编译/链接旗标启用（不走 `LSCQ_ENABLE_SANITIZERS` 开关）

---

## 3. 关键代码说明

### 3.1 CAS2 关键数据结构：`lscq::Entry`

位置：`include/lscq/cas2.hpp`

- `Entry` 以 16 字节对齐（`alignas(16)`），大小固定为 16 bytes
- 两个 64-bit 字段用于“循环标记/flags + index/pointer”的组合
- 提供 `operator== / operator!=` 便于测试断言与预期值更新

### 3.2 CAS2 API：`lscq::cas2()` 与预期更新语义

位置：`include/lscq/cas2.hpp`

- `bool cas2(Entry* ptr, Entry& expected, const Entry& desired)`
  - 成功：`*ptr` 从 `expected` 原子更新为 `desired`，返回 `true`
  - 失败：读取当前值写回到 `expected`，返回 `false`
  - `ptr == nullptr`：直接返回 `false`（测试覆盖该分支）

### 3.3 回退实现：`detail::cas2_mutex`

位置：`include/lscq/cas2.hpp`

- 使用全局互斥锁（`detail::g_cas2_fallback_mutex`）保证“比较 + 写入”的原子性
- 适用于：
  - 平台/架构不支持 128-bit CAS
  - 指针不满足 16-byte 对齐（避免未定义行为或硬件异常）

### 3.4 原生实现（Windows x86-64）：`detail::cas2_native`

位置：`include/lscq/cas2.hpp`

- 条件编译：
  - `LSCQ_ARCH_X86_64 && LSCQ_PLATFORM_WINDOWS && LSCQ_COMPILER_MSVC`
  - 在 clang-cl 下，`_MSC_VER` 存在，因此走 intrinsics 路径
- 关键点：
  - 通过 `detail::is_aligned_16(ptr)` 检查对齐，未对齐则回退到 mutex 路径
  - 使用 `_InterlockedCompareExchange128` 完成 128-bit CAS

### 3.5 运行时能力检测：`has_cas2_support()`

位置：`include/lscq/cas2.hpp` / `include/lscq/detail/platform.hpp`

- 当 `LSCQ_ENABLE_CAS2=OFF`：
  - `has_cas2_support()` 直接返回 `false`（测试验证该分支）
- 当 `LSCQ_ENABLE_CAS2=ON`：
  - x86-64：基于 CPU feature（`CMPXCHG16B`）检测
  - 其它架构：返回 `false`

### 3.6 平台/开关配置：`config.hpp`

位置：`include/lscq/config.hpp`

- 通过 CMake 传入的宏映射为 constexpr：
  - `kEnableCas2`
  - `kEnableSanitizers`
  - `kCompilerIsClang`
- 在头文件独立使用时，对关键宏提供默认值（避免未定义宏导致编译错误）

---

## 4. 测试覆盖率报告

### 4.1 测试集概览

位置：`tests/unit/test_cas2.cpp`、`tests/unit/test_smoke.cpp`

- `Cas2.EntryLayout`：结构体大小/对齐保证
- `Cas2_BasicOperation.*`：成功/失败/空指针等基础语义
- `Cas2_Concurrent.MultiThreadedIncrementIsCorrect`：并发 CAS 语义与线程安全
- `Cas2_RuntimeDetection.HasCas2SupportMatchesConfigurationAndCpu`：运行时检测逻辑
- `Smoke.HeaderIncludesAndMacrosAreCoherent`：宏与 constexpr 映射一致性

### 4.2 覆盖率现状（Line Coverage）

说明：

- 当前项目未在 CI 中自动产出覆盖率报告工件（Artifacts）。
- 若需要持续化覆盖率追踪，建议在后续阶段引入 `llvm-profdata/llvm-cov`（Linux）或 `llvm-cov gcov`（Windows `--coverage`）并上传报告。

本仓库本地曾以 `--coverage` 模式运行测试并生成 `.gcno/.gcda`，可用 `llvm-cov gcov` 得到典型 line coverage（示例）：

- `include/lscq/cas2.hpp`：≈ 90%+（核心分支已覆盖，少量容错/平台分支在单一平台下无法触达）
- `include/lscq/detail/platform.hpp`：≈ 100%（对齐/特性检测路径基本可达）
- `tests/unit/*`：接近 100%（测试本身的可执行语句基本全走到）

### 4.3 生成覆盖率报告（推荐流程）

#### 方案 A：Windows（clang-cl + `--coverage`）

1) 以覆盖率旗标构建（示例：Debug + Ninja）：

```powershell
cmake -S . -B build/coverage -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_C_FLAGS="--coverage" `
  -DCMAKE_CXX_FLAGS="--coverage"
cmake --build build/coverage
ctest --test-dir build/coverage --output-on-failure
```

2) 用 `llvm-cov gcov` 汇总（会生成 `.gcov` 输出文件，建议在临时目录执行）：

```powershell
Push-Location build/coverage
llvm-cov gcov (Get-ChildItem -Recurse -Filter *.gcno | % { $_.FullName })
Pop-Location
```

#### 方案 B：Linux（clang + `llvm-profdata/llvm-cov`）

- 该方案更适合后续接入 CI 工件（HTML/LCOV），并且可与 Sanitizers job 协同。

---

## 5. 已知问题与限制

### 5.1 Windows 上的 ThreadSanitizer（TSan）限制

- Windows clang-cl/MSVC runtime 组合下，TSan 支持通常受限（工具链/运行库/平台限制）。
- 当前 CI 选择在 Linux job 中以 `-fsanitize=thread` 验证数据竞争问题。
- Windows job 主要承担 ASan（内存错误）与 Debug/Release 构建回归。

### 5.2 CMake Presets 的可移植性

- `CMakePresets.json` 中的编译器与 Ninja 路径为本机路径示例（便于本地一键构建）。
- 在 CI 中不直接依赖这些 presets，而是显式指定 `clang-cl` 与 `Ninja`，避免路径耦合。
- 若后续希望 CI 与 presets 完全一致，建议将 presets 改为“相对路径/环境探测”方式（Phase 2 可做）。

### 5.3 覆盖率未在 CI 中落地

- 当前 CI 不上传覆盖率报告（仅保证构建、测试、sanitizers、格式通过）。
- 若项目需要质量门禁（coverage threshold），建议引入覆盖率 job 与阈值检查。

---

## 6. 后续开发建议

### 6.1 功能层面（进入 Phase 2 前的建议）

- 明确队列 API（enqueue/dequeue、容量策略、内存序语义），并补齐接口文档
- 为 MPMC 关键路径补充微基准（不同线程数/不同 payload）的对比报告
- 对 CAS2 失败率、退避策略（backoff）、伪共享（false sharing）做专项验证

### 6.2 工程层面（持续集成/质量）

- 将 clang-format 扩展到 `benchmarks/**/*.cpp`（当前格式检查只覆盖 include/src/tests）
- 引入 `clang-tidy`（可选：只对增量文件执行）以提前发现未定义行为/性能陷阱
- 在 CI 中引入 coverage artifacts（HTML/LCOV），并在 PR 上展示 summary

### 6.3 平台与工具链

- 若需要在 Windows 上补齐“数据竞争”类验证，可考虑：
  - 在 WSL2/Ubuntu 环境运行 TSan
  - 或在 Linux CI 中提升测试负载/并发强度（增加竞争暴露概率）

---

## 7. 构建与测试指南

### 7.1 clang-format（格式检查）

> 说明：Windows PowerShell 不会自动展开 `**/*.cpp` 这种 glob；建议用脚本收集文件列表。

#### Bash（GitHub Actions / Linux / Git-Bash）

```bash
clang-format --dry-run --Werror include/**/*.hpp src/**/*.cpp tests/**/*.cpp
```

#### PowerShell（推荐）

```powershell
$files = @()
$files += Get-ChildItem include -Recurse -Filter *.hpp | % { $_.FullName }
$files += Get-ChildItem src -Recurse -Filter *.cpp | % { $_.FullName }
$files += Get-ChildItem tests -Recurse -Filter *.cpp | % { $_.FullName }
clang-format --dry-run --Werror @files
```

### 7.2 构建与测试（Windows + Clang）

#### Debug（可选启用 ASan）

```powershell
cmake --preset windows-clang-debug -DLSCQ_ENABLE_SANITIZERS=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

#### Release

```powershell
cmake --preset windows-clang-release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

### 7.3 基准测试（可选）

```powershell
cmake --preset windows-clang-debug
cmake --build build/debug
.\build\debug\benchmarks\benchmark_cas2.exe
```

### 7.4 CI 本地对齐验证（建议顺序）

1) `clang-format` 检查
2) Debug + ASan：`LSCQ_ENABLE_SANITIZERS=ON`
3) Release：无 sanitizers
4) （可选）在 Linux/WSL2 下跑 TSan

---

附：Coverage summary（如需在 PR 中展示）

- 当前 CI 未输出 coverage summary。
- 推荐做法：Linux job 生成 `llvm-cov` 报告并上传为 artifacts，同时在 job summary 写入覆盖率摘要（Phase 2 建议项）。
