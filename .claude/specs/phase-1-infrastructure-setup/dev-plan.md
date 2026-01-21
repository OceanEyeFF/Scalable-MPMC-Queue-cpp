# 阶段一基础设施搭建 - 开发计划

## 概述
搭建 LSCQ C++ 项目的完整基础设施，包括目录结构、CMake 构建系统、CAS2 抽象层、测试框架集成、代码检查工具和 CI/CD 流程。

## 任务分解

### 任务 1: 项目骨架与 CMake 核心配置
- **ID**: task-1
- **type**: default
- **描述**: 创建项目目录结构并配置 CMake 构建系统，支持 C++17、编译器检测、构建选项和 Sanitizers
- **文件范围**:
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\CMakeLists.txt` (根 CMake 文件)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\CMakePresets.json` (CMake 预设配置)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\CMakeLists.txt` (测试构建配置)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\CMakeLists.txt` (基准测试构建配置)
  - 新建目录: `include/`, `src/`, `tests/`, `benchmarks/`, `.github/workflows/`, `docs/`
- **依赖关系**: 无
- **测试命令**:
  ```bash
  cd e:\gitee\Scaleable-MPMC-Queue-cpp
  cmake --preset windows-clang-debug
  cmake --build build/debug
  ctest --test-dir build/debug --output-on-failure
  ```
- **测试重点**:
  - CMake 配置成功，无错误或警告
  - 构建系统能够检测到 Clang 编译器
  - 构建选项 `LSCQ_BUILD_TESTS`, `LSCQ_ENABLE_CAS2`, `LSCQ_ENABLE_SANITIZERS` 正常工作
  - 目录结构完整且符合设计
  - Google Test 和 Google Benchmark 通过 FetchContent 成功集成

### 任务 2: CAS2 抽象层实现
- **ID**: task-2
- **type**: default
- **描述**: 实现 CAS2 抽象层，包括平台检测、x86-64 原生支持、std::mutex 回退机制和运行时检测
- **文件范围**:
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\config.hpp` (平台检测宏定义)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\cas2.hpp` (CAS2 公共 API)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\src\cas2.cpp` (运行时检测实现，可选)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\include\lscq\detail\platform.hpp` (平台工具，可选)
- **依赖关系**: 依赖 task-1
- **测试命令**:
  ```bash
  cd e:\gitee\Scaleable-MPMC-Queue-cpp
  cmake --preset windows-clang-debug -DLSCQ_ENABLE_CAS2=ON
  cmake --build build/debug
  ctest --test-dir build/debug --output-on-failure
  ```
- **测试重点**:
  - CAS2 API 编译无错误
  - 头文件可正常包含和使用
  - `has_cas2_support()` 函数能正确检测平台能力
  - `cas2()` 函数签名正确 (接受 `Entry` 结构)
  - 在 x86-64 平台上使用原生 CMPXCHG16B 指令
  - 在不支持的平台上回退到 std::mutex 实现

### 任务 3: CAS2 单元测试与基准测试
- **ID**: task-3
- **type**: default
- **描述**: 编写全面的 CAS2 单元测试，覆盖 4 大测试类别，确保代码覆盖率 ≥90%，并实现基准测试
- **文件范围**:
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\tests\unit\test_cas2.cpp` (单元测试)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\benchmarks\benchmark_cas2.cpp` (基准测试)
- **依赖关系**: 依赖 task-1, task-2
- **测试命令**:
  ```bash
  # 测试原生 CAS2 支持
  cd e:\gitee\Scaleable-MPMC-Queue-cpp
  cmake --preset windows-clang-debug -DLSCQ_ENABLE_CAS2=ON -DLSCQ_ENABLE_SANITIZERS=ON
  cmake --build build/debug
  ctest --test-dir build/debug --output-on-failure

  # 测试回退机制
  cmake --preset windows-clang-debug -DLSCQ_ENABLE_CAS2=OFF -DLSCQ_ENABLE_SANITIZERS=ON
  cmake --build build/debug
  ctest --test-dir build/debug --output-on-failure

  # 运行基准测试
  cd build/debug/benchmarks
  ./benchmark_cas2
  ```
- **测试重点**:
  - **BasicOperation 测试**: 成功的 CAS2 操作，验证值更新正确性
  - **FailedOperation 测试**: 失败的 CAS2 操作，验证值保持不变
  - **Concurrent 测试**: 多线程并发 CAS2 操作，验证线程安全性
  - **RuntimeDetection 测试**: 验证 `has_cas2_support()` 返回值正确
  - AddressSanitizer 无内存泄漏或非法访问
  - ThreadSanitizer 无数据竞争
  - 代码覆盖率 ≥90% (使用 `--coverage` 编译选项)
  - 基准测试能正常运行并输出性能数据

