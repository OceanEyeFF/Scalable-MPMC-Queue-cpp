# 跨平台优化与完善 - 开发计划

## 概览
为 lscq 无锁队列库实现 ARM64 原生 CAS2 支持、优化 Fallback 降级策略、基于 profiling 驱动的性能优化、补全文档体系以及多平台 CI/CD 集成，确保跨平台编译验证和性能提升。

## 任务分解

### 任务 1: ARM64 CAS2 实现与平台检测
- **ID**: task-1
- **type**: default
- **描述**: 为 ARM64 架构实现原生 CAS2（基于 CASP 指令或 LL/SC 序列），并在 `platform.hpp` 中添加 ARM64 架构检测宏，确保编译时正确选择实现路径
- **文件范围**:
  - `include/lscq/cas2.hpp`
  - `include/lscq/detail/platform.hpp`
  - `tests/unit/test_cas2.cpp`
- **依赖关系**: 无
- **测试命令**:
  ```bash
  cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build/debug && ctest --test-dir build/debug --output-on-failure
  ```
- **测试重点**:
  - ARM64 分支下 CAS2 功能正确性（编译验证，无需真实硬件）
  - 平台检测宏 `LSCQ_ARCH_ARM64` 正确定义
  - x86-64 和 ARM64 路径的条件编译逻辑
  - 单元测试覆盖 ARM64 CAS2 的基本操作（成功/失败路径）
  - 代码覆盖率 ≥90%（新增代码）

### 任务 2: Fallback 降级策略优化
- **ID**: task-2
- **type**: default
- **描述**: 将全局单锁的 `g_cas2_fallback_mutex` 替换为条带化锁（striped locks）机制，降低非 CAS2 原生平台的锁争用，提升并发性能
- **文件范围**:
  - `include/lscq/cas2.hpp` (fallback 实现部分)
  - `src/scqp.cpp` (如有直接使用 fallback 的逻辑)
  - `tests/unit/test_cas2.cpp` (新增 fallback 并发测试)
- **依赖关系**: 无（可与 task-1 并行）
- **测试命令**:
  ```bash
  cmake -S . -B build/coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLSCQ_ENABLE_COVERAGE=ON && cmake --build build/coverage && ctest --test-dir build/coverage --output-on-failure && gcovr --root . --filter include/ --filter src/ --print-summary
  ```
- **测试重点**:
  - 条带化锁的正确性（多线程并发访问不同/相同条带）
  - Fallback 路径的功能回归测试
  - 性能基准对比（单锁 vs 条带锁，记录在测试输出）
  - 代码覆盖率 ≥90%（覆盖条带索引计算、锁获取路径）

### 任务 3: Profiling 驱动的热点优化
- **ID**: task-3
- **type**: default
- **描述**: 使用 profiler（如 perf、VTune、Tracy）对现有基准测试进行性能分析，识别热点函数（预期包括 `cache_remap`、threshold 计算、Entry 布局），实施优化并验证性能提升
- **文件范围**:
  - `src/scq.cpp`
  - `src/scqp.cpp`
  - `include/lscq/detail/ncq_impl.hpp`
  - `benchmarks/benchmark_scq.cpp`
  - `benchmarks/benchmark_scqp.cpp`
  - `benchmarks/benchmark_lscq.cpp`
- **依赖关系**: 无（建议在 task-1、task-2 完成后执行，便于跨平台性能对比）
- **测试命令** (Windows 示例):
  ```bash
  cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build/release && .\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter=".*" --benchmark_out=results.json --benchmark_out_format=json
  ```
- **测试重点**:
  - Profiling 报告生成（火焰图或热点列表）
  - 针对 TOP-3 热点实施优化（如缓存友好访问、分支预测优化）
  - 基准测试对比（优化前后吞吐量/延迟改进 ≥5%）
  - 回归测试：单元测试全部通过
  - 覆盖优化后的代码路径 ≥90%

