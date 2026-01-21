#!/usr/bin/env python3
"""
Benchmark runner script that runs each test configuration in a separate process.

This avoids EBR thread-local state pollution between test runs, ensuring clean
state for each benchmark configuration.

Usage:
    python scripts/run_benchmarks.py [options]

Options:
    --benchmark-exe PATH    Path to benchmark executable
    --output PATH           Output JSON file path
    --filter PATTERN        Benchmark filter pattern (default: all)
    --min-time SECONDS      Minimum benchmark time (default: 1.0)
    --timeout SECONDS       Timeout per test (default: 120)
    --threads LIST          Comma-separated thread counts (default: 1,2,4,8,12,16,24)
"""

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run benchmarks with process isolation")
    parser.add_argument(
        "--benchmark-exe",
        type=Path,
        default=Path("build/release/benchmarks/lscq_benchmarks.exe"),
        help="Path to benchmark executable",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("benchmark_results.json"),
        help="Output JSON file path",
    )
    parser.add_argument(
        "--filter",
        type=str,
        default="",
        help="Benchmark filter pattern (e.g., 'BM_LSCQ_Pair')",
    )
    parser.add_argument(
        "--min-time",
        type=float,
        default=1.0,
        help="Minimum benchmark time in seconds",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="Timeout per test in seconds",
    )
    parser.add_argument(
        "--threads",
        type=str,
        default="1,2,4,8,12,16,24",
        help="Comma-separated thread counts",
    )
    parser.add_argument(
        "--benchmarks",
        type=str,
        default="BM_NCQ_Pair,BM_SCQ_Pair,BM_SCQP_Pair,BM_LSCQ_Pair,BM_MSQueue_Pair,BM_MutexQueue_Pair",
        help="Comma-separated benchmark names",
    )
    return parser.parse_args()


def run_single_benchmark(
    exe: Path,
    benchmark_name: str,
    threads: int,
    min_time: float,
    timeout: int,
) -> dict[str, Any] | None:
    """Run a single benchmark configuration and return the result."""
    filter_pattern = f"{benchmark_name}/real_time/threads:{threads}$"
    cmd = [
        str(exe),
        f"--benchmark_filter={filter_pattern}",
        f"--benchmark_min_time={min_time}s",
        "--benchmark_format=json",
        "--benchmark_repetitions=1",
    ]

    print(f"  Running: {benchmark_name} threads:{threads} ...", end=" ", flush=True)
    start_time = time.time()

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        elapsed = time.time() - start_time

        if result.returncode != 0:
            print(f"FAILED (exit code {result.returncode}, {elapsed:.1f}s)")
            if result.stderr:
                print(f"    stderr: {result.stderr[:200]}")
            return None

        # Parse JSON output
        try:
            data = json.loads(result.stdout)
            if data.get("benchmarks"):
                print(f"OK ({elapsed:.1f}s)")
                return data
            else:
                print(f"NO RESULTS ({elapsed:.1f}s)")
                return None
        except json.JSONDecodeError as e:
            print(f"JSON ERROR ({elapsed:.1f}s): {e}")
            return None

    except subprocess.TimeoutExpired:
        elapsed = time.time() - start_time
        print(f"TIMEOUT ({elapsed:.1f}s)")
        return None


def merge_results(results: list[dict[str, Any]]) -> dict[str, Any]:
    """Merge multiple benchmark results into a single JSON structure."""
    if not results:
        return {"benchmarks": []}

    # Use the first result as the base (for context info)
    merged = results[0].copy()
    merged["benchmarks"] = []

    for result in results:
        if result and "benchmarks" in result:
            merged["benchmarks"].extend(result["benchmarks"])

    return merged


def main() -> int:
    args = parse_args()

    # Validate executable
    if not args.benchmark_exe.exists():
        print(f"Error: Benchmark executable not found: {args.benchmark_exe}")
        return 1

    # Parse configuration
    thread_counts = [int(t.strip()) for t in args.threads.split(",")]
    benchmark_names = [b.strip() for b in args.benchmarks.split(",")]

    # Apply filter if specified
    if args.filter:
        benchmark_names = [b for b in benchmark_names if args.filter in b]

    print("=" * 60)
    print("Benchmark Runner with Process Isolation")
    print("=" * 60)
    print(f"Executable: {args.benchmark_exe}")
    print(f"Output: {args.output}")
    print(f"Benchmarks: {', '.join(benchmark_names)}")
    print(f"Thread counts: {thread_counts}")
    print(f"Min time: {args.min_time}s")
    print(f"Timeout: {args.timeout}s")
    print("=" * 60)
    print()

    all_results: list[dict[str, Any]] = []
    total_tests = len(benchmark_names) * len(thread_counts)
    completed = 0
    failed = 0

    start_time = datetime.now()

    for benchmark_name in benchmark_names:
        print(f"\n[{benchmark_name}]")
        for threads in thread_counts:
            result = run_single_benchmark(
                args.benchmark_exe,
                benchmark_name,
                threads,
                args.min_time,
                args.timeout,
            )
            if result:
                all_results.append(result)
                completed += 1
            else:
                failed += 1

    # Merge and save results
    merged = merge_results(all_results)
    merged["run_info"] = {
        "start_time": start_time.isoformat(),
        "end_time": datetime.now().isoformat(),
        "total_tests": total_tests,
        "completed": completed,
        "failed": failed,
        "script_version": "1.0.0",
    }

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=2)

    print()
    print("=" * 60)
    print("Summary")
    print("=" * 60)
    print(f"Total tests: {total_tests}")
    print(f"Completed: {completed}")
    print(f"Failed: {failed}")
    print(f"Results saved to: {args.output}")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
