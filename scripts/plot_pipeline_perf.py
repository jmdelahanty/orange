#!/usr/bin/env python3
import argparse
import csv
from datetime import datetime, timezone
import glob
import json
import math
import os
import re
import statistics
import sys

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")

import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator


CAM_FILE_RE = re.compile(r"Cam([^/]+?)_pipeline_perf\.csv$")

DEFAULT_FPS_FIELDS = [
    "acq_fps",
    "pre_fps",
    "enc_fps",
]

DEFAULT_QUEUE_FIELDS = [
    "display_q",
    "yolo_q",
    "pre_q",
    "enc_q",
    "pending_requeues",
]

DEFAULT_RESOURCE_FIELDS = [
    "acq_free_entries",
    "acq_free_entries_low",
    "acq_free_events",
    "acq_free_events_low",
    "pre_buffers",
    "pre_events",
]

DEFAULT_COUNTER_FIELDS = [
    "acq_starve",
    "pre_waits",
    "pre_drops",
    "enc_fail",
    "enc_slow",
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot pipeline perf CSVs (Cam*_pipeline_perf.csv)."
    )
    parser.add_argument(
        "paths",
        nargs="+",
        help="CSV file(s) or directory(ies) containing Cam*_pipeline_perf.csv",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Recursively search directories for Cam*_pipeline_perf.csv.",
    )
    parser.add_argument(
        "--x",
        default="elapsed_s",
        choices=["index", "elapsed_s", "frame_id", "recording_frame_id"],
        help="X-axis field (default: %(default)s).",
    )
    parser.add_argument(
        "--fps-fields",
        default=",".join(DEFAULT_FPS_FIELDS),
        help="Comma-separated FPS fields to plot (default: %(default)s).",
    )
    parser.add_argument(
        "--queue-fields",
        default=",".join(DEFAULT_QUEUE_FIELDS),
        help="Comma-separated queue-depth fields to plot (default: %(default)s).",
    )
    parser.add_argument(
        "--resource-fields",
        default=",".join(DEFAULT_RESOURCE_FIELDS),
        help="Comma-separated resource fields to plot (default: %(default)s).",
    )
    parser.add_argument(
        "--counter-fields",
        default=",".join(DEFAULT_COUNTER_FIELDS),
        help="Comma-separated counter fields to plot (default: %(default)s).",
    )
    parser.add_argument(
        "--stats-fields",
        default="",
        help=(
            "Comma-separated fields to summarize. "
            "Default: union of plotted fields plus gpu_direct,gpu_ring,gpu_copy."
        ),
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
        help="Print per-file summary stats.",
    )
    parser.add_argument(
        "--stats-out",
        default="",
        help="Write stats CSV (directory or file path, default: disabled).",
    )
    return parser.parse_args()


def parse_field_list(text):
    return [field.strip() for field in text.split(",") if field.strip()]


def collect_csv_paths(paths, recursive):
    collected = set()
    for path in paths:
        if os.path.isdir(path):
            if recursive:
                for root, _, _ in os.walk(path):
                    for csv_path in glob.glob(os.path.join(root, "Cam*_pipeline_perf.csv")):
                        collected.add(csv_path)
            else:
                for csv_path in glob.glob(os.path.join(path, "Cam*_pipeline_perf.csv")):
                    collected.add(csv_path)
        else:
            collected.add(path)
    return sorted(collected)


def read_csv(path):
    columns = {}
    with open(path, "r", newline="") as handle:
        reader = csv.DictReader(handle)
        for name in reader.fieldnames or []:
            columns[name] = []
        for row in reader:
            for name, value in row.items():
                columns[name].append(value)
    return columns


def to_float_list(values):
    out = []
    for value in values:
        try:
            out.append(float(value))
        except (TypeError, ValueError):
            out.append(float("nan"))
    return out


def to_int_list(values):
    out = []
    for value in values:
        try:
            out.append(int(value))
        except (TypeError, ValueError):
            out.append(0)
    return out


def moving_average(values, window):
    if window <= 1:
        return values
    out = []
    acc = 0.0
    q = []
    for value in values:
        q.append(0.0 if math.isnan(value) else value)
        acc += q[-1]
        if len(q) > window:
            acc -= q.pop(0)
        out.append(acc / len(q))
    return out


def parse_timestamp_utc(value):
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None


def make_x_axis(columns, x_field):
    if x_field == "index":
        return list(range(len(next(iter(columns.values()), []))))
    if x_field in ("frame_id", "recording_frame_id"):
        return to_int_list(columns.get(x_field, []))

    timestamps = [parse_timestamp_utc(value) for value in columns.get("timestamp_utc", [])]
    valid = [ts for ts in timestamps if ts is not None]
    if not valid:
        return list(range(len(timestamps)))
    t0 = valid[0]
    out = []
    for ts in timestamps:
        if ts is None:
            out.append(float("nan"))
        else:
            out.append((ts - t0).total_seconds())
    return out