### 任务 4: CI 流程、代码格式化与交接文档
- **ID**: task-4
- **type**: default
- **描述**: 配置 GitHub Actions CI 工作流（Windows + Clang）、设置 clang-format 代码格式化规则、创建阶段一交接文档
- **文件范围**:
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\.github\workflows\ci.yml` (CI 工作流配置)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\.clang-format` (代码格式化配置)
  - `e:\gitee\Scaleable-MPMC-Queue-cpp\docs\Phase1-交接文档.md` (交接文档)
- **依赖关系**: 依赖 task-1, task-3
- **测试命令**:
  ```bash
  # 本地验证 CI 步骤
  cd e:\gitee\Scaleable-MPMC-Queue-cpp

  # 格式检查
  clang-format --dry-run --Werror include/**/*.hpp src/**/*.cpp tests/**/*.cpp

  # 构建与测试 (Debug)
  cmake --preset windows-clang-debug -DLSCQ_ENABLE_SANITIZERS=ON
  cmake --build build/debug
  ctest --test-dir build/debug --output-on-failure

  # 构建与测试 (Release)
  cmake --preset windows-clang-release
  cmake --build build/release
  ctest --test-dir build/release --output-on-failure

  # 验证 GitHub Actions (需要 Git 仓库并推送到 GitHub)
  git push origin main
  ```
- **测试重点**:
  - GitHub Actions CI 工作流成功运行
  - Windows + Clang 环境下构建成功
  - Debug 和 Release 配置均编译通过，无警告
  - 所有测试在 CI 环境中通过
  - Sanitizers 检查通过（AddressSanitizer + ThreadSanitizer）
  - clang-format 检查通过，代码风格符合 Google Style（IndentWidth=4）
  - 交接文档完整，包含全部 7 个必需章节:
    1. 阶段目标与完成情况
    2. 技术实现总结
    3. 关键代码说明
    4. 测试覆盖率报告
    5. 已知问题与限制
    6. 后续开发建议
    7. 构建与测试指南
  - 交接文档篇幅 ≥200 行，内容详实

## 验收标准
- [ ] **G1**: Debug 和 Release 配置下编译成功，零警告
- [ ] **G2**: 所有 CAS2 单元测试通过，代码覆盖率 ≥90%
- [ ] **G3**: Sanitizers 测试通过（无数据竞争、无内存泄漏）
- [ ] **G4**: GitHub Actions CI 流程配置完成并通过
- [ ] **G5**: 代码格式符合规范（clang-format 检查通过）
- [ ] **G6**: 交接文档完整，包含全部 7 个章节，篇幅 ≥200 行
- [ ] **G7**: 项目目录结构完整（include/, src/, tests/, benchmarks/, .github/workflows/, docs/）
- [ ] **G8**: CMake 构建选项正常工作（LSCQ_BUILD_TESTS, LSCQ_ENABLE_CAS2, LSCQ_ENABLE_SANITIZERS）
- [ ] **G9**: CAS2 抽象层在 x86-64 和回退模式下均正常工作

## 技术要点
- **库架构**: `lscq` 为 INTERFACE 库（仅头文件），可选 `lscq_impl` STATIC 库（运行时检测代码）
- **CAS2 实现**: 分离式设计，头文件 + .cpp 文件支持运行时检测
- **CAS2 API**:
  - `struct Entry { uintptr_t value; uintptr_t tag; }`
  - `bool has_cas2_support()`
  - `bool cas2(Entry* ptr, Entry& expected, const Entry& desired)`
- **回退机制**: 不支持 CAS2 的平台使用 std::mutex 保证线程安全
- **构建选项**:
  - `LSCQ_BUILD_TESTS=ON/OFF` (默认 ON)
  - `LSCQ_ENABLE_CAS2=ON/OFF` (默认 ON)
  - `LSCQ_ENABLE_SANITIZERS=ON/OFF` (默认 OFF)
- **Sanitizers**: AddressSanitizer 和 ThreadSanitizer 通过 CMake 开关控制；Windows 环境 TSan 支持有限，建议在 Linux 上进行完整测试
- **代码风格**: Google Style，IndentWidth=4，使用 clang-format 强制执行
- **CI 平台**: GitHub Actions，主要测试 Windows + Clang 组合
- **测试框架**: Google Test (单元测试) + Google Benchmark (性能测试)
- **依赖管理**: 使用 CMake FetchContent 自动下载和构建依赖
- **平台支持**: 优先支持 x86-64 平台的 CMPXCHG16B 指令，其他平台回退到互斥锁
- **文档要求**: 交接文档需详细记录实现细节、测试结果、已知问题和后续开发路线图
