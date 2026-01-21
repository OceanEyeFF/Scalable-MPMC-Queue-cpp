# LSCQ Benchmark 测试指南

本文档提供从零开始在干净环境中运行 LSCQ Benchmark 的完整指南。

---

## 📋 目录

1. [系统要求](#系统要求)
2. [环境准备](#环境准备)
3. [构建步骤](#构建步骤)
4. [运行 Benchmark](#运行-benchmark)
5. [结果分析](#结果分析)
6. [常见问题排查](#常见问题排查)
7. [高级配置](#高级配置)

---

## 系统要求

### Windows

| 项目 | 最低要求 | 推荐配置 |
|------|---------|---------|
| 操作系统 | Windows 10 1903+ | Windows 11 |
| CPU | x86-64 with SSE4.2 | 支持 CMPXCHG16B 的多核 CPU |
| 内存 | 4 GB | 16 GB+ |
| 磁盘 | 2 GB 可用空间 | SSD |

**编译器（任选其一）**:
- Visual Studio 2022 (17.0+) with Clang tools
- Clang 15+ (clang-cl)
- MSVC 2022 (仅支持有限的 CAS2 功能)

**构建工具**:
- CMake 3.20+
- Ninja 1.10+ (推荐) 或 MSBuild

### Linux

| 项目 | 最低要求 | 推荐配置 |
|------|---------|---------|
| 发行版 | Ubuntu 20.04 / CentOS 8 | Ubuntu 22.04 / Fedora 38 |
| CPU | x86-64 with SSE4.2 | 支持 CMPXCHG16B 的多核 CPU |
| 内存 | 4 GB | 16 GB+ |
| 磁盘 | 2 GB 可用空间 | SSD |

**编译器（任选其一）**:
- GCC 11+
- Clang 15+

**构建工具**:
- CMake 3.20+
- Ninja 或 Make

### macOS

| 项目 | 最低要求 | 推荐配置 |
|------|---------|---------|
| 版本 | macOS 12 (Monterey) | macOS 14 (Sonoma) |
| 架构 | x86-64 或 ARM64 | Apple Silicon (M1/M2/M3) |
| 内存 | 8 GB | 16 GB+ |

**编译器**:
- AppleClang 14+ (Xcode 14+)

**构建工具**:
- CMake 3.20+
- Ninja 或 Make

---

## 环境准备

### Windows 环境准备

#### 1. 安装 Visual Studio 2022

从 [Visual Studio 下载页面](https://visualstudio.microsoft.com/downloads/) 下载并安装。

**必选组件**:
- "使用 C++ 的桌面开发"
- "适用于 Windows 的 C++ Clang 工具" (在"单个组件"中选择)

#### 2. 安装 CMake

```powershell
# 使用 winget
winget install Kitware.CMake

# 或使用 Chocolatey
choco install cmake

# 验证安装
cmake --version
```

#### 3. 安装 Ninja（可选但推荐）

```powershell
# 使用 winget
winget install Ninja-build.Ninja

# 或使用 Chocolatey
choco install ninja

# 验证安装
ninja --version
```

#### 4. 配置环境变量

打开 "Developer PowerShell for VS 2022" 或运行：

```powershell
# 初始化 VS 2022 环境
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"
```

### Linux 环境准备

#### Ubuntu/Debian

```bash
# 更新包管理器
sudo apt update

# 安装编译器和工具
sudo apt install -y build-essential cmake ninja-build git

# 安装 Clang（可选，推荐）
sudo apt install -y clang-15 lld-15

# 验证安装
cmake --version
gcc --version
clang-15 --version
```

#### Fedora/RHEL

```bash
# 安装编译器和工具
sudo dnf install -y gcc-c++ cmake ninja-build git

# 安装 Clang（可选）
sudo dnf install -y clang lld

# 验证安装
cmake --version
g++ --version
```

### macOS 环境准备

```bash
# 安装 Xcode 命令行工具
xcode-select --install

# 安装 Homebrew（如果未安装）
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 安装 CMake 和 Ninja
brew install cmake ninja

# 验证安装
cmake --version
clang --version
```

### Python 分析环境准备

所有平台通用：

```bash
# 安装 Miniconda（如果未安装）
# 下载地址: https://docs.conda.io/en/latest/miniconda.html

# 创建分析环境
conda env create -f scripts/environment.yml

# 激活环境
conda activate lscq-bench

# 验证安装
python --version
python -c "import pandas; import matplotlib; print('OK')"
```

---

## 构建步骤

### Windows 构建（推荐方式）

#### 使用 CMake Presets

```powershell
# 进入项目目录
cd path\to\Scaleable-MPMC-Queue-cpp

# 配置（使用预设）
cmake --preset windows-clang-release-perf

# 构建
cmake --build build/release-perf --config Release

# 查看生成的可执行文件
ls build/release-perf/benchmarks/
```

#### 手动配置

```powershell
# 创建构建目录
mkdir build\release-perf
cd build\release-perf

# 配置（使用 clang-cl）
cmake -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="clang-cl" ^
  -DCMAKE_CXX_COMPILER="clang-cl" ^
  -DLSCQ_BUILD_BENCHMARKS=ON ^
  -DLSCQ_ENABLE_PERF_OPTS=ON ^
  -DLSCQ_ENABLE_CAS2=ON ^
  ..\..

# 构建
cmake --build . --config Release
```

### Linux 构建

```bash
# 进入项目目录
cd path/to/Scaleable-MPMC-Queue-cpp

# 创建构建目录
mkdir -p build/release-perf
cd build/release-perf

# 配置（使用 Clang）
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLSCQ_BUILD_BENCHMARKS=ON \
  -DLSCQ_ENABLE_PERF_OPTS=ON \
  -DLSCQ_ENABLE_CAS2=ON \
  ../..

# 构建
cmake --build .

# 查看生成的可执行文件
ls benchmarks/
```

### macOS 构建

```bash
# 进入项目目录
cd path/to/Scaleable-MPMC-Queue-cpp

# 创建构建目录
mkdir -p build/release-perf
cd build/release-perf

# 配置
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLSCQ_BUILD_BENCHMARKS=ON \
  -DLSCQ_ENABLE_PERF_OPTS=ON \
  -DLSCQ_ENABLE_CAS2=ON \
  ../..

# 构建
cmake --build .
```

### 构建验证

```bash
# 检查可执行文件是否生成
# Windows
dir build\release-perf\benchmarks\*.exe

# Linux/macOS
ls -la build/release-perf/benchmarks/
```

预期输出：
- `lscq_benchmarks` (或 `.exe`) - 主 Benchmark 套件
- `benchmark_stress` (或 `.exe`) - 压力测试套件
- `benchmark_cas2` (或 `.exe`) - CAS2 专项测试
- `benchmark_lscq_simple` (或 `.exe`) - LSCQ 简化测试

---

## 运行 Benchmark

### 运行主 Benchmark 套件

```bash
# Windows
build\release-perf\benchmarks\lscq_benchmarks.exe ^
  --benchmark_out=all.json ^
  --benchmark_out_format=json ^
  --benchmark_repetitions=3 ^
  --benchmark_min_time=2s

# Linux/macOS
./build/release-perf/benchmarks/lscq_benchmarks \
  --benchmark_out=all.json \
  --benchmark_out_format=json \
  --benchmark_repetitions=3 \
  --benchmark_min_time=2s
```

### 运行压力测试套件

```bash
# Windows
build\release-perf\benchmarks\benchmark_stress.exe ^
  --benchmark_out=stress.json ^
  --benchmark_out_format=json ^
  --benchmark_repetitions=3 ^
  --benchmark_min_time=2s

# Linux/macOS
./build/release-perf/benchmarks/benchmark_stress \
  --benchmark_out=stress.json \
  --benchmark_out_format=json \
  --benchmark_repetitions=3 \
  --benchmark_min_time=2s
```

### 运行特定测试

```bash
# 只运行 Pair 场景
./lscq_benchmarks --benchmark_filter=".*Pair.*"

# 只运行 LSCQ 相关测试
./lscq_benchmarks --benchmark_filter=".*LSCQ.*"

# 只运行特定线程数
./lscq_benchmarks --benchmark_filter=".*threads:8.*"
```

### Benchmark 参数说明

| 参数 | 说明 | 示例 |
|------|------|------|
| `--benchmark_out` | 输出文件路径 | `results.json` |
| `--benchmark_out_format` | 输出格式 (json/csv/console) | `json` |
| `--benchmark_filter` | 正则过滤测试 | `".*Pair.*"` |
| `--benchmark_repetitions` | 重复次数 | `3` |
| `--benchmark_min_time` | 最小运行时间 | `2s` |
| `--benchmark_min_warmup_time` | 预热时间 | `1s` |

### 预期运行时间

| 套件 | 测试数量 | 预计时间 |
|------|---------|---------|
| `lscq_benchmarks` | ~100+ | 15-30 分钟 |
| `benchmark_stress` | ~16 | 3-5 分钟 |
| `benchmark_cas2` | ~10 | 1-2 分钟 |

---

## 结果分析

### 使用 Python 脚本分析

```bash
# 激活 Conda 环境
conda activate lscq-bench

# 分析结果（基本用法）
python scripts/analyze_benchmarks.py \
  --inputs all.json stress.json \
  --out-dir docs

# 启用中文标签
python scripts/analyze_benchmarks.py \
  --inputs all.json stress.json \
  --out-dir docs \
  --enable-chinese

# 自定义图表目录
python scripts/analyze_benchmarks.py \
  --inputs all.json stress.json \
  --out-dir results \
  --fig-dir results/charts \
  --report results/report.md
```

### 输出文件

分析完成后，`docs/` 目录下会生成：

```
docs/
├── figures/
│   ├── throughput_Pair.png
│   ├── throughput_50E50D.png
│   ├── throughput_30E70D.png
│   ├── latency_Pair.png
│   ├── latency_50E50D.png
│   ├── queue_compare_throughput_Pair_t24.png
│   ├── queue_compare_latency_Pair_t24.png
│   └── memory_efficiency_estimated_mb.png
└── benchmark_report.md
```

### 手动查看 JSON 结果

```bash
# 使用 jq 格式化查看（需要安装 jq）
jq '.benchmarks[] | {name, real_time, cpu_time, counters}' all.json | head -50

# 使用 Python 查看
python -c "import json; d=json.load(open('all.json')); print(json.dumps(d['benchmarks'][0], indent=2))"
```

### 关键指标说明

| 指标 | 单位 | 说明 |
|------|------|------|
| `Mops` | Mops/s | 每秒百万次操作（吞吐量） |
| `real_time` | ns | 实际运行时间（墙钟时间） |
| `cpu_time` | ns | CPU 时间 |
| `total_ops` | 次 | 总操作数 |
| `threads` | 个 | 线程数 |

---

## 常见问题排查

### 编译问题

#### Q: 找不到 clang-cl

```
错误: 'clang-cl' 不是内部或外部命令
```

**解决方案**:
1. 确保安装了 Visual Studio 的 "适用于 Windows 的 C++ Clang 工具"
2. 使用 Developer PowerShell for VS 2022
3. 或手动添加路径：
   ```powershell
   $env:PATH += ";C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin"
   ```

#### Q: CMake 版本过低

```
错误: CMake 3.20 or higher is required
```

**解决方案**:
```bash
# Windows
winget upgrade Kitware.CMake

# Linux
pip install --upgrade cmake
# 或从源码编译最新版本

# macOS
brew upgrade cmake
```

#### Q: 找不到 Ninja

**解决方案**:
```bash
# 使用 Make 代替
cmake -G "Unix Makefiles" ...  # Linux/macOS
cmake -G "NMake Makefiles" ... # Windows
```

### 运行问题

#### Q: CMPXCHG16B 不支持

```
Warning: CAS2 (CMPXCHG16B) not supported on this CPU
```

**说明**:
- 这是正常警告，表示 CPU 不支持 16 字节原子操作
- 程序会自动降级使用条带化锁实现
- 性能会略有下降，但功能正常

#### Q: Benchmark 运行时间过长

**解决方案**:
```bash
# 减少重复次数
--benchmark_repetitions=1

# 减少最小运行时间
--benchmark_min_time=0.5s

# 只运行部分测试
--benchmark_filter=".*Pair.*"
```

#### Q: 内存不足

**解决方案**:
```bash
# 减少并发线程数（修改代码）
# 或增加系统交换空间
```

### 分析问题

#### Q: Python 缺少依赖

```
ImportError: No module named 'pandas'
```

**解决方案**:
```bash
# 重新创建环境
conda env remove -n lscq-bench
conda env create -f scripts/environment.yml
conda activate lscq-bench
```

#### Q: 中文标签显示为方块

**解决方案**:
1. 安装中文字体：
   - Windows: 已内置
   - Linux: `sudo apt install fonts-noto-cjk`
   - macOS: 已内置

2. 或禁用中文标签：
   ```bash
   python scripts/analyze_benchmarks.py --inputs all.json
   # 不使用 --enable-chinese
   ```

#### Q: JSON 解析失败

```
JSONDecodeError: Expecting value
```

**解决方案**:
1. 确保 Benchmark 正常完成
2. 检查 JSON 文件是否完整
3. 重新运行 Benchmark

---

## 高级配置

### 自定义线程数

修改 `benchmarks/benchmark_utils.hpp`：

```cpp
// 默认线程数配置
constexpr std::array<int, 7> kThreadCounts = {1, 2, 4, 8, 12, 16, 24};

// 可修改为更少的线程数以加快测试
constexpr std::array<int, 4> kThreadCounts = {1, 4, 8, 16};
```

### 自定义队列容量

修改 `benchmarks/benchmark_utils.hpp`：

```cpp
// 默认容量
constexpr std::size_t kSharedCapacity = 1u << 16;  // 65536

// 可调整为更小的容量
constexpr std::size_t kSharedCapacity = 1u << 14;  // 16384
```

### 禁用特定队列

在 Benchmark 源文件中注释掉不需要的 `BENCHMARK` 宏：

```cpp
// 注释掉 MutexQueue 测试
// BENCHMARK(BM_Pair<lscq::MutexQueue<lscq_bench::Value>>)->Name("BM_MutexQueue_Pair")->Apply(apply_threads);
```

### 启用 CPU 亲和性

Benchmark 代码已包含 CPU 亲和性设置（`pin_thread_index`），可在 `benchmark_utils.hpp` 中调整。

### 性能优化建议

运行 Benchmark 前的系统优化：

```bash
# Linux: 禁用 CPU 频率调节
sudo cpupower frequency-set --governor performance

# Linux: 禁用地址空间随机化
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

# Windows: 以管理员权限运行
# 关闭不必要的后台程序
```

---

## 附录

### A. 完整 Benchmark 运行脚本

#### Windows (PowerShell)

```powershell
# run_benchmarks.ps1

$ErrorActionPreference = "Stop"

# 配置
$BUILD_DIR = "build\release-perf"
$OUTPUT_DIR = "benchmark_results"

# 创建输出目录
New-Item -ItemType Directory -Force -Path $OUTPUT_DIR | Out-Null

# 记录环境信息
$env_info = @{
    date = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    hostname = $env:COMPUTERNAME
    cpu = (Get-WmiObject Win32_Processor).Name
    cores = (Get-WmiObject Win32_Processor).NumberOfCores
    memory_gb = [math]::Round((Get-WmiObject Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 2)
}
$env_info | ConvertTo-Json | Out-File "$OUTPUT_DIR\environment.json"

# 运行主 Benchmark
Write-Host "Running main benchmarks..."
& "$BUILD_DIR\benchmarks\lscq_benchmarks.exe" `
    --benchmark_out="$OUTPUT_DIR\all.json" `
    --benchmark_out_format=json `
    --benchmark_repetitions=3 `
    --benchmark_min_time=2s

# 运行压力测试
Write-Host "Running stress tests..."
& "$BUILD_DIR\benchmarks\benchmark_stress.exe" `
    --benchmark_out="$OUTPUT_DIR\stress.json" `
    --benchmark_out_format=json `
    --benchmark_repetitions=3 `
    --benchmark_min_time=2s

Write-Host "Done! Results saved to $OUTPUT_DIR"
```

#### Linux/macOS (Bash)

```bash
#!/bin/bash
# run_benchmarks.sh

set -e

# 配置
BUILD_DIR="build/release-perf"
OUTPUT_DIR="benchmark_results"

# 创建输出目录
mkdir -p "$OUTPUT_DIR"

# 记录环境信息
cat > "$OUTPUT_DIR/environment.json" << EOF
{
  "date": "$(date '+%Y-%m-%d %H:%M:%S')",
  "hostname": "$(hostname)",
  "cpu": "$(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)",
  "cores": $(nproc),
  "memory_gb": $(free -g | awk '/Mem:/ {print $2}')
}
EOF

# 运行主 Benchmark
echo "Running main benchmarks..."
"$BUILD_DIR/benchmarks/lscq_benchmarks" \
    --benchmark_out="$OUTPUT_DIR/all.json" \
    --benchmark_out_format=json \
    --benchmark_repetitions=3 \
    --benchmark_min_time=2s

# 运行压力测试
echo "Running stress tests..."
"$BUILD_DIR/benchmarks/benchmark_stress" \
    --benchmark_out="$OUTPUT_DIR/stress.json" \
    --benchmark_out_format=json \
    --benchmark_repetitions=3 \
    --benchmark_min_time=2s

echo "Done! Results saved to $OUTPUT_DIR"
```

### B. 结果对比脚本

```python
#!/usr/bin/env python3
# compare_results.py

import json
import sys
from pathlib import Path

def load_results(path):
    with open(path) as f:
        data = json.load(f)
    results = {}
    for b in data.get('benchmarks', []):
        name = b.get('name', '')
        mops = b.get('counters', {}).get('Mops')
        if mops:
            results[name] = mops
    return results

def compare(baseline_path, current_path):
    baseline = load_results(baseline_path)
    current = load_results(current_path)

    print(f"{'Benchmark':<50} {'Baseline':>10} {'Current':>10} {'Change':>10}")
    print("-" * 80)

    for name in sorted(set(baseline.keys()) & set(current.keys())):
        b = baseline[name]
        c = current[name]
        change = (c - b) / b * 100
        sign = "+" if change > 0 else ""
        print(f"{name:<50} {b:>10.2f} {c:>10.2f} {sign}{change:>9.1f}%")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <baseline.json> <current.json>")
        sys.exit(1)
    compare(sys.argv[1], sys.argv[2])
```

---

**文档版本**: 1.0
**最后更新**: 2026-01-21
