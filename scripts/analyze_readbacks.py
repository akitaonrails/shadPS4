#!/usr/bin/env python3
"""Parse readback-benchmark CSVs and print a comparison table.

Usage:
    scripts/analyze_readbacks.py <run-dir-or-csv> [<run-dir-or-csv>...]

Either pass a run directory (bench/<timestamp>/), which will pick up
every *.csv inside and group by filename stem, or pass individual CSV
files directly.

Outputs a markdown-friendly table ordered so the user can spot
finish-per-frame and frame-time deltas across modes at a glance.
"""

from __future__ import annotations

import pathlib
import sys
from typing import Iterable

RAW_COLS = [
    "frame_count",
    "finish_count",
    "finish_ns",
    "wait_count",
    "wait_ns",
    "submit_count",
    "buffer_read_count",
    "buffer_read_ns",
    "buffer_download_count",
    "buffer_download_bytes",
    "buffer_download_ns",
    "image_download_count",
    "image_download_bytes",
    "image_download_ns",
    "image_download_batches",
    "image_download_batched",
    "fault_dispatch_count",
    "page_protect_count",
    "page_protect_ns",
    "page_invalidate_count",
    "frame_p50_ns",
    "frame_p95_ns",
    "frame_p99_ns",
]


def load(csv: pathlib.Path) -> dict[str, int]:
    out: dict[str, int] = {}
    for line in csv.read_text().splitlines()[1:]:
        if not line.strip():
            continue
        k, v = line.split(",", 1)
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = 0
    return out


def collect(arg: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    if arg.is_file() and arg.suffix == ".csv":
        return [(arg.stem, arg)]
    if arg.is_dir():
        return [(p.stem, p) for p in sorted(arg.glob("*.csv"))]
    raise SystemExit(f"not a csv or dir: {arg}")


def fmt_ns(n: int) -> str:
    if n >= 1_000_000_000:
        return f"{n/1e9:.2f}s"
    if n >= 1_000_000:
        return f"{n/1e6:.1f}ms"
    if n >= 1_000:
        return f"{n/1e3:.0f}us"
    return f"{n}ns"


def fmt_mb(n: int) -> str:
    return f"{n/1024/1024:.1f}MB" if n else "0"


def per_frame(total: int, frames: int) -> float:
    return total / frames if frames else 0.0


def main(args: Iterable[str]) -> None:
    cells: list[tuple[str, dict[str, int]]] = []
    for a in args:
        for label, path in collect(pathlib.Path(a)):
            cells.append((label, load(path)))
    if not cells:
        raise SystemExit("no CSVs to analyze")

    rows = []
    for label, m in cells:
        f = m.get("frame_count", 0) or 1
        rows.append({
            "cell": label,
            "frames": m.get("frame_count", 0),
            "p50": m.get("frame_p50_ns", 0) / 1e6,
            "p95": m.get("frame_p95_ns", 0) / 1e6,
            "p99": m.get("frame_p99_ns", 0) / 1e6,
            "finish/f": per_frame(m.get("finish_count", 0), f),
            "finish_total": fmt_ns(m.get("finish_ns", 0)),
            "finish_per_f_ms": per_frame(m.get("finish_ns", 0), f) / 1e6,
            "read/f": per_frame(m.get("buffer_read_count", 0), f),
            "dl_total": fmt_mb(m.get("buffer_download_bytes", 0)),
            "dl/f_kb": per_frame(m.get("buffer_download_bytes", 0), f) / 1024,
            "protect/f": per_frame(m.get("page_protect_count", 0), f),
            "protect_per_f_us": per_frame(m.get("page_protect_ns", 0), f) / 1e3,
            "inval/f": per_frame(m.get("page_invalidate_count", 0), f),
            "fault_d": m.get("fault_dispatch_count", 0),
            "img_dl": m.get("image_download_count", 0),
        })

    header = [
        "cell",
        "frames",
        "p50ms",
        "p95ms",
        "p99ms",
        "finish/f",
        "finish/f_ms",
        "read/f",
        "dl/f_kb",
        "protect/f",
        "protect/f_us",
        "inval/f",
        "fault_d",
        "img_dl",
    ]
    widths = {h: max(len(h), 4) for h in header}
    fmt_rows: list[dict[str, str]] = []
    for r in rows:
        s = {
            "cell": r["cell"],
            "frames": str(r["frames"]),
            "p50ms": f"{r['p50']:.2f}",
            "p95ms": f"{r['p95']:.2f}",
            "p99ms": f"{r['p99']:.2f}",
            "finish/f": f"{r['finish/f']:.2f}",
            "finish/f_ms": f"{r['finish_per_f_ms']:.3f}",
            "read/f": f"{r['read/f']:.2f}",
            "dl/f_kb": f"{r['dl/f_kb']:.1f}",
            "protect/f": f"{r['protect/f']:.1f}",
            "protect/f_us": f"{r['protect_per_f_us']:.1f}",
            "inval/f": f"{r['inval/f']:.1f}",
            "fault_d": str(r["fault_d"]),
            "img_dl": str(r["img_dl"]),
        }
        for h in header:
            widths[h] = max(widths[h], len(s[h]))
        fmt_rows.append(s)

    sep_line = "| " + " | ".join("-" * widths[h] for h in header) + " |"
    head_line = "| " + " | ".join(h.ljust(widths[h]) for h in header) + " |"
    print(head_line)
    print(sep_line)
    for s in fmt_rows:
        print("| " + " | ".join(s[h].ljust(widths[h]) for h in header) + " |")


if __name__ == "__main__":
    main(sys.argv[1:] or ["/mnt/data/Projects/shadPS4/bench"])