def downsample_stride(length, max_points):
    if max_points <= 0:
        return 1
    return max(1, int(math.ceil(length / max_points)))


def downsample(values, stride):
    if stride <= 1:
        return values
    return values[::stride]


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
    original = []
    for value in values:
        if math.isnan(value) or value < 0.0:
            continue
        cleaned.append(value)
        original.append(value)
    if not cleaned:
        return None
    cleaned.sort()
    count = len(cleaned)
    mean = statistics.mean(cleaned)
    stdev = statistics.pstdev(cleaned) if count > 1 else 0.0
    first = original[0]
    last = original[-1]
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
        "first": first,
        "last": last,
        "delta": last - first,
    }


def resolve_stats_fields(fps_fields, queue_fields, resource_fields, counter_fields, stats_fields_arg):
    if stats_fields_arg:
        return parse_field_list(stats_fields_arg)
    out = []
    for field in fps_fields + queue_fields + resource_fields + counter_fields:
        if field not in out:
            out.append(field)
    for field in ("gpu_direct", "gpu_ring", "gpu_copy"):
        if field not in out:
            out.append(field)
    return out


def camera_id_from_path(path):
    match = CAM_FILE_RE.search(path)
    if match:
        return match.group(1)
    return os.path.splitext(os.path.basename(path))[0]


def load_snapshot_metadata(path):
    camera_serial = camera_id_from_path(path)
    metadata = {
        "recording_id": "",
        "camera_serial": camera_serial,
        "gpu_id": "",
        "codec": "",
        "preset": "",
        "tuning": "",
        "width": "",
        "height": "",
        "target_fps": "",
        "snapshot_path": "",
    }

    snapshot_path = os.path.join(os.path.dirname(path), "recording_snapshot.json")
    if not os.path.isfile(snapshot_path):
        return metadata

    metadata["snapshot_path"] = snapshot_path
    try:
        with open(snapshot_path, "r", encoding="utf-8") as handle:
            snapshot = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return metadata

    metadata["recording_id"] = str(snapshot.get("recording_id", "") or "")

    pipeline_info = snapshot.get("pipeline_metrics", {}).get(camera_serial, {})
    if isinstance(pipeline_info, dict):
        gpu_id = pipeline_info.get("gpu_id")
        if gpu_id is not None:
            metadata["gpu_id"] = str(gpu_id)

    encoder_info = snapshot.get("encoders", {}).get(camera_serial, {})
    if isinstance(encoder_info, dict) and "outputs" in encoder_info:
        outputs = encoder_info.get("outputs", {})
        if isinstance(outputs, dict):
            encoder_info = outputs.get("full") or next(iter(outputs.values()), {})
    if isinstance(encoder_info, dict):
        metadata["codec"] = str(encoder_info.get("codec", "") or "")
        metadata["preset"] = str(encoder_info.get("preset", "") or "")
        metadata["tuning"] = str(encoder_info.get("tuning", "") or "")
        gpu_id = encoder_info.get("gpu_id")
        if gpu_id is not None and metadata["gpu_id"] == "":
            metadata["gpu_id"] = str(gpu_id)
        resolution = encoder_info.get("resolution", {})
        if isinstance(resolution, dict):
            width = resolution.get("width")
            height = resolution.get("height")
            if width is not None:
                metadata["width"] = str(width)
            if height is not None:
                metadata["height"] = str(height)
        fps = encoder_info.get("fps")
        if fps is not None:
            metadata["target_fps"] = str(fps)

    return metadata


def make_title(path, metadata):
    parts = [os.path.basename(path)]
    if metadata["camera_serial"]:
        parts.append(f"Cam {metadata['camera_serial']}")

    detail_parts = []
    if metadata["gpu_id"]:
        detail_parts.append(f"GPU {metadata['gpu_id']}")
    if metadata["codec"]:
        codec = metadata["codec"]
        if metadata["preset"]:
            codec += f" {metadata['preset']}"
        if metadata["tuning"]:
            codec += f" {metadata['tuning']}"
        detail_parts.append(codec)
    if metadata["width"] and metadata["height"]:
        resolution = f"{metadata['width']}x{metadata['height']}"
        if metadata["target_fps"]:
            resolution += f"@{metadata['target_fps']}"
        detail_parts.append(resolution)
    if metadata["recording_id"]:
        detail_parts.append(metadata["recording_id"])

    if detail_parts:
        parts.append(" | ".join(detail_parts))
    return "\n".join(parts)


