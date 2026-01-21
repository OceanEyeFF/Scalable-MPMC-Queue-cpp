# Phase 1 StarterPrompt: 基础设施搭建

> **任务代号**: LSCQ-Phase1-Infrastructure
> **预计工期**: 1-2周
> **前置依赖**: Phase 0 完成（文档已就绪）
> **后续阶段**: Phase 2 (NCQ实现与验证)

---

## 1. 任务概述

### 1.1 核心目标
建立 LSCQ C++ 项目的完整基础设施，为后续算法实现铺平道路。本阶段专注于**工具链配置、构建系统、测试框架和CAS2抽象层**，确保开发环境健壮可靠。

### 1.2 任务价值
- ✅ 统一团队开发环境，避免"在我机器上能跑"问题
- ✅ 建立自动化测试框架，保证代码质量
- ✅ 实现CAS2跨平台抽象，为核心算法提供原子操作基础
- ✅ 配置CI/CD流水线，实现持续验证

---

## 2. 任务边界

### 2.1 In Scope (本阶段必须完成)
- [x] 项目目录结构创建
- [x] CMake构建系统配置
- [x] Google Test + Google Benchmark 集成
- [x] CAS2平台检测机制（编译时+运行时）
- [x] CAS2抽象层实现（x86-64 + Fallback）
- [x] CAS2单元测试编写
- [x] GitHub Actions CI配置
- [x] Sanitizers集成（AddressSanitizer + ThreadSanitizer）
- [x] 代码格式化配置（clang-format）

### 2.2 Out of Scope (本阶段不涉及)
- ❌ 任何队列算法实现（NCQ/SCQ/LSCQ）
- ❌ 性能Benchmark（仅验证测试框架可用）
- ❌ ARM64平台CAS2实现（Phase 6处理）
- ❌ 内存回收机制（Phase 5处理）
- ❌ 生产环境优化

---

## 3. 前置条件与输入

### 3.1 前置依赖
- ✅ `docs/00-本地开发环境配置.md` 已完成
- ✅ `docs/01-技术实现思路.md` 已完成
- ✅ `docs/02-分段开发计划.md` 已完成

### 3.2 输入资料
1. **必读文档**:
   - `docs/00-本地开发环境配置.md` (第3-5节: Clang++配置、CMake、VSCode)
   - `docs/01-技术实现思路.md` (第3.1节: CAS2实现策略)
   - `docs/02-分段开发计划.md` (第3节: Phase 1详细计划)

2. **参考代码**:
   - 文档中的CMake配置示例
   - 文档中的CAS2检测代码示例
   - 文档中的test_cas2.cpp测试示例

### 3.3 环境要求
- Windows 10/11
- Clang++ 14.0+
- CMake 3.20+
- Ninja
- Git

---

## 4. 详细任务清单

### 4.1 项目结构搭建 (Day 1)

```bash
# 创建目录结构
Scaleable-MPMC-Queue-cpp/
├── CMakeLists.txt                    # 根构建文件
├── CMakePresets.json                 # 构建预设
├── .clang-format                     # 代码格式配置
├── include/
│   └── lscq/
│       ├── config.hpp                # 平台检测宏
│       ├── cas2.hpp                  # CAS2抽象层
│       └── detail/
│           └── platform.hpp          # 平台工具函数
├── src/
│   └── cas2.cpp                      # CAS2实现（如果需要）
├── tests/
│   ├── CMakeLists.txt
│   └── unit/
│       └── test_cas2.cpp             # CAS2单元测试
├── benchmarks/
│   ├── CMakeLists.txt
│   └── benchmark_cas2.cpp            # CAS2 Benchmark（验证框架）
├── .github/
│   └── workflows/
│       └── ci.yml                    # CI配置
└── docs/
    └── Phase1-交接文档.md            # 本阶段完成后创建
```

**验收点**: `tree` 命令输出符合上述结构

---

### 4.2 CMake构建系统配置 (Day 2-3)

#### 4.2.1 根CMakeLists.txt
参考 `docs/02-分段开发计划.md` 第3.2.2节，实现：
- C++17标准强制
- 编译器检测（Clang/GCC/MSVC）
- 构建选项（LSCQ_BUILD_TESTS、LSCQ_ENABLE_CAS2、LSCQ_ENABLE_SANITIZERS）
- Google Test/Benchmark依赖管理（FetchContent）
- 主库目标（INTERFACE库）

#### 4.2.2 CMakePresets.json
参考 `docs/00-本地开发环境配置.md` 第4节，配置：
- `windows-clang-debug` 预设（带Sanitizers）
- `windows-clang-release` 预设（-O3 -march=native）

