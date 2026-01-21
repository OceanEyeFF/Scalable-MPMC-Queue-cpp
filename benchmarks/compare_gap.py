#!/usr/bin/env python3
import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Row:
    x: int
    ncq_mops: float | None
    scq_mops: float | None
    msq_mops: float | None
    ncq_ns_per_op: float | None
    scq_ns_per_op: float | None
    msq_ns_per_op: float | None


NAME_RE = re.compile(
    r"^(?P<bench>BM_(?P<queue>NCQ|SCQ|MSQueue)_(?P<scenario>Pair|MultiEnqueue|MultiDequeue)).*/threads:(?P<threads>\d+)(?:\b|_)"
)


def _get_field(b: dict, key: str):
    if key in b:
        return b.get(key)
    counters = b.get("counters", {}) or {}
    return counters.get(key)


def _time_to_ns(value: float, unit: str) -> float:
    unit = (unit or "ns").lower()
    if unit == "ns":
        return float(value)
    if unit == "us":
        return float(value) * 1e3
    if unit == "ms":
        return float(value) * 1e6
    if unit == "s":
        return float(value) * 1e9
    raise ValueError(f"unsupported time_unit: {unit!r}")


def _x_for_scenario(scenario: str, threads: int, producers: int, consumers: int) -> int:
    if scenario == "Pair":
        return threads
    if scenario == "MultiEnqueue":
        return producers
    if scenario == "MultiDequeue":
        return consumers
    raise ValueError(f"unsupported scenario: {scenario!r}")


def _ops_per_iter(scenario: str, threads: int, producers: int, consumers: int, ops_per_thread: int) -> int:
    if scenario == "Pair":
        return threads * ops_per_thread
    if scenario == "MultiEnqueue":
        return 2 * producers * ops_per_thread
    if scenario == "MultiDequeue":
        return 2 * consumers * ops_per_thread
    raise ValueError(f"unsupported scenario: {scenario!r}")


