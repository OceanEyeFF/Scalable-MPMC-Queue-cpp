#!/usr/bin/env python3
"""
Analyze Google Benchmark JSON outputs and generate charts + Markdown report.

Features:
  - Parse one or more Google Benchmark JSON files (e.g. all.json, stress.json)
  - Plot throughput (Mops/s) vs threads by scenario
  - Plot latency (ns/op) vs threads by scenario
  - Plot per-scenario bar charts to compare queue types at the max thread count
  - Emit a Markdown report with embedded figures

Dependencies (conda env: lscq-bench):
  - pandas, numpy, matplotlib
"""

from __future__ import annotations

import argparse
import json
import math
import re
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable

pd = None  # loaded lazily in _require_plotting_deps()
plt = None  # loaded lazily in _require_plotting_deps()


QUEUE_ORDER = ["NCQ", "SCQ", "SCQP", "LSCQ", "MSQueue", "MutexQueue"]
SCENARIO_ORDER = [
    "Pair",
    "50E50D",
    "30E70D",
    "70E30D(stress)",
    "EmptyQueue",
    "MemoryEfficiency",
]


NAME_PREFIX_RE = re.compile(r"^(?P<prefix>BM_[^/]+)")
THREADS_RE = re.compile(r"(?:^|/)threads:(?P<threads>\d+)(?:$|_)")


def _get_field(b: dict[str, Any], key: str) -> Any:
    if key in b:
        return b.get(key)
    counters = b.get("counters", {}) or {}
    return counters.get(key)


def _to_float(v: Any) -> float | None:
    if v is None:
        return None
    try:
        if isinstance(v, (int, float)):
            if isinstance(v, float) and (math.isnan(v) or math.isinf(v)):
                return None
            return float(v)
        s = str(v).strip()
        if not s:
            return None
        f = float(s)
        if math.isnan(f) or math.isinf(f):
            return None
        return f
    except Exception:
        return None


def _to_int(v: Any) -> int | None:
    f = _to_float(v)
    if f is None:
        return None
    return int(round(f))


def _time_to_ns(value: float, unit: str | None) -> float:
    u = (unit or "ns").lower()
    if u == "ns":
        return float(value)
    if u == "us":
        return float(value) * 1e3
    if u == "ms":
        return float(value) * 1e6
    if u == "s":
        return float(value) * 1e9
    raise ValueError(f"unsupported time_unit: {unit!r}")


def _parse_queue_scenario(name: str) -> tuple[str | None, str | None]:
    """
    Expected patterns:
      - BM_<Queue>_<Scenario>/real_time/threads:...
      - BM_<Queue>_<Scenario>
      - BM_<Queue>_<Scenario>_... (rare)
    """
    m = NAME_PREFIX_RE.match(name or "")
    if not m:
        return None, None

    prefix = m.group("prefix")
    if not prefix.startswith("BM_"):
        return None, None

    core = prefix[len("BM_") :]
    parts = core.split("_")
    if len(parts) < 2:
        return None, None

    queue = parts[0]
    scenario_raw = "_".join(parts[1:])

    # Normalize stress naming used in benchmark_stress.cpp
    if scenario_raw.endswith("_Stress"):
        base = scenario_raw[: -len("_Stress")]
        scenario = f"{base}(stress)"
    elif scenario_raw.endswith("_StressTest"):
        base = scenario_raw[: -len("_StressTest")]
        scenario = f"{base}(stress)"
    else:
        scenario = scenario_raw

    return queue, scenario


def _parse_threads(name: str, b: dict[str, Any]) -> int | None:
    v = _to_int(_get_field(b, "threads"))
    if v is not None and v > 0:
        return v
    m = THREADS_RE.search(name or "")
    if m:
        return int(m.group("threads"))
    return None


@dataclass(frozen=True)
class LoadResult:
    context: dict[str, Any] | None
    records: list[dict[str, Any]]