**验收点**:
```bash
cmake --list-presets           # 显示2个预设
cmake --preset windows-clang-debug
cmake --build build/debug      # 编译成功
```

---

### 4.3 CAS2抽象层实现 (Day 4-5)

#### 4.3.1 config.hpp - 平台检测
参考 `docs/02-分段开发计划.md` 第3.2.3节，实现：
```cpp
// 检测CPU架构
#if defined(__x86_64__) || defined(_M_X64)
    #define LSCQ_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define LSCQ_ARCH_ARM64 1
#else
    #define LSCQ_ARCH_UNKNOWN 1
#endif

// 检测CAS2支持（编译时）
#if defined(LSCQ_ARCH_X86_64) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
    #define LSCQ_HAS_CAS2_COMPILE_TIME 1
#else
    #define LSCQ_HAS_CAS2_COMPILE_TIME 0
#endif

namespace lscq::config {
    constexpr size_t DEFAULT_SCQSIZE = 1 << 16;  // 65536
    constexpr size_t DEFAULT_QSIZE = DEFAULT_SCQSIZE / 2;
}
```

#### 4.3.2 cas2.hpp - CAS2接口
参考 `docs/01-技术实现思路.md` 第3.1节，实现：
```cpp
namespace lscq {

// Entry结构（16字节对齐）
struct alignas(16) Entry {
    uint64_t cycle_flags;
    uint64_t index_or_ptr;

    bool operator==(const Entry& other) const;
};

// 运行时检测
bool has_cas2_support();

// CAS2主接口
bool cas2(Entry* ptr, Entry& expected, Entry& desired);

#if !LSCQ_HAS_CAS2_COMPILE_TIME
// Fallback实现（使用std::mutex）
class CAS2Fallback {
    mutable std::mutex mutex_;
public:
    bool cas2(Entry* ptr, Entry& expected, Entry& desired);
};
#endif

}  // namespace lscq
```

#### 4.3.3 x86-64 CAS2实现
使用 `__sync_bool_compare_and_swap_16` 或 `__atomic_compare_exchange_16`:
```cpp
#ifdef LSCQ_ARCH_X86_64
inline bool cas2(Entry* ptr, Entry& expected, Entry& desired) {
    __int128 exp = *reinterpret_cast<__int128*>(&expected);
    __int128 des = *reinterpret_cast<__int128*>(&desired);

    bool success = __sync_bool_compare_and_swap_16(
        reinterpret_cast<__int128*>(ptr), exp, des
    );

    if (!success) {
        expected = *ptr;  // 更新expected
    }
    return success;
}
#endif
```

**验收点**: `include/lscq/cas2.hpp` 可被包含，无编译错误

---

### 4.4 测试框架搭建 (Day 6-7)

#### 4.4.1 test_cas2.cpp
参考 `docs/02-分段开发计划.md` 第3.2.4节，实现：
1. **BasicOperation测试** - 单线程CAS2成功
2. **FailedOperation测试** - CAS2失败时expected更新
3. **Concurrent测试** - 多线程竞争，验证原子性
4. **RuntimeDetection测试** - 运行时检测函数

示例测试（来自文档）：
```cpp
TEST(CAS2Test, BasicOperation) {
    alignas(16) Entry entry{0, 0};
    Entry expected{0, 0};
    Entry desired{1, 42};

    bool success = cas2(&entry, expected, desired);
    EXPECT_TRUE(success);
    EXPECT_EQ(entry.cycle_flags, 1);
    EXPECT_EQ(entry.index_or_ptr, 42);
}

TEST(CAS2Test, Concurrent) {
    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS = 10000;

    alignas(16) Entry entry{0, 0};
    std::atomic<int> success_count{0};

    auto worker = [&]() {
        for (int i = 0; i < ITERATIONS; ++i) {
            Entry expected = entry;
            Entry desired{expected.cycle_flags + 1, expected.index_or_ptr + 1};
            if (cas2(&entry, expected, desired)) {
                success_count.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    // 验证原子性
    EXPECT_EQ(success_count.load(), entry.cycle_flags);
}
```

#### 4.4.2 benchmark_cas2.cpp
简单的性能验证（非优化目标）：
```cpp
static void BM_CAS2_SingleThread(benchmark::State& state) {
    alignas(16) Entry entry{0, 0};
    for (auto _ : state) {
        Entry expected = entry;
        Entry desired{expected.cycle_flags + 1, 0};
        benchmark::DoNotOptimize(cas2(&entry, expected, desired));
    }
}
BENCHMARK(BM_CAS2_SingleThread);
```

