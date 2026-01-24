#!/usr/bin/env python3
import argparse
import csv
import glob
import math
import os
import statistics
import sys

import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator


DEFAULT_TIMING_FIELDS = [
    "enqueue_ms",
    "infer_ms",
    "sync_ms",
    "queue_ms",
    "total_ms",
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot YOLO per-frame perf CSVs (Cam*_yolo_perf.csv)."
    )
    parser.add_argument(
        "paths",
        nargs="+",
        help="CSV file(s) or directory(ies) containing Cam*_yolo_perf.csv",
    )
    parser.add_argument(
        "--fields",
        default=",".join(DEFAULT_TIMING_FIELDS),
        help="Comma-separated timing fields to plot (default: %(default)s).",
    )
    parser.add_argument(
        "--x",
        default="timestamp_sys",
        choices=["index", "frame_id", "recording_frame_id", "timestamp", "timestamp_sys"],
        help="X-axis field (default: %(default)s).",
    )
    parser.add_argument(
        "--max-points",
        type=int,
        default=10000,
        help="Max points to plot (downsamples by stride, default: %(default)s).",
    )
    parser.add_argument(
        "--smooth",
        type=int,
        default=0,
        help="Moving average window (0 disables, default: %(default)s).",
    )
    parser.add_argument(
        "--out-dir",
        default="",
        help="Output directory for plots (default: same directory as CSV).",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="Show plots interactively instead of saving.",
    )
    parser.add_argument(
        "--stats",
        action="store_true",
        help="Print per-file summary stats for selected fields.",
    )
    parser.add_argument(
        "--stats-fields",
        default="",
        help="Comma-separated fields to summarize (default: queue_depth,fps + plot fields).",
    )
    parser.add_argument(
        "--stats-out",
        default="",
        help="Write stats CSV (directory or file path, default: disabled).",
    )
    return parser.parse_args()


def collect_csv_paths(paths):
    collected = []
    for path in paths:
        if os.path.isdir(path):
            collected.extend(
                glob.glob(os.path.join(path, "Cam*_yolo_perf.csv"))
            )
        else:
            collected.append(path)
    return sorted(set(collected))


