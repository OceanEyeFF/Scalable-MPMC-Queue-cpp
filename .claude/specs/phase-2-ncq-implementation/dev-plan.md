# Phase 2 NCQ 实现 - 开发计划

## 概述

实现基于论文 Figure 5 的 Naive Circular Queue (NCQ)，作为 LSCQ 的前置验证版本。NCQ 使用双字 CAS (cas2) 在循环队列槽位上原子操作 cycle 计数和 index/指针字段，支持无界 MPMC 并发，并通过 cache-remap 技术优化缓存行利用率。本阶段需完成核心算法实现、单元测试(覆盖率 ≥90%)、性能基准测试(含 M&S Queue 对比)，并输出完整交接文档。

---

## 任务分解

### Task 1: NCQ 公共 API 声明与配置常量
- **ID**: task-1
- **type**: default
- **描述**: 在配置头文件中添加队列大小常量，创建 NCQ 类模板的公共接口声明，包含构造函数、enqueue/dequeue 方法及必要的 Doxygen 文档注释
- **文件范围**:
  - `include/lscq/config.hpp` (添加 `DEFAULT_SCQSIZE=65536`, `DEFAULT_QSIZE=32768`)
  - `include/lscq/ncq.hpp` (新建，声明 `template<class T> class NCQ`)
  - `include/lscq/lscq.hpp` (如需要，添加 NCQ 相关引用)
- **依赖**: None
- **测试命令**:
  ```powershell
  cmake --preset windows-clang-debug
  cmake --build build/debug
  ctest --test-dir build/debug --output-on-failure
  ```
- **测试重点**:
  - 编译通过，无警告(clang-cl -Werror)
  - 头文件包含链路正确
  - 配置常量可被其他模块访问

---

### Task 2: NCQ 核心算法实现与原子加载
- **ID**: task-2
- **type**: default
- **描述**: 实现 NCQ 的 enqueue/dequeue/cache_remap 核心逻辑，遵循 Figure 5 控制流；添加 TSan 安全的 entry_load 辅助函数(基于 CAS2 的原子读取模式)；处理 Entry 结构布局映射(cycle→cycle_flags, index→index_or_ptr)；实现内存分配(64 字节对齐)和 RAII 析构
- **文件范围**:
  - `src/ncq.cpp` 或 `include/lscq/ncq.hpp` (根据头文件实现方案)
  - `include/lscq/detail/ncq_impl.hpp` (可选，辅助函数)
  - 显式模板实例化: `uint64_t`, `uint32_t`
- **依赖**: task-1
- **测试命令**:
  ```powershell
  # Windows Debug 构建
  cmake --preset windows-clang-debug
  cmake --build build/debug
  ctest --test-dir build/debug --output-on-failure

  # Linux TSan 验证(CI 环境)
  cmake -B build/tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
  cmake --build build/tsan
  ctest --test-dir build/tsan --output-on-failure
  ```
- **测试重点**:
  - **算法正确性**: 单线程 enqueue/dequeue FIFO 顺序
  - **并发安全性**: ThreadSanitizer 零数据竞争报告
  - **边界条件**: 队列满(返回 false)、队列空(返回 T 的默认值或特殊标记)
  - **Cycle 回绕**: 测试 cycle 计数器溢出场景
  - **cache_remap 正确性**: 验证 `remap = offset * (SCQSIZE / ENTRIES_PER_LINE) + line` 映射公式

**技术实现要点**:
```cpp
// Entry 原子加载(TSan 安全模式)
Entry entry_load(Entry* ptr) {
    Entry expected = {0, 0};
    cas2(ptr, expected, expected);  // 不修改值，仅触发原子语义
    return expected;
}

// cache_remap 算法
constexpr size_t ENTRIES_PER_LINE = 64 / sizeof(Entry);  // 4 for 16B Entry
size_t cache_remap(size_t idx, size_t scqsize) {
    size_t line = idx / ENTRIES_PER_LINE;
    size_t offset = idx % ENTRIES_PER_LINE;
    return offset * (scqsize / ENTRIES_PER_LINE) + line;
}

// 内存分配(64 字节对齐)
auto entries = std::unique_ptr<Entry[], decltype(&operator delete[])>(
    static_cast<Entry*>(::operator new[](scqsize * sizeof(Entry), std::align_val_t(64))),
    [](Entry* p) { ::operator delete[](p, std::align_val_t(64)); }
);
```