**验收点**:
```bash
cd build/debug
ctest --output-on-failure      # 所有测试通过
./benchmarks/benchmark_cas2    # Benchmark可运行
```

---

### 4.5 Sanitizers集成 (Day 8)

#### 4.5.1 CMake配置
```cmake
if(LSCQ_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fsanitize=address -fsanitize=thread -g)
        add_link_options(-fsanitize=address -fsanitize=thread)
    endif()
endif()
```

#### 4.5.2 验证
```bash
cmake --preset windows-clang-debug -DLSCQ_ENABLE_SANITIZERS=ON
cmake --build build/debug
./build/debug/tests/lscq_tests   # 无Sanitizer报错
```

**验收点**: ThreadSanitizer检测无data race

---

### 4.6 CI/CD配置 (Day 9)

#### 4.6.1 .github/workflows/ci.yml
参考 `docs/02-分段开发计划.md` 第10.1节：
```yaml
name: CI

on: [push, pull_request]

jobs:
  test-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3

      - name: Setup Clang
        run: choco install llvm --version=14.0.0

      - name: Configure
        run: cmake --preset windows-clang-debug

      - name: Build
        run: cmake --build build/debug

      - name: Test
        run: ctest --test-dir build/debug --output-on-failure

      - name: Sanitizer Test
        run: |
          cmake --preset windows-clang-debug -DLSCQ_ENABLE_SANITIZERS=ON
          cmake --build build/debug
          ./build/debug/tests/lscq_tests
```

**验收点**: GitHub Actions绿色勾√

---

### 4.7 代码格式化配置 (Day 10)

#### 4.7.1 .clang-format
```yaml
---
Language: Cpp
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
PointerAlignment: Left
ReferenceAlignment: Left
NamespaceIndentation: None
AllowShortFunctionsOnASingleLine: Empty
---
```

**验收点**:
```bash
clang-format -i include/**/*.hpp tests/**/*.cpp
git diff  # 无格式变更
```

---

## 5. 交付物清单

### 5.1 代码文件
- [ ] `CMakeLists.txt` - 根构建文件
- [ ] `CMakePresets.json` - 构建预设
- [ ] `.clang-format` - 格式配置
- [ ] `include/lscq/config.hpp` - 平台检测
- [ ] `include/lscq/cas2.hpp` - CAS2抽象层
- [ ] `tests/unit/test_cas2.cpp` - CAS2单元测试
- [ ] `benchmarks/benchmark_cas2.cpp` - CAS2性能测试
- [ ] `.github/workflows/ci.yml` - CI配置

### 5.2 构建产物
- [ ] `build/debug/tests/lscq_tests` - 可执行测试
- [ ] `build/debug/benchmarks/benchmark_cas2` - 可执行Benchmark
- [ ] `build/debug/compile_commands.json` - Clangd索引

### 5.3 文档
- [ ] `docs/Phase1-交接文档.md` - **必须创建**（见第7节）

---

## 6. 验收标准 (Gate Conditions)

### 6.1 编译验收
- ✅ **G1.1**: `cmake --preset windows-clang-debug` 配置成功
- ✅ **G1.2**: `cmake --build build/debug` 零警告编译通过
- ✅ **G1.3**: `cmake --build build/release` Release模式编译通过

### 6.2 测试验收
- ✅ **G2.1**: 所有CAS2单元测试通过（至少4个测试用例）
- ✅ **G2.2**: 并发测试无data race（ThreadSanitizer检测）
- ✅ **G2.3**: 测试覆盖率 > 80%（使用 `llvm-cov`）
- ✅ **G2.4**: 无内存泄漏（AddressSanitizer检测）

### 6.3 功能验收
- ✅ **G3.1**: `has_cas2_support()` 正确检测CPU能力
- ✅ **G3.2**: x86-64平台CAS2正常工作
- ✅ **G3.3**: Fallback机制正常工作（可选：通过宏禁用CAS2验证）

### 6.4 自动化验收
- ✅ **G4.1**: GitHub Actions CI通过
- ✅ **G4.2**: clang-format检查无格式问题
- ✅ **G4.3**: clang-tidy静态检查无错误

### 6.5 文档验收
- ✅ **G5.1**: `docs/Phase1-交接文档.md` 已创建（见第7节）
- ✅ **G5.2**: 交接文档包含所有要求章节

### 6.6 可选加分项
- 🌟 **Bonus 1**: 集成代码覆盖率报告到CI
- 🌟 **Bonus 2**: 添加MSVC编译器支持
- 🌟 **Bonus 3**: Docker构建环境

---

## 7. 下一阶段预览