### 任务 4: 文档体系与 API 示例补全
- **ID**: task-4
- **type**: default
- **描述**: 创建项目 README.md、配置 Doxygen 生成 API 文档、补充 `docs/` 目录架构说明和使用指南、提供 `examples/` 目录的示例代码
- **文件范围**:
  - `README.md`（新建）
  - `Doxyfile`（新建）
  - `docs/architecture.md`（补充）
  - `docs/usage.md`（补充）
  - `examples/simple_queue.cpp`（新建）
  - `examples/benchmark_custom.cpp`（新建）
  - `CMakeLists.txt`（添加 Doxygen 目标）
- **依赖关系**: 无
- **测试命令**:
  ```bash
  # 构建示例
  cmake -S . -B build/docs -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/docs --target examples

  # 生成 Doxygen 文档
  doxygen Doxyfile
  ```
- **测试重点**:
  - README 包含：项目简介、快速开始、编译说明、平台支持列表、许可证
  - Doxygen 文档正确生成至 `build/docs/html/` 并涵盖所有公共 API
  - 示例代码可编译运行，演示核心 API 用法（SCQ、SCQP、LSCQ）
  - 文档语言与代码注释保持一致性（中文或英文，根据团队要求）

### 任务 5: CI/CD 多平台集成
- **ID**: task-5
- **type**: default
- **描述**: 扩展 GitHub Actions CI 配置，新增 macOS (x86-64/ARM64) 和 Linux ARM64 交叉编译 job，确保多平台编译通过、单元测试通过、代码覆盖率达标
- **文件范围**:
  - `.github/workflows/ci.yml`（修改或新建）
  - `cmake/toolchains/aarch64-linux-gnu.cmake`（可选，交叉编译工具链）
- **依赖关系**: task-1（ARM64 CAS2 实现必须先通过本地验证）
- **测试命令** (CI 自动执行，本地等价命令):
  ```bash
  # Linux ARM64 交叉编译示例
  cmake -S . -B build/arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/arm64

  # macOS 本地构建（M1/M2）
  cmake -S . -B build/macos -G Ninja -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/macos
  ctest --test-dir build/macos --output-on-failure
  ```
- **测试重点**:
  - 多平台编译成功（Windows x64、Linux x64/ARM64、macOS x64/ARM64）
  - 单元测试在所有平台通过（或仅编译验证 ARM64 若无硬件）
  - 代码覆盖率报告上传（如 Codecov）
  - CI 矩阵配置包含所有目标平台和构建类型（Debug/Release）

## 任务依赖关系图

```
task-1 (ARM64 CAS2) ──────────┐
                              ├──> task-5 (CI/CD)
task-2 (Fallback优化) ────────┤
                              │
task-3 (性能优化) ────────────┘

task-4 (文档) ──────────────── (独立)
```

**并行执行建议**:
- 第一波：task-1、task-2、task-4 可并行启动
- 第二波：task-1 完成后启动 task-5，task-2 完成后执行 task-3

## 验收标准

### 功能完整性
- [ ] ARM64 平台下 CAS2 功能正确实现（CASP 或 LL/SC）
- [ ] Fallback 降级使用条带化锁，并发争用显著降低
- [ ] 识别并优化至少 3 个性能热点函数
- [ ] 文档体系完整：README、API 文档、架构说明、使用示例
- [ ] CI/CD 覆盖 5 个平台配置（Windows x64、Linux x64、Linux ARM64、macOS x64、macOS ARM64）

### 性能指标
- [ ] Fallback 条带锁在多线程基准测试中吞吐量提升 ≥15%（相比单锁）
- [ ] 热点优化后，目标场景（高并发 enqueue/dequeue）性能提升 ≥5%
- [ ] ARM64 编译产物二进制大小增长 <10%（相比 x86-64）

### 质量保障
- [ ] 所有单元测试通过（5 个平台）
- [ ] 新增代码覆盖率 ≥90%（由 gcovr 或 llvm-cov 验证）
- [ ] 基准测试无性能回归（回归容忍度 ±3%）
- [ ] Doxygen 文档无警告或错误
- [ ] 示例代码可编译并运行成功

