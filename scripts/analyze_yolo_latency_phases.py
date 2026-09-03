#!/usr/bin/env python3
"""Phase-resolved YOLO latency analysis for one recording folder.

Reads Cam*_yolo_perf.csv (and, when present, the external recorder's
Cam*_external_gop_routing.csv and Cam*_external_detach.csv) and reports where
the detector's time goes, split by the things that were found to modulate it
in the 2026-08-25 four-camera review:

  * which encoder shard / GPU was active for the frame (same die as detect,
    or the other die of the A16 pair),
  * position within the split-GOP cycle (gop_length x shard count frames),
  * the decimated PTP register latch on the acquisition thread
    (frame_id mod ptp decimation),
  * periodic cudaEventRecord stalls (fractional second of run time),
  * the GUI preview cadence (frame_id mod 10).

Only numpy and the standard library are required. Output is a human-readable
summary on stdout and, with --json, a machine-readable file that the review
page or a notebook can consume.

Example:

  python3 scripts/analyze_yolo_latency_phases.py \\
      /home/jeremy/orange_data/exp/unsorted/2026_08_25_23_12_06 \\
      --json /tmp/latency_phases.json
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import sys
from typing import Any

import numpy as np

YOLO_PERF_SUFFIX = "_yolo_perf.csv"
ROUTING_SUFFIX = "_external_gop_routing.csv"
DETACH_SUFFIX = "_external_detach.csv"

# Columns pulled from Cam*_yolo_perf.csv. Missing columns are tolerated.
PERF_COLUMNS = [
    "frame_id",
    "recording_frame_id",
    "timestamp_sys",
    "ok",
    "fps",
    "queue_depth_at_worker_start",
    "acquisition_to_worker_start_ms",
    "acquisition_to_yolo_enqueue_ms",
    "acquisition_to_ptp_done_ms",
    "yolo_queue_wait_ms",
    "acquisition_to_detect_done_ms",
    "worker_start_to_detect_done_ms",
    "total_ms",
    "cpu_pre_sync_ms",
    "cpu_post_sync_ms",
    "cpu_event_record_ms",
    "yolo_affinity_effective_cpus",
    "sync_mode",
]

FLOAT_COLUMNS = {
    "fps",
    "acquisition_to_worker_start_ms",
    "acquisition_to_yolo_enqueue_ms",
    "acquisition_to_ptp_done_ms",
    "yolo_queue_wait_ms",
    "acquisition_to_detect_done_ms",
    "worker_start_to_detect_done_ms",
    "total_ms",
    "cpu_pre_sync_ms",
    "cpu_post_sync_ms",
    "cpu_event_record_ms",
    "copy_ms",
}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("run_dir", help="Recording folder containing Cam*_yolo_perf.csv")
    parser.add_argument(
        "--cameras",
        default="",
        help="Comma-separated camera serials to include (default: all found).",
    )
    parser.add_argument(
        "--steady-after",
        type=int,
        default=50,
        help="Ignore the first N rows per camera as warmup (default: %(default)s).",
    )
    parser.add_argument(
        "--cycle",
        type=int,
        default=0,
        help="Split-GOP cycle length in frames (gop_length x shards). "
        "0 derives it from the routing CSV, falling back to 50.",
    )
    parser.add_argument(
        "--ptp-decimate",
        type=int,
        default=100,
        help="PTP register latch decimation (default: %(default)s).",
    )
    parser.add_argument(
        "--stall-threshold-ms",
        type=float,
        default=1.0,
        help="cpu_event_record_ms above this counts as a driver stall (default: %(default)s).",
    )
    parser.add_argument(
        "--hist-bin-ms",
        type=float,
        default=0.05,
        help="Histogram bin width for total_ms (default: %(default)s).",
    )
    parser.add_argument(
        "--hist-range",
        default="2.0,6.0",
        help="Histogram range for total_ms as lo,hi (default: %(default)s).",
    )
    parser.add_argument(
        "--external-recorder-dir",
        default="",
        help="Directory holding Cam*_external_gop_routing.csv and Cam*_external_detach.csv "
        "(default: <run_dir>/external_recorder; headless runs write them to the "
        "recorder artifact_root under /tmp instead).",
    )
    parser.add_argument(
        "--baseline-json",
        default="",
        help="A result JSON from an earlier invocation; prints per-camera deltas "
        "(this run minus baseline) after the summary.",
    )
    parser.add_argument("--json", default="", help="Write the full result to this JSON path.")
    parser.add_argument(
        "--quiet", action="store_true", help="Suppress the stdout summary (JSON only)."
    )
    return parser.parse_args(argv)


def camera_serial_from_path(path: str, suffix: str) -> str | None:
    name = os.path.basename(path)
    match = re.fullmatch(r"Cam(.+)" + re.escape(suffix), name)
    return match.group(1) if match else None


def find_cameras(run_dir: str) -> list[str]:
    serials = []
    for name in sorted(os.listdir(run_dir)):
        serial = camera_serial_from_path(name, YOLO_PERF_SUFFIX)
        if serial:
            serials.append(serial)
    return serials


def read_columns(path: str, wanted: list[str]) -> dict[str, np.ndarray]:
    """Read selected columns from a CSV into numpy arrays (float64 or int64)."""
    with open(path, newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader)
        index = {name: i for i, name in enumerate(header)}
        present = [name for name in wanted if name in index]
        positions = [index[name] for name in present]
        buffers: list[list[str]] = [[] for _ in present]
        for row in reader:
            if len(row) < len(header):
                continue
            for slot, pos in enumerate(positions):
                buffers[slot].append(row[pos])
    columns: dict[str, np.ndarray] = {}
    for name, values in zip(present, buffers):
        if name in FLOAT_COLUMNS:
            columns[name] = np.asarray(values, dtype=np.float64)
        elif name in ("yolo_affinity_effective_cpus", "sync_mode"):
            columns[name] = np.asarray(values, dtype=object)
        else:
            columns[name] = np.asarray(values, dtype=np.int64)
    return columns


def quantiles(values: np.ndarray) -> dict[str, float]:
    valid = values[values >= 0]
    if valid.size == 0:
        return {"n": 0}
    return {
        "n": int(valid.size),
        "mean": float(valid.mean()),
        "p50": float(np.quantile(valid, 0.50)),
        "p95": float(np.quantile(valid, 0.95)),
        "p99": float(np.quantile(valid, 0.99)),
        "p999": float(np.quantile(valid, 0.999)),
        "max": float(valid.max()),
    }


def group_stats(values: np.ndarray, phase: np.ndarray, n_phases: int) -> dict[str, list[float]]:
    means, p95s, counts = [], [], []
    for k in range(n_phases):
        sel = values[phase == k]
        counts.append(int(sel.size))
        if sel.size:
            means.append(float(sel.mean()))
            p95s.append(float(np.quantile(sel, 0.95)))
        else:
            means.append(math.nan)
            p95s.append(math.nan)
    return {"mean": means, "p95": p95s, "count": counts}


def read_routing(path: str) -> dict[str, np.ndarray] | None:
    if not os.path.exists(path):
        return None
    cols = read_columns(
        path,
        ["recording_frame_id", "gop_index", "source_gpu_id", "assigned_gpu_id", "assigned_shard_id"],
    )
    if "recording_frame_id" not in cols or "assigned_gpu_id" not in cols:
        return None
    return cols


def read_detach(path: str) -> dict[str, Any] | None:
    if not os.path.exists(path):
        return None
    cols = read_columns(path, ["assigned_gpu_id", "copy_ms"])
    if "assigned_gpu_id" not in cols:
        return None
    copy_ms = np.asarray(cols.get("copy_ms", np.zeros(0)), dtype=np.float64)
    out: dict[str, Any] = {}
    for gpu in np.unique(cols["assigned_gpu_id"]):
        sel = copy_ms[cols["assigned_gpu_id"] == gpu]
        out[str(int(gpu))] = quantiles(sel)
    return out


def derive_cycle(routing: dict[str, np.ndarray] | None, fallback: int = 50) -> int:
    if routing is None or "gop_index" not in routing or "assigned_shard_id" not in routing:
        return fallback
    gop = routing["gop_index"]
    shards = np.unique(routing["assigned_shard_id"]).size
    if gop.size < 2 or shards == 0:
        return fallback
    # GOP length = frames sharing gop_index 0 (first GOP is complete in a long run).
    gop_len = int(np.sum(gop == gop[0]))
    if gop_len <= 0:
        return fallback
    return gop_len * max(1, shards)


def analyze_camera(
    run_dir: str,
    serial: str,
    args: argparse.Namespace,
    hist_edges: np.ndarray,
) -> dict[str, Any]:
    perf_path = os.path.join(run_dir, f"Cam{serial}{YOLO_PERF_SUFFIX}")
    perf = read_columns(perf_path, PERF_COLUMNS)
    n_total = int(perf["frame_id"].size) if "frame_id" in perf else 0
    steady = np.arange(n_total) >= args.steady_after
    for key in list(perf.keys()):
        perf[key] = perf[key][steady]
    n = int(perf["frame_id"].size)

    total = perf["total_ms"]
    a2d = perf["acquisition_to_detect_done_ms"]
    result: dict[str, Any] = {
        "camera": serial,
        "rows_total": n_total,
        "rows_steady": n,
        "steady_after": args.steady_after,
        "ok_fraction": float(perf["ok"].mean()) if "ok" in perf and n else math.nan,
        "fps_mean": float(perf["fps"].mean()) if "fps" in perf and n else math.nan,
        "affinity_cpus": (
            str(perf["yolo_affinity_effective_cpus"][0])
            if "yolo_affinity_effective_cpus" in perf and n
            else ""
        ),
        "sync_mode": str(perf["sync_mode"][0]) if "sync_mode" in perf and n else "unknown",
        "queue_depth_nonzero_fraction": (
            float((perf["queue_depth_at_worker_start"] > 0).mean())
            if "queue_depth_at_worker_start" in perf and n
            else math.nan
        ),
        "acquisition_to_detect_done_ms": quantiles(a2d),
        "worker_total_ms": quantiles(total),
        "acquisition_to_worker_start_ms": quantiles(perf["acquisition_to_worker_start_ms"]),
        "yolo_queue_wait_ms": quantiles(perf["yolo_queue_wait_ms"]),
        "cpu_pre_sync_ms": quantiles(perf["cpu_pre_sync_ms"]),
        "cpu_post_sync_ms": quantiles(perf["cpu_post_sync_ms"]),
    }
    gpu_wait = total - perf["cpu_pre_sync_ms"] - perf["cpu_post_sync_ms"]
    result["gpu_wait_ms"] = quantiles(gpu_wait)

    # Encoder die split and cycle phase (needs the external recorder routing CSV).
    recorder_dir = args.external_recorder_dir or os.path.join(run_dir, "external_recorder")
    routing = read_routing(os.path.join(recorder_dir, f"Cam{serial}{ROUTING_SUFFIX}"))
    cycle = args.cycle or derive_cycle(routing)
    result["cycle_frames"] = cycle
    rec_id = perf["recording_frame_id"]
    phase = rec_id % cycle
    result["cycle_phase"] = group_stats(total, phase, cycle)
    if routing is not None:
        lookup = dict(zip(routing["recording_frame_id"].tolist(), routing["assigned_gpu_id"].tolist()))
        source_gpu = dict(zip(routing["recording_frame_id"].tolist(), routing["source_gpu_id"].tolist()))
        assigned = np.asarray([lookup.get(int(r), -1) for r in rec_id.tolist()], dtype=np.int64)
        source = np.asarray([source_gpu.get(int(r), -1) for r in rec_id.tolist()], dtype=np.int64)
        known = assigned >= 0
        same = known & (assigned == source)
        other = known & (assigned != source)
        result["encoder_die_split"] = {
            "detect_gpu": int(source[known][0]) if known.any() else -1,
            "other_gpus": sorted({int(g) for g in assigned[other].tolist()}),
            "same_die": quantiles(total[same]),
            "other_die": quantiles(total[other]),
            "same_die_fraction": float(same.sum() / max(1, known.sum())),
        }
        hist_same, _ = np.histogram(total[same], bins=hist_edges)
        hist_other, _ = np.histogram(total[other], bins=hist_edges)
        result["total_ms_histogram"] = {
            "edges_ms": hist_edges.tolist(),
            "same_die": hist_same.tolist(),
            "other_die": hist_other.tolist(),
        }
    else:
        hist_all, _ = np.histogram(total, bins=hist_edges)
        result["encoder_die_split"] = None
        result["total_ms_histogram"] = {"edges_ms": hist_edges.tolist(), "all": hist_all.tolist()}

    result["detach_copy_ms_by_gpu"] = read_detach(
        os.path.join(recorder_dir, f"Cam{serial}{DETACH_SUFFIX}")
    )

    # PTP latch: acquisition-to-worker-start by frame_id mod decimation.
    dec = max(1, args.ptp_decimate)
    ptp_phase = perf["frame_id"] % dec
    a2ws = perf["acquisition_to_worker_start_ms"]
    latch = ptp_phase == 0
    result["ptp_latch"] = {
        "decimate": dec,
        "latch_frames": int(latch.sum()),
        "latch_frames_acq_to_worker_start_ms": quantiles(a2ws[latch]),
        "other_frames_acq_to_worker_start_ms": quantiles(a2ws[~latch]),
        "acq_to_ptp_done_on_latch_frames_ms": (
            quantiles(perf["acquisition_to_ptp_done_ms"][latch])
            if "acquisition_to_ptp_done_ms" in perf
            else None
        ),
        "by_phase_mean_ms": group_stats(a2ws, ptp_phase, dec)["mean"],
    }

    # Driver stalls seen as long cudaEventRecord, by fractional second.
    stalls = perf["cpu_event_record_ms"] > args.stall_threshold_ms
    ts = perf["timestamp_sys"].astype(np.float64) / 1e9
    t0 = float(ts[0]) if ts.size else 0.0
    frac = (ts[stalls] - t0) % 1.0
    hist_frac, _ = np.histogram(frac, bins=np.linspace(0.0, 1.0, 21))
    result["event_record_stalls"] = {
        "threshold_ms": args.stall_threshold_ms,
        "count": int(stalls.sum()),
        "duration_ms": quantiles(perf["cpu_event_record_ms"][stalls]),
        "fractional_second_hist_50ms": hist_frac.tolist(),
        "peak_bin_fraction": float(hist_frac.max() / max(1, hist_frac.sum())),
        "hidden_in_gpu_wait": bool(
            stalls.any() and total[stalls].mean() < result["worker_total_ms"].get("p95", math.inf)
        ),
    }

    # Preview cadence: total_ms by frame_id mod 10.
    result["preview_cadence_mod10"] = group_stats(total, perf["frame_id"] % 10, 10)

    return result


def fmt(value: Any, digits: int = 3) -> str:
    if isinstance(value, float):
        return "nan" if math.isnan(value) else f"{value:.{digits}f}"
    return str(value)


def print_summary(result: dict[str, Any]) -> None:
    print(f"run: {result['run_dir']}")
    print(f"cameras: {', '.join(result['cameras'].keys())}")
    print()
    header = (
        f"{'camera':>9} {'rows':>7} {'ok':>5} {'sync':>6} {'a2d mean':>9} {'a2d p95':>8} "
        f"{'a2d p99':>8} {'total mean':>10} {'total p95':>9} {'gpu wait':>8} {'queue':>6}"
    )
    print(header)
    for serial, cam in result["cameras"].items():
        a2d = cam["acquisition_to_detect_done_ms"]
        tot = cam["worker_total_ms"]
        print(
            f"{serial:>9} {cam['rows_steady']:>7} {fmt(cam['ok_fraction'], 3):>5} "
            f"{cam['sync_mode']:>6} {fmt(a2d.get('mean', math.nan)):>9} {fmt(a2d.get('p95', math.nan)):>8} "
            f"{fmt(a2d.get('p99', math.nan)):>8} {fmt(tot.get('mean', math.nan)):>10} "
            f"{fmt(tot.get('p95', math.nan)):>9} {fmt(cam['gpu_wait_ms'].get('mean', math.nan)):>8} "
            f"{fmt(cam['yolo_queue_wait_ms'].get('mean', math.nan)):>6}"
        )
    print()
    print("encoder die split (worker total_ms):")
    for serial, cam in result["cameras"].items():
        split = cam["encoder_die_split"]
        if not split:
            print(f"  {serial}: no external recorder routing CSV")
            continue
        s, o = split["same_die"], split["other_die"]
        print(
            f"  {serial}: detect GPU {split['detect_gpu']} | same die mean {fmt(s.get('mean', math.nan))} "
            f"p95 {fmt(s.get('p95', math.nan))} | other die mean {fmt(o.get('mean', math.nan))} "
            f"p95 {fmt(o.get('p95', math.nan))} | delta p95 "
            f"{fmt(s.get('p95', math.nan) - o.get('p95', math.nan))}"
        )
    print()
    print("cycle phase (mean total_ms by recording_frame_id mod cycle):")
    for serial, cam in result["cameras"].items():
        means = cam["cycle_phase"]["mean"]
        finite = [m for m in means if not math.isnan(m)]
        if finite:
            print(
                f"  {serial}: cycle {cam['cycle_frames']} | min {min(finite):.3f} max {max(finite):.3f} "
                f"spread {max(finite) - min(finite):.3f}"
            )
    print()
    print("PTP latch (acquisition_to_worker_start_ms):")
    for serial, cam in result["cameras"].items():
        p = cam["ptp_latch"]
        lf, of = p["latch_frames_acq_to_worker_start_ms"], p["other_frames_acq_to_worker_start_ms"]
        print(
            f"  {serial}: {p['latch_frames']} latch frames mean {fmt(lf.get('mean', math.nan))} "
            f"max {fmt(lf.get('max', math.nan))} | other frames mean {fmt(of.get('mean', math.nan))}"
        )
    print()
    print("cudaEventRecord stalls:")
    for serial, cam in result["cameras"].items():
        st = cam["event_record_stalls"]
        print(
            f"  {serial}: {st['count']} stalls > {st['threshold_ms']} ms, median "
            f"{fmt(st['duration_ms'].get('p50', math.nan))} ms, {st['peak_bin_fraction']*100:.0f}% in one "
            f"50 ms slice of the second"
        )
    print()
    print("preview cadence (mean total_ms by frame_id mod 10):")
    for serial, cam in result["cameras"].items():
        means = cam["preview_cadence_mod10"]["mean"]
        finite = [m for m in means if not math.isnan(m)]
        if finite:
            print(f"  {serial}: spread {max(finite) - min(finite):.3f} ms")


def print_comparison(result: dict[str, Any], baseline: dict[str, Any]) -> None:
    """Per-camera deltas, this run minus the baseline, for the headline metrics."""
    print()
    print(f"deltas vs baseline {baseline.get('run_dir', '?')} (this run minus baseline, ms):")
    metrics = [
        ("acq→detect mean", lambda c: c["acquisition_to_detect_done_ms"].get("mean")),
        ("acq→detect p95", lambda c: c["acquisition_to_detect_done_ms"].get("p95")),
        ("acq→detect p99", lambda c: c["acquisition_to_detect_done_ms"].get("p99")),
        ("worker total mean", lambda c: c["worker_total_ms"].get("mean")),
        ("worker total p95", lambda c: c["worker_total_ms"].get("p95")),
        ("gpu wait mean", lambda c: c["gpu_wait_ms"].get("mean")),
        ("acq→worker p99", lambda c: c["acquisition_to_worker_start_ms"].get("p99")),
        ("acq→worker p99.9", lambda c: c["acquisition_to_worker_start_ms"].get("p999")),
        (
            "same-die mean",
            lambda c: (c["encoder_die_split"] or {}).get("same_die", {}).get("mean"),
        ),
        (
            "other-die mean",
            lambda c: (c["encoder_die_split"] or {}).get("other_die", {}).get("mean"),
        ),
        ("latch frames mean", lambda c: c["ptp_latch"]["latch_frames_acq_to_worker_start_ms"].get("mean")),
        ("stall count", lambda c: float(c["event_record_stalls"]["count"])),
    ]
    cameras = [s for s in result["cameras"] if s in baseline.get("cameras", {})]
    header = f"{'metric':>20}" + "".join(f"{s:>22}" for s in cameras)
    print(header)
    for label, getter in metrics:
        row = f"{label:>20}"
        for serial in cameras:
            a = getter(result["cameras"][serial])
            b = getter(baseline["cameras"][serial])
            if a is None or b is None:
                row += f"{'n/a':>22}"
            else:
                row += f"{b:>9.3f} → {a:<6.3f} {a - b:+.3f}".rjust(22)
        print(row)
    print(
        f"{'sync mode':>20}"
        + "".join(
            f"{baseline['cameras'][s]['sync_mode']} → {result['cameras'][s]['sync_mode']}".rjust(22)
            for s in cameras
        )
    )


def analyze_run(args: argparse.Namespace) -> dict[str, Any]:
    run_dir = os.path.abspath(args.run_dir)
    serials = find_cameras(run_dir)
    if args.cameras:
        wanted = {s.strip() for s in args.cameras.split(",") if s.strip()}
        serials = [s for s in serials if s in wanted]
    if not serials:
        raise SystemExit(f"no Cam*{YOLO_PERF_SUFFIX} files found in {run_dir}")
    lo, hi = (float(v) for v in args.hist_range.split(","))
    hist_edges = np.arange(lo, hi + args.hist_bin_ms / 2, args.hist_bin_ms)
    result: dict[str, Any] = {
        "schema_id": "orange.yolo_latency_phases",
        "schema_version": 1,
        "run_dir": run_dir,
        "steady_after": args.steady_after,
        "cameras": {},
    }
    for serial in serials:
        result["cameras"][serial] = analyze_camera(run_dir, serial, args, hist_edges)
    return result


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    result = analyze_run(args)
    if args.json:
        with open(args.json, "w") as handle:
            json.dump(result, handle, indent=1)
    if not args.quiet:
        print_summary(result)
        if args.baseline_json:
            with open(args.baseline_json) as handle:
                print_comparison(result, json.load(handle))
    return 0


if __name__ == "__main__":
    sys.exit(main())
