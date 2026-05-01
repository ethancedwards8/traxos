#!/usr/bin/env python3
"""Run pt-paxos batches and summarize completed operation counts."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


RESULT_RE = re.compile(
    r"^\s*(?P<lock>\d+)\s+lock,\s+"
    r"(?P<write>\d+)\s+write,\s+"
    r"(?P<clear>\d+)\s+clear,\s+"
    r"(?P<unlock>\d+)\s+unlock\s*$"
)

METRICS = ("lock", "write", "clear", "unlock", "total")


@dataclass(frozen=True)
class RunResult:
    batch: int
    run: int
    lock: int
    write: int
    clear: int
    unlock: int

    @property
    def total(self) -> int:
        return self.lock + self.write + self.clear + self.unlock


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run build/pt-paxos -R 200, collect each per-seed result line, "
            "and write stats plus an SVG graph."
        )
    )
    parser.add_argument(
        "--binary",
        default="build/pt-paxos",
        help="pt-paxos binary to run (default: build/pt-paxos)",
    )
    parser.add_argument(
        "-R",
        "--random-seeds",
        type=int,
        default=200,
        help="value to pass to pt-paxos -R (default: 200)",
    )
    parser.add_argument(
        "--batches",
        type=int,
        default=1,
        help="number of times to invoke the -R run (default: 1)",
    )
    parser.add_argument(
        "--out-dir",
        default="pt-paxos-stats",
        help="directory for logs, CSV, JSON, text summary, and graph (default: pt-paxos-stats)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=None,
        help="optional timeout in seconds for each batch",
    )
    parser.add_argument(
        "pt_paxos_args",
        nargs=argparse.REMAINDER,
        help="extra pt-paxos arguments after --, for example: -- -l 0.05",
    )
    return parser.parse_args()


def percentile(values: list[int], pct: float) -> float:
    if not values:
        return math.nan
    if len(values) == 1:
        return float(values[0])
    ordered = sorted(values)
    pos = (len(ordered) - 1) * pct
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return float(ordered[lo])
    weight = pos - lo
    return ordered[lo] * (1 - weight) + ordered[hi] * weight


def summarize(results: list[RunResult]) -> dict[str, dict[str, float]]:
    summary: dict[str, dict[str, float]] = {}
    for metric in METRICS:
        values = [int(getattr(result, metric)) for result in results]
        if not values:
            summary[metric] = {}
            continue
        summary[metric] = {
            "count": len(values),
            "min": min(values),
            "max": max(values),
            "mean": statistics.fmean(values),
            "median": statistics.median(values),
            "variance": statistics.variance(values) if len(values) > 1 else 0.0,
            "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
            "p10": percentile(values, 0.10),
            "p90": percentile(values, 0.90),
        }
    return summary


def process_output(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode(errors="replace")
    return value


def run_batch(args: argparse.Namespace, batch: int, out_dir: Path) -> tuple[list[RunResult], dict[str, object]]:
    extra_args = args.pt_paxos_args
    if extra_args and extra_args[0] == "--":
        extra_args = extra_args[1:]
    command = [args.binary, "-R", str(args.random_seeds), *extra_args]
    log = {
        "batch": batch,
        "command": command,
        "returncode": None,
        "timed_out": False,
    }

    try:
        completed = subprocess.run(
            command,
            text=True,
            capture_output=True,
            timeout=args.timeout,
            check=False,
        )
        stdout = process_output(completed.stdout)
        stderr = process_output(completed.stderr)
        log["returncode"] = completed.returncode
    except subprocess.TimeoutExpired as exc:
        stdout = process_output(exc.stdout)
        stderr = process_output(exc.stderr)
        log["timed_out"] = True
        log["returncode"] = "timeout"

    (out_dir / f"batch-{batch:03d}.stdout").write_text(stdout)
    (out_dir / f"batch-{batch:03d}.stderr").write_text(stderr)

    results: list[RunResult] = []
    for line in stdout.splitlines():
        match = RESULT_RE.match(line)
        if not match:
            continue
        results.append(
            RunResult(
                batch=batch,
                run=len(results) + 1,
                lock=int(match.group("lock")),
                write=int(match.group("write")),
                clear=int(match.group("clear")),
                unlock=int(match.group("unlock")),
            )
        )

    log["parsed_results"] = len(results)
    return results, log


def write_csv(path: Path, results: list[RunResult]) -> None:
    with path.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=("batch", "run", *METRICS))
        writer.writeheader()
        for result in results:
            writer.writerow(
                {
                    "batch": result.batch,
                    "run": result.run,
                    "lock": result.lock,
                    "write": result.write,
                    "clear": result.clear,
                    "unlock": result.unlock,
                    "total": result.total,
                }
            )


def write_summary_text(path: Path, summary: dict[str, dict[str, float]], logs: list[dict[str, object]]) -> None:
    lines = ["pt-paxos stats", ""]
    lines.append("Batches:")
    for log in logs:
        lines.append(
            f"  batch {log['batch']}: parsed={log['parsed_results']} "
            f"returncode={log['returncode']} timed_out={log['timed_out']}"
        )
    lines.append("")
    lines.append("Metric       count       mean     median        var      stdev        min        max        p10        p90")
    for metric in METRICS:
        stats = summary.get(metric, {})
        if not stats:
            continue
        lines.append(
            f"{metric:<8} "
            f"{stats['count']:>8.0f} "
            f"{stats['mean']:>10.2f} "
            f"{stats['median']:>10.2f} "
            f"{stats['variance']:>10.2f} "
            f"{stats['stdev']:>10.2f} "
            f"{stats['min']:>10.0f} "
            f"{stats['max']:>10.0f} "
            f"{stats['p10']:>10.2f} "
            f"{stats['p90']:>10.2f}"
        )
    path.write_text("\n".join(lines) + "\n")


def write_svg(path: Path, results: list[RunResult], summary: dict[str, dict[str, float]]) -> None:
    width = 960
    height = 560
    margin_left = 70
    margin_right = 30
    margin_top = 50
    margin_bottom = 70
    plot_w = width - margin_left - margin_right
    plot_h = height - margin_top - margin_bottom
    totals = [result.total for result in results]

    if not totals:
        path.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="960" height="180">'
            '<text x="24" y="48" font-family="sans-serif" font-size="20">No pt-paxos results parsed</text>'
            "</svg>\n"
        )
        return

    y_min = min(totals)
    y_max = max(totals)
    if y_min == y_max:
        y_min -= 1
        y_max += 1

    def x_for(index: int) -> float:
        if len(totals) == 1:
            return margin_left + plot_w / 2
        return margin_left + index * plot_w / (len(totals) - 1)

    def y_for(value: float) -> float:
        return margin_top + (y_max - value) * plot_h / (y_max - y_min)

    points = " ".join(f"{x_for(i):.2f},{y_for(value):.2f}" for i, value in enumerate(totals))
    mean_y = y_for(summary["total"]["mean"])
    median_y = y_for(summary["total"]["median"])

    elements = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        "<style>text{font-family:Arial,sans-serif;fill:#222}.axis{stroke:#222;stroke-width:1}.grid{stroke:#ddd;stroke-width:1}.series{fill:none;stroke:#1261a6;stroke-width:2}.mean{stroke:#c0352b;stroke-dasharray:6 4}.median{stroke:#247a3d;stroke-dasharray:4 4}.dot{fill:#1261a6}</style>",
        f'<text x="{margin_left}" y="28" font-size="22">pt-paxos completed operations per run</text>',
        f'<line class="axis" x1="{margin_left}" y1="{margin_top + plot_h}" x2="{margin_left + plot_w}" y2="{margin_top + plot_h}"/>',
        f'<line class="axis" x1="{margin_left}" y1="{margin_top}" x2="{margin_left}" y2="{margin_top + plot_h}"/>',
    ]

    for tick in range(6):
        value = y_min + (y_max - y_min) * tick / 5
        y = y_for(value)
        elements.append(f'<line class="grid" x1="{margin_left}" y1="{y:.2f}" x2="{margin_left + plot_w}" y2="{y:.2f}"/>')
        elements.append(f'<text x="{margin_left - 12}" y="{y + 4:.2f}" font-size="12" text-anchor="end">{value:.0f}</text>')

    elements.append(f'<polyline class="series" points="{points}"/>')
    for i, value in enumerate(totals):
        if len(totals) <= 250 or i % max(1, len(totals) // 250) == 0:
            elements.append(f'<circle class="dot" cx="{x_for(i):.2f}" cy="{y_for(value):.2f}" r="2"/>')

    elements.extend(
        [
            f'<line class="mean" x1="{margin_left}" y1="{mean_y:.2f}" x2="{margin_left + plot_w}" y2="{mean_y:.2f}"/>',
            f'<line class="median" x1="{margin_left}" y1="{median_y:.2f}" x2="{margin_left + plot_w}" y2="{median_y:.2f}"/>',
            f'<text x="{margin_left + plot_w - 8}" y="{mean_y - 6:.2f}" font-size="12" text-anchor="end">mean {summary["total"]["mean"]:.2f}</text>',
            f'<text x="{margin_left + plot_w - 8}" y="{median_y + 16:.2f}" font-size="12" text-anchor="end">median {summary["total"]["median"]:.2f}</text>',
            f'<text x="{margin_left + plot_w / 2}" y="{height - 22}" font-size="14" text-anchor="middle">run number</text>',
            f'<text x="18" y="{margin_top + plot_h / 2}" font-size="14" transform="rotate(-90 18 {margin_top + plot_h / 2})" text-anchor="middle">completed operations</text>',
            f'<text x="{margin_left}" y="{height - 46}" font-size="12">runs={len(totals)} min={min(totals)} max={max(totals)} stdev={summary["total"]["stdev"]:.2f}</text>',
            "</svg>",
        ]
    )
    path.write_text("\n".join(elements) + "\n")


def main() -> int:
    args = parse_args()
    if args.random_seeds <= 0:
        print("-R/--random-seeds must be positive", file=sys.stderr)
        return 2
    if args.batches <= 0:
        print("--batches must be positive", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    all_results: list[RunResult] = []
    logs: list[dict[str, object]] = []
    for batch in range(1, args.batches + 1):
        print(f"running batch {batch}/{args.batches}: {args.binary} -R {args.random_seeds}", file=sys.stderr)
        results, log = run_batch(args, batch, out_dir)
        all_results.extend(results)
        logs.append(log)

    summary = summarize(all_results)
    write_csv(out_dir / "results.csv", all_results)
    (out_dir / "summary.json").write_text(
        json.dumps({"batches": logs, "summary": summary}, indent=2, sort_keys=True) + "\n"
    )
    write_summary_text(out_dir / "summary.txt", summary, logs)
    write_svg(out_dir / "graph.svg", all_results, summary)

    print(f"parsed {len(all_results)} result lines")
    print(f"wrote {out_dir / 'results.csv'}")
    print(f"wrote {out_dir / 'summary.txt'}")
    print(f"wrote {out_dir / 'summary.json'}")
    print(f"wrote {out_dir / 'graph.svg'}")
    return 0 if all(log["returncode"] == 0 for log in logs) and all_results else 1


if __name__ == "__main__":
    raise SystemExit(main())