### 文档规范
- [ ] README 包含平台支持矩阵（架构、编译器、C++ 标准）
- [ ] 每个公共 API 有 Doxygen 注释（参数、返回值、线程安全性）
- [ ] `docs/` 目录包含架构图（ASCII 或 Mermaid）
- [ ] 示例代码有内联注释说明关键步骤

## 技术要点

### ARM64 CAS2 实现选择
**推荐方案**: 内联汇编 CASP 指令（ARMv8.1+ LSE 扩展）
```cpp
// 伪代码示例
inline bool cas2_arm64(Entry* ptr, Entry& expected, const Entry& desired) {
    bool success;
    __asm__ __volatile__(
        "casp %[exp_lo], %[exp_hi], %[des_lo], %[des_hi], [%[ptr]]"
        : [exp_lo] "+r" (expected.cycle_flags),
          [exp_hi] "+r" (expected.index_or_ptr),
          [success] "=@ccne" (success)
        : [ptr] "r" (ptr),
          [des_lo] "r" (desired.cycle_flags),
          [des_hi] "r" (desired.index_or_ptr)
        : "memory"
    );
    return success;
}
```
**降级方案**: LL/SC（LDXP/STXP）适用于 ARMv8.0

### Fallback 条带化锁设计
- 条带数量：16 或 32（2 的幂次，便于掩码计算）
- 哈希函数：`stripe_index = (uintptr_t(ptr) >> 4) & (STRIPE_COUNT - 1)`
- 锁类型：`std::mutex` 数组
- 注意：避免 false sharing（每个锁独占缓存行，添加 padding）

### 性能优化关键点
1. **cache_remap**: 考虑使用位操作替代除法/取模
2. **threshold 计算**: 预计算或缓存频繁访问的阈值
3. **Entry 布局**: 评估是否可通过字段重排序减少跨缓存行访问

### Doxygen 配置要点
```doxyfile
PROJECT_NAME           = "lscq"
OUTPUT_DIRECTORY       = ./build/docs
INPUT                  = include/ src/ README.md
RECURSIVE              = YES
EXTRACT_ALL            = NO
EXTRACT_PRIVATE        = NO
GENERATE_HTML          = YES
GENERATE_LATEX         = NO
```

### CI 矩阵示例
```yaml
strategy:
  matrix:
    os: [ubuntu-22.04, macos-13, macos-14, windows-2022]
    compiler: [gcc-11, clang-15, msvc-2022]
    arch: [x64, arm64]
    exclude:
      - os: windows-2022
        arch: arm64  # Windows ARM64 需交叉编译工具链
```

## 风险与应对

### 风险 1: ARM64 硬件测试缺失
**影响**: 无法验证 CASP 指令在真实硬件的正确性
**应对**:
- 使用 QEMU 模拟器进行基本功能测试
- CI 中添加 ARM64 Linux Docker 容器（通过 QEMU 用户模式）
- 明确文档标注"ARM64 实现已编译验证，待真实硬件测试"

### 风险 2: 条带化锁引入新 Bug
**影响**: 并发场景下可能出现数据竞争或死锁
**应对**:
- 增加压力测试用例（1000+ 线程并发访问）
- 启用 ThreadSanitizer 进行数据竞争检测
- 代码审查重点关注锁的获取顺序和释放逻辑

### 风险 3: 性能优化无明显收益
**影响**: Profiling 后发现热点已高度优化，改进空间有限
**应对**:
- 设定最小目标：优化后性能提升 ≥3%（降低预期）
- 记录优化尝试和结果（即使失败）到文档
- 探索算法层面优化（如调整队列容量动态扩展策略）