---

### Task 3: 单元测试与代码覆盖率
- **ID**: task-3
- **type**: default
- **描述**: 编写 NCQ 的全面单元测试套件，包含基础操作、FIFO 顺序、并发入队、并发生产-消费(4P4C×10000)、cycle 回绕等测试组；配置代码覆盖率收集(llvm-cov)，确保 ≥90% 行覆盖率
- **文件范围**:
  - `tests/unit/test_ncq.cpp` (新建)
  - `tests/CMakeLists.txt` (添加测试目标)
  - `.github/workflows/*.yml` (如需更新 CI 覆盖率配置)
- **依赖**: task-2
- **测试命令**:
  ```powershell
  # 功能测试
  cmake --preset windows-clang-debug
  cmake --build build/debug
  ctest --test-dir build/debug --output-on-failure --verbose

  # 代码覆盖率(Windows LLVM)
  cmake --preset windows-clang-debug -DCMAKE_CXX_FLAGS="--coverage"
  cmake --build build/debug
  ctest --test-dir build/debug
  llvm-profdata merge -sparse default.profraw -o coverage.profdata
  llvm-cov report ./build/debug/tests/lscq_tests.exe -instr-profile=coverage.profdata
  llvm-cov show ./build/debug/tests/lscq_tests.exe -instr-profile=coverage.profdata -format=html -output-dir=coverage_html

  # TSan 并发验证(Linux CI)
  cmake -B build/tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
  cmake --build build/tsan
  ctest --test-dir build/tsan --output-on-failure
  ```
- **测试重点**:
  - **BasicOperation**: 单线程 enqueue 100 元素 → dequeue 100 元素，验证 FIFO 顺序
  - **FIFO_Order**: 插入 1000 个连续整数，验证出队顺序严格递增
  - **ConcurrentEnqueue**: 8 线程同时入队各 10000 元素，验证无重复/丢失
  - **ConcurrentEnqueueDequeue**: 4 生产者 + 4 消费者各操作 10000 次，使用 spin barrier 同步启动，验证总量守恒
  - **CycleWrap**: 模拟 cycle 计数器接近溢出场景(手动设置高初始值)
  - **EdgeCases**: 队列满时 enqueue 失败，空队列 dequeue 返回默认值
  - **覆盖率目标**: ≥90% 行覆盖率(关注 enqueue/dequeue 所有分支、cache_remap、entry_load)

**测试模式参考(Phase 1 风格)**:
```cpp
// 并发测试启动模式
std::atomic<size_t> ready{0};
std::atomic<bool> start{false};
std::vector<std::thread> threads;

for (size_t i = 0; i < num_threads; ++i) {
    threads.emplace_back([&, tid = i]() {
        ready.fetch_add(1);
        while (!start.load()) { /* spin */ }
        // 执行测试操作...
    });
}

while (ready.load() < num_threads) { /* wait */ }
start.store(true);
for (auto& t : threads) t.join();
```

---

### Task 4: 性能基准测试与 MSQueue 对比
- **ID**: task-4
- **type**: default
- **描述**: 实现 NCQ 的 pair benchmark(1/2/4/8/16 线程配对测试)，测量吞吐量(Mops/sec)；实现简化版 Michael-Scott Queue 作为基准线；计算 NCQ 与 MSQueue 的性能差距，验证 G3.2(单线程 >5 Mops/sec)和 G3.4(gap <50%)目标
- **文件范围**:
  - `benchmarks/benchmark_ncq.cpp` (新建或扩展现有 benchmark 文件)
  - `include/lscq/msqueue.hpp` + `src/msqueue.cpp` (MSQueue 实现)
  - `benchmarks/CMakeLists.txt` (添加编译目标)
- **依赖**: task-2 (NCQ 完成)，可与 task-3 部分并行开发
- **运行命令**:
  ```powershell
  cmake --preset windows-clang-release
  cmake --build build/release --config Release
  .\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter=NCQ
  .\build\release\benchmarks\lscq_benchmarks.exe --benchmark_filter=MSQueue
  ```