def load_google_benchmark_json(path: Path, *, source: str) -> LoadResult:
    data = json.loads(path.read_text(encoding="utf-8"))
    context = data.get("context") if isinstance(data, dict) else None

    records: list[dict[str, Any]] = []
    for b in data.get("benchmarks", []) if isinstance(data, dict) else []:
        if not isinstance(b, dict):
            continue

        name = str(b.get("name", "") or "")
        queue, scenario = _parse_queue_scenario(name)
        if not queue or not scenario:
            continue

        threads = _parse_threads(name, b)
        iterations = _to_int(b.get("iterations"))

        mops = _to_float(_get_field(b, "Mops"))
        real_time = _to_float(b.get("real_time"))
        time_unit = str(b.get("time_unit", "ns") or "ns")
        total_ops = _to_float(_get_field(b, "total_ops"))

        latency_ns: float | None = None
        if real_time is not None:
            try:
                time_ns_per_iter = _time_to_ns(real_time, time_unit)
                if total_ops is not None and iterations and total_ops > 0 and iterations > 0:
                    # Note: total_ops is recorded with kAvgThreads in this repo, so it is per-thread total ops.
                    # Google Benchmark's real_time is time per iteration, so ns/op = ns/iter / (ops/iter).
                    ops_per_iter = total_ops / float(iterations)
                    if ops_per_iter > 0:
                        latency_ns = time_ns_per_iter / ops_per_iter
                    else:
                        latency_ns = None
                else:
                    latency_ns = time_ns_per_iter
            except Exception:
                latency_ns = None

        # For memory efficiency benchmarks, threads may be absent; keep range arg for x-axis.
        args = b.get("args")
        if args is None and "arg" in b:
            args = [b.get("arg")]
        arg0 = None
        if isinstance(args, list) and args:
            arg0 = _to_float(args[0])

        rec: dict[str, Any] = {
            "source": source,
            "file": str(path),
            "name": name,
            "queue": queue,
            "scenario": scenario,
            "threads": threads,
            "iterations": iterations,
            "mops": mops,
            "latency_ns": latency_ns,
            "time_unit": time_unit,
            "arg0": arg0,
        }

        # Keep a few useful counters if present (for report/debug and MemoryEfficiency charts)
        for k in [
            "enqueue_pct",
            "dequeue_pct",
            "estimated_mb",
            "estimated_bytes",
            "capacity",
            "qsize_req",
            "qsize",
            "scqsize",
            "node_scqsize",
            "elements",
        ]:
            rec[k] = _to_float(_get_field(b, k))

        records.append(rec)

    return LoadResult(context=context, records=records)


def _scenario_sort_key(s: str) -> tuple[int, str]:
    try:
        return (SCENARIO_ORDER.index(s), s)
    except ValueError:
        return (999, s)


def _queue_sort_key(q: str) -> tuple[int, str]:
    try:
        return (QUEUE_ORDER.index(q), q)
    except ValueError:
        return (999, q)


def _configure_matplotlib(*, enable_chinese: bool) -> None:
    if plt is None:
        raise RuntimeError("matplotlib is not available (did you activate conda env 'lscq-bench'?)")
    plt.style.use("seaborn-v0_8-whitegrid")
    plt.rcParams["figure.dpi"] = 120
    plt.rcParams["savefig.dpi"] = 150
    plt.rcParams["axes.unicode_minus"] = False
    if enable_chinese:
        # Optional: best-effort common fonts. If missing, matplotlib will fallback.
        plt.rcParams["font.sans-serif"] = [
            "Microsoft YaHei",
            "SimHei",
            "Noto Sans CJK SC",
            "WenQuanYi Zen Hei",
            "Arial Unicode MS",
            "DejaVu Sans",
        ]


def _safe_filename(s: str) -> str:
    return re.sub(r"[^a-zA-Z0-9._()-]+", "_", s)