### 风险 4: 文档生成工具依赖
**影响**: Doxygen 版本差异导致文档格式不一致
**应对**:
- CI 中固定 Doxygen 版本（如 1.9.6）
- 提供预生成的文档到 `docs/prebuilt/` 目录
- README 中说明 Doxygen 安装要求

### 风险 5: CI 超时或资源限制
**影响**: 多平台矩阵构建耗时过长（>1小时）
**应对**:
- 分离快速检查（编译+单元测试）和慢速检查（基准测试）
- 使用 ccache 或 sccache 加速重复构建
- 限制并行 job 数量（GitHub Free 限制 20 并发）

## 时间估算

| 任务ID | 任务名称 | 预估工时 | 关键路径 |
|--------|---------|---------|---------|
| task-1 | ARM64 CAS2 实现 | 12h | 是 |
| task-2 | Fallback 优化 | 8h | 否 |
| task-3 | 性能优化 | 16h | 否 |
| task-4 | 文档补全 | 10h | 否 |
| task-5 | CI/CD 集成 | 6h | 是 |
| **总计** | | **52h** | |

**关键路径**: task-1 → task-5（18h）
**并行执行理想总时长**: ~24h（假设 3 任务并行）
**缓冲时间**: +30%（考虑调试和返工）= **31h**

## 里程碑

1. **M1 - ARM64 基础支持** (task-1 完成)
   - 交付物：ARM64 CAS2 实现、平台检测宏、单元测试
   - 验证点：编译通过、测试覆盖率 ≥90%

2. **M2 - 并发性能提升** (task-2 完成)
   - 交付物：条带化锁实现、性能对比报告
   - 验证点：基准测试吞吐量提升 ≥15%

3. **M3 - 性能优化验证** (task-3 完成)
   - 交付物：Profiling 报告、优化代码、性能对比数据
   - 验证点：目标场景性能提升 ≥5%

4. **M4 - 文档就绪** (task-4 完成)
   - 交付物：README、API 文档、示例代码
   - 验证点：文档可访问、示例可运行

5. **M5 - 生产就绪** (task-5 完成)
   - 交付物：多平台 CI 通过、覆盖率报告
   - 验证点：5 个平台全部通过 CI

## 附录

### A. 平台支持矩阵（目标状态）

| 平台 | 架构 | 编译器 | CAS2 实现 | CI 状态 |
|------|------|--------|-----------|---------|
| Windows | x64 | MSVC 2022 | CMPXCHG16B | 已支持 |
| Windows | x64 | Clang 15+ | CMPXCHG16B | 已支持 |
| Linux | x64 | GCC 11+ | CMPXCHG16B | 已支持 |
| Linux | ARM64 | GCC 11+ | CASP/LL+SC | 新增 |
| macOS | x64 | Clang 14+ | CMPXCHG16B | 新增 |
| macOS | ARM64 | Clang 14+ | CASP | 新增 |

### B. 代码覆盖率配置示例

```cmake
# CMakeLists.txt 片段
option(LSCQ_ENABLE_COVERAGE "Enable code coverage reporting" OFF)

if(LSCQ_ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lscq_impl PRIVATE --coverage)
    target_link_options(lscq_impl PRIVATE --coverage)
  endif()
endif()
```

**验证命令**:
```bash
gcovr --root . --filter include/ --filter src/ \
      --html-details coverage.html \
      --print-summary --fail-under-line 90
```

### C. 参考资料

- [ARM LSE Atomics](https://developer.arm.com/documentation/ddi0602/latest)
- [Intel CMPXCHG16B](https://www.felixcloutier.com/x86/cmpxchg8b:cmpxchg16b)
- [Google Benchmark User Guide](https://github.com/google/benchmark/blob/main/docs/user_guide.md)
- [Doxygen Manual](https://www.doxygen.nl/manual/index.html)
- [GitHub Actions - Matrix Strategy](https://docs.github.com/en/actions/using-jobs/using-a-matrix-for-your-jobs)

---

**文档版本**: 1.0
**最后更新**: 2026-01-20
**维护者**: Development Team