- **测试重点**:
  - **Pair Benchmark**: 对称生产者-消费者配对，每对线程执行 1000000 次操作
  - **线程扩展性**: 1/2/4/8/16 线程配置下的吞吐量曲线
  - **MSQueue 基准**: 实现标准 M&S Queue(含安全析构)，测量相同负载下性能
  - **性能指标**:
    - 单线程 NCQ ≥ 5 Mops/sec (G3.2)
    - NCQ 多线程吞吐量与 MSQueue 的 gap < 50% (G3.4)，例如若 MSQueue 达到 20 Mops/sec，NCQ 应 ≥ 10 Mops/sec
  - **输出格式**: 包含平均吞吐量、标准差、CPU 时间，可生成 CSV 或 JSON 用于图表

**MSQueue 实现要点**:
```cpp
// 简化版 Michael-Scott Queue(仅支持 enqueue/dequeue)
template<class T>
class MSQueue {
    struct Node {
        T data;
        std::atomic<Node*> next;
    };
    alignas(64) std::atomic<Node*> head_;
    alignas(64) std::atomic<Node*> tail_;
public:
    MSQueue();
    ~MSQueue();  // 析构时需等待所有线程停止，然后回收节点链表
    bool enqueue(const T& value);
    bool dequeue(T& value);
};
```

---

### Task 5: Phase 2 交接文档与 API 文档
- **ID**: task-5
- **type**: default
- **描述**: 编写完整的 Phase 2 交接文档(≥300 行，9 个必需章节)，包含实现总结、代码索引、算法验证结果、性能数据、已知问题、Phase 3 接口预留、快速启动指南、经验教训和附录；补充 NCQ 类公共接口的 Doxygen 注释
- **文件范围**:
  - `docs/Phase2-交接文档.md` (新建)
  - `include/lscq/ncq.hpp` (增强 Doxygen 注释)
  - `include/lscq/config.hpp` (为新常量添加文档)
- **依赖**: 可与 task-2/3/4 并行开发，但需在测试/benchmark 数据可用后完成最终更新
- **验证命令**:
  ```powershell
  # 检查文档行数
  Get-Content docs/Phase2-交接文档.md | Measure-Object -Line

  # 生成 Doxygen 文档(可选)
  doxygen Doxyfile
  ```
- **测试重点**:
  - **行数要求**: ≥300 行(不含空行)
  - **章节完整性**: 必须包含以下 9 章
    1. **完成情况概览**: 列出已完成的功能点、通过的测试、性能指标达成情况
    2. **关键代码位置索引**: 文件路径 + 关键函数/类名 + 简要说明
    3. **NCQ 算法验证结果**: 单元测试通过情况、并发测试结果、TSan/ASan 报告摘要
    4. **性能 Benchmark 结果**: 吞吐量表格(1/2/4/8/16 线程)、NCQ vs MSQueue 对比、性能曲线图或 CSV 数据
    5. **已知问题和限制**: 当前实现的限制(如仅支持 POD 类型、固定队列大小等)、未解决的 bug、性能瓶颈
    6. **Phase 3 接口预留**: 为 SCQ/LSCQ 预留的扩展点(如虚函数、策略参数等)
    7. **下阶段快速启动指南**: Phase 3 开发者如何基于 NCQ 继续开发(构建步骤、测试命令、代码入口)
    8. **经验教训和最佳实践**: TSan 使用经验、CAS2 调试技巧、并发测试设计模式
    9. **附录**: 参考文献、性能数据原始日志、CI 配置说明
  - **Doxygen 完整性**: NCQ 类的所有公共方法必须有 `@brief`、`@param`、`@return`、`@note` 注释

