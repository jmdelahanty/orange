#!/usr/bin/env python3
"""Compare per-camera timestamp alignment from Cam*_meta.csv files."""

import argparse
import csv
import math
import os
import re
import statistics
import sys
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


CAM_FILE_RE = re.compile(r"Cam([^/]+?)_meta\.csv$")


@dataclass
class CameraData:
    camera_id: str
    path: str
    # join_key -> timestamp
    series: Dict[int, int]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare timestamp skew across camera metadata CSV files "
            "(Cam*_meta.csv)."
        )
    )
    parser.add_argument(
        "paths",
        nargs="+",
        help="CSV file(s) and/or directory(ies) to scan.",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Recursively search directories for Cam*_meta.csv.",
    )
    parser.add_argument(
        "--join-field",
        default="frame_id",
        help="Field used to align rows across cameras (default: frame_id).",
    )
    parser.add_argument(
        "--timestamp-field",
        default="timestamp",
        help="Timestamp column to compare (default: timestamp).",
    )
    parser.add_argument(
        "--reference",
        default="",
        help=(
            "Reference camera id (e.g. 2010096) or CSV path. "
            "Default: first camera by id."
        ),
    )
    parser.add_argument(
        "--units",
        choices=("ns", "us", "ms"),
        default="us",
        help="Units for printed skew stats (default: us).",
    )
    parser.add_argument(
        "--summary-out",
        default="",
        help="Optional output CSV for summary stats.",
    )
    return parser.parse_args()


def collect_csv_paths(paths: Sequence[str], recursive: bool) -> List[str]:
    out = set()
    for path in paths:
        if os.path.isdir(path):
            if recursive:
                for root, _, files in os.walk(path):
                    for name in files:
                        if CAM_FILE_RE.match(name):
                            out.add(os.path.join(root, name))
            else:
                for name in os.listdir(path):
                    if CAM_FILE_RE.match(name):
                        out.add(os.path.join(path, name))
        elif os.path.isfile(path):
            out.add(path)
    return sorted(out)


def camera_id_from_path(path: str) -> str:
    name = os.path.basename(path)
    match = CAM_FILE_RE.match(name)
    if match:
        return match.group(1)
    # fallback for non-standard name
    return os.path.splitext(name)[0]


def parse_int_field(row: dict, key: str) -> Optional[int]:
    val = row.get(key)
    if val is None or val == "":
        return None
    try:
        return int(val)
    except ValueError:
        return None


