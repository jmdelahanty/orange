#!/usr/bin/env python3
"""Run the camera-free TensorRT/NVENC contention comparison.

The runner never discovers or kills processes by name. Every child starts in a
new process group, and failure cleanup signals and reaps only those owned
groups. GPU metadata is sampled before each condition while no owned workload
is running; nvidia-smi is never polled during a measurement.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import sys
import time
from typing import Any, Iterable, Sequence


CONDITIONS = ("baseline", "linear_registered", "native_per_frame")
NATIVE_KERNEL_CONDITION = "native_kernel"
DEFAULT_ORDERS = (
    ("baseline", "linear_registered", "native_per_frame"),
    ("native_per_frame", "linear_registered", "baseline"),
    ("linear_registered", "baseline", "native_per_frame"),
)
NATIVE_KERNEL_ORDERS = (
    ("baseline", "linear_registered", "native_per_frame", "native_kernel"),
    ("native_kernel", "native_per_frame", "linear_registered", "baseline"),
    ("linear_registered", "native_kernel", "baseline", "native_per_frame"),
)
CSV_INFERENCE_METRICS = (
    "start_deadline_lateness_us",
    "start_interval_us",
    "cpu_submit_us",
    "cpu_sync_us",
    "gpu_graph_ms",
    "service_us",
    "completion_vs_next_deadline_us",
)
INFERENCE_METRICS = CSV_INFERENCE_METRICS + ("scheduled_to_completion_us",)
INFERENCE_COLUMNS = {
    "iteration",
    "scheduled_monotonic_ns",
    "submit_start_monotonic_ns",
    "completion_monotonic_ns",
    "deadline_missed",
    *CSV_INFERENCE_METRICS,
}
ENCODER_COLUMNS = {
    "frame_index",
    "phase",
    "source",
    "input",
    "update",
    "source_layout",
    "pacing",
    "timeline_frame_index",
    "burst_index",
    "frame_in_burst",
    "idle_periods_before",
    "scheduled_steady_ns",
    "input_ready_steady_ns",
    "frame_done_steady_ns",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def file_record(path: Path) -> dict[str, Any]:
    stat = path.stat()
    return {
        "path": str(path),
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "sha256": sha256(path),
    }


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
    temporary.replace(path)


def require_file(path: Path, *, executable: bool = False) -> Path:
    resolved = path.expanduser().resolve(strict=True)
    if not resolved.is_file():
        raise ValueError(f"Required path is not a regular file: {resolved}")
    if executable and not os.access(resolved, os.X_OK):
        raise ValueError(f"Required path is not executable: {resolved}")
    return resolved


def parse_phase_offsets(value: str) -> tuple[float, ...]:
    fields = [field.strip() for field in value.split(",")]
    if not fields or any(not field for field in fields):
        raise argparse.ArgumentTypeError(
            "phase offsets must be a comma-separated list of numbers")
    try:
        offsets = tuple(float(field) for field in fields)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "phase offsets must be a comma-separated list of numbers") from error
    if not all(math.isfinite(offset) for offset in offsets):
        raise argparse.ArgumentTypeError("phase offsets must be finite")
    return offsets


def capture(command: Sequence[str], timeout: float = 15.0) -> dict[str, Any]:
    started_wall_ns = time.time_ns()
    started_monotonic_ns = time.monotonic_ns()
    completed = subprocess.run(
        list(command), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=timeout, check=False)
    return {
        "argv": list(command),
        "started_wall_ns": started_wall_ns,
        "started_monotonic_ns": started_monotonic_ns,
        "finished_monotonic_ns": time.monotonic_ns(),
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def gpu_precheck(nvidia_smi: Path, gpu_id: int, allow_busy: bool) -> dict[str, Any]:
    gpu = capture([
        str(nvidia_smi), f"--id={gpu_id}",
        "--query-gpu=index,uuid,name,driver_version,pci.bus_id,compute_mode,pstate,memory.total",
        "--format=csv,noheader,nounits",
    ])
    processes = capture([
        str(nvidia_smi), f"--id={gpu_id}",
        "--query-compute-apps=gpu_uuid,pid,process_name,used_gpu_memory",
        "--format=csv,noheader,nounits",
    ])
    for name, result in (("GPU query", gpu), ("compute-process query", processes)):
        if result["returncode"] != 0:
            raise RuntimeError(
                f"nvidia-smi {name} failed: {result['stderr'].strip()}")
    active_lines = [line.strip() for line in processes["stdout"].splitlines()
                    if line.strip() and "No running" not in line]
    if active_lines and not allow_busy:
        raise RuntimeError(
            "Target GPU already has compute processes; use --allow-busy-gpu only "
            f"when that background load is intentional: {active_lines}")
    return {
        "captured_wall_ns": time.time_ns(),
        "captured_monotonic_ns": time.monotonic_ns(),
        "gpu": gpu,
        "compute_processes": processes,
        "active_compute_lines": active_lines,
        "allow_busy_gpu": allow_busy,
    }


def percentile(values: Sequence[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize(values: Sequence[float]) -> dict[str, Any]:
    finite = [value for value in values if math.isfinite(value)]
    if not finite:
        return {
            "count": 0, "mean": None, "p50": None, "p95": None,
            "p99": None, "min": None, "max": None,
        }
    return {
        "count": len(finite),
        "mean": statistics.fmean(finite),
        "p50": percentile(finite, 0.50),
        "p95": percentile(finite, 0.95),
        "p99": percentile(finite, 0.99),
        "min": min(finite),
        "max": max(finite),
    }


def read_csv(path: Path, required: set[str]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        columns = set(reader.fieldnames or ())
        missing = required - columns
        if missing:
            raise ValueError(f"{path} is missing columns: {sorted(missing)}")
        rows = list(reader)
    if not rows:
        raise ValueError(f"CSV contains no data rows: {path}")
    return rows


def ns(row: dict[str, str], name: str) -> int:
    return int(row[name])


def active_burst_intervals(
    encoder_rows: Sequence[dict[str, str]], shared_start: int, shared_end: int,
) -> list[tuple[int, int]]:
    bursts: dict[int, list[dict[str, str]]] = {}
    for row in encoder_rows:
        if row["phase"] != "measure":
            continue
        bursts.setdefault(int(row["burst_index"]), []).append(row)
    intervals: list[tuple[int, int]] = []
    for rows in bursts.values():
        start = min(ns(row, "input_ready_steady_ns") for row in rows)
        end = max(ns(row, "frame_done_steady_ns") for row in rows)
        start = max(start, shared_start)
        end = min(end, shared_end)
        if end > start:
            intervals.append((start, end))
    intervals.sort()
    return intervals


def overlaps_any(start: int, end: int, intervals: Sequence[tuple[int, int]]) -> bool:
    for interval_start, interval_end in intervals:
        if interval_start >= end:
            return False
        if start < interval_end and end > interval_start:
            return True
    return False


def scope_summary(rows: Sequence[dict[str, str]]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "samples": len(rows),
        "deadline_misses": sum(int(row["deadline_missed"]) for row in rows),
    }
    result["deadline_miss_rate"] = (
        result["deadline_misses"] / len(rows) if rows else None)
    def metric_value(row: dict[str, str], metric: str) -> float:
        if metric == "scheduled_to_completion_us":
            return (
                float(row["start_deadline_lateness_us"])
                + float(row["service_us"]))
        return float(row[metric])
    result["metrics"] = {
        metric: summarize([
            metric_value(row, metric) for row in rows
            if not (
                metric == "start_interval_us"
                and metric_value(row, metric) == 0.0)
        ])
        for metric in INFERENCE_METRICS
    }
    return result


def validate_encoder_contract(
    condition: str, rows: Sequence[dict[str, str]], source_frames: int,
    encoder_frames: int, encoder_warmup_frames: int, on_frames: int,
    idle_frames: int,
) -> dict[str, Any]:
    expected = {
        "linear_registered": {
            "input": "linear", "update": "registered-source",
        },
        "native_per_frame": {
            "input": "native-array", "update": "per-frame",
        },
        "native_kernel": {
            "input": "native-array", "update": "per-frame",
        },
    }[condition]
    for field, value in expected.items():
        actual = {row[field] for row in rows}
        if actual != {value}:
            raise ValueError(f"Unexpected encoder {field}: {sorted(actual)}")
    for field, value in (("source_layout", "packed"), ("pacing", "split-gop")):
        actual = {row[field] for row in rows}
        if actual != {value}:
            raise ValueError(f"Unexpected encoder {field}: {sorted(actual)}")
    if len(rows) != encoder_frames:
        raise ValueError(
            f"Encoder produced {len(rows)} rows, expected {encoder_frames}")
    for expected_index, row in enumerate(rows):
        frame_index = int(row["frame_index"])
        if frame_index != expected_index:
            raise ValueError(
                f"Encoder frame sequence is not contiguous at row {expected_index}: "
                f"frame_index={frame_index}")
        expected_phase = (
            "warmup" if frame_index < encoder_warmup_frames else "measure")
        if row["phase"] != expected_phase:
            raise ValueError(
                f"Unexpected encoder phase at frame {frame_index}: {row['phase']}")
        expected_burst = frame_index // on_frames
        expected_frame_in_burst = frame_index % on_frames
        expected_idle_before = (
            idle_frames if expected_frame_in_burst == 0 and frame_index != 0 else 0)
        expected_timeline = frame_index + expected_burst * idle_frames
        cadence = {
            "burst_index": expected_burst,
            "frame_in_burst": expected_frame_in_burst,
            "idle_periods_before": expected_idle_before,
            "timeline_frame_index": expected_timeline,
        }
        for field, expected_value in cadence.items():
            if int(row[field]) != expected_value:
                raise ValueError(
                    f"Unexpected {field} at frame {frame_index}: "
                    f"{row[field]} (expected {expected_value})")
    sources = {int(row["source"]) for row in rows}
    if not sources or min(sources) < 0 or max(sources) >= source_frames:
        raise ValueError(f"Encoder source indices are outside raw cache: {sorted(sources)}")
    result = {
        "rows": len(rows),
        "measured_rows": sum(row["phase"] == "measure" for row in rows),
        "bursts": len({int(row["burst_index"]) for row in rows}),
        "source_indices_seen": sorted(sources),
        "input": expected["input"],
        "update": expected["update"],
        "source_layout": "packed",
        "pacing": "split-gop",
        "on_frames": on_frames,
        "idle_frames": idle_frames,
    }
    if condition in ("native_per_frame", NATIVE_KERNEL_CONDITION):
        if "native_update" not in rows[0]:
            raise ValueError("Native encoder CSV is missing native_update")
        expected_native_update = (
            "kernel" if condition == NATIVE_KERNEL_CONDITION else "copy")
        native_updates = {row["native_update"] for row in rows}
        if native_updates != {expected_native_update}:
            raise ValueError(
                f"Unexpected native encoder native_update: {sorted(native_updates)}")
        if condition == NATIVE_KERNEL_CONDITION:
            result["native_update"] = expected_native_update
    return result


def analyze_run(
    condition: str, inference_csv: Path, encoder_csv: Path | None,
    source_frames: int, encoder_frames: int, encoder_warmup_frames: int,
    on_frames: int, idle_frames: int, min_overlap: int, min_active: int,
    min_idle: int,
) -> tuple[dict[str, Any], dict[str, list[dict[str, str]]]]:
    inference_rows = read_csv(inference_csv, INFERENCE_COLUMNS)
    infer_start = min(ns(row, "submit_start_monotonic_ns") for row in inference_rows)
    infer_end = max(ns(row, "completion_monotonic_ns") for row in inference_rows)
    encoder_contract = None
    intervals: list[tuple[int, int]] = []
    if encoder_csv is None:
        shared_start, shared_end = infer_start, infer_end
        encoder_rows: list[dict[str, str]] = []
    else:
        encoder_rows = read_csv(encoder_csv, ENCODER_COLUMNS)
        encoder_contract = validate_encoder_contract(
            condition, encoder_rows, source_frames, encoder_frames,
            encoder_warmup_frames, on_frames, idle_frames)
        encoder_start = min(ns(row, "input_ready_steady_ns") for row in encoder_rows)
        encoder_end = max(ns(row, "frame_done_steady_ns") for row in encoder_rows)
        shared_start = max(infer_start, encoder_start)
        shared_end = min(infer_end, encoder_end)
        if shared_end <= shared_start:
            raise ValueError("Inference and encoder have no actual timestamp overlap")
        intervals = active_burst_intervals(encoder_rows, shared_start, shared_end)

    overlap = [
        row for row in inference_rows
        if ns(row, "submit_start_monotonic_ns") >= shared_start
        and ns(row, "completion_monotonic_ns") <= shared_end
    ]
    if len(overlap) < min_overlap:
        raise ValueError(
            f"Only {len(overlap)} inference rows are fully inside actual overlap; "
            f"minimum is {min_overlap}")

    active = [
        row for row in overlap
        if overlaps_any(
            ns(row, "submit_start_monotonic_ns"),
            ns(row, "completion_monotonic_ns"), intervals)
    ]
    idle = [row for row in overlap if row not in active]
    if condition != "baseline":
        if len(active) < min_active:
            raise ValueError(
                f"Only {len(active)} overlapping inference rows occur in active bursts; "
                f"minimum is {min_active}")
        if len(idle) < min_idle:
            raise ValueError(
                f"Only {len(idle)} overlapping inference rows occur between bursts; "
                f"minimum is {min_idle}")

    scopes = {"all_overlap": overlap}
    if condition != "baseline":
        scopes["active_burst"] = active
        scopes["idle_between_bursts"] = idle
    public = {
        "condition": condition,
        "timestamp_clock": "CLOCK_MONOTONIC / Linux steady_clock",
        "shared_interval": {
            "start_ns": shared_start,
            "end_ns": shared_end,
            "duration_seconds": (shared_end - shared_start) / 1e9,
            "row_rule": "submit_start >= start and completion <= end",
        },
        "active_burst_definition": (
            "host-side proxy per measured encoder burst: first actual "
            "input_ready_steady_ns through last actual frame_done_steady_ns; "
            "inference graph interval overlap; this is not exact GPU kernel overlap"
            if condition != "baseline" else None),
        "active_burst_intervals": [
            {"start_ns": start, "end_ns": end} for start, end in intervals
        ],
        "encoder_contract": encoder_contract,
        "scopes": {name: scope_summary(rows) for name, rows in scopes.items()},
    }
    return public, scopes


def terminate_owned(process: subprocess.Popen[bytes], grace: float = 5.0) -> int:
    if process.poll() is not None:
        return process.returncode
    for sig, wait_seconds in ((signal.SIGINT, grace), (signal.SIGTERM, 2.0)):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        try:
            return process.wait(timeout=wait_seconds)
        except subprocess.TimeoutExpired:
            pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    return process.wait()


def launch(command: Sequence[str], log_path: Path) -> tuple[subprocess.Popen[bytes], Any]:
    log = log_path.open("wb")
    try:
        process = subprocess.Popen(
            list(command), stdout=log, stderr=subprocess.STDOUT,
            start_new_session=True, close_fds=True)
    except Exception:
        log.close()
        raise
    return process, log


def wait_for_pair(
    inference: subprocess.Popen[bytes], encoder: subprocess.Popen[bytes] | None,
    inference_timeout: float, encoder_timeout: float,
) -> tuple[int, int | None]:
    inference_deadline = time.monotonic() + inference_timeout
    while inference.poll() is None:
        if encoder is not None and encoder.poll() is not None:
            terminate_owned(inference)
            raise RuntimeError(
                f"Encoder exited before inference completed (rc={encoder.returncode})")
        if time.monotonic() >= inference_deadline:
            terminate_owned(inference)
            if encoder is not None:
                terminate_owned(encoder)
            raise TimeoutError("Inference child exceeded timeout")
        time.sleep(0.1)
    inference_rc = inference.returncode
    if inference_rc != 0:
        if encoder is not None:
            terminate_owned(encoder)
        return inference_rc, encoder.returncode if encoder is not None else None

    if encoder is None:
        return inference_rc, None
    encoder_deadline = time.monotonic() + encoder_timeout
    while encoder.poll() is None and time.monotonic() < encoder_deadline:
        time.sleep(0.1)
    if encoder.poll() is None:
        terminate_owned(encoder)
        raise TimeoutError("Encoder child exceeded post-inference timeout")
    return inference_rc, encoder.returncode


def orders(
    repetitions: int, include_native_kernel: bool = False,
) -> list[tuple[str, ...]]:
    available_orders = (
        NATIVE_KERNEL_ORDERS if include_native_kernel else DEFAULT_ORDERS)
    return [available_orders[index % len(available_orders)]
            for index in range(repetitions)]


def encoder_command(
    args: argparse.Namespace, condition: str, run_dir: Path,
    start_at_monotonic_ns: int | None = None,
) -> list[str]:
    if condition == "linear_registered":
        input_kind, update = "linear", "registered-source"
    elif condition in ("native_per_frame", NATIVE_KERNEL_CONDITION):
        input_kind, update = "native-array", "per-frame"
    else:
        raise ValueError(f"No encoder command for condition {condition}")
    command = [
        str(args.nvenc_probe),
        "--gpu-id", str(args.gpu_id),
        "--input", input_kind,
        "--update", update,
        "--width", str(args.width),
        "--height", str(args.height),
        "--native-storage-width", str(args.native_storage_width),
        "--native-register-pitch", str(args.native_register_pitch),
        "--fps", str(args.fps),
        "--pacing", "split-gop",
        "--split-gop-idle-frames", str(args.idle_frames),
        "--frames", str(args.encoder_frames),
        "--warmup-frames", str(args.encoder_warmup_frames),
        "--source-frames", str(args.source_frames),
        "--source-layout", "packed",
        "--extra-output-delay", str(args.encoder_output_delay),
        "--raw-file", str(args.raw_file),
        "--raw-pitch", str(args.raw_pitch),
        "--raw-frame-bytes", str(args.raw_frame_bytes),
        "--bitstream-out", str(run_dir / "encoder.hevc"),
        "--csv", str(run_dir / "encoder.csv"),
    ]
    if start_at_monotonic_ns is not None:
        command.extend([
            "--start-at-monotonic-ns", str(start_at_monotonic_ns),
        ])
    if condition == "native_per_frame":
        command.extend(["--native-update", "copy"])
    elif condition == NATIVE_KERNEL_CONDITION:
        command.extend([
            "--native-update", "kernel",
            "--native-kernel-ptx", str(args.native_kernel_ptx),
        ])
    return command


def inference_command(
    args: argparse.Namespace, condition: str, run_dir: Path,
    start_at_monotonic_ns: int | None = None,
) -> list[str]:
    command = [
        str(args.trt_probe),
        "--engine", str(args.engine),
        "--gpu-id", str(args.gpu_id),
        "--fps", str(args.fps),
        "--warmup", str(args.inference_warmup),
        "--iterations", str(args.inference_iterations),
        "--label", condition,
        "--csv", str(run_dir / "inference.csv"),
        "--summary-json", str(run_dir / "inference_summary.json"),
    ]
    if start_at_monotonic_ns is not None:
        command.extend([
            "--start-at-monotonic-ns", str(start_at_monotonic_ns),
        ])
    return command


def artifact_records(run_dir: Path) -> dict[str, Any]:
    names = (
        "commands.json", "status.json", "analysis.json", "inference.csv",
        "inference_summary.json", "inference.log", "encoder.csv",
        "encoder.hevc", "encoder.log", "gpu_precheck.json",
    )
    return {
        name: file_record(run_dir / name)
        for name in names if (run_dir / name).is_file()
    }


def flatten_summary_rows(
    completed: Sequence[dict[str, Any]],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    aggregates: dict[tuple[str, str], dict[str, Any]] = {}
    for run in completed:
        for scope, summary in run["analysis"]["scopes"].items():
            row: dict[str, Any] = {
                "row_type": "run",
                "repetition": run["repetition"],
                "sequence_index": run["sequence_index"],
                "condition": run["condition"],
                "scope": scope,
                "samples": summary["samples"],
                "deadline_misses": summary["deadline_misses"],
                "deadline_miss_rate": summary["deadline_miss_rate"],
            }
            for metric, values in summary["metrics"].items():
                for statistic, value in values.items():
                    row[f"{metric}_{statistic}"] = value
            rows.append(row)

            key = (run["condition"], scope)
            bucket = aggregates.setdefault(key, {
                "runs": 0, "samples": 0, "deadline_misses": 0,
                "run_metric_means": {metric: [] for metric in INFERENCE_METRICS},
            })
            bucket["runs"] += 1
            bucket["samples"] += summary["samples"]
            bucket["deadline_misses"] += summary["deadline_misses"]
            for metric in INFERENCE_METRICS:
                value = summary["metrics"][metric]["mean"]
                if value is not None:
                    bucket["run_metric_means"][metric].append(value)

    public_aggregates: dict[str, Any] = {}
    for (condition, scope), bucket in aggregates.items():
        name = f"{condition}/{scope}"
        aggregate = {
            "runs": bucket["runs"],
            "samples": bucket["samples"],
            "deadline_misses": bucket["deadline_misses"],
            "deadline_miss_rate": (
                bucket["deadline_misses"] / bucket["samples"]
                if bucket["samples"] else None),
            "run_mean_distribution": {
                metric: summarize(values)
                for metric, values in bucket["run_metric_means"].items()
            },
        }
        public_aggregates[name] = aggregate
        row = {
            "row_type": "aggregate_run_means",
            "repetition": "",
            "sequence_index": "",
            "condition": condition,
            "scope": scope,
            "samples": aggregate["samples"],
            "deadline_misses": aggregate["deadline_misses"],
            "deadline_miss_rate": aggregate["deadline_miss_rate"],
        }
        for metric, values in aggregate["run_mean_distribution"].items():
            for statistic, value in values.items():
                row[f"{metric}_{statistic}"] = value
        rows.append(row)
    return rows, public_aggregates


def write_comparison_summary(root: Path, completed: Sequence[dict[str, Any]]) -> None:
    rows, aggregates = flatten_summary_rows(completed)
    write_json(root / "comparison_summary.json", {
        "percentile_method": "linear interpolation over each retained scope",
        "aggregate_method": "distribution of per-run metric means",
        "runs": list(completed),
        "aggregates": aggregates,
    })
    if not rows:
        return
    columns: list[str] = []
    for row in rows:
        for key in row:
            if key not in columns:
                columns.append(key)
    with (root / "comparison_summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trt-probe", type=Path, required=True)
    parser.add_argument("--nvenc-probe", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--raw-file", type=Path, required=True)
    parser.add_argument("--nvidia-smi", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--gpu-id", type=int, default=1)
    parser.add_argument("--width", type=int, default=4512)
    parser.add_argument("--height", type=int, default=4512)
    parser.add_argument("--native-storage-width", type=int, default=4608)
    parser.add_argument("--native-register-pitch", type=int, default=4608)
    parser.add_argument("--include-native-kernel", action="store_true")
    parser.add_argument("--native-kernel-ptx", type=Path)
    parser.add_argument("--raw-pitch", type=int, required=True)
    parser.add_argument("--raw-frame-bytes", type=int, required=True)
    parser.add_argument("--source-frames", type=int, default=32)
    parser.add_argument("--fps", type=int, default=100)
    parser.add_argument("--inference-warmup", type=int, default=100)
    parser.add_argument("--inference-iterations", type=int, default=2000)
    parser.add_argument("--encoder-frames", type=int, default=1300)
    parser.add_argument("--encoder-warmup-frames", type=int, default=50)
    parser.add_argument("--encoder-output-delay", type=int, default=3)
    parser.add_argument("--idle-frames", type=int, default=25)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument(
        "--phase-offsets-ms", type=parse_phase_offsets,
        help="Comma-separated encoder-to-inference phase offset per repetition")
    parser.add_argument(
        "--coordinated-start-delay-seconds", type=float, default=6.0)
    parser.add_argument("--encoder-lead-seconds", type=float, default=2.0)
    parser.add_argument("--settle-seconds", type=float, default=2.0)
    parser.add_argument("--inference-timeout-seconds", type=float, default=45.0)
    parser.add_argument("--encoder-timeout-seconds", type=float, default=15.0)
    parser.add_argument("--min-overlap-samples", type=int, default=1800)
    parser.add_argument("--min-active-samples", type=int, default=800)
    parser.add_argument("--min-idle-samples", type=int, default=400)
    parser.add_argument("--allow-busy-gpu", action="store_true")
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    args.trt_probe = require_file(args.trt_probe, executable=True)
    args.nvenc_probe = require_file(args.nvenc_probe, executable=True)
    args.engine = require_file(args.engine)
    args.raw_file = require_file(args.raw_file)
    args.nvidia_smi = require_file(args.nvidia_smi, executable=True)
    if args.include_native_kernel:
        if args.native_kernel_ptx is None:
            raise ValueError(
                "--native-kernel-ptx is required with --include-native-kernel")
        args.native_kernel_ptx = require_file(args.native_kernel_ptx)
    elif args.native_kernel_ptx is not None:
        raise ValueError(
            "--native-kernel-ptx requires --include-native-kernel")
    args.output_dir = args.output_dir.expanduser().absolute()
    if args.output_dir.exists():
        raise ValueError(f"Output directory must be new: {args.output_dir}")
    integer_positive = (
        "width", "height", "raw_pitch", "raw_frame_bytes", "source_frames",
        "fps", "inference_iterations", "encoder_frames", "idle_frames",
        "repetitions", "min_overlap_samples", "min_active_samples",
        "min_idle_samples",
    )
    for name in integer_positive:
        if getattr(args, name) <= 0:
            raise ValueError(f"--{name.replace('_', '-')} must be positive")
    if args.gpu_id < 0 or args.inference_warmup < 0 or args.encoder_warmup_frames < 0:
        raise ValueError("GPU id and warmup counts must be nonnegative")
    if args.native_storage_width < args.width:
        raise ValueError("--native-storage-width must be at least --width")
    if args.native_register_pitch < args.width:
        raise ValueError("--native-register-pitch must be at least --width")
    if args.encoder_warmup_frames >= args.encoder_frames:
        raise ValueError("--encoder-warmup-frames must be less than --encoder-frames")
    for name in (
        "encoder_lead_seconds", "settle_seconds", "inference_timeout_seconds",
        "encoder_timeout_seconds", "coordinated_start_delay_seconds",
    ):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) < 0:
            raise ValueError(f"--{name.replace('_', '-')} must be nonnegative")
    if args.phase_offsets_ms is not None:
        if len(args.phase_offsets_ms) != args.repetitions:
            raise ValueError(
                "--phase-offsets-ms must provide exactly one value per repetition")
        frame_period_ms = 1000.0 / args.fps
        if any(offset < 0.0 or offset >= frame_period_ms
               for offset in args.phase_offsets_ms):
            raise ValueError(
                "Every phase offset must be nonnegative and less than one "
                f"inference period ({frame_period_ms} ms)")
        encoder_warmup_timeline_periods = (
            args.encoder_warmup_frames
            + (args.encoder_warmup_frames // 25) * args.idle_frames)
        encoder_period_ns = 1_000_000_000 // args.fps
        encoder_warmup_timeline_seconds = (
            encoder_warmup_timeline_periods * encoder_period_ns / 1e9)
        inference_period_ns = round(1_000_000_000 / args.fps)
        inference_warmup_seconds = (
            args.inference_warmup * inference_period_ns / 1e9)
        if args.coordinated_start_delay_seconds < 1.0:
            raise ValueError(
                "--coordinated-start-delay-seconds must be at least 1.0 "
                "when phase coordination is enabled")
        minimum_initialization_headroom = (
            args.coordinated_start_delay_seconds
            + encoder_warmup_timeline_seconds
            + min(args.phase_offsets_ms) / 1000.0
            - args.encoder_lead_seconds
            - inference_warmup_seconds)
        if minimum_initialization_headroom < 1.0:
            raise ValueError(
                "Coordinated start leaves less than one second for TensorRT "
                "initialization before its paced warmup and measurement deadline")
    if args.raw_pitch < args.width:
        raise ValueError("--raw-pitch must be at least --width")
    minimum_record = args.raw_pitch * args.height
    if args.raw_frame_bytes < minimum_record:
        raise ValueError("--raw-frame-bytes is smaller than raw-pitch*height")
    required_raw_size = args.source_frames * args.raw_frame_bytes
    if args.raw_file.stat().st_size < required_raw_size:
        raise ValueError(
            f"Raw cache has {args.raw_file.stat().st_size} bytes; "
            f"{required_raw_size} required for {args.source_frames} frames")
    if os.environ.get("CUDA_VISIBLE_DEVICES"):
        raise ValueError(
            "Unset CUDA_VISIBLE_DEVICES so --gpu-id has the same ordinal in "
            "nvidia-smi, TensorRT, and NVENC")


def main() -> int:
    args = parse_args()
    validate_args(args)
    args.output_dir.mkdir(parents=True, exist_ok=False)

    run_orders = orders(args.repetitions, args.include_native_kernel)
    run_conditions = (
        CONDITIONS + (NATIVE_KERNEL_CONDITION,)
        if args.include_native_kernel else CONDITIONS)
    manifest: dict[str, Any] = {
        "schema": 1,
        "status": "running",
        "started_wall_ns": time.time_ns(),
        "started_monotonic_ns": time.monotonic_ns(),
        "host": {
            "hostname": platform.node(),
            "platform": platform.platform(),
            "python": sys.version,
            "cuda_visible_devices": os.environ.get("CUDA_VISIBLE_DEVICES"),
        },
        "design": {
            "conditions": list(run_conditions),
            "orders": [list(order) for order in run_orders],
            "pacing": {
                "inference_fps": args.fps,
                "encoder_fps": args.fps,
                "encoder_on_frames": 25,
                "encoder_idle_frames": args.idle_frames,
                "nominal_encoder_average_fps": (
                    args.fps * 25 / (25 + args.idle_frames)),
            },
            "inference": {
                "warmup_iterations": args.inference_warmup,
                "measured_iterations": args.inference_iterations,
                "zero_cached_input": True,
                "output_d2h_in_graph": True,
            },
            "encoder": {
                "frames": args.encoder_frames,
                "warmup_frames": args.encoder_warmup_frames,
                "source_frames": args.source_frames,
                "source_layout": "packed",
                "extra_output_delay": args.encoder_output_delay,
                "one_process_one_shard": True,
            },
            "encoder_lead_seconds": args.encoder_lead_seconds,
            "settle_seconds": args.settle_seconds,
        },
        "inputs": {
            "runner": file_record(Path(__file__).resolve()),
            "trt_probe": file_record(args.trt_probe),
            "nvenc_probe": file_record(args.nvenc_probe),
            "engine": file_record(args.engine),
            "raw_file": file_record(args.raw_file),
            "nvidia_smi": file_record(args.nvidia_smi),
            "raw_pitch": args.raw_pitch,
            "raw_frame_bytes": args.raw_frame_bytes,
        },
        "runs": [],
    }
    if args.phase_offsets_ms is not None:
        encoder_warmup_timeline_periods = (
            args.encoder_warmup_frames
            + (args.encoder_warmup_frames // 25) * args.idle_frames)
        encoder_period_ns = 1_000_000_000 // args.fps
        encoder_warmup_timeline_ns = (
            encoder_warmup_timeline_periods * encoder_period_ns)
        manifest["design"]["coordinated_start"] = {
            "enabled": True,
            "clock": "CLOCK_MONOTONIC / Linux steady_clock",
            "phase_offsets_ms_by_repetition": list(args.phase_offsets_ms),
            "start_delay_seconds": args.coordinated_start_delay_seconds,
            "encoder_period_ns": encoder_period_ns,
            "encoder_warmup_timeline_periods": encoder_warmup_timeline_periods,
            "encoder_warmup_timeline_ns": encoder_warmup_timeline_ns,
            "encoder_warmup_timeline_seconds": encoder_warmup_timeline_ns / 1e9,
            "encoder_start_rule": "T",
            "inference_measurement_start_rule": (
                "T + encoder_warmup_timeline + repetition_phase_offset"),
        }
    if args.native_kernel_ptx is not None:
        native_kernel_ptx = file_record(args.native_kernel_ptx)
        native_kernel_ptx["provenance"] = "explicit --native-kernel-ptx input"
        manifest["inputs"]["native_kernel_ptx"] = native_kernel_ptx
    write_json(args.output_dir / "runner_manifest.json", manifest)
    topology = capture([str(args.nvidia_smi), "topo", "-m"])
    write_json(args.output_dir / "gpu_topology.json", topology)
    if topology["returncode"] != 0:
        raise RuntimeError("nvidia-smi topology query failed")

    completed: list[dict[str, Any]] = []
    try:
        for repetition, order in enumerate(run_orders, start=1):
            for sequence_index, condition in enumerate(order, start=1):
                if completed and args.settle_seconds > 0:
                    time.sleep(args.settle_seconds)
                run_name = f"rep{repetition:02d}_{sequence_index:02d}_{condition}"
                run_dir = args.output_dir / run_name
                run_dir.mkdir()

                precheck = gpu_precheck(
                    args.nvidia_smi, args.gpu_id, args.allow_busy_gpu)
                write_json(run_dir / "gpu_precheck.json", precheck)
                coordination = None
                if args.phase_offsets_ms is not None:
                    computed_monotonic_ns = time.monotonic_ns()
                    encoder_start_monotonic_ns = (
                        computed_monotonic_ns
                        + round(args.coordinated_start_delay_seconds * 1e9))
                    encoder_period_ns = 1_000_000_000 // args.fps
                    encoder_warmup_timeline_periods = (
                        args.encoder_warmup_frames
                        + (args.encoder_warmup_frames // 25) * args.idle_frames)
                    encoder_warmup_timeline_ns = (
                        encoder_warmup_timeline_periods * encoder_period_ns)
                    phase_offset_ms = args.phase_offsets_ms[repetition - 1]
                    phase_offset_ns = round(phase_offset_ms * 1e6)
                    inference_start_monotonic_ns = (
                        encoder_start_monotonic_ns
                        + encoder_warmup_timeline_ns
                        + phase_offset_ns)
                    inference_period_ns = round(1_000_000_000 / args.fps)
                    inference_warmup_start_monotonic_ns = (
                        inference_start_monotonic_ns
                        - args.inference_warmup * inference_period_ns)
                    coordination = {
                        "clock": "CLOCK_MONOTONIC / Linux steady_clock",
                        "computed_monotonic_ns": computed_monotonic_ns,
                        "encoder_start_monotonic_ns": encoder_start_monotonic_ns,
                        "encoder_period_ns": encoder_period_ns,
                        "encoder_warmup_timeline_periods": (
                            encoder_warmup_timeline_periods),
                        "encoder_warmup_timeline_ns": (
                            encoder_warmup_timeline_ns),
                        "phase_offset_ms": phase_offset_ms,
                        "phase_offset_ns": phase_offset_ns,
                        "inference_period_ns": inference_period_ns,
                        "inference_warmup_start_monotonic_ns": (
                            inference_warmup_start_monotonic_ns),
                        "inference_measurement_start_monotonic_ns": (
                            inference_start_monotonic_ns),
                    }
                infer_command = inference_command(
                    args, condition, run_dir,
                    coordination["inference_measurement_start_monotonic_ns"]
                    if coordination is not None else None)
                encode_command = (
                    encoder_command(
                        args, condition, run_dir,
                        coordination["encoder_start_monotonic_ns"]
                        if coordination is not None else None)
                    if condition != "baseline" else None)
                commands = {
                    "condition": condition,
                    "repetition": repetition,
                    "sequence_index": sequence_index,
                    "inference": infer_command,
                    "encoder": encode_command,
                    "shell_used": False,
                }
                if coordination is not None:
                    commands["coordinated_start"] = coordination
                write_json(run_dir / "commands.json", commands)

                run_status: dict[str, Any] = {
                    "status": "running",
                    "started_wall_ns": time.time_ns(),
                    "started_monotonic_ns": time.monotonic_ns(),
                    "condition": condition,
                    "repetition": repetition,
                    "sequence_index": sequence_index,
                }
                if coordination is not None:
                    run_status["coordinated_start"] = coordination
                write_json(run_dir / "status.json", run_status)

                encoder = None
                inference = None
                encoder_log = None
                inference_log = None
                try:
                    if encode_command is not None:
                        encoder, encoder_log = launch(
                            encode_command, run_dir / "encoder.log")
                        run_status["encoder_pid"] = encoder.pid
                        run_status["encoder_started_monotonic_ns"] = time.monotonic_ns()
                        lead_deadline = time.monotonic() + args.encoder_lead_seconds
                        while time.monotonic() < lead_deadline:
                            if encoder.poll() is not None:
                                raise RuntimeError(
                                    f"Encoder exited during lead interval (rc={encoder.returncode})")
                            time.sleep(min(0.1, max(0.0, lead_deadline - time.monotonic())))

                    inference, inference_log = launch(
                        infer_command, run_dir / "inference.log")
                    run_status["inference_pid"] = inference.pid
                    run_status["inference_started_monotonic_ns"] = time.monotonic_ns()
                    write_json(run_dir / "status.json", run_status)
                    inference_rc, encoder_rc = wait_for_pair(
                        inference, encoder, args.inference_timeout_seconds,
                        args.encoder_timeout_seconds)
                    run_status["inference_returncode"] = inference_rc
                    run_status["encoder_returncode"] = encoder_rc
                    if inference_rc != 0 or (encoder_rc is not None and encoder_rc != 0):
                        raise RuntimeError(
                            f"Child failed: inference={inference_rc} encoder={encoder_rc}")
                except BaseException:
                    if inference is not None and inference.poll() is None:
                        terminate_owned(inference)
                    if encoder is not None and encoder.poll() is None:
                        terminate_owned(encoder)
                    if inference is not None:
                        run_status["inference_returncode"] = inference.returncode
                    if encoder is not None:
                        run_status["encoder_returncode"] = encoder.returncode
                    error = sys.exc_info()[1]
                    run_status.update({
                        "status": "failed",
                        "error": f"{type(error).__name__}: {error}",
                        "finished_wall_ns": time.time_ns(),
                        "finished_monotonic_ns": time.monotonic_ns(),
                    })
                    write_json(run_dir / "status.json", run_status)
                    raise
                finally:
                    if inference_log is not None:
                        inference_log.close()
                    if encoder_log is not None:
                        encoder_log.close()

                try:
                    analysis, _scopes = analyze_run(
                        condition,
                        run_dir / "inference.csv",
                        run_dir / "encoder.csv" if condition != "baseline" else None,
                        args.source_frames,
                        args.encoder_frames,
                        args.encoder_warmup_frames,
                        25,
                        args.idle_frames,
                        args.min_overlap_samples,
                        args.min_active_samples,
                        args.min_idle_samples,
                    )
                except BaseException as error:
                    run_status.update({
                        "status": "failed",
                        "error": f"{type(error).__name__}: {error}",
                        "finished_wall_ns": time.time_ns(),
                        "finished_monotonic_ns": time.monotonic_ns(),
                    })
                    write_json(run_dir / "status.json", run_status)
                    raise
                if coordination is not None:
                    analysis["coordinated_start"] = coordination
                write_json(run_dir / "analysis.json", analysis)
                run_status.update({
                    "status": "completed",
                    "finished_wall_ns": time.time_ns(),
                    "finished_monotonic_ns": time.monotonic_ns(),
                })
                write_json(run_dir / "status.json", run_status)
                artifacts = artifact_records(run_dir)
                run_record = {
                    "name": run_name,
                    "condition": condition,
                    "repetition": repetition,
                    "sequence_index": sequence_index,
                    "commands": commands,
                    "returncodes": {
                        "inference": run_status["inference_returncode"],
                        "encoder": run_status.get("encoder_returncode"),
                    },
                    "analysis": analysis,
                    "artifacts": artifacts,
                }
                completed.append(run_record)
                manifest_run = {
                    "name": run_name,
                    "condition": condition,
                    "repetition": repetition,
                    "sequence_index": sequence_index,
                    "status": "completed",
                    "status_path": str(run_dir / "status.json"),
                    "analysis_path": str(run_dir / "analysis.json"),
                }
                if coordination is not None:
                    manifest_run["coordinated_start"] = coordination
                manifest["runs"].append(manifest_run)
                write_json(args.output_dir / "runner_manifest.json", manifest)
                write_comparison_summary(args.output_dir, completed)
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = f"{type(error).__name__}: {error}"
        manifest["finished_wall_ns"] = time.time_ns()
        manifest["finished_monotonic_ns"] = time.monotonic_ns()
        write_json(args.output_dir / "runner_manifest.json", manifest)
        raise

    manifest["status"] = "completed"
    manifest["finished_wall_ns"] = time.time_ns()
    manifest["finished_monotonic_ns"] = time.monotonic_ns()
    manifest["comparison_summary_json"] = str(
        args.output_dir / "comparison_summary.json")
    manifest["comparison_summary_csv"] = str(
        args.output_dir / "comparison_summary.csv")
    write_json(args.output_dir / "runner_manifest.json", manifest)
    write_comparison_summary(args.output_dir, completed)
    print(args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
