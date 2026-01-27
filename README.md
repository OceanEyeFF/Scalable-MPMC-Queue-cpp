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

## Performance (Latest Release Benchmarks)

All numbers below are **measured** results from the latest stable Release build (2026-01-27). Your results may vary based on system load and hardware configuration.

### Test environment

| Item | Value |
|---|---|
| CPU | AMD Ryzen 9 5900X (12C/24T @ 3.7GHz) |
| Memory | DDR4-3600 |
| Compiler | MSVC 2022 (Release build) |
| Build Type | Release |
| Test Date | 2026-01-27 |

### Throughput scaling (Mops/s)

| Scenario | Queue | 12 threads | 16 threads | 24 threads |
|---|---|---:|---:|---:|
| **Pair** | LSCQ | 31.71 | 23.78 | 25.86 |
|  | MSQueue | 10.67 | 9.02 | 6.69 |
|  | MutexQueue | 23.16 | 6.77 | 3.23 |
| **50E50D** | LSCQ | 32.77 | 24.32 | 26.22 |
|  | MSQueue | 9.21 | 7.25 | 5.95 |
|  | MutexQueue | 18.13 | 5.97 | 3.01 |
| **30E70D** | LSCQ | 24.71 | 16.26 | 16.85 |
|  | MSQueue | 13.48 | 10.00 | 7.72 |
|  | MutexQueue | 17.89 | 11.12 | 3.99 |

**Key observations:**
- LSCQ shows best performance at 12 threads (physical core count)
- MutexQueue performance degrades severely beyond physical cores (24 threads)
- LSCQ maintains strong performance even with hyperthreading (16/24 threads)

### Relative speedups @ 24 threads (LSCQ vs others)

| Scenario | vs MSQueue | vs MutexQueue |
|---|---:|---:|
| **Pair** | 3.9× | 8.0× |
| **50E50D** | 4.4× | 8.7× |
| **30E70D** | 2.2× | 4.2× |

### Visual comparison (Mops/s @ 24 threads)

```
Pair       : LSCQ 25.86      | █████████████
             MSQueue 6.69    | ███
             MutexQueue 3.23 | ██

50E50D     : LSCQ 26.22      | █████████████
             MSQueue 5.95    | ███
             MutexQueue 3.01 | ██

30E70D     : LSCQ 16.85      | ████████
             MSQueue 7.72    | ████
             MutexQueue 3.99 | ██
```

**Scenario descriptions:**
- **Pair**: Paired enqueue/dequeue operations (1 enqueue + 1 dequeue per iteration)
- **50E50D**: 50% enqueue, 50% dequeue mixed workload
- **30E70D**: 30% enqueue, 70% dequeue (dequeue-heavy workload)

To reproduce benchmarks, start from `docs/benchmark-testing-guide.md` (includes step-by-step instructions for Release builds).

### ObjectPool Performance (Phase 1 Optimization)

The project includes an optimized ObjectPool component with local caching. Performance comparison of three implementations:

**ObjectPool Throughput @ High Thread Counts (Million items/s)**

| Implementation | 12 threads | 16 threads | 24 threads | Description |
|---|---:|---:|---:|---|
| **Baseline** | **22.48** | **29.26** | **24.91** | Original per-pool map with shared_mutex |
| **TLS** | 10.43 | 9.76 | 11.97 | Phase 1: Single thread_local slot |
| **Map** | 4.92 | 5.12 | 6.47 | Phase 1: Map-based per-thread cache |

**Key observations:**
- Baseline implementation performs best at 16 threads (29.26 M items/s)
- Phase 1 optimizations show different trade-offs under high contention
- Performance testing continues in Phase 2 with multi-slot TLS cache

See `benchmarks/benchmark_components.cpp` for detailed microbenchmarks and `docs/ObjectPool-Optimization-Plan.md` for optimization roadmap.

## Platform notes (quick)

- `SCQP<T>` / `LSCQ<T>` require 64-bit pointers (`sizeof(T*) == 8`).
- `SCQ<T>` stores *index values* (algorithm constraint: the value must be `< (SCQSIZE - 1)` and cannot equal `kEmpty`).

## License

MIT License. See `LICENSE`.
