# lscq — Scalable MPMC Queues in C++17

[![CI](https://github.com/OceanEyeFF/Scalable-MPMC-Queue-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/OceanEyeFF/Scalable-MPMC-Queue-cpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)

This repository provides a family of **MPMC (Multi-Producer Multi-Consumer)** queues aimed at high throughput and scalability under contention.

Key goals:

- High multi-thread throughput and scalability (vs. classic Michael-Scott style queues).
- Portable builds across compilers/platforms, with an automatic **CAS2 (128-bit compare-exchange)** fast path when available.
- Practical tooling: unit tests, benchmarks, and Doxygen API docs.

## What’s included

### Queue types

- `lscq::NCQ<T>`: bounded circular queue (Naive Circular Queue control flow from the paper).
- `lscq::SCQ<T>`: bounded scalable circular queue (**effective capacity is ~ half of the ring size**).
- `lscq::SCQP<T>`: pointer API version of `SCQ` (`T*`), storing pointers directly when CAS2 is available; otherwise falls back to index + side-pointer-array.
- `lscq::LSCQ<T>`: unbounded queue, linking multiple `SCQP` nodes; uses EBR to reclaim empty nodes.

### Baselines (for benchmarking / comparison)

- `lscq::MSQueue<T>`: simplified Michael-Scott queue implementation.
- `lscq::MutexQueue<T>`: a minimal mutex + `std::queue` baseline.

## Documentation

- `docs/architecture.md` (Chinese): architecture overview and key components.
- `docs/usage.md` (Chinese): quick usage guide and best practices.
- `docs/Tutorial.md` (Chinese): detailed tutorial and API overview (including `LSCQ`, `MSQueue`, `MutexQueue`).
- `docs/Troubleshooting.md` (Chinese): common build/runtime issues and performance tuning.
- `docs/benchmark-testing-guide.md` (Chinese): step-by-step benchmark guide.

## Quick Start

Examples below use **Ninja**, but other generators (Visual Studio / Makefiles) work as well.

### Build the library

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

### Build and run examples

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target examples

# run
./build/release/examples/simple_queue
./build/release/examples/benchmark_custom --help
```

### Run unit tests (optional)

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

### Generate API docs (Doxygen)

```bash
doxygen Doxyfile
# output: ./build/docs/html/index.html
```

Or via CMake:

```bash
cmake --build build/release --target doxygen
```

## Build options (CMake)

- `LSCQ_BUILD_TESTS` (default: ON): build GoogleTest unit tests
- `LSCQ_BUILD_BENCHMARKS` (default: ON): build Google Benchmark benchmarks
- `LSCQ_BUILD_EXAMPLES` (default: ON): build `examples/`
- `LSCQ_ENABLE_CAS2` (default: ON): enable CAS2 code path (still gated by runtime `lscq::has_cas2_support()`)
- `LSCQ_ENABLE_SANITIZERS` (default: OFF): enable sanitizers when supported

Language requirement: **C++17**.

## Performance (from Phase 7 benchmarks)

All numbers below are **measured** results taken from `docs/Phase7-交接文档.md` and are approximate (`~`), to be used as a reference point (your results may vary).

### Test environment

| Item | Value |
|---|---|
| CPU | AMD Ryzen 9 5900X (12C/24T) |
| Memory | DDR4-3600 |
| Compiler | clang-cl 17 |
| Threads | 24 |

### Throughput @ 24 threads (Mops/s)

| Queue | Pair | 50E50D | 30E70D | EmptyQueue |
|---|---:|---:|---:|---:|
| **LSCQ** | ~39 | ~45 | ~85 | ~180 |
| **MSQueue** | ~3 | ~5 | ~8 | ~30 |

### Relative speedups (LSCQ)

| Comparison | Pair | 50E50D |
|---|---:|---:|
| LSCQ vs MutexQueue | ~78× | ~75× |
| LSCQ vs MSQueue | ~13× | ~9× |

### Visual (bar chart, Mops/s @ 24 threads)

```
Pair       : LSCQ ~39  | ████████████████████
             MSQueue ~3 | ██
50E50D     : LSCQ ~45  | ███████████████████████
             MSQueue ~5 | ███
30E70D     : LSCQ ~85  | ██████████████████████████████████████████
             MSQueue ~8 | ████
EmptyQueue : LSCQ ~180 | ████████████████████████████████████████████████████████████████████████████
             MSQueue ~30 | ███████████████
```

To reproduce benchmarks, start from `docs/benchmark-testing-guide.md` (includes a `windows-clang-release-perf` CMake preset).

## Platform notes (quick)

- `SCQP<T>` / `LSCQ<T>` require 64-bit pointers (`sizeof(T*) == 8`).
- `SCQ<T>` stores *index values* (algorithm constraint: the value must be `< (SCQSIZE - 1)` and cannot equal `kEmpty`).

## License

MIT License. See `LICENSE`.