def read_csv(path):
    columns = {}
    with open(path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for name in reader.fieldnames or []:
            columns[name] = []
        for row in reader:
            for name, val in row.items():
                columns[name].append(val)
    return columns


def to_float_list(values):
    out = []
    for v in values:
        try:
            out.append(float(v))
        except (TypeError, ValueError):
            out.append(float("nan"))
    return out


def to_int_list(values):
    out = []
    for v in values:
        try:
            out.append(int(v))
        except (TypeError, ValueError):
            out.append(0)
    return out


def moving_average(values, window):
    if window <= 1:
        return values
    out = []
    acc = 0.0
    q = []
    for v in values:
        if math.isnan(v):
            q.append(0.0)
        else:
            q.append(v)
        acc += q[-1]
        if len(q) > window:
            acc -= q.pop(0)
        out.append(acc / len(q))
    return out


def make_x_axis(columns, x_field):
    if x_field == "index":
        return list(range(len(next(iter(columns.values()), []))))
    values = columns.get(x_field, [])
    if x_field in ("frame_id", "recording_frame_id"):
        return to_int_list(values)
    x = to_float_list(values)
    if x:
        x0 = x[0]
        if not math.isnan(x0):
            x = [(v - x0) / 1e6 for v in x]
    return x


def downsample_stride(length, max_points):
    if max_points <= 0:
        return 1
    return max(1, int(math.ceil(length / max_points)))


def downsample(values, stride):
    if stride <= 1:
        return values
    return values[::stride]

def resolve_stats_fields(fields, stats_fields_arg):
    if stats_fields_arg:
        return [f.strip() for f in stats_fields_arg.split(",") if f.strip()]
    out = []
    for name in ("queue_depth", "fps"):
        if name not in out:
            out.append(name)
    for name in fields:
        if name not in out:
            out.append(name)
    return out


def percentile(sorted_vals, pct):
    if not sorted_vals:
        return float("nan")
    if pct <= 0:
        return sorted_vals[0]
    if pct >= 100:
        return sorted_vals[-1]
    k = (len(sorted_vals) - 1) * (pct / 100.0)
    f = int(math.floor(k))
    c = int(math.ceil(k))
    if f == c:
        return sorted_vals[f]
    d0 = sorted_vals[f] * (c - k)
    d1 = sorted_vals[c] * (k - f)
    return d0 + d1


def summarize_series(values):
    cleaned = []
    for v in values:
        if math.isnan(v) or v < 0.0:
            continue
        cleaned.append(v)
    if not cleaned:
        return None
    cleaned.sort()
    count = len(cleaned)
    mean = statistics.mean(cleaned)
    stdev = statistics.pstdev(cleaned) if count > 1 else 0.0
    return {
        "count": count,
        "mean": mean,
        "p50": percentile(cleaned, 50),
        "p90": percentile(cleaned, 90),
        "p95": percentile(cleaned, 95),
        "p99": percentile(cleaned, 99),
        "min": cleaned[0],
        "max": cleaned[-1],
        "stdev": stdev,
    }


def emit_stats(path, stats_rows, stats_out, print_stats):
    if not stats_rows:
        return
    if print_stats:
        print(f"[STATS] {os.path.basename(path)}")
        print("field,count,mean,p50,p90,p95,p99,min,max,stdev")
        for row in stats_rows:
            print(
                f"{row['field']},{row['count']},"
                f"{row['mean']:.6f},{row['p50']:.6f},"
                f"{row['p90']:.6f},{row['p95']:.6f},"
                f"{row['p99']:.6f},{row['min']:.6f},"
                f"{row['max']:.6f},{row['stdev']:.6f}"
            )
    if not stats_out:
        return
    if stats_out.endswith(os.sep) or os.path.isdir(stats_out):
        os.makedirs(stats_out, exist_ok=True)
        base = os.path.basename(path).replace(".csv", "_stats.csv")
        out_path = os.path.join(stats_out, base)
        write_header = True
    else:
        out_path = stats_out
        write_header = not os.path.exists(out_path)
    with open(out_path, "a", newline="") as f:
        writer = csv.writer(f)
        if write_header:
            writer.writerow(
                [
                    "file",
                    "field",
                    "count",
                    "mean",
                    "p50",
                    "p90",
                    "p95",
                    "p99",
                    "min",
                    "max",
                    "stdev",
                ]
            )
        for row in stats_rows:
            writer.writerow(
                [
                    os.path.basename(path),
                    row["field"],
                    row["count"],
                    f"{row['mean']:.6f}",
                    f"{row['p50']:.6f}",
                    f"{row['p90']:.6f}",
                    f"{row['p95']:.6f}",
                    f"{row['p99']:.6f}",
                    f"{row['min']:.6f}",
                    f"{row['max']:.6f}",
                    f"{row['stdev']:.6f}",
                ]
            )


def plot_file(path, fields, x_field, max_points, smooth, out_dir, show, stats_fields, stats_out, print_stats):
    columns = read_csv(path)
    if not columns:
        print(f"[WARN] No data in {path}")
        return

    x = make_x_axis(columns, x_field)
    stride = downsample_stride(len(x), max_points)
    x = downsample(x, stride)

    queue_depth = to_float_list(columns.get("queue_depth", []))
    fps = to_float_list(columns.get("fps", []))
    queue_depth = downsample(queue_depth, stride)
    fps = downsample(fps, stride)

    stats_rows = []
    if print_stats or stats_out:
        for field in stats_fields:
            if field not in columns:
                continue
            summary = summarize_series(to_float_list(columns[field]))
            if not summary:
                continue
            summary["field"] = field
            stats_rows.append(summary)

    fig, axes = plt.subplots(
        2, 1, sharex=True, figsize=(12, 8), gridspec_kw={"height_ratios": [1, 2]}
    )
    fig.suptitle(os.path.basename(path))

    ax0 = axes[0]
    ax0.plot(x, queue_depth, label="queue_depth", color="tab:blue", linewidth=1.0)
    ax0.set_ylabel("Queue Depth")
    ax0.grid(True, alpha=0.3)
    ax0b = ax0.twinx()
    ax0b.plot(x, fps, label="fps", color="tab:orange", linewidth=1.0)
    ax0b.set_ylabel("FPS")

    ax1 = axes[1]
    for field in fields:
        if field not in columns:
            continue
        series = to_float_list(columns[field])
        series = downsample(series, stride)
        if smooth > 1:
            series = moving_average(series, smooth)
        ax1.plot(x, series, label=field, linewidth=1.0)
    ax1.set_ylabel("ms")
    ax1.set_xlabel(x_field)
    ax1.grid(True, alpha=0.3)
    ax1.legend(loc="upper right", ncol=2, fontsize=8)
    ax1.yaxis.set_major_locator(MaxNLocator(6))

    handles0, labels0 = ax0.get_legend_handles_labels()
    handles1, labels1 = ax0b.get_legend_handles_labels()
    ax0.legend(handles0 + handles1, labels0 + labels1, loc="upper right", fontsize=8)

    fig.tight_layout()

    if show:
        plt.show()
        emit_stats(path, stats_rows, stats_out, print_stats)
        return

    if not out_dir:
        out_dir = os.path.dirname(path)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(
        out_dir, os.path.basename(path).replace(".csv", "_plot.png")
    )
    fig.savefig(out_path, dpi=150)
    print(f"Wrote {out_path}")
    emit_stats(path, stats_rows, stats_out, print_stats)


def main():
    args = parse_args()
    csv_paths = collect_csv_paths(args.paths)
    if not csv_paths:
        print("No CSV files found.")
        return 1
    fields = [f.strip() for f in args.fields.split(",") if f.strip()]
    stats_fields = resolve_stats_fields(fields, args.stats_fields)
    for path in csv_paths:
        plot_file(
            path,
            fields,
            args.x,
            args.max_points,
            args.smooth,
            args.out_dir,
            args.show,
            stats_fields,
            args.stats_out,
            args.stats,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
