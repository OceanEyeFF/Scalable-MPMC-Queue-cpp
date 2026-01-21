# 故障排查：编译/运行/性能问题（中文）

本文档收集在 Windows/Linux/macOS 上构建与运行本项目时常见的报错与处理方法，并附带性能调优建议。

相关参考：

- `docs/benchmark-testing-guide.md`：从零搭建 benchmark 环境（更详细）
- `docs/usage.md`：队列语义与最佳实践（更精简）

---

## 1. 构建相关问题（CMake / 编译器 / 生成器）

### 1.1 `cmake: command not found` / 版本过低

症状：

- 找不到 `cmake`
- 或提示版本不满足（本项目 `CMakePresets.json` 标注最低 3.20）

处理：

- 安装 CMake 3.20+，并确保 `cmake --version` 可用
- Windows 可使用 `winget install Kitware.CMake`（或自行安装）

### 1.2 `ninja: command not found` / 生成器报错

症状：

- `-G Ninja` 时报 Ninja 不存在
- 或 `CMAKE_MAKE_PROGRAM` 找不到

处理：

- 安装 Ninja（Windows 可 `winget install Ninja-build.Ninja`）
- 或改用 Visual Studio 生成器（例如 VS 2022）

### 1.3 Windows 下 clang-cl 找不到 / VS 环境未初始化

症状：

- CMake 找不到 `clang-cl`
- 或找不到 `link.exe` / `rc.exe` / `mt.exe`

处理：

- 用 “Developer PowerShell for VS 2022” 打开终端再运行 CMake
- 或在 PowerShell 中执行 VS 的 DevShell 初始化脚本（见 `docs/benchmark-testing-guide.md`）

### 1.4 运行 CMake Preset 失败（路径/工具链不一致）

症状：

- `cmake --preset windows-clang-release-perf` 报错：preset 中的编译器路径不存在

说明：

`CMakePresets.json` 里的 Windows clang-cl 路径可能是某个开发机的绝对路径。不同机器上，clang-cl 的路径可能不同。

处理建议（择一）：

1. 手动指定编译器：
   ```powershell
   cmake -S . -B build/release-perf -G Ninja `
     -DCMAKE_BUILD_TYPE=Release `
     -DCMAKE_C_COMPILER=clang-cl `
     -DCMAKE_CXX_COMPILER=clang-cl `
     -DLSCQ_ENABLE_PERF_OPTS=ON
   ```
2. 或编辑 `CMakePresets.json` 将编译器路径改为你本机的安装路径（建议保持结构不变，仅更新路径）。

---

## 2. CAS2 / 回退路径相关问题

### 2.1 `has_cas2_support()` 返回 `false`

原因可能包括：

- CPU/平台不支持 128-bit CAS（例如非 x86_64 的 CMPXCHG16B 场景）
- 编译器/目标未启用相关指令或原子支持
- 运行在虚拟机/受限环境导致能力不可见

处理：

- 先确认这是预期行为：即便 CAS2 不可用，队列仍会走正确但更慢的回退路径。
- 在 Windows + clang-cl + x86_64 场景下，如需最大化 CAS2 路径，可确保启用 CMPXCHG16B（工程里有相应探测/开关逻辑）。
- 用 `examples/simple_queue.cpp` 输出确认：
  - `CAS2 supported: yes/no`
  - `SCQP using fallback: yes/no`

### 2.2 `SCQP::is_using_fallback()` 显示 `yes`

说明：

- 这表示 `SCQP` 没有在 slot 中直接存 `T*`，而是回退为 index + side-array。
- 该模式仍然正确，但吞吐量会受影响。

建议：

- 用 Release-Perf 构建（避免 Debug 的额外开销）
- 确认运行环境支持 CAS2（见上节）

---

## 3. 运行时语义问题（“看起来像 bug”，其实是用法不对）

### 3.1 `enqueue(nullptr)` 失败

适用范围：

- `SCQP<T>`、`LSCQ<T>`（指针队列）

解释：

- `nullptr` 被用作“空队列返回值”，因此不允许入队。

处理：

- 确保生产者入队的是有效指针；需要表达“空任务”时，用业务层的哨兵对象/状态码实现。

### 3.2 `SCQ<T>` 出队返回 `kEmpty`

解释：

- `SCQ<T>::kEmpty` 表示空队列，这是 API 约定。

处理：

- 使用 `while ((v = q.dequeue()) != kEmpty)` 的模式
- 并确保 **不要** 入队 `kEmpty`，也不要入队达到 `SCQSIZE - 1` 的值（算法约束）

### 3.3 `LSCQ` 析构时崩溃 / 野指针

常见原因：

- 队列析构时仍有线程在使用队列
- 指针队列入队对象生命周期不足（对象已释放/移动）

处理：

- 确保所有生产者/消费者线程退出后再销毁队列
- 确保入队对象在出队前一直有效（建议由消费者统一释放或使用对象池）
- `EBRManager` 必须比 `LSCQ` 活得更久（`LSCQ` 构造函数接收的是引用）

---

## 4. Sanitizer（ASan/UBSan）相关问题

### 4.1 Windows 上 ASan 配置失败

说明：

- Windows + clang-cl 的 sanitizer 组合在不同版本上差异较大（工具链、运行库、符号化等）。

建议：

- 先在 Linux/macOS 上用 Clang 跑 ASan（更稳定）
- Windows 上优先保证 Release/Debug 正常构建；需要 ASan 时再打开 `LSCQ_ENABLE_SANITIZERS=ON` 并参考构建输出提示

---

## 5. Benchmark 结果“很慢”/波动大（性能调优建议）

请先确认你在对比同一类构建与同一类运行环境：

1. **必须是 Release（最好 Release-Perf）**
   - Windows 推荐 `cmake --preset windows-clang-release-perf`
2. **线程数与 CPU 拓扑匹配**
   - 例如 Ryzen 9 5900X 是 12C/24T，Phase 7 的参考结果在 24 线程下取得
3. **确认没有走回退路径**
   - `has_cas2_support()` / `SCQP::is_using_fallback()` 可以快速定位
4. **尽量减少系统噪声**
   - 关闭后台占用 CPU 的程序
   - 使用高性能电源计划（Windows）
   - 多次运行取中位数，而不是单次结果
5. **避免在 Debug 下做性能结论**
   - Debug 会引入大量额外开销（断言、无优化、符号等）

如果你需要完整的“从构建到分析图表”的流程，请按 `docs/benchmark-testing-guide.md` 操作（包含 Python 分析环境与常见排查）。

---

## 6. Doxygen 文档生成问题

### 6.1 `doxygen: command not found`

处理：

- 安装 Doxygen，并确保 `doxygen --version` 可用
- 然后在仓库根目录运行：`doxygen Doxyfile`

### 6.2 输出目录找不到

默认输出路径：

- `build/docs/html/index.html`

如果你用 CMake target：

- `cmake --build build/release --target doxygen`

---

## 7. 仍然无法解决？

建议附上以下信息，方便定位：

- 操作系统与版本
- 编译器与版本（`clang --version` / `cl` / `gcc --version`）
- CMake 版本与生成器
- 你的构建命令（或 preset 名称）
- 关键报错日志（精简到最相关的 20~50 行）