**文档模板框架**:
```markdown
# Phase 2 NCQ 实现 - 交接文档

## 1. 完成情况概览
- ✅ NCQ 核心算法实现(enqueue/dequeue/cache_remap)
- ✅ 单元测试覆盖率 92% (目标 ≥90%)
- ✅ 性能指标: 单线程 6.2 Mops/sec (目标 ≥5), 与 MSQueue gap 38% (目标 <50%)
- ✅ 零 TSan/ASan 报告

## 2. 关键代码位置索引
| 文件路径 | 关键符号 | 说明 |
|---------|---------|------|
| include/lscq/ncq.hpp | template<class T> class NCQ | 公共 API 声明 |
| src/ncq.cpp | NCQ<T>::enqueue() | 入队实现 |
| ... | ... | ... |

## 3. NCQ 算法验证结果
### 3.1 单元测试
- BasicOperation: ✅ PASS
- ConcurrentEnqueueDequeue (4P4C×10000): ✅ PASS
- ...

### 3.2 Sanitizer 报告
- ThreadSanitizer: 0 data races
- AddressSanitizer: 0 leaks

## 4. 性能 Benchmark 结果
| 线程数 | NCQ (Mops/sec) | MSQueue (Mops/sec) | Gap |
|--------|----------------|---------------------|-----|
| 1      | 6.2            | 8.1                 | 23% |
| ...    | ...            | ...                 | ... |

## 5. 已知问题和限制
- 当前仅支持 POD 类型(uint64_t/uint32_t)
- 队列大小编译期固定为 DEFAULT_SCQSIZE
- ...

## 6. Phase 3 接口预留
- NCQ 析构函数设计为 virtual，便于 SCQ 继承
- ...

## 7. 下阶段快速启动指南
```powershell
# 构建 Phase 2
cmake --preset windows-clang-debug
cmake --build build/debug
ctest --test-dir build/debug
```

## 8. 经验教训和最佳实践
- TSan 需要使用 CAS2 实现原子读(entry_load 模式)
- ...

## 9. 附录
- 论文: Fatourou & Kallimanis, "Revisiting the Combining Synchronization Technique", PPoPP 2012
- ...
```

---

## 验收标准

### 全局目标

- **G1 - 零警告构建**:
  - [ ] Debug 和 Release 配置均无编译警告
  - [ ] Windows clang-cl 主平台测试通过
  - [ ] 使用 `-Werror` 标志验证

- **G2 - 测试覆盖率 ≥90%**:
  - [ ] 所有单元测试通过(BasicOperation, FIFO_Order, ConcurrentEnqueue, ConcurrentEnqueueDequeue, CycleWrap, EdgeCases)
  - [ ] llvm-cov 报告显示 NCQ 核心代码行覆盖率 ≥90%
  - [ ] 分支覆盖率 ≥80%(enqueue/dequeue 的成功/失败路径均被测试)

- **G3 - 性能目标**:
  - [ ] G3.2: 单线程 NCQ 吞吐量 ≥5 Mops/sec
  - [ ] G3.4: NCQ 与 Michael-Scott Queue 的性能差距 <50%
  - [ ] Pair benchmark 在 1/2/4/8/16 线程配置下均有测量数据

- **G4 - Sanitizer 清洁**:
  - [ ] ThreadSanitizer: 零数据竞争报告(Linux CI 或本地 WSL 测试)
  - [ ] AddressSanitizer: 零内存泄漏(destructor 正确释放队列内存)

- **G5 - 代码风格**:
  - [ ] clang-format 检查通过(Google Style)
  - [ ] 无 clang-tidy 严重警告

- **G6 - 文档完整性**:
  - [ ] `docs/Phase2-交接文档.md` ≥300 行
  - [ ] 包含全部 9 个必需章节(完成情况、代码索引、算法验证、性能结果、已知问题、接口预留、启动指南、经验教训、附录)
  - [ ] NCQ 类公共 API 有完整 Doxygen 注释

### 各任务验收标准

**Task 1**:
- [ ] `include/lscq/config.hpp` 包含 `DEFAULT_SCQSIZE=65536` 和 `DEFAULT_QSIZE=32768`
- [ ] `include/lscq/ncq.hpp` 声明 `template<class T> class NCQ` 及构造函数/enqueue/dequeue 方法签名
- [ ] 编译通过，无警告

