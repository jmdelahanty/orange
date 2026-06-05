#!/usr/bin/env python3
"""Run and summarize manifest-driven external IPC encoding experiments."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import shlex
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = (
    REPO_ROOT / "experiment_specs" / "encoding_master_singlecam_60fps_stage0_manifest.json"
)
DEFAULT_BENCHMARK = "/usr/local/bin/orange-local-benchmark"
DEFAULT_ORANGE_CLIENT = str(REPO_ROOT / "targets" / "release" / "orange_client")


def utc_stamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d_%H%M%S")


def utc_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise ValueError(f"expected object JSON in {path}")
    return payload


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")


def sanitize(text: Any) -> str:
    raw = str(text)
    out = []
    for char in raw:
        if char.isalnum() or char in ("-", "_", "."):
            out.append(char)
        else:
            out.append("_")
    value = "".join(out).strip("._")
    return value or "run"


def path_from_repo(value: str | Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else REPO_ROOT / path


def as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    return value if isinstance(value, list) else [value]


def first_item(value: Any, default: Any = "") -> Any:
    values = as_list(value)
    return values[0] if values else default


def optional_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def optional_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (len(ordered) - 1) * pct
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    if lower == upper:
        return float(ordered[lower])
    fraction = rank - lower
    return float(ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction)


def parse_dmon_number(text: str) -> float | None:
    stripped = str(text).strip()
    if not stripped or stripped == "-":
        return None
    try:
        return float(stripped)
    except ValueError:
        return None


def summarize_dmon(path: Path) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "dmon_present": False,
        "dmon_samples": 0,
        "dmon_gpus": "",
        "dmon_enc_mean_all": 0.0,
        "dmon_enc_p95_all": 0.0,
        "dmon_enc_max_all": 0.0,
        "dmon_sm_mean_all": 0.0,
        "dmon_rxpci_max_all": 0.0,
        "dmon_txpci_max_all": 0.0,
        "dmon_enc_max_by_gpu": "",
    }
    if not path.exists():
        return summary

    header: list[str] | None = None
    rows: list[dict[str, str]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        parts = stripped.split()
        if stripped.startswith("#Date"):
            header = [token.lstrip("#").lower() for token in parts]
            continue
        if stripped.startswith("#"):
            continue
        if header is None or len(parts) != len(header):
            continue
        rows.append({header[index]: parts[index] for index in range(len(header))})

    if not rows:
        return summary

    summary["dmon_present"] = True
    summary["dmon_samples"] = len(rows)
    gpus = sorted({row.get("gpu", "") for row in rows if row.get("gpu", "")})
    summary["dmon_gpus"] = ",".join(gpus)

    def values(metric: str) -> list[float]:
        out = []
        for row in rows:
            value = parse_dmon_number(row.get(metric, ""))
            if value is not None:
                out.append(value)
        return out

    enc_values = values("enc")
    sm_values = values("sm")
    rx_values = values("rxpci")
    tx_values = values("txpci")
    if enc_values:
        summary["dmon_enc_mean_all"] = sum(enc_values) / len(enc_values)
        summary["dmon_enc_p95_all"] = percentile(enc_values, 0.95)
        summary["dmon_enc_max_all"] = max(enc_values)
    if sm_values:
        summary["dmon_sm_mean_all"] = sum(sm_values) / len(sm_values)
    if rx_values:
        summary["dmon_rxpci_max_all"] = max(rx_values)
    if tx_values:
        summary["dmon_txpci_max_all"] = max(tx_values)

    enc_by_gpu: dict[str, list[float]] = {}
    for row in rows:
        gpu = row.get("gpu", "")
        value = parse_dmon_number(row.get("enc", ""))
        if gpu and value is not None:
            enc_by_gpu.setdefault(gpu, []).append(value)
    summary["dmon_enc_max_by_gpu"] = ";".join(
        f"{gpu}:{max(values):.1f}" for gpu, values in sorted(enc_by_gpu.items())
    )
    return summary


def find_single_run_folder(analytics_root: Path) -> Path | None:
    candidates = sorted(analytics_root.glob("run_*"))
    if len(candidates) == 1 and candidates[0].is_dir():
        return candidates[0]
    return None


def ffprobe_duration(path: Path) -> float:
    if not path.exists():
        return 0.0
    command = [
        "ffprobe",
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        str(path),
    ]
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return 0.0
    if result.returncode != 0:
        return 0.0
    return optional_float(result.stdout.strip(), 0.0)


@dataclass
class ResolvedRun:
    run_id: str
    stage: str
    description: str
    tags: list[str]
    spec_path: Path
    artifact_root: Path
    analytics_root: Path
    command: list[str]
    verify_command: list[str] | None
    spec: dict[str, Any]
    manifest_run: dict[str, Any]
    status: str = "planned"
    returncode: int | None = None
    verifier_returncode: int | None = None
    started_at_utc: str = ""
    ended_at_utc: str = ""
    error: str = ""
    rows: list[dict[str, Any]] = field(default_factory=list)
    shard_rows: list[dict[str, Any]] = field(default_factory=list)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a manifest-driven Orange external IPC encoding matrix and "
            "aggregate recorder/dmon/storage metrics."
        )
    )
    parser.add_argument(
        "--manifest",
        default=str(DEFAULT_MANIFEST),
        help=f"Manifest JSON. Default: {DEFAULT_MANIFEST}",
    )
    parser.add_argument(
        "--output-root",
        default="",
        help="Matrix output root. Default: manifest defaults.artifact_root.",
    )
    parser.add_argument(
        "--stamp",
        default="",
        help="Run stamp. Default: current UTC timestamp.",
    )
    parser.add_argument(
        "--execute",
        action="store_true",
        help="Actually run the benchmark. Default is dry-run/planning only.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Force planning only, even if --execute is present.",
    )
    parser.add_argument(
        "--run-id",
        action="append",
        default=[],
        help="Run only this manifest run_id. May be repeated.",
    )
    parser.add_argument(
        "--stage",
        action="append",
        default=[],
        help="Run only this stage. May be repeated.",
    )
    parser.add_argument(
        "--duration-s",
        type=int,
        default=None,
        help="Override fixed.duration_s for every selected run.",
    )
    parser.add_argument(
        "--warmup-s",
        type=int,
        default=None,
        help="Override fixed.warmup_s for every selected run.",
    )
    parser.add_argument(
        "--benchmark",
        default="",
        help=f"Benchmark wrapper. Default: manifest default or {DEFAULT_BENCHMARK}.",
    )
    parser.add_argument(
        "--orange-client",
        default="",
        help=f"orange_client binary. Default: manifest default or {DEFAULT_ORANGE_CLIENT}.",
    )
    parser.add_argument(
        "--no-sudo",
        action="store_true",
        help="Do not prefix the benchmark command with sudo -n.",
    )
    parser.add_argument(
        "--continue-on-failure",
        action="store_true",
        help="Continue later runs after benchmark or verifier failure.",
    )
    parser.add_argument(
        "--no-verify",
        action="store_true",
        help="Do not run verify_external_recorder_session.py after each executed run.",
    )
    parser.add_argument(
        "--print-commands",
        action="store_true",
        help="Print planned benchmark commands.",
    )
    return parser.parse_args()


def selected_runs(manifest: dict[str, Any], run_ids: list[str], stages: list[str]) -> list[dict[str, Any]]:
    wanted_ids = set(run_ids)
    wanted_stages = set(stages)
    out = []
    for run in manifest.get("runs", []):
        if not isinstance(run, dict):
            continue
        if run.get("enabled", True) is False:
            continue
        if wanted_ids and run.get("run_id") not in wanted_ids:
            continue
        if wanted_stages and run.get("stage", "") not in wanted_stages:
            continue
        out.append(run)
    return out


def patch_matrix(spec: dict[str, Any], run: dict[str, Any]) -> None:
    matrix = spec.setdefault("matrix", {})
    overrides = run.get("matrix_overrides", {})
    if isinstance(overrides, dict):
        for key, value in overrides.items():
            matrix[key] = value if isinstance(value, list) else [value]


def patch_stream_paths(contract: dict[str, Any], artifact_root: Path) -> None:
    streams = contract.get("streams", {})
    if not isinstance(streams, dict):
        return
    for serial, stream in streams.items():
        if not isinstance(stream, dict):
            continue
        serial_text = str(stream.get("camera_serial") or serial)
        stream["summary_json"] = str(artifact_root / f"Cam{serial_text}_external_recorder_summary.json")
        stream["status_json"] = str(artifact_root / f"Cam{serial_text}_external_recorder_status.json")
        stream["video_sanity_json"] = str(artifact_root / f"Cam{serial_text}_external_video_sanity.json")
        stream["mp4"] = str(artifact_root / f"Cam{serial_text}_external.mp4")
        stream["gop_routing_csv"] = str(artifact_root / f"Cam{serial_text}_external_gop_routing.csv")


def apply_stream_overrides(contract: dict[str, Any], run: dict[str, Any]) -> None:
    streams = contract.get("streams", {})
    if not isinstance(streams, dict):
        return
    overrides = run.get("stream_overrides", {})
    if not isinstance(overrides, dict):
        return
    for serial, stream_override in overrides.items():
        if not isinstance(stream_override, dict):
            continue
        stream = streams.get(serial)
        if not isinstance(stream, dict):
            continue
        for key, value in stream_override.items():
            stream[key] = value
        shard_gpus = stream.get("expected_shard_gpu_ids")
        if isinstance(shard_gpus, list) and len(shard_gpus) > 1:
            stream["routing_policy"] = stream.get("routing_policy") or "gop_modulo"
        elif isinstance(shard_gpus, list) and len(shard_gpus) == 1:
            stream["routing_policy"] = stream.get("routing_policy") or "single_shard"


def resolve_run(
    manifest: dict[str, Any],
    run: dict[str, Any],
    matrix_root: Path,
    stamp: str,
    args: argparse.Namespace,
) -> ResolvedRun:
    defaults = manifest.get("defaults", {})
    run_id = sanitize(run.get("run_id", "run"))
    stage = str(run.get("stage", ""))
    description = str(run.get("description", ""))
    tags = [str(item) for item in as_list(run.get("tags"))]
    base_spec_path = path_from_repo(str(run["base_spec"]))
    spec = read_json(base_spec_path)

    manifest_id = sanitize(manifest.get("experiment_id", "encoding_master"))
    spec_experiment_id = f"{manifest_id}_{run_id}_{stamp}"
    artifact_root = matrix_root / "recorder_artifacts" / run_id
    analytics_output_root = Path(
        str(run.get("analytics_output_root") or defaults.get("analytics_output_root") or "/home/jeremy/orange_data/exp/unsorted")
    )

    fixed = spec.setdefault("fixed", {})
    fixed["duration_s"] = int(
        args.duration_s
        if args.duration_s is not None
        else run.get("duration_s", defaults.get("duration_s", fixed.get("duration_s", 0)))
    )
    fixed["warmup_s"] = int(
        args.warmup_s
        if args.warmup_s is not None
        else run.get("warmup_s", defaults.get("warmup_s", fixed.get("warmup_s", 0)))
    )
    fixed["output_root"] = str(analytics_output_root)
    spec["experiment_id"] = spec_experiment_id
    notes = [str(spec.get("notes", "")).strip(), f"Generated by {Path(__file__).name} from {base_spec_path.name}."]
    if description:
        notes.append(description)
    spec["notes"] = " ".join(item for item in notes if item)

    contract = fixed.setdefault("external_recorder_contract", {})
    contract["artifact_root"] = str(artifact_root)
    contract["session_id"] = spec_experiment_id
    contract["supervise_processes"] = True
    contract["require_summary"] = True
    contract["require_status"] = True
    contract["require_storage_preflight"] = True
    contract["require_protocol_hello"] = True
    contract["require_video_sanity"] = True
    contract["require_gop_routing"] = True
    streams = contract.get("streams", {})
    if isinstance(streams, dict):
        for serial, stream in streams.items():
            if not isinstance(stream, dict):
                continue
            if "analytics_gpu_id" not in stream:
                stream["analytics_gpu_id"] = defaults.get("analytics_gpu_id", 0)
            if "recorder_gpu_id" not in stream:
                stream["recorder_gpu_id"] = stream.get("analytics_gpu_id", defaults.get("analytics_gpu_id", 0))
    apply_stream_overrides(contract, run)
    patch_stream_paths(contract, artifact_root)
    patch_matrix(spec, run)

    spec_path = matrix_root / "resolved_specs" / f"{run_id}.json"
    analytics_root = analytics_output_root / spec_experiment_id
    benchmark = str(args.benchmark or defaults.get("benchmark") or DEFAULT_BENCHMARK)
    orange_client = str(args.orange_client or defaults.get("orange_client") or DEFAULT_ORANGE_CLIENT)
    use_sudo = bool(defaults.get("sudo", True)) and not args.no_sudo
    command = ["sudo", "-n", benchmark] if use_sudo else [benchmark]
    command.extend(["--orange-client", orange_client])
    if defaults.get("yolo_perf_log", True):
        command.append("--yolo-perf-log")
    sample = defaults.get("yolo_perf_sample", 1)
    if sample is not None:
        command.extend(["--yolo-perf-sample", str(sample)])
    command.append(str(spec_path))

    verify_command: list[str] | None = None
    if not args.no_verify and defaults.get("verify_external_recorder", True):
        verify_command = [
            str(REPO_ROOT / "scripts" / "verify_external_recorder_session.py"),
            str(artifact_root),
            "--analytics-root",
            str(analytics_root),
        ]
    return ResolvedRun(
        run_id=run_id,
        stage=stage,
        description=description,
        tags=tags,
        spec_path=spec_path,
        artifact_root=artifact_root,
        analytics_root=analytics_root,
        command=command,
        verify_command=verify_command,
        spec=spec,
        manifest_run=run,
    )


def run_command(command: list[str], stdout_path: Path, stderr_path: Path) -> int:
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open("w", encoding="utf-8") as stderr:
        proc = subprocess.run(command, stdout=stdout, stderr=stderr, text=True, check=False)
    return int(proc.returncode)


def run_resolved(resolved: ResolvedRun, execute: bool) -> None:
    write_json(resolved.spec_path, resolved.spec)
    run_dir = resolved.spec_path.parent.parent / "run_logs" / resolved.run_id
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "command.txt").write_text(
        shlex.join(resolved.command) + "\n", encoding="utf-8"
    )
    if resolved.verify_command:
        (run_dir / "verify_command.txt").write_text(
            shlex.join(resolved.verify_command) + "\n", encoding="utf-8"
        )

    if not execute:
        resolved.status = "planned"
        return

    resolved.started_at_utc = utc_iso()
    resolved.status = "running"
    try:
        resolved.returncode = run_command(
            resolved.command,
            run_dir / "orange_local_benchmark.stdout.log",
            run_dir / "orange_local_benchmark.stderr.log",
        )
        if resolved.returncode != 0:
            resolved.status = "failed"
            resolved.error = f"benchmark exited {resolved.returncode}"
            return
        if resolved.verify_command:
            resolved.verifier_returncode = run_command(
                resolved.verify_command,
                run_dir / "verify_external_recorder.stdout.log",
                run_dir / "verify_external_recorder.stderr.log",
            )
            if resolved.verifier_returncode != 0:
                resolved.status = "failed"
                resolved.error = f"verifier exited {resolved.verifier_returncode}"
                return
        resolved.status = "completed"
    except Exception as exc:  # noqa: BLE001 - runner must preserve failure detail.
        resolved.status = "failed"
        resolved.error = str(exc)
    finally:
        resolved.ended_at_utc = utc_iso()


def load_analytics_camera_results(analytics_root: Path) -> dict[str, dict[str, Any]]:
    runs_json_path = analytics_root / "runs.json"
    if not runs_json_path.exists():
        return {}
    try:
        payload = read_json(runs_json_path)
    except Exception:
        return {}
    out: dict[str, dict[str, Any]] = {}
    for run in payload.get("runs", []):
        if not isinstance(run, dict):
            continue
        for camera_result in run.get("camera_results", []):
            if isinstance(camera_result, dict):
                serial = str(camera_result.get("camera_serial", ""))
                if serial:
                    out[serial] = camera_result
    return out


def load_video_sanity(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        return read_json(path)
    except Exception:
        return {}


def summary_duration(summary: dict[str, Any], sanity: dict[str, Any], mp4_path: Path) -> float:
    container = sanity.get("container", {}) if isinstance(sanity, dict) else {}
    if isinstance(container, dict):
        duration = optional_float(container.get("duration"), 0.0)
        if duration > 0.0:
            return duration
    fps = optional_float(summary.get("fps"), 0.0)
    frames = optional_int(summary.get("frames_encoded"), 0)
    if fps > 0.0 and frames > 0:
        return frames / fps
    return ffprobe_duration(mp4_path)


def summarize_resolved(resolved: ResolvedRun) -> None:
    camera_results = load_analytics_camera_results(resolved.analytics_root)
    run_folder = find_single_run_folder(resolved.analytics_root)
    dmon = summarize_dmon(run_folder / "nvidia_smi_dmon.csv") if run_folder else summarize_dmon(Path("/nonexistent"))
    contract = resolved.spec.get("fixed", {}).get("external_recorder_contract", {})
    streams = contract.get("streams", {})
    if not isinstance(streams, dict):
        return
    for serial, stream in streams.items():
        if not isinstance(stream, dict):
            continue
        summary_path = Path(str(stream.get("summary_json", "")))
        summary = read_json(summary_path) if summary_path.exists() else {}
        sanity_path = Path(str(stream.get("video_sanity_json", "")))
        sanity = load_video_sanity(sanity_path)
        mp4_path = Path(str(stream.get("mp4") or summary.get("outputs", {}).get("mp4", "")))
        mp4_size = 0
        if mp4_path.exists():
            mp4_size = mp4_path.stat().st_size
        if not mp4_size:
            merged = summary.get("merged_output", {}) if isinstance(summary, dict) else {}
            mp4_size = optional_int(merged.get("bytes_written"), 0)
        if not mp4_size:
            external_encode = summary.get("external_encode", {}) if isinstance(summary, dict) else {}
            mp4_size = optional_int(external_encode.get("mp4_bytes"), 0)
        duration_s = summary_duration(summary, sanity, mp4_path)
        frames_encoded = optional_int(summary.get("frames_encoded"), 0)
        bytes_per_frame = (mp4_size / frames_encoded) if frames_encoded > 0 else 0.0
        achieved_mbps = (mp4_size * 8.0 / duration_s / 1_000_000.0) if duration_s > 0.0 else 0.0
        tb_day_1 = (mp4_size / duration_s * 86400.0 / 1_000_000_000_000.0) if duration_s > 0.0 else 0.0
        analytics = camera_results.get(str(serial), {})
        external_encode = summary.get("external_encode", {}) if isinstance(summary, dict) else {}
        row = {
            "manifest_run_id": resolved.run_id,
            "stage": resolved.stage,
            "status": resolved.status,
            "benchmark_returncode": resolved.returncode if resolved.returncode is not None else "",
            "verifier_returncode": resolved.verifier_returncode if resolved.verifier_returncode is not None else "",
            "error": resolved.error,
            "analytics_root": str(resolved.analytics_root),
            "recorder_artifact_root": str(resolved.artifact_root),
            "camera_serial": str(serial),
            "analytics_gpu_id": stream.get("analytics_gpu_id", ""),
            "recorder_gpu_id": stream.get("recorder_gpu_id", ""),
            "shard_gpu_ids": ",".join(str(item) for item in as_list(stream.get("expected_shard_gpu_ids"))),
            "shard_count": optional_int(summary.get("shard_count"), len(as_list(stream.get("expected_shard_gpu_ids")))),
            "codec": summary.get("codec", stream.get("codec", "")),
            "preset": summary.get("preset", stream.get("preset", "")),
            "tuning": summary.get("tuning", stream.get("tuning", "")),
            "fps": summary.get("fps", stream.get("encode_fps", "")),
            "encode_max_fps": summary.get("encode_max_fps", stream.get("encode_max_fps", "")),
            "gop": stream.get("gop", first_item(resolved.spec.get("matrix", {}).get("gop_length"), "")),
            "rate_control_mode": summary.get(
                "rate_control_mode",
                stream.get("rate_control_mode", first_item(resolved.spec.get("matrix", {}).get("rate_control_mode"), "")),
            ),
            "quality_value": summary.get(
                "quality_value",
                stream.get("quality_value", first_item(resolved.spec.get("matrix", {}).get("quality_value"), "")),
            ),
            "bitrate_bps": stream.get("bitrate_bps", ""),
            "max_bitrate_bps": stream.get("max_bitrate_bps", ""),
            "vbv_buffer_size": stream.get("vbv_buffer_size", ""),
            "frames_received": optional_int(summary.get("frames_received"), 0),
            "acks_sent": optional_int(summary.get("acks_sent"), 0),
            "encode_enqueued": optional_int(summary.get("encode_enqueued"), 0),
            "encode_skipped": optional_int(summary.get("encode_skipped"), 0),
            "encode_dropped": optional_int(summary.get("encode_dropped"), 0),
            "frames_encoded": frames_encoded,
            "worker_failed": bool(summary.get("worker_failed", False)),
            "encode_queue_depth": optional_int(summary.get("encode_queue_depth"), optional_int(stream.get("encode_queue_depth"), 0)),
            "encode_queue_high_water": optional_int(summary.get("encode_queue_high_water"), 0),
            "enqueue_age_p95_ms": optional_float(external_encode.get("enqueue_age_p95_ms"), 0.0),
            "encode_total_p95_ms": optional_float(external_encode.get("encode_total_p95_ms"), 0.0),
            "lock_bitstream_p95_ms": optional_float(external_encode.get("lock_bitstream_p95_ms"), 0.0),
            "mp4_write_mean_ms": optional_float(external_encode.get("mp4_write_mean_ms"), 0.0),
            "mp4_write_max_ms": optional_float(external_encode.get("mp4_write_max_ms"), 0.0),
            "mp4_path": str(mp4_path),
            "mp4_size_bytes": mp4_size,
            "duration_s": duration_s,
            "achieved_mbps": achieved_mbps,
            "bytes_per_frame": bytes_per_frame,
            "tb_per_day_1cam": tb_day_1,
            "tb_per_day_4cam": tb_day_1 * 4.0,
            "tb_per_day_8cam": tb_day_1 * 8.0,
            "video_sanity_status": sanity.get("status", ""),
            "video_content_valid": sanity.get("content_valid", ""),
            "mean_luma": sanity.get("mean_luma", ""),
            "max_stddev": sanity.get("max_stddev", ""),
            "max_black_fraction_lt8": sanity.get("max_black_fraction_lt8", ""),
            "acq_fps_mean": analytics.get("acq_fps_mean", ""),
            "acq_fps_p95": analytics.get("acq_fps_p95", ""),
            "camera_frame_id_gaps": analytics.get("camera_frame_id_gaps", ""),
            "get_frame_errors_final": analytics.get("get_frame_errors_final", ""),
            "external_ipc_failures_final": analytics.get("external_ipc_failures_final", ""),
            "external_ipc_ack_timeouts_final": analytics.get("external_ipc_ack_timeouts_final", ""),
            **dmon,
        }
        resolved.rows.append(row)
        for shard in summary.get("external_encode_shards", []) if isinstance(summary, dict) else []:
            if not isinstance(shard, dict):
                continue
            shard_mp4 = Path(str(shard.get("mp4", "")))
            shard_bytes = optional_int(shard.get("mp4_bytes"), 0)
            if not shard_bytes and shard_mp4.exists():
                shard_bytes = shard_mp4.stat().st_size
            resolved.shard_rows.append({
                "manifest_run_id": resolved.run_id,
                "stage": resolved.stage,
                "camera_serial": str(serial),
                "assigned_shard_id": shard.get("assigned_shard_id", ""),
                "assigned_gpu_id": shard.get("assigned_gpu_id", ""),
                "routing_policy": shard.get("routing_policy", ""),
                "frames_encoded": shard.get("frames_encoded", ""),
                "frames_dropped": shard.get("frames_dropped", ""),
                "mp4_bytes": shard_bytes,
                "encode_total_p95_ms": shard.get("encode_total_p95_ms", ""),
                "lock_bitstream_p95_ms": shard.get("lock_bitstream_p95_ms", ""),
                "slot_reuse_wait_p95_ms": shard.get("slot_reuse_wait_p95_ms", ""),
                "mp4_path": str(shard_mp4),
            })


def write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if fieldnames is None:
        keys: list[str] = []
        seen = set()
        for row in rows:
            for key in row:
                if key not in seen:
                    keys.append(key)
                    seen.add(key)
        fieldnames = keys
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_review_report(path: Path, manifest: dict[str, Any], resolved_runs: list[ResolvedRun]) -> None:
    lines = [
        "# Encoding Master Experiment Report",
        "",
        f"Generated: {utc_iso()}",
        f"Manifest: `{manifest.get('experiment_id', '')}`",
        "",
        "## Runs",
        "",
        "| Run | Stage | Status | Camera | Preset | Tuning | GOP | Shards | Encoded | Drops | Queue | Mbps | TB/day 1 cam | MP4 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | ---: | ---: | --- | ---: | ---: | --- |",
    ]
    for resolved in resolved_runs:
        if not resolved.rows:
            lines.append(
                f"| `{resolved.run_id}` | {resolved.stage} | {resolved.status} | | | | | | | | | | | |"
            )
            continue
        for row in resolved.rows:
            queue = f"{row.get('encode_queue_high_water', '')}/{row.get('encode_queue_depth', '')}"
            lines.append(
                "| "
                f"`{row.get('manifest_run_id', '')}` | "
                f"{row.get('stage', '')} | "
                f"{row.get('status', '')} | "
                f"{row.get('camera_serial', '')} | "
                f"{row.get('preset', '')} | "
                f"{row.get('tuning', '')} | "
                f"{row.get('gop', '')} | "
                f"{row.get('shard_gpu_ids', '')} | "
                f"{row.get('frames_encoded', '')} | "
                f"{row.get('encode_dropped', '')} | "
                f"{queue} | "
                f"{optional_float(row.get('achieved_mbps'), 0.0):.1f} | "
                f"{optional_float(row.get('tb_per_day_1cam'), 0.0):.2f} | "
                f"`{row.get('mp4_path', '')}` |"
            )
    lines.extend([
        "",
        "## Outputs",
        "",
        "- `matrix_summary.csv`: one row per run/camera.",
        "- `per_camera_summary.csv`: same schema as matrix summary for this first slice.",
        "- `per_shard_summary.csv`: one row per run/camera/shard.",
        "- `storage_projection.csv`: storage-focused subset.",
        "- `run_results.jsonl`: machine-readable run rows.",
        "",
        "## Review Notes",
        "",
        "Human visual review is still required for lossy settings. Use this report to",
        "screen out failed, dropped, queue-unstable, or storage-impossible candidates",
        "before spending time on visual comparison.",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    args = parse_args()
    manifest_path = path_from_repo(args.manifest)
    manifest = read_json(manifest_path)
    stamp = sanitize(args.stamp or utc_stamp())
    defaults = manifest.get("defaults", {})
    root_base = Path(str(args.output_root or defaults.get("artifact_root") or "/tmp/orange_encoding_master"))
    matrix_root = root_base / f"{sanitize(manifest.get('experiment_id', 'encoding_master'))}_{stamp}"
    matrix_root.mkdir(parents=True, exist_ok=True)

    runs = selected_runs(manifest, args.run_id, args.stage)
    if not runs:
        print("No manifest runs selected.", file=sys.stderr)
        return 2

    execute = bool(args.execute and not args.dry_run)
    resolved_runs = [
        resolve_run(manifest, run, matrix_root, stamp, args)
        for run in runs
    ]

    write_json(matrix_root / "manifest_input.json", manifest)
    write_json(
        matrix_root / "resolved_manifest.json",
        {
            "schema_id": "orange.encoding_master_experiment.resolved_manifest",
            "schema_version": 1,
            "created_at_utc": utc_iso(),
            "source_manifest": str(manifest_path),
            "execute": execute,
            "matrix_root": str(matrix_root),
            "runs": [
                {
                    "run_id": run.run_id,
                    "stage": run.stage,
                    "description": run.description,
                    "tags": run.tags,
                    "spec_path": str(run.spec_path),
                    "artifact_root": str(run.artifact_root),
                    "analytics_root": str(run.analytics_root),
                    "command": run.command,
                    "verify_command": run.verify_command,
                }
                for run in resolved_runs
            ],
        },
    )

    if args.print_commands or not execute:
        for run in resolved_runs:
            print(f"[{run.run_id}] {shlex.join(run.command)}")
            if run.verify_command:
                print(f"[{run.run_id} verify] {shlex.join(run.verify_command)}")

    for run in resolved_runs:
        print(f"[encoding-master] {run.run_id}: {'executing' if execute else 'planning'}")
        run_resolved(run, execute)
        summarize_resolved(run)
        if run.status == "failed" and not args.continue_on_failure:
            break

    rows = [row for run in resolved_runs for row in run.rows]
    shard_rows = [row for run in resolved_runs for row in run.shard_rows]
    planned_rows = [
        {
            "manifest_run_id": run.run_id,
            "stage": run.stage,
            "status": run.status,
            "spec_path": str(run.spec_path),
            "analytics_root": str(run.analytics_root),
            "recorder_artifact_root": str(run.artifact_root),
            "command": shlex.join(run.command),
            "error": run.error,
        }
        for run in resolved_runs
    ]

    if rows:
        write_csv(matrix_root / "matrix_summary.csv", rows)
        write_csv(matrix_root / "per_camera_summary.csv", rows)
        write_csv(
            matrix_root / "storage_projection.csv",
            rows,
            [
                "manifest_run_id",
                "stage",
                "camera_serial",
                "codec",
                "preset",
                "tuning",
                "rate_control_mode",
                "quality_value",
                "gop",
                "shard_count",
                "mp4_size_bytes",
                "duration_s",
                "achieved_mbps",
                "bytes_per_frame",
                "tb_per_day_1cam",
                "tb_per_day_4cam",
                "tb_per_day_8cam",
                "mp4_path",
            ],
        )
    else:
        write_csv(matrix_root / "planned_runs.csv", planned_rows)
    write_csv(matrix_root / "per_shard_summary.csv", shard_rows)
    with (matrix_root / "run_results.jsonl").open("w", encoding="utf-8") as handle:
        for run in resolved_runs:
            payload = {
                "run_id": run.run_id,
                "stage": run.stage,
                "status": run.status,
                "returncode": run.returncode,
                "verifier_returncode": run.verifier_returncode,
                "error": run.error,
                "analytics_root": str(run.analytics_root),
                "artifact_root": str(run.artifact_root),
                "rows": run.rows,
                "shard_rows": run.shard_rows,
            }
            handle.write(json.dumps(payload, sort_keys=True) + "\n")
    write_review_report(matrix_root / "review_report.md", manifest, resolved_runs)

    failures = [run for run in resolved_runs if run.status == "failed"]
    print(f"[encoding-master] matrix_root={matrix_root}")
    print(f"[encoding-master] selected_runs={len(resolved_runs)} executed={execute}")
    print(f"[encoding-master] failures={len(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