def _write_png(fig: plt.Figure, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(path, format="png")
    plt.close(fig)


def plot_metric_by_scenario(
    df: pd.DataFrame,
    *,
    metric: str,
    ylabel: str,
    out_dir: Path,
    title_prefix: str,
    file_prefix: str,
) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for scenario in sorted(df["scenario"].dropna().unique(), key=_scenario_sort_key):
        sub = df[df["scenario"] == scenario].copy()
        sub = sub.dropna(subset=["threads", metric])
        if sub.empty:
            continue

        fig, ax = plt.subplots(figsize=(8.5, 5.0))
        for queue in sorted(sub["queue"].unique(), key=_queue_sort_key):
            qdf = sub[sub["queue"] == queue].sort_values("threads")
            if qdf.empty:
                continue
            ax.plot(
                qdf["threads"].astype(int).to_list(),
                qdf[metric].astype(float).to_list(),
                marker="o",
                linewidth=2.0,
                label=queue,
            )

        ax.set_title(f"{title_prefix} - {scenario}")
        ax.set_xlabel("Threads")
        ax.set_ylabel(ylabel)
        ax.legend(loc="best", fontsize=9, ncol=2)
        ax.grid(True, alpha=0.35)

        out_path = out_dir / f"{_safe_filename(file_prefix)}_{_safe_filename(scenario)}.png"
        _write_png(fig, out_path)
        out[scenario] = out_path
    return out


def plot_bar_compare_at_max_threads(
    df: pd.DataFrame,
    *,
    metric: str,
    ylabel: str,
    out_dir: Path,
    title_prefix: str,
    file_prefix: str,
) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for scenario in sorted(df["scenario"].dropna().unique(), key=_scenario_sort_key):
        sub = df[df["scenario"] == scenario].copy()
        sub = sub.dropna(subset=["threads", metric])
        if sub.empty:
            continue

        max_threads = int(sub["threads"].max())
        at_max = sub[sub["threads"] == max_threads].copy()
        if at_max.empty:
            continue

        # If duplicates exist, average.
        agg = at_max.groupby("queue", as_index=False)[metric].mean()
        agg = agg.sort_values(metric, ascending=True)

        fig, ax = plt.subplots(figsize=(8.5, 5.0))
        ax.barh(agg["queue"].to_list(), agg[metric].astype(float).to_list())
        ax.set_title(f"{title_prefix} - {scenario} @ {max_threads} threads")
        ax.set_xlabel(ylabel)
        ax.set_ylabel("Queue")
        ax.grid(True, axis="x", alpha=0.35)

        out_path = out_dir / f"{_safe_filename(file_prefix)}_{_safe_filename(scenario)}_t{max_threads}.png"
        _write_png(fig, out_path)
        out[scenario] = out_path
    return out


def plot_memory_efficiency(df: pd.DataFrame, *, out_dir: Path) -> Path | None:
    sub = df[df["scenario"] == "MemoryEfficiency"].copy()
    if sub.empty:
        return None

    # Prefer estimated_mb when present.
    sub = sub.dropna(subset=["estimated_mb"])
    if sub.empty:
        return None

    # Choose x-axis from available counters (prefer elements, then qsize_req/capacity, then arg0)
    def choose_x(row: pd.Series) -> float | None:
        for k in ["elements", "qsize_req", "capacity", "arg0"]:
            v = row.get(k)
            if pd.notna(v):
                return float(v)
        return None

    sub["x"] = sub.apply(choose_x, axis=1)
    sub = sub.dropna(subset=["x"])
    if sub.empty:
        return None

    fig, ax = plt.subplots(figsize=(8.5, 5.0))
    for queue in sorted(sub["queue"].unique(), key=_queue_sort_key):
        qdf = sub[sub["queue"] == queue].sort_values("x")
        if qdf.empty:
            continue
        ax.plot(
            qdf["x"].astype(float).to_list(),
            qdf["estimated_mb"].astype(float).to_list(),
            marker="o",
            linewidth=2.0,
            label=queue,
        )
    ax.set_title("MemoryEfficiency - Estimated Memory (MB)")
    ax.set_xlabel("Target elements / capacity")
    ax.set_ylabel("Estimated MB")
    ax.legend(loc="best", fontsize=9, ncol=2)
    ax.grid(True, alpha=0.35)

    out_path = out_dir / "memory_efficiency_estimated_mb.png"
    _write_png(fig, out_path)
    return out_path


def _markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    def esc(x: str) -> str:
        return (x or "").replace("|", "\\|")

    out = []
    out.append("| " + " | ".join(esc(h) for h in headers) + " |")
    out.append("| " + " | ".join(["---"] * len(headers)) + " |")
    for r in rows:
        out.append("| " + " | ".join(esc(c) for c in r) + " |")
    return "\n".join(out)


def build_report_markdown(
    df: pd.DataFrame,
    *,
    contexts: list[dict[str, Any] | None],
    input_files: list[Path],
    fig_paths: dict[str, dict[str, Path]],
    out_report: Path,
    fig_dir: Path,
) -> None:
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    ctx = next((c for c in contexts if c), None) or {}

    # Make figure paths relative to the report location.
    def rel(p: Path) -> str:
        try:
            return str(p.relative_to(out_report.parent)).replace("\\", "/")
        except Exception:
            return str(p).replace("\\", "/")

    lines: list[str] = []
    lines.append("# Benchmark 分析报告")
    lines.append("")
    lines.append(f"- 生成时间: `{now}`")
    if ctx:
        lines.append(f"- Host: `{ctx.get('host_name', '')}`")
        lines.append(f"- CPU: `{ctx.get('num_cpus', '')}` cores, `{ctx.get('mhz_per_cpu', '')}` MHz")
        lines.append(f"- Google Benchmark: `{ctx.get('library_version', '')}` ({ctx.get('library_build_type', '')})")
    lines.append("- 输入文件:")
    for p in input_files:
        lines.append(f"  - `{p.as_posix()}`")
    lines.append("")

    # Summary: best throughput at max threads per scenario
    lines.append("## 总览（各场景最大线程数的吞吐/延迟）")
    summary_rows: list[list[str]] = []
    for scenario in sorted(df["scenario"].dropna().unique(), key=_scenario_sort_key):
        if scenario == "MemoryEfficiency":
            continue
        sub = df[(df["scenario"] == scenario)].copy()
        if sub.empty or sub["threads"].dropna().empty:
            continue
        max_threads = int(sub["threads"].dropna().max())
        at_max = sub[sub["threads"] == max_threads].copy()

        def best_of(metric: str, higher_is_better: bool) -> tuple[str, float]:
            t = at_max.dropna(subset=[metric])
            if t.empty:
                return ("-", float("nan"))
            agg = t.groupby("queue", as_index=False)[metric].mean()
            agg = agg.sort_values(metric, ascending=not higher_is_better)
            row = agg.iloc[0]
            return (str(row["queue"]), float(row[metric]))

        best_mops_q, best_mops_v = best_of("mops", True)
        best_lat_q, best_lat_v = best_of("latency_ns", False)

        summary_rows.append(
            [
                scenario,
                str(max_threads),
                f"{best_mops_q} ({best_mops_v:.3f})" if math.isfinite(best_mops_v) else "-",
                f"{best_lat_q} ({best_lat_v:.2f})" if math.isfinite(best_lat_v) else "-",
            ]
        )

    if summary_rows:
        lines.append(_markdown_table(["场景", "Max Threads", "最佳吞吐 (Mops/s)", "最低延迟 (ns/op)"], summary_rows))
    else:
        lines.append("_无可用吞吐/延迟数据_")
    lines.append("")

    # Per scenario sections
    for scenario in sorted(df["scenario"].dropna().unique(), key=_scenario_sort_key):
        lines.append(f"## 场景：{scenario}")
        lines.append("")

        if scenario == "MemoryEfficiency":
            mem_fig = fig_paths.get("memory", {}).get("MemoryEfficiency")
            if mem_fig:
                lines.append(f"![MemoryEfficiency]({rel(mem_fig)})")
                lines.append("")
            else:
                lines.append("_未生成 MemoryEfficiency 图表（缺少 estimated_mb 数据）。_")
                lines.append("")
            continue

        thr = fig_paths.get("throughput", {}).get(scenario)
        lat = fig_paths.get("latency", {}).get(scenario)
        bthr = fig_paths.get("bar_throughput", {}).get(scenario)
        blat = fig_paths.get("bar_latency", {}).get(scenario)

        if thr:
            lines.append(f"![Throughput]({rel(thr)})")
        if lat:
            lines.append(f"![Latency]({rel(lat)})")
        if bthr:
            lines.append(f"![Bar Throughput]({rel(bthr)})")
        if blat:
            lines.append(f"![Bar Latency]({rel(blat)})")
        if any([thr, lat, bthr, blat]):
            lines.append("")

        sub = df[(df["scenario"] == scenario)].copy()
        sub = sub.dropna(subset=["threads"])
        if sub.empty:
            lines.append("_该场景无 threads 数据。_")
            lines.append("")
            continue

        max_threads = int(sub["threads"].max())
        at_max = sub[sub["threads"] == max_threads].copy()

        # Table: per-queue @ max threads
        rows: list[list[str]] = []
        for queue in sorted(at_max["queue"].dropna().unique(), key=_queue_sort_key):
            qdf = at_max[at_max["queue"] == queue]
            mops = qdf["mops"].dropna()
            latns = qdf["latency_ns"].dropna()
            rows.append(
                [
                    queue,
                    f"{mops.mean():.3f}" if not mops.empty else "-",
                    f"{latns.mean():.2f}" if not latns.empty else "-",
                ]
            )
        if rows:
            lines.append(f"### @{max_threads} threads 队列对比")
            lines.append("")
            lines.append(_markdown_table(["Queue", "Throughput (Mops/s)", "Latency (ns/op)"], rows))
            lines.append("")

    out_report.parent.mkdir(parents=True, exist_ok=True)
    out_report.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Analyze Google Benchmark JSON outputs and generate report/figures.")
    ap.add_argument(
        "--inputs",
        nargs="+",
        type=Path,
        default=[Path("all.json"), Path("stress.json")],
        help="Input Google Benchmark JSON files (default: all.json stress.json)",
    )
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=Path("docs"),
        help="Output base directory (default: docs/)",
    )
    ap.add_argument(
        "--fig-dir",
        type=Path,
        default=None,
        help="Figures output directory (default: <out-dir>/figures)",
    )
    ap.add_argument(
        "--report",
        type=Path,
        default=None,
        help="Markdown report path (default: <out-dir>/benchmark_report.md)",
    )
    ap.add_argument(
        "--enable-chinese",
        action="store_true",
        help="Best-effort Chinese labels font config",
    )
    ap.add_argument(
        "--sources",
        nargs="*",
        default=None,
        help="Optional labels for each input file (must match --inputs count)",
    )
    return ap.parse_args()