def load_rows(path: Path, *, aggregate: str | None) -> dict[str, dict[int, Row]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    out: dict[str, dict[int, Row]] = {}

    for b in data.get("benchmarks", []):
        agg_name = b.get("aggregate_name")
        if aggregate is not None and agg_name is not None and agg_name != aggregate:
            continue

        name = b.get("name", "")
        m = NAME_RE.match(name)
        if not m:
            continue

        bench = m.group("bench")
        queue = m.group("queue")
        scenario = m.group("scenario")
        threads = int(_get_field(b, "threads") or m.group("threads"))
        producers = int(float(_get_field(b, "producers") or 0))
        consumers = int(float(_get_field(b, "consumers") or 0))
        ops_per_thread = int(float(_get_field(b, "ops_per_thread") or 1_000_000))

        x = _x_for_scenario(scenario, threads, producers, consumers)

        mops = _get_field(b, "Mops")
        if mops is None:
            continue

        ns_per_op = None
        real_time = b.get("real_time")
        time_unit = b.get("time_unit")
        if real_time is not None:
            try:
                real_time_ns = _time_to_ns(float(real_time), str(time_unit))
                total_ops_field = _get_field(b, "total_ops")
                if total_ops_field is not None and float(total_ops_field) > 0:
                    ops_iter = int(float(total_ops_field))
                else:
                    ops_iter = _ops_per_iter(scenario, threads, producers, consumers, ops_per_thread)
                if ops_iter > 0:
                    ns_per_op = real_time_ns / float(ops_iter)
            except Exception:
                ns_per_op = None

        rows = out.setdefault(scenario, {})
        row = rows.get(x) or Row(
            x=x,
            ncq_mops=None,
            scq_mops=None,
            msq_mops=None,
            ncq_ns_per_op=None,
            scq_ns_per_op=None,
            msq_ns_per_op=None,
        )

        if queue == "NCQ":
            row.ncq_mops = float(mops)
            row.ncq_ns_per_op = ns_per_op
        elif queue == "SCQ":
            row.scq_mops = float(mops)
            row.scq_ns_per_op = ns_per_op
        elif queue == "MSQueue":
            row.msq_mops = float(mops)
            row.msq_ns_per_op = ns_per_op

        rows[x] = row

    return out


def fmt(x: float | None) -> str:
    if x is None:
        return "-"
    return f"{x:8.3f}"


def fmt_ns(x: float | None) -> str:
    if x is None:
        return "-"
    return f"{x:10.2f}"


def _png_write_rgba(path: Path, width: int, height: int, pixels: bytearray) -> None:
    # Minimal PNG writer (RGBA, 8-bit) with "no filter" scanlines.
    import struct
    import zlib

    if len(pixels) != width * height * 4:
        raise ValueError("invalid pixel buffer size")

    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag)
        crc = zlib.crc32(data, crc) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0
        start = y * stride
        raw.extend(pixels[start : start + stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # RGBA
    idat = zlib.compress(bytes(raw), level=6)

    out = bytearray()
    out.extend(b"\x89PNG\r\n\x1a\n")
    out.extend(chunk(b"IHDR", ihdr))
    out.extend(chunk(b"IDAT", idat))
    out.extend(chunk(b"IEND", b""))
    path.write_bytes(out)


def _png_set_px(pixels: bytearray, width: int, x: int, y: int, rgba: tuple[int, int, int, int]) -> None:
    if x < 0 or y < 0:
        return
    if x >= width:
        return
    idx = (y * width + x) * 4
    pixels[idx + 0] = rgba[0]
    pixels[idx + 1] = rgba[1]
    pixels[idx + 2] = rgba[2]
    pixels[idx + 3] = rgba[3]


def _png_line(pixels: bytearray, width: int, height: int, x0: int, y0: int, x1: int, y1: int,
              rgba: tuple[int, int, int, int]) -> None:
    # Bresenham line.
    dx = abs(x1 - x0)
    sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0)
    sy = 1 if y0 < y1 else -1
    err = dx + dy

    x, y = x0, y0
    while True:
        if 0 <= x < width and 0 <= y < height:
            _png_set_px(pixels, width, x, y, rgba)
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x += sx
        if e2 <= dx:
            err += dx
            y += sy


def _write_svg_line_chart(path: Path, *, title: str, xlabel: str, ylabel: str, xs: list[int],
                          series: list[tuple[str, str, list[float | None]]]) -> None:
    width, height = 900, 520
    margin_l, margin_r, margin_t, margin_b = 70, 20, 50, 70
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    y_max = 0.0
    for _, _, ys in series:
        for v in ys:
            if v is not None and float(v) > y_max:
                y_max = float(v)
    if y_max <= 0.0:
        y_max = 1.0
    y_max *= 1.10

    x_min, x_max = min(xs), max(xs)
    if x_min == x_max:
        x_min -= 1
        x_max += 1

    def x_to_px(x: int) -> float:
        return margin_l + (float(x - x_min) / float(x_max - x_min)) * plot_w

    def y_to_px(y: float) -> float:
        return margin_t + (1.0 - (y / y_max)) * plot_h

    parts: list[str] = []
    parts.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">')
    parts.append('<rect width="100%" height="100%" fill="white"/>')
    parts.append(f'<text x="{width/2:.1f}" y="28" text-anchor="middle" font-family="sans-serif" font-size="18">{title}</text>')

    # Axes
    x0, y0 = margin_l, margin_t + plot_h
    x1, y1 = margin_l + plot_w, margin_t
    parts.append(f'<line x1="{x0}" y1="{y0}" x2="{x1}" y2="{y0}" stroke="#333" stroke-width="1"/>')
    parts.append(f'<line x1="{x0}" y1="{y0}" x2="{x0}" y2="{y1}" stroke="#333" stroke-width="1"/>')

    # Grid + ticks
    for i in range(6):
        yv = y_max * (i / 5.0)
        yp = y_to_px(yv)
        parts.append(f'<line x1="{x0}" y1="{yp:.1f}" x2="{x1}" y2="{yp:.1f}" stroke="#ddd" stroke-width="1"/>')
        parts.append(f'<text x="{x0-8}" y="{yp+4:.1f}" text-anchor="end" font-family="sans-serif" font-size="11" fill="#333">{yv:.2f}</text>')

    for x in xs:
        xp = x_to_px(x)
        parts.append(f'<line x1="{xp:.1f}" y1="{y0}" x2="{xp:.1f}" y2="{y0+5}" stroke="#333" stroke-width="1"/>')
        parts.append(f'<text x="{xp:.1f}" y="{y0+20}" text-anchor="middle" font-family="sans-serif" font-size="11" fill="#333">{x}</text>')

    parts.append(f'<text x="{width/2:.1f}" y="{height-18}" text-anchor="middle" font-family="sans-serif" font-size="13">{xlabel}</text>')
    parts.append(
        f'<text x="18" y="{height/2:.1f}" text-anchor="middle" font-family="sans-serif" font-size="13" transform="rotate(-90 18 {height/2:.1f})">{ylabel}</text>'
    )

    # Series lines
    for name, color, ys in series:
        pts: list[str] = []
        for x, y in zip(xs, ys, strict=False):
            if y is None:
                continue
            pts.append(f"{x_to_px(x):.1f},{y_to_px(float(y)):.1f}")
        if len(pts) >= 2:
            parts.append(f'<polyline fill="none" stroke="{color}" stroke-width="2" points="{" ".join(pts)}"/>')
        for x, y in zip(xs, ys, strict=False):
            if y is None:
                continue
            parts.append(f'<circle cx="{x_to_px(x):.1f}" cy="{y_to_px(float(y)):.1f}" r="3" fill="{color}"/>')

    # Legend
    lx, ly = margin_l + 10, margin_t + 10
    for i, (name, color, _) in enumerate(series):
        y = ly + i * 18
        parts.append(f'<rect x="{lx}" y="{y-10}" width="12" height="12" fill="{color}"/>')
        parts.append(f'<text x="{lx+18}" y="{y}" font-family="sans-serif" font-size="12" fill="#333">{name}</text>')

    parts.append("</svg>")
    path.write_text("\n".join(parts), encoding="utf-8")


def _write_png_line_chart(path: Path, *, title: str, xs: list[int], series: list[tuple[str, tuple[int, int, int, int], list[float | None]]]) -> None:
    # Minimal PNG chart without text (kept dependency-free).
    width, height = 900, 520
    margin_l, margin_r, margin_t, margin_b = 70, 20, 50, 70
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    y_max = 0.0
    for _, _, ys in series:
        for v in ys:
            if v is not None and float(v) > y_max:
                y_max = float(v)
    if y_max <= 0.0:
        y_max = 1.0
    y_max *= 1.10

    x_min, x_max = min(xs), max(xs)
    if x_min == x_max:
        x_min -= 1
        x_max += 1

    def x_to_px(x: int) -> int:
        return int(round(margin_l + (float(x - x_min) / float(x_max - x_min)) * plot_w))

    def y_to_px(y: float) -> int:
        return int(round(margin_t + (1.0 - (y / y_max)) * plot_h))

    white = (255, 255, 255, 255)
    pixels = bytearray(width * height * 4)
    for i in range(0, len(pixels), 4):
        pixels[i + 0] = white[0]
        pixels[i + 1] = white[1]
        pixels[i + 2] = white[2]
        pixels[i + 3] = white[3]

    axis = (51, 51, 51, 255)
    x0, y0 = margin_l, margin_t + plot_h
    x1, y1 = margin_l + plot_w, margin_t
    _png_line(pixels, width, height, x0, y0, x1, y0, axis)
    _png_line(pixels, width, height, x0, y0, x0, y1, axis)

    for _, color, ys in series:
        prev = None
        for x, y in zip(xs, ys, strict=False):
            if y is None:
                prev = None
                continue
            p = (x_to_px(x), y_to_px(float(y)))
            if prev is not None:
                _png_line(pixels, width, height, prev[0], prev[1], p[0], p[1], color)
            prev = p

    _png_write_rgba(path, width, height, pixels)


def _plot(outdir: Path, scenario: str, rows: list[Row]) -> None:
    xs = [r.x for r in rows]

    def s(getter):
        out = []
        for r in rows:
            v = getter(r)
            out.append(None if v is None else float(v))
        return out

    xlabel = "threads" if scenario == "Pair" else ("enqueue_threads" if scenario == "MultiEnqueue" else "dequeue_threads")

    outdir.mkdir(parents=True, exist_ok=True)

    throughput_svg = outdir / f"gap_{scenario.lower()}_throughput.svg"
    latency_svg = outdir / f"gap_{scenario.lower()}_latency.svg"
    throughput_png = outdir / f"gap_{scenario.lower()}_throughput.png"
    latency_png = outdir / f"gap_{scenario.lower()}_latency.png"

    _write_svg_line_chart(
        throughput_svg,
        title=f"{scenario}: Throughput",
        xlabel=xlabel,
        ylabel="Mops/s",
        xs=xs,
        series=[
            ("NCQ", "#1f77b4", s(lambda r: r.ncq_mops)),
            ("SCQ", "#ff7f0e", s(lambda r: r.scq_mops)),
            ("MSQueue", "#2ca02c", s(lambda r: r.msq_mops)),
        ],
    )
    _write_svg_line_chart(
        latency_svg,
        title=f"{scenario}: Latency (avg)",
        xlabel=xlabel,
        ylabel="ns/op",
        xs=xs,
        series=[
            ("NCQ", "#1f77b4", s(lambda r: r.ncq_ns_per_op)),
            ("SCQ", "#ff7f0e", s(lambda r: r.scq_ns_per_op)),
            ("MSQueue", "#2ca02c", s(lambda r: r.msq_ns_per_op)),
        ],
    )

    _write_png_line_chart(
        throughput_png,
        title=f"{scenario}: Throughput",
        xs=xs,
        series=[
            ("NCQ", (31, 119, 180, 255), s(lambda r: r.ncq_mops)),
            ("SCQ", (255, 127, 14, 255), s(lambda r: r.scq_mops)),
            ("MSQueue", (44, 160, 44, 255), s(lambda r: r.msq_mops)),
        ],
    )
    _write_png_line_chart(
        latency_png,
        title=f"{scenario}: Latency (avg)",
        xs=xs,
        series=[
            ("NCQ", (31, 119, 180, 255), s(lambda r: r.ncq_ns_per_op)),
            ("SCQ", (255, 127, 14, 255), s(lambda r: r.scq_ns_per_op)),
            ("MSQueue", (44, 160, 44, 255), s(lambda r: r.msq_ns_per_op)),
        ],
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compare NCQ vs SCQ (and MSQueue if present) from Google Benchmark JSON; emit plots."
    )
    ap.add_argument("json", type=Path, help="Path to --benchmark_format=json output")
    ap.add_argument("--outdir", type=Path, default=None, help="Output directory for charts (default: <json_dir>/out)")
    ap.add_argument(
        "--aggregate",
        default="mean",
        help="Aggregate selector when JSON contains aggregates (default: mean; use 'none' to accept non-aggregate runs)",
    )
    args = ap.parse_args()

    aggregate = None if args.aggregate == "none" else args.aggregate
    by_scenario = load_rows(args.json, aggregate=aggregate)
    if not by_scenario:
        raise SystemExit("no BM_(NCQ|SCQ|MSQueue)_(Pair|MultiEnqueue|MultiDequeue) entries found")

    outdir = args.outdir or (args.json.parent / "out")

    for scenario in sorted(by_scenario.keys()):
        rows = [by_scenario[scenario][x] for x in sorted(by_scenario[scenario].keys())]

        x_label = "threads"
        if scenario == "MultiEnqueue":
            x_label = "enqueue_threads"
        elif scenario == "MultiDequeue":
            x_label = "dequeue_threads"

        print(f"\n[{scenario}]")
        print(f"{x_label:>14}   NCQ(Mops)   SCQ(Mops)  MSQueue(Mops)   SCQ/NCQ   NCQ(ns/op)   SCQ(ns/op)")
        for row in rows:
            ratio_str = "-"
            if row.scq_mops is not None and row.ncq_mops is not None and row.ncq_mops != 0.0:
                ratio_str = f"{(row.scq_mops / row.ncq_mops):7.3f}"
            print(
                f"{row.x:14d} {fmt(row.ncq_mops)} {fmt(row.scq_mops)} {fmt(row.msq_mops)}"
                f" {ratio_str:>8} {fmt_ns(row.ncq_ns_per_op)} {fmt_ns(row.scq_ns_per_op)}"
            )

        _plot(outdir, scenario, rows)

    pair = by_scenario.get("Pair", {})
    if 16 in pair and pair[16].ncq_mops is not None and pair[16].scq_mops is not None:
        speedup = pair[16].scq_mops / pair[16].ncq_mops if pair[16].ncq_mops else 0.0
        print(f"\n[check] Pair @ 16 threads: NCQ={pair[16].ncq_mops:.3f} Mops, SCQ={pair[16].scq_mops:.3f} Mops, speedup={speedup:.3f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