def emit_stats(path, metadata, stats_rows, stats_out, print_stats):
    if not stats_rows:
        return

    if print_stats:
        print(f"[PIPELINE_STATS] {os.path.basename(path)}")
        meta_parts = []
        if metadata["recording_id"]:
            meta_parts.append(f"recording_id={metadata['recording_id']}")
        if metadata["camera_serial"]:
            meta_parts.append(f"camera={metadata['camera_serial']}")
        if metadata["gpu_id"]:
            meta_parts.append(f"gpu={metadata['gpu_id']}")
        if metadata["codec"]:
            meta_parts.append(f"codec={metadata['codec']}")
        if metadata["preset"]:
            meta_parts.append(f"preset={metadata['preset']}")
        if metadata["width"] and metadata["height"]:
            meta_parts.append(f"resolution={metadata['width']}x{metadata['height']}")
        if meta_parts:
            print("meta," + ",".join(meta_parts))
        print("field,count,mean,p50,p90,p95,p99,min,max,stdev,first,last,delta")
        for row in stats_rows:
            print(
                f"{row['field']},{row['count']},"
                f"{row['mean']:.6f},{row['p50']:.6f},"
                f"{row['p90']:.6f},{row['p95']:.6f},"
                f"{row['p99']:.6f},{row['min']:.6f},"
                f"{row['max']:.6f},{row['stdev']:.6f},"
                f"{row['first']:.6f},{row['last']:.6f},{row['delta']:.6f}"
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
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        write_header = not os.path.exists(out_path)

    with open(out_path, "a", newline="") as handle:
        writer = csv.writer(handle)
        if write_header:
            writer.writerow(
                [
                    "file",
                    "recording_id",
                    "camera_serial",
                    "gpu_id",
                    "codec",
                    "preset",
                    "tuning",
                    "width",
                    "height",
                    "target_fps",
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
                    "first",
                    "last",
                    "delta",
                ]
            )
        for row in stats_rows:
            writer.writerow(
                [
                    os.path.basename(path),
                    metadata["recording_id"],
                    metadata["camera_serial"],
                    metadata["gpu_id"],
                    metadata["codec"],
                    metadata["preset"],
                    metadata["tuning"],
                    metadata["width"],
                    metadata["height"],
                    metadata["target_fps"],
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
                    f"{row['first']:.6f}",
                    f"{row['last']:.6f}",
                    f"{row['delta']:.6f}",
                ]
            )


def plot_group(ax, x, columns, fields, stride, smooth, ylabel):
    plotted = False
    for field in fields:
        if field not in columns:
            continue
        series = downsample(to_float_list(columns[field]), stride)
        if smooth > 1:
            series = moving_average(series, smooth)
        ax.plot(x, series, label=field, linewidth=1.0)
        plotted = True

    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)
    ax.yaxis.set_major_locator(MaxNLocator(6))
    if plotted:
        ax.legend(loc="upper right", ncol=2, fontsize=8)
    else:
        ax.text(0.5, 0.5, "No matching fields", ha="center", va="center", transform=ax.transAxes)


def plot_file(
    path,
    x_field,
    fps_fields,
    queue_fields,
    resource_fields,
    counter_fields,
    max_points,
    smooth,
    out_dir,
    show,
    stats_fields,
    stats_out,
    print_stats,
):
    columns = read_csv(path)
    if not columns or not next(iter(columns.values()), []):
        print(f"[WARN] No data in {path}")
        return

    metadata = load_snapshot_metadata(path)
    x = make_x_axis(columns, x_field)
    stride = downsample_stride(len(x), max_points)
    x = downsample(x, stride)

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

    fig, axes = plt.subplots(4, 1, sharex=True, figsize=(14, 11))
    fig.suptitle(make_title(path, metadata))

    plot_group(axes[0], x, columns, fps_fields, stride, smooth, "FPS")
    plot_group(axes[1], x, columns, queue_fields, stride, smooth, "Queue depth")
    plot_group(axes[2], x, columns, resource_fields, stride, smooth, "Resources")
    plot_group(axes[3], x, columns, counter_fields, stride, smooth, "Counters")
    axes[3].set_xlabel(x_field)

    fig.tight_layout()

    if show:
        plt.show()
        emit_stats(path, metadata, stats_rows, stats_out, print_stats)
        return

    resolved_out_dir = out_dir or os.path.dirname(path)
    os.makedirs(resolved_out_dir, exist_ok=True)
    out_path = os.path.join(
        resolved_out_dir, os.path.basename(path).replace(".csv", "_plot.png")
    )
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Wrote {out_path}")
    emit_stats(path, metadata, stats_rows, stats_out, print_stats)


def main():
    args = parse_args()
    csv_paths = collect_csv_paths(args.paths, args.recursive)
    if not csv_paths:
        print("No CSV files found.")
        return 1

    fps_fields = parse_field_list(args.fps_fields)
    queue_fields = parse_field_list(args.queue_fields)
    resource_fields = parse_field_list(args.resource_fields)
    counter_fields = parse_field_list(args.counter_fields)
    stats_fields = resolve_stats_fields(
        fps_fields,
        queue_fields,
        resource_fields,
        counter_fields,
        args.stats_fields,
    )

    for path in csv_paths:
        plot_file(
            path,
            args.x,
            fps_fields,
            queue_fields,
            resource_fields,
            counter_fields,
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