def _require_plotting_deps() -> None:
    global pd, plt
    try:
        import pandas as _pd  # type: ignore
        import matplotlib.pyplot as _plt  # type: ignore
    except Exception as e:  # pragma: no cover
        msg = (
            "Missing Python dependencies (pandas/matplotlib). "
            "Please create and activate the conda env first:\n"
            "  conda env create -f scripts/environment.yml\n"
            "  conda activate lscq-bench\n"
        )
        raise SystemExit(msg) from e
    pd = _pd
    plt = _plt


def main() -> int:
    args = parse_args()
    _require_plotting_deps()
    _configure_matplotlib(enable_chinese=bool(args.enable_chinese))

    out_dir: Path = args.out_dir
    fig_dir: Path = args.fig_dir if args.fig_dir is not None else (out_dir / "figures")
    report_path: Path = args.report if args.report is not None else (out_dir / "benchmark_report.md")

    inputs = [p for p in args.inputs]
    sources = args.sources
    if sources is not None and len(sources) != len(inputs):
        raise SystemExit("--sources count must match --inputs count")
    if sources is None:
        sources = [p.stem for p in inputs]

    contexts: list[dict[str, Any] | None] = []
    all_records: list[dict[str, Any]] = []
    missing: list[Path] = []

    for p, s in zip(inputs, sources, strict=False):
        if not p.exists():
            missing.append(p)
            continue
        loaded = load_google_benchmark_json(p, source=s)
        contexts.append(loaded.context)
        all_records.extend(loaded.records)

    if missing:
        print("Warning: missing inputs:")
        for p in missing:
            print(f"  - {p}")

    if not all_records:
        raise SystemExit("no benchmark records found in inputs (check file paths and benchmark names)")

    df = pd.DataFrame.from_records(all_records)

    # Normalize dtypes
    df["threads"] = pd.to_numeric(df["threads"], errors="coerce")
    df["mops"] = pd.to_numeric(df["mops"], errors="coerce")
    df["latency_ns"] = pd.to_numeric(df["latency_ns"], errors="coerce")

    # Charts
    thr_df = df.dropna(subset=["threads", "mops"])
    lat_df = df.dropna(subset=["threads", "latency_ns"])

    fig_paths: dict[str, dict[str, Path]] = {}
    fig_paths["throughput"] = plot_metric_by_scenario(
        thr_df,
        metric="mops",
        ylabel="Mops/s",
        out_dir=fig_dir,
        title_prefix="Throughput (Mops/s) vs Threads",
        file_prefix="throughput",
    )
    fig_paths["latency"] = plot_metric_by_scenario(
        lat_df,
        metric="latency_ns",
        ylabel="ns/op",
        out_dir=fig_dir,
        title_prefix="Latency (ns/op) vs Threads",
        file_prefix="latency",
    )
    fig_paths["bar_throughput"] = plot_bar_compare_at_max_threads(
        thr_df,
        metric="mops",
        ylabel="Mops/s",
        out_dir=fig_dir,
        title_prefix="Throughput (Mops/s) queue compare",
        file_prefix="queue_compare_throughput",
    )
    fig_paths["bar_latency"] = plot_bar_compare_at_max_threads(
        lat_df,
        metric="latency_ns",
        ylabel="ns/op",
        out_dir=fig_dir,
        title_prefix="Latency (ns/op) queue compare",
        file_prefix="queue_compare_latency",
    )

    mem_fig = plot_memory_efficiency(df, out_dir=fig_dir)
    if mem_fig:
        fig_paths["memory"] = {"MemoryEfficiency": mem_fig}

    build_report_markdown(
        df,
        contexts=contexts,
        input_files=[p for p in inputs if p.exists()],
        fig_paths=fig_paths,
        out_report=report_path,
        fig_dir=fig_dir,
    )

    print(f"Figures: {fig_dir}")
    print(f"Report:  {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