### 7.1 Phase 2概述
**阶段名称**: NCQ实现与验证
**核心任务**: 实现论文中的Naive Circular Queue算法
**关键技术**: Ring Buffer + FAA + CAS2
**预计工期**: 1周

### 7.2 Phase 2依赖本阶段的关键产出
1. **CAS2抽象层** - NCQ的enqueue/dequeue依赖双字宽CAS
2. **测试框架** - 用于NCQ正确性测试
3. **Benchmark框架** - 用于NCQ性能基线测试
4. **构建系统** - 添加NCQ源文件和测试

### 7.3 Phase 2需要的预先知识
- Ring Buffer索引计算：`idx % SCQSIZE`
- Cycle计数器：`cycle = idx / SCQSIZE`
- Cache remap函数：避免false sharing
- FAA操作：`head_.fetch_add(1)`

---

## 8. 阶段完成交接文档要求

### 8.1 文档结构
在完成本阶段所有任务后，**必须创建** `docs/Phase1-交接文档.md`，包含以下章节：

```markdown
# Phase 1 交接文档

## 1. 完成情况概览
- 所有验收Gate通过情况
- 实际工期 vs 预估工期
- 遇到的主要问题和解决方案

## 2. 关键代码位置索引
- CAS2实现：`include/lscq/cas2.hpp:L42-L67`
- CAS2测试：`tests/unit/test_cas2.cpp:L15-L120`
- CMake配置：`CMakeLists.txt:L25-L40`（标注重要选项）

## 3. 平台能力检测结果
- 当前开发机器CAS2支持情况
- 编译时宏定义值（LSCQ_HAS_CAS2_COMPILE_TIME）
- 运行时检测函数返回值

## 4. 构建系统使用指南
- 常用CMake命令清单
- Preset使用说明
- 测试运行命令

## 5. 已知问题和限制
- 当前仅支持x86-64平台
- Fallback性能未优化（Phase 2无影响）
- 其他已知技术债

## 6. Phase 2接口预留
- Entry结构是否需要扩展？（NCQ需要cycle字段）
- cache_remap函数是否需要预先实现？
- 测试框架是否需要添加FIFO顺序验证？

## 7. 下阶段快速启动指南
**Phase 2开发者应该首先阅读**:
1. 本文档第2节（代码位置索引）
2. `include/lscq/cas2.hpp` - 理解CAS2接口
3. `tests/unit/test_cas2.cpp` - 理解测试模式
4. `docs/01-技术实现思路.md` 第4.1节 - NCQ算法伪代码

**Phase 2开发应该复用**:
- CAS2接口直接使用，无需修改
- 测试框架模式（TEST宏、并发测试模式）
- CMake添加新目标的模式

**Phase 2开发需要新建**:
- `include/lscq/ncq.hpp` - NCQ类声明
- `src/ncq.cpp` - NCQ实现（或header-only）
- `tests/unit/test_ncq.cpp` - NCQ单元测试
- `benchmarks/benchmark_ncq.cpp` - NCQ性能测试
```

### 8.2 交接文档验收标准
- ✅ 所有章节完整填写（不少于200行）
- ✅ 代码位置索引精确到行号
- ✅ 下阶段快速启动指南包含至少3个"应该阅读"和3个"应该复用"
- ✅ 至少记录1个实际遇到的问题和解决方案

---

## 9. 常见问题（FAQ）

### Q1: CAS2在我的CPU上不支持怎么办？
**A**: Fallback机制会自动启用，使用`std::mutex`实现。虽然性能较差，但功能正确。验收标准允许Fallback通过。

### Q2: Google Test下载失败（网络问题）？
**A**: 修改CMakeLists.txt，使用本地安装或镜像：
```cmake
set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "C:/path/to/googletest")
```

### Q3: ThreadSanitizer误报？
**A**: 确认是真实data race还是false positive。如果是false positive，可在测试中添加：
```cpp
__attribute__((no_sanitize("thread")))
```

### Q4: 测试覆盖率不足80%？
**A**: 补充以下测试场景：
- 边界条件（cycle溢出）
- 异常路径（nullptr、未对齐地址）
- 平台检测分支

---

## 10. 参考资料

- `docs/00-本地开发环境配置.md` - 环境搭建
- `docs/01-技术实现思路.md` - CAS2技术细节
- `docs/02-分段开发计划.md` - 本阶段详细计划
- [CMake Documentation](https://cmake.org/documentation/)
- [Google Test Primer](https://google.github.io/googletest/primer.html)
- [Clang ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html)

---

**StarterPrompt版本**: v1.0
**创建日期**: 2026-01-18
**适用范围**: LSCQ项目 Phase 1