def load_camera(path: str, join_field: str, timestamp_field: str) -> CameraData:
    camera_id = camera_id_from_path(path)
    series: Dict[int, int] = {}

    with open(path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            join_val = parse_int_field(row, join_field)
            ts_val = parse_int_field(row, timestamp_field)
            if join_val is None or ts_val is None:
                continue
            # keep first value for a join key
            series.setdefault(join_val, ts_val)

    return CameraData(camera_id=camera_id, path=path, series=series)


def percentile(sorted_vals: Sequence[float], pct: float) -> float:
    if not sorted_vals:
        return float("nan")
    if pct <= 0:
        return float(sorted_vals[0])
    if pct >= 100:
        return float(sorted_vals[-1])
    k = (len(sorted_vals) - 1) * (pct / 100.0)
    f = int(math.floor(k))
    c = int(math.ceil(k))
    if f == c:
        return float(sorted_vals[f])
    d0 = sorted_vals[f] * (c - k)
    d1 = sorted_vals[c] * (k - f)
    return float(d0 + d1)


def unit_scale(units: str) -> float:
    if units == "ns":
        return 1.0
    if units == "us":
        return 1_000.0
    if units == "ms":
        return 1_000_000.0
    raise ValueError(f"unsupported units: {units}")


def fmt_scaled(value: float, scale: float) -> str:
    return f"{value / scale:.3f}"


def summarize(values: Sequence[int]) -> dict:
    if not values:
        return {}
    vals = sorted(float(v) for v in values)
    return {
        "count": len(vals),
        "mean": statistics.mean(vals),
        "stdev": statistics.pstdev(vals) if len(vals) > 1 else 0.0,
        "p50": percentile(vals, 50),
        "p90": percentile(vals, 90),
        "p95": percentile(vals, 95),
        "p99": percentile(vals, 99),
        "min": vals[0],
        "max": vals[-1],
        "first": vals[0],
        "last": vals[-1],
    }


def summarize_frame_interval_ns(series: Dict[int, int]) -> Optional[dict]:
    if len(series) < 2:
        return None
    keys = sorted(series.keys())
    intervals = []
    for i in range(1, len(keys)):
        prev_k = keys[i - 1]
        cur_k = keys[i]
        intervals.append(series[cur_k] - series[prev_k])
    return summarize(intervals)


def resolve_reference(cameras: Sequence[CameraData], reference: str) -> CameraData:
    if not reference:
        return sorted(cameras, key=lambda c: c.camera_id)[0]

    for cam in cameras:
        if cam.camera_id == reference:
            return cam
        if os.path.abspath(cam.path) == os.path.abspath(reference):
            return cam

    raise ValueError(f"reference '{reference}' did not match camera id or path")


def write_summary_csv(path: str, rows: Iterable[dict]) -> None:
    rows = list(rows)
    if not rows:
        return
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "reference_camera",
                "camera",
                "samples",
                "mean_ns",
                "p95_ns",
                "p99_ns",
                "min_ns",
                "max_ns",
                "stdev_ns",
                "drift_ns",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    csv_paths = collect_csv_paths(args.paths, args.recursive)

    if not csv_paths:
        print("No CSV files found. Expected Cam*_meta.csv files.", file=sys.stderr)
        return 1

    cameras = []
    for path in csv_paths:
        cam = load_camera(path, args.join_field, args.timestamp_field)
        if cam.series:
            cameras.append(cam)

    if not cameras:
        print(
            f"No usable rows found for join_field='{args.join_field}' and "
            f"timestamp_field='{args.timestamp_field}'.",
            file=sys.stderr,
        )
        return 1

    print(f"Loaded {len(cameras)} camera file(s):")
    for cam in sorted(cameras, key=lambda c: c.camera_id):
        print(f"  Cam{cam.camera_id}: {len(cam.series)} rows ({cam.path})")

    if len(cameras) < 2:
        print("Need at least 2 cameras to compare skew.")
        return 0

    try:
        ref = resolve_reference(cameras, args.reference)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    scale = unit_scale(args.units)
    print()
    print(
        f"Reference camera: Cam{ref.camera_id} "
        f"(join='{args.join_field}', ts='{args.timestamp_field}', units={args.units})"
    )

    summary_rows = []

    header = (
        f"{'camera':>10} {'samples':>8} {'mean':>11} {'p95':>11} "
        f"{'min':>11} {'max':>11} {'stdev':>11} {'drift':>11}"
    )
    print(header)
    print("-" * len(header))

    for cam in sorted(cameras, key=lambda c: c.camera_id):
        if cam.camera_id == ref.camera_id:
            continue
        common_keys = sorted(set(ref.series).intersection(cam.series))
        if len(common_keys) < 2:
            print(f"{('Cam' + cam.camera_id):>10} {len(common_keys):>8}  insufficient overlap")
            continue

        deltas = [cam.series[k] - ref.series[k] for k in common_keys]
        stats = summarize(deltas)
        drift = deltas[-1] - deltas[0]
        print(
            f"{('Cam' + cam.camera_id):>10} "
            f"{stats['count']:>8d} "
            f"{fmt_scaled(stats['mean'], scale):>11} "
            f"{fmt_scaled(stats['p95'], scale):>11} "
            f"{fmt_scaled(stats['min'], scale):>11} "
            f"{fmt_scaled(stats['max'], scale):>11} "
            f"{fmt_scaled(stats['stdev'], scale):>11} "
            f"{fmt_scaled(drift, scale):>11}"
        )

        summary_rows.append(
            {
                "reference_camera": ref.camera_id,
                "camera": cam.camera_id,
                "samples": stats["count"],
                "mean_ns": f"{stats['mean']:.3f}",
                "p95_ns": f"{stats['p95']:.3f}",
                "p99_ns": f"{stats['p99']:.3f}",
                "min_ns": f"{stats['min']:.3f}",
                "max_ns": f"{stats['max']:.3f}",
                "stdev_ns": f"{stats['stdev']:.3f}",
                "drift_ns": f"{drift:.3f}",
            }
        )

    print()
    print("Per-camera frame interval summary (same timestamp field):")
    int_header = f"{'camera':>10} {'samples':>8} {'mean':>11} {'p95':>11} {'min':>11} {'max':>11}"
    print(int_header)
    print("-" * len(int_header))
    for cam in sorted(cameras, key=lambda c: c.camera_id):
        stats = summarize_frame_interval_ns(cam.series)
        if not stats:
            print(f"{('Cam' + cam.camera_id):>10} {0:>8}  insufficient rows")
            continue
        print(
            f"{('Cam' + cam.camera_id):>10} "
            f"{stats['count']:>8d} "
            f"{fmt_scaled(stats['mean'], scale):>11} "
            f"{fmt_scaled(stats['p95'], scale):>11} "
            f"{fmt_scaled(stats['min'], scale):>11} "
            f"{fmt_scaled(stats['max'], scale):>11}"
        )

    if args.summary_out:
        write_summary_csv(args.summary_out, summary_rows)
        print()
        print(f"Wrote summary CSV: {args.summary_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
