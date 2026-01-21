# Contributing

Thanks for your interest in contributing to **lscq**. This project focuses on correctness and performance under high contention, so small changes can have large effects. Please read the guidelines below before opening a pull request.

## Code of Conduct

This project follows the rules in `CODE_OF_CONDUCT.md`. By participating, you are expected to uphold that code.

## Ways to contribute

- Report bugs (include a minimal repro when possible).
- Improve documentation and examples.
- Add tests for edge cases and concurrency scenarios.
- Performance improvements (include benchmark evidence and methodology).

## Development setup

### Prerequisites

- CMake 3.20+
- A C++17 compiler (clang/clang-cl, gcc, MSVC)
- Ninja (recommended) or Visual Studio / Make

### Configure & build

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

Windows users may prefer presets from `CMakePresets.json` (e.g. `windows-clang-release` / `windows-clang-release-perf`).

### Run tests

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

### Run benchmarks (optional)

See `docs/benchmark-testing-guide.md` for the full end-to-end workflow.

## Coding guidelines

- **Language:** C++17.
- **Correctness first:** do not trade correctness for speed; document any subtle memory-ordering changes.
- **Keep API semantics stable:** changing empty-return conventions, sentinel values, or ownership rules requires a clear justification and documentation updates.
- **Performance sensitivity:** avoid unnecessary allocations in hot paths and keep false-sharing in mind.
- **Formatting:** follow the repository style and run `clang-format` if you touch formatting-sensitive code.

## Pull request process

1. Create a focused branch (one feature/fix per PR).
2. Keep commits small and descriptive.
3. Include:
   - What changed and why
   - How to test (commands + expected output)
   - Benchmark numbers (if performance-related)
4. Ensure local tests pass before requesting review.

## License

By contributing, you agree that your contributions will be licensed under the MIT License (see `LICENSE`).