**Task 2**:
- [ ] `enqueue(const T& value)` 和 `dequeue(T& value)` 实现遵循 Figure 5 伪代码
- [ ] `cache_remap(size_t idx)` 公式正确: `offset * (SCQSIZE / ENTRIES_PER_LINE) + line`
- [ ] `entry_load(Entry* ptr)` 使用 CAS2 模式实现原子读取
- [ ] Entry 结构字段映射正确(cycle→cycle_flags, index→index_or_ptr)
- [ ] 内存分配 64 字节对齐，析构时正确释放
- [ ] 显式模板实例化 `uint64_t` 和 `uint32_t`
- [ ] 单线程功能测试通过
- [ ] TSan 检测零数据竞争

**Task 3**:
- [ ] 至少 5 个测试组(BasicOperation, FIFO_Order, ConcurrentEnqueue, ConcurrentEnqueueDequeue, CycleWrap)
- [ ] 4P4C×10000 并发测试无数据丢失/重复
- [ ] llvm-cov 报告覆盖率 ≥90%
- [ ] ctest 输出显示所有测试 PASSED

**Task 4**:
- [ ] NCQ pair benchmark 实现 1/2/4/8/16 线程配置
- [ ] MSQueue 实现通过基础正确性测试
- [ ] 单线程 NCQ ≥5 Mops/sec
- [ ] NCQ vs MSQueue gap <50%
- [ ] benchmark 输出包含吞吐量(Mops/sec)、CPU 时间、迭代次数

**Task 5**:
- [ ] `docs/Phase2-交接文档.md` 文件存在且 ≥300 行
- [ ] 9 个必需章节全部包含
- [ ] 性能数据表格完整(从 task-4 实际测量结果填充)
- [ ] 代码索引列出至少 5 个关键文件/函数
- [ ] Doxygen 注释包含 `@brief`, `@param`, `@return`, `@note`

---

## 技术备注

### 1. Entry 结构布局
```cpp
// 复用 Phase 1 的 lscq::Entry
struct Entry {
    uint64_t cycle_flags;    // NCQ 使用: 低位存储 cycle 计数
    uint64_t index_or_ptr;   // NCQ 使用: 存储元素索引或指针
};
// 确保 16 字节对齐，与 cas2 兼容
static_assert(sizeof(Entry) == 16);
static_assert(alignof(Entry) == 16);
```

### 2. cache_remap 缓存优化算法
**目的**: 将逻辑连续的槽位映射到不同缓存行，避免 false sharing

**实现**:
```cpp
constexpr size_t CACHE_LINE_SIZE = 64;
constexpr size_t ENTRIES_PER_LINE = CACHE_LINE_SIZE / sizeof(Entry);  // 64 / 16 = 4

size_t cache_remap(size_t idx, size_t scqsize) {
    size_t line = idx / ENTRIES_PER_LINE;          // 计算原始缓存行号
    size_t offset = idx % ENTRIES_PER_LINE;        // 行内偏移
    size_t num_lines = scqsize / ENTRIES_PER_LINE; // 总缓存行数
    return offset * num_lines + line;              // 交错映射
}
```

**示例**: SCQSIZE=16, ENTRIES_PER_LINE=4
- 逻辑索引 0,1,2,3 → 物理索引 0,4,8,12 (分散到 4 个不同缓存行)
- 逻辑索引 4,5,6,7 → 物理索引 1,5,9,13

### 3. TSan 安全的原子 Entry 加载
**问题**: 直接读取 Entry 结构体会被 TSan 报告数据竞争(两个字段的非原子读取)

**解决方案**: 使用 CAS2 实现原子读取
```cpp
Entry entry_load(Entry* ptr) {
    Entry expected = {0, 0};
    cas2(ptr, expected, expected);  // 尝试交换相同值，不修改数据，但触发原子语义
    return expected;  // expected 被更新为实际值
}
```

### 4. 内存分配与 RAII
```cpp
// 分配 64 字节对齐的 Entry 数组
auto entries = std::unique_ptr<Entry[], decltype(&operator delete[])>(
    static_cast<Entry*>(::operator new[](scqsize * sizeof(Entry), std::align_val_t(64))),
    [](Entry* p) { ::operator delete[](p, std::align_val_t(64)); }
);

// 初始化槽位
for (size_t i = 0; i < scqsize; ++i) {
    new (&entries[i]) Entry{0, 0};  // placement new
}
```

### 5. 并发测试模式(Spin Barrier)
```cpp
// 确保所有线程同时启动，避免先启动线程占优
std::atomic<size_t> ready{0};
std::atomic<bool> start{false};

std::vector<std::thread> threads;
for (size_t i = 0; i < num_threads; ++i) {
    threads.emplace_back([&]() {
        ready.fetch_add(1, std::memory_order_relaxed);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // 执行测试负载...
    });
}

// 主线程等待所有子线程就绪
while (ready.load(std::memory_order_acquire) < num_threads) {
    std::this_thread::yield();
}
start.store(true, std::memory_order_release);  // 释放起跑令

for (auto& t : threads) t.join();
```

### 6. 性能测试注意事项
- **编译优化**: 必须使用 Release 配置(`-O3` 或 `/O2`)
- **亲和性**: 固定线程到物理核心(Windows: `SetThreadAffinityMask`, Linux: `pthread_setaffinity_np`)
- **预热**: 每个 benchmark 运行前执行若干次预热迭代，避免缓存冷启动影响
- **统计**: 多次运行取中位数或平均值，报告标准差

### 7. MSQueue 实现约束
- **简化版**: 仅实现 enqueue/dequeue，不需要 try_dequeue 等扩展接口
- **安全析构**: 析构函数必须在所有线程停止后调用，遍历链表释放所有节点
- **ABA 问题**: 使用指针 + 计数器的 tagged pointer 或依赖 C++20 `std::atomic<std::shared_ptr>` (如工具链支持)

### 8. 代码覆盖率工具链(Windows LLVM)
```powershell
# 1. 编译时添加覆盖率标志
cmake -B build/coverage -DCMAKE_CXX_FLAGS="--coverage -fprofile-instr-generate -fcoverage-mapping"
cmake --build build/coverage

# 2. 运行测试生成 profraw 文件
$env:LLVM_PROFILE_FILE="coverage-%p.profraw"
ctest --test-dir build/coverage

# 3. 合并 profraw 文件
llvm-profdata merge -sparse coverage-*.profraw -o coverage.profdata

# 4. 生成报告
llvm-cov report .\build\coverage\tests\lscq_tests.exe -instr-profile=coverage.profdata
llvm-cov show .\build\coverage\tests\lscq_tests.exe -instr-profile=coverage.profdata -format=html -output-dir=coverage_html

# 5. 查看 HTML 报告
Start-Process .\coverage_html\index.html
```

### 9. 关键约束
- **线程模型**: NCQ 支持无界 MPMC，但测试时需确保队列容量足够(SCQSIZE ≥ 生产者数 × 操作数)
- **元素类型**: 当前仅支持 POD 类型(uint64_t/uint32_t)，不支持自定义析构函数
- **队列大小**: 编译期常量，运行时不可变；必须是 ENTRIES_PER_LINE 的整数倍
- **Cycle 溢出**: uint64_t cycle 计数器溢出需 2^64 次操作，实际测试中模拟高初始值场景

### 10. Phase 3 扩展点预留
- **虚函数**: NCQ 析构函数声明为 virtual，便于 SCQ 继承并扩展 combining 逻辑
- **策略参数**: 构造函数预留模板参数位置，用于传递分配器、同步策略等
- **统计接口**: 预留 `get_stats()` 方法签名，用于收集操作计数、失败次数等调试信息

---

## 参考资料

1. **论文**: Panagiota Fatourou and Nikolaos D. Kallimanis, "Revisiting the Combining Synchronization Technique", PPoPP 2012, Figure 5 (NCQ 伪代码)
2. **Phase 1 实现**: `tests/unit/test_cas2.cpp` (CAS2 测试模式), `include/lscq/lscq.hpp` (Entry 定义)
3. **ThreadSanitizer 文档**: https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual
4. **LLVM Code Coverage**: https://clang.llvm.org/docs/SourceBasedCodeCoverage.html
5. **Michael-Scott Queue**: Maged M. Michael and Michael L. Scott, "Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms", PODC 1996

---

**文档版本**: 1.0
**创建日期**: 2026-01-18
**目标完成日期**: Phase 2 结束
**负责人**: Phase 2 开发团队
