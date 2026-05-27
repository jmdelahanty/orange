#!/usr/bin/env python3
"""Summarize a production-like Orange GUI validation recording folder."""

from __future__ import annotations

import argparse
import csv
import json
import math
import shutil
import subprocess
from pathlib import Path
from typing import Any

from recording_output_validation import build_recording_output_summary


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")
DEFAULT_GUI_RECORDING_ROOT = Path("/home/jeremy/orange_data/exp/unsorted")
GUI_TIMING_BREAKDOWN_BUCKETS = [
    ("main_texture_upload_ms", "main-texture-upload"),
    ("crop_texture_upload_ms", "crop-texture-upload"),
    ("camera_window_draw_ms", "camera-window-draw"),
    ("crop_window_draw_ms", "crop-window-draw"),
    ("speed_graph_draw_ms", "speed-graph-draw"),
    ("render_present_ms", "render-present"),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Summarize the fields used for two-camera GUI validation: selected "
            "YOLO engine, PTP register-read decimation, YOLO latency, pipeline "
            "health counters, spatial calibrations, and main MP4 sanity."
        )
    )
    parser.add_argument(
        "recording_folder",
        nargs="?",
        help="Recording folder or parent containing one run folder.",
    )
    parser.add_argument(
        "--latest",
        nargs="?",
        const=str(DEFAULT_GUI_RECORDING_ROOT),
        metavar="ROOT",
        help=(
            "Summarize the newest direct child of ROOT containing "
            "recording_snapshot.json. With no ROOT, uses "
            f"{DEFAULT_GUI_RECORDING_ROOT}."
        ),
    )
    parser.add_argument(
        "--latest-complete",
        nargs="?",
        const=str(DEFAULT_GUI_RECORDING_ROOT),
        metavar="ROOT",
        help=(
            "Summarize the newest direct child of ROOT that looks like a real "
            "recording: recording_snapshot.json plus matching main MP4, "
            "pipeline perf CSV, and YOLO perf CSV for at least one camera. "
            f"With no ROOT, uses {DEFAULT_GUI_RECORDING_ROOT}."
        ),
    )
    parser.add_argument(
        "--steady-after-frame",
        type=int,
        default=50,
        help="Frame id threshold for steady-state YOLO p95 calculations. Default: 50.",
    )
    parser.add_argument(
        "--ffprobe",
        default=str(DEFAULT_FFPROBE if DEFAULT_FFPROBE.exists() else "ffprobe"),
        help="ffprobe executable path. Defaults to Orange ffprobe when available.",
    )
    parser.add_argument("--json", action="store_true", help="Print only machine-readable JSON.")
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def path_from_recording_folder(recording_folder: Path, value: Any) -> Path:
    path = Path(str(value or ""))
    return path if path.is_absolute() else recording_folder / path


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            return list(csv.DictReader(handle))
    except OSError:
        return []


def external_detach_queue_high_water(summary_path: Path) -> int | None:
    name = summary_path.name
    if not name.endswith("_summary.json"):
        return None
    detach_path = summary_path.with_name(name[: -len("_summary.json")] + "_detach.csv")
    rows = read_csv_rows(detach_path)
    values = [
        value
        for row in rows
        for value in [int_field(row, "encode_queue_depth")]
        if value is not None
    ]
    return max(values) if values else None


def resolve_recording_folder(path: Path) -> Path:
    path = path.expanduser().resolve()
    if (path / "recording_snapshot.json").exists():
        return path
    if not path.is_dir():
        return path
    candidates = sorted(child for child in path.iterdir() if (child / "recording_snapshot.json").exists())
    if len(candidates) == 1:
        return candidates[0]
    return path


def camera_serials_with_complete_artifacts(recording_folder: Path) -> set[str]:
    videos = {
        serial
        for path in recording_folder.glob("Cam*.mp4")
        if path.stat().st_size > 0
        for serial in [camera_serial_from_video(path)]
        if serial is not None
    }
    manifest = read_json(recording_folder / "recording_session.json")
    camera_artifacts = manifest.get("camera_artifacts")
    camera_artifacts = camera_artifacts if isinstance(camera_artifacts, dict) else {}
    for serial, artifact in camera_artifacts.items():
        artifact = artifact if isinstance(artifact, dict) else {}
        video_path = path_from_recording_folder(recording_folder, artifact.get("video"))
        if video_path.exists() and video_path.stat().st_size > 0:
            videos.add(str(serial))
    pipeline = {
        serial
        for path in recording_folder.glob("Cam*_pipeline_perf.csv")
        if path.stat().st_size > 0
        for serial in [camera_serial_from_pipeline_perf(path)]
        if serial is not None
    }
    yolo = {
        serial
        for path in recording_folder.glob("Cam*_yolo_perf.csv")
        if path.stat().st_size > 0
        for serial in [camera_serial_from_yolo_perf(path)]
        if serial is not None
    }
    return videos & pipeline & yolo


def is_complete_recording_candidate(recording_folder: Path) -> bool:
    return (
        (recording_folder / "recording_snapshot.json").exists()
        and bool(camera_serials_with_complete_artifacts(recording_folder))
    )


def resolve_latest_recording_folder(root: Path, *, require_complete: bool = False) -> Path:
    root = root.expanduser().resolve()
    if (root / "recording_snapshot.json").exists():
        if require_complete and not is_complete_recording_candidate(root):
            raise SystemExit(f"--latest-complete root is not a complete recording folder: {root}")
        return root
    if not root.is_dir():
        option = "--latest-complete" if require_complete else "--latest"
        raise SystemExit(f"{option} root is not a directory: {root}")

    candidates: list[tuple[float, Path]] = []
    for snapshot_path in root.glob("*/recording_snapshot.json"):
        recording_folder = snapshot_path.parent
        if require_complete and not is_complete_recording_candidate(recording_folder):
            continue
        try:
            mtime = snapshot_path.stat().st_mtime
        except OSError:
            continue
        candidates.append((mtime, recording_folder))
    if not candidates:
        if require_complete:
            raise SystemExit(
                "--latest-complete found no direct child with recording_snapshot.json, "
                f"main MP4, pipeline perf CSV, and YOLO perf CSV under {root}"
            )
        raise SystemExit(f"--latest found no recording_snapshot.json under direct children of {root}")
    candidates.sort(key=lambda item: (item[0], str(item[1])))
    return candidates[-1][1]


def resolve_requested_recording_folder(args: argparse.Namespace) -> Path:
    latest_modes = [args.latest is not None, args.latest_complete is not None]
    if sum(latest_modes) > 1:
        raise SystemExit("pass only one of --latest or --latest-complete")
    if any(latest_modes) and args.recording_folder:
        raise SystemExit("pass either recording_folder or a latest-mode option, not both")
    if args.latest is not None:
        return resolve_latest_recording_folder(Path(args.latest))
    if args.latest_complete is not None:
        return resolve_latest_recording_folder(Path(args.latest_complete), require_complete=True)
    if not args.recording_folder:
        raise SystemExit("recording_folder is required unless --latest or --latest-complete is used")
    return resolve_recording_folder(Path(args.recording_folder))


def camera_serial_from_yolo_perf(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith("_yolo_perf.csv"):
        return None
    return name[len("Cam") : -len("_yolo_perf.csv")]


def camera_serial_from_pipeline_perf(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith("_pipeline_perf.csv"):
        return None
    return name[len("Cam") : -len("_pipeline_perf.csv")]


def camera_serial_from_cadence(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith("_acquisition_cadence_probe.csv"):
        return None
    return name[len("Cam") : -len("_acquisition_cadence_probe.csv")]


def camera_serial_from_video(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith(".mp4") or name.endswith("_crop.mp4"):
        return None
    return name[len("Cam") : -len(".mp4")]


def camera_serial_from_pose_events(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith("_pose_events.jsonl"):
        return None
    return name[len("Cam") : -len("_pose_events.jsonl")]


def camera_serial_from_crop_artifact(path: Path, suffix: str) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith(suffix):
        return None
    return name[len("Cam") : -len(suffix)]


def float_field(row: dict[str, str], field: str) -> float | None:
    value = row.get(field)
    if value in (None, ""):
        return None
    try:
        return float(value)
    except ValueError:
        return None


def int_field(row: dict[str, str], field: str) -> int | None:
    value = row.get(field)
    if value in (None, ""):
        return None
    try:
        return int(float(value))
    except ValueError:
        return None


def number_value(value: Any) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def int_value(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def first_present(*values: Any) -> Any:
    for value in values:
        if value is not None:
            return value
    return None


def nested_dict(value: Any, *keys: str) -> dict[str, Any]:
    current = value
    for key in keys:
        if not isinstance(current, dict):
            return {}
        current = current.get(key)
    return current if isinstance(current, dict) else {}


def serial_map_value(container: dict[str, Any], map_name: str, serial: str) -> Any:
    mapping = nested_dict(container, map_name)
    return mapping.get(serial) if mapping else None


def summarize_gui_timing_diagnosis(gui_display_frame_rate: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(gui_display_frame_rate, dict):
        return {}
    timings = gui_display_frame_rate.get("timings")
    timings = timings if isinstance(timings, dict) else {}
    if not timings:
        return {}

    def timing_bucket(bucket_name: str) -> dict[str, Any]:
        bucket = timings.get(bucket_name)
        return bucket if isinstance(bucket, dict) else {}

    frame_total_bucket = timing_bucket("frame_total_ms")
    frame_total_p95_ms = number_value(frame_total_bucket.get("p95_ms"))
    ranked: list[dict[str, Any]] = []
    for bucket_name, label in GUI_TIMING_BREAKDOWN_BUCKETS:
        bucket = timing_bucket(bucket_name)
        p95_ms = number_value(bucket.get("p95_ms"))
        if p95_ms is None:
            continue
        ranked.append(
            {
                "bucket": bucket_name,
                "label": label,
                "p95_ms": p95_ms,
                "sample_count": int_value(bucket.get("sample_count")),
            }
        )
    ranked.sort(key=lambda item: item["p95_ms"], reverse=True)

    out: dict[str, Any] = {}
    if frame_total_p95_ms is not None:
        out["frame_total_p95_ms"] = frame_total_p95_ms
    if ranked:
        dominant = ranked[0]
        out["dominant_timing_bucket"] = dominant["bucket"]
        out["dominant_timing_label"] = dominant["label"]
        out["dominant_timing_p95_ms"] = dominant["p95_ms"]
        if frame_total_p95_ms and frame_total_p95_ms > 0.0:
            out["dominant_timing_fraction_of_frame_total_p95"] = (
                dominant["p95_ms"] / frame_total_p95_ms
            )
        out["timing_p95_ranked"] = ranked
    return out


def percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = math.ceil((pct / 100.0) * len(ordered)) - 1
    rank = min(max(rank, 0), len(ordered) - 1)
    return ordered[rank]


def summarize_metric(rows: list[dict[str, str]], field: str, steady_after_frame: int) -> dict[str, Any]:
    all_values: list[float] = []
    steady_values: list[float] = []
    for row in rows:
        value = float_field(row, field)
        if value is None:
            continue
        all_values.append(value)
        frame_id = int_field(row, "frame_id")
        if frame_id is not None and frame_id >= steady_after_frame:
            steady_values.append(value)
    return {
        "samples": len(all_values),
        "p95": percentile(all_values, 95.0),
        "max": max(all_values) if all_values else None,
        "steady_after_frame": steady_after_frame,
        "steady_samples": len(steady_values),
        "steady_p95": percentile(steady_values, 95.0),
        "steady_max": max(steady_values) if steady_values else None,
    }


def summarize_yolo(recording_folder: Path, steady_after_frame: int) -> dict[str, Any]:
    fields = [
        "acquisition_to_detect_done_ms",
        "capture_to_detect_done_ms",
        "worker_start_to_detect_done_ms",
        "total_ms",
        "yolo_queue_wait_ms",
        "cpu_pre_sync_ms",
        "acquisition_to_ptp_done_ms",
    ]
    summaries: dict[str, Any] = {}
    for path in sorted(recording_folder.glob("Cam*_yolo_perf.csv")):
        serial = camera_serial_from_yolo_perf(path)
        if serial is None:
            continue
        rows = read_csv_rows(path)
        metrics = {
            field: summarize_metric(rows, field, steady_after_frame)
            for field in fields
            if rows and field in rows[0]
        }
        ok_values = [int_field(row, "ok") for row in rows if "ok" in row]
        summaries[serial] = {
            "path": str(path),
            "rows": len(rows),
            "ok_rows": sum(1 for value in ok_values if value == 1),
            "metrics": metrics,
        }
    return summaries


def summarize_pipeline(recording_folder: Path) -> dict[str, Any]:
    summaries: dict[str, Any] = {}
    fields = [
        "camera_dropped_frames",
        "camera_frame_id_gaps",
        "get_frame_errors",
        "enc_fail",
        "enc_slow",
        "external_ipc_frames_acked",
        "external_ipc_failures",
        "external_ipc_ack_timeouts",
        "display_q",
        "display_preview_max_fps",
        "display_preview_eligible",
        "display_preview_selected",
        "display_preview_skipped",
        "gpu_direct",
        "gpu_ring",
        "gpu_copy",
    ]
    for path in sorted(recording_folder.glob("Cam*_pipeline_perf.csv")):
        serial = camera_serial_from_pipeline_perf(path)
        if serial is None:
            continue
        rows = read_csv_rows(path)
        final = rows[-1] if rows else {}
        summaries[serial] = {
            "path": str(path),
            "rows": len(rows),
            "final": {field: int_field(final, field) for field in fields if field in final},
        }
    return summaries


def output_path(recording_folder: Path, output: dict[str, Any], key: str, default_name: str) -> Path:
    paths = output.get("paths")
    paths = paths if isinstance(paths, dict) else {}
    path_status = paths.get(key)
    path_status = path_status if isinstance(path_status, dict) else {}
    if path_status.get("path"):
        return Path(str(path_status["path"]))
    value = output.get(key)
    if value:
        return path_from_recording_folder(recording_folder, value)
    return recording_folder / default_name


def summarize_crop_recording(
    recording_folder: Path,
    outputs: dict[str, dict[str, Any]],
    manifest: dict[str, Any],
) -> dict[str, Any]:
    summaries: dict[str, Any] = {}
    crop_recording_backend = nested_dict(manifest, "recording_backend", "crop_recording")
    serials: set[str] = {
        str(serial)
        for serial, camera_outputs in outputs.items()
        if isinstance(camera_outputs, dict) and isinstance(camera_outputs.get("crop"), dict)
    }
    for path, suffix in (
        (recording_folder.glob("Cam*_crop_meta.csv"), "_crop_meta.csv"),
        (recording_folder.glob("Cam*_crop_perf.csv"), "_crop_perf.csv"),
        (recording_folder.glob("Cam*_crop_sidecar_perf.csv"), "_crop_sidecar_perf.csv"),
    ):
        for item in path:
            serial = camera_serial_from_crop_artifact(item, suffix)
            if serial is not None:
                serials.add(serial)

    for serial in sorted(serials):
        crop_output = outputs.get(serial, {}).get("crop")
        crop_output = crop_output if isinstance(crop_output, dict) else {}
        metadata_path = output_path(
            recording_folder,
            crop_output,
            "metadata",
            f"Cam{serial}_crop_meta.csv",
        )
        perf_path = output_path(
            recording_folder,
            crop_output,
            "perf",
            f"Cam{serial}_crop_perf.csv",
        )
        sidecar_path = output_path(
            recording_folder,
            crop_output,
            "sidecar_perf",
            f"Cam{serial}_crop_sidecar_perf.csv",
        )

        metadata_rows = read_csv_rows(metadata_path)
        perf_rows = read_csv_rows(perf_path)
        sidecar_rows = read_csv_rows(sidecar_path)
        sidecar_final = sidecar_rows[-1] if sidecar_rows else {}
        summary_path = output_path(
            recording_folder,
            crop_output,
            "summary",
            f"Cam{serial}_crop_external_summary.json",
        )
        external_summary = read_json(summary_path) if summary_path.exists() else {}
        external_encode = external_summary.get("external_encode")
        external_encode = external_encode if isinstance(external_encode, dict) else {}
        queue_high_water = external_summary.get("encode_queue_high_water")
        if queue_high_water is None and summary_path.exists():
            queue_high_water = external_detach_queue_high_water(summary_path)
        stream_config = serial_map_value(crop_recording_backend, "stream_config", serial)
        stream_config = stream_config if isinstance(stream_config, dict) else {}
        external_frames_received = int_value(first_present(
            external_summary.get("frames_received"),
            serial_map_value(crop_recording_backend, "frames_received", serial),
        ))
        external_frames_encoded = int_value(first_present(
            external_summary.get("frames_encoded"),
            serial_map_value(crop_recording_backend, "frames_encoded", serial),
        ))
        external_encode_dropped = int_value(first_present(
            external_summary.get("encode_dropped"),
            serial_map_value(crop_recording_backend, "encode_dropped", serial),
        ))
        external_frames_dropped = int_value(first_present(
            external_encode.get("frames_dropped"),
            serial_map_value(crop_recording_backend, "external_frames_dropped", serial),
        ))
        external_encode_queue_depth = int_value(first_present(
            external_summary.get("encode_queue_depth"),
            serial_map_value(crop_recording_backend, "encode_queue_depth", serial),
            stream_config.get("encode_queue_depth"),
        ))
        external_encode_queue_high_water = int_value(first_present(
            queue_high_water,
            serial_map_value(crop_recording_backend, "encode_queue_high_water", serial),
        ))
        external_enqueue_age_p95_ms = number_value(first_present(
            external_encode.get("enqueue_age_p95_ms"),
            serial_map_value(crop_recording_backend, "enqueue_age_p95_ms", serial),
        ))

        summaries[serial] = {
            "backend": crop_output.get("backend"),
            "status": crop_output.get("status"),
            "frame_count": crop_output.get("frame_count"),
            "metadata_path": str(metadata_path),
            "metadata_rows": len(metadata_rows) if metadata_path.exists() else None,
            "metadata_detection_rows": (
                sum(1 for row in metadata_rows if int_field(row, "has_detection") == 1)
                if metadata_path.exists()
                else None
            ),
            "perf_path": str(perf_path),
            "perf_rows": len(perf_rows) if perf_path.exists() else None,
            "perf_dropped_rows": (
                sum(1 for row in perf_rows if int_field(row, "dropped") == 1)
                if perf_path.exists()
                else None
            ),
            "sidecar_path": str(sidecar_path),
            "sidecar_rows": len(sidecar_rows) if sidecar_path.exists() else None,
            "crop_frame_pool_size": int_field(sidecar_final, "crop_frame_pool_size"),
            "preview_max_fps": int_field(sidecar_final, "preview_max_fps"),
            "preview_disabled": int_field(sidecar_final, "preview_disabled"),
            "preview_display_enabled_final": int_field(sidecar_final, "preview_display_enabled_final"),
            "preview_frames_offered": int_field(sidecar_final, "preview_frames_offered"),
            "preview_frames_updated": int_field(sidecar_final, "preview_frames_updated"),
            "preview_frames_skipped_by_cadence": int_field(
                sidecar_final,
                "preview_frames_skipped_by_cadence",
            ),
            "preview_queue_full_drops": int_field(sidecar_final, "preview_queue_full_drops"),
            "producer_recording_crop_frame_offered": int_field(
                sidecar_final,
                "producer_recording_crop_frame_offered",
            ),
            "producer_recording_crop_frame_accepted": int_field(
                sidecar_final,
                "producer_recording_crop_frame_accepted",
            ),
            "producer_recording_crop_frame_dropped": int_field(
                sidecar_final,
                "producer_recording_crop_frame_dropped",
            ),
            "producer_preview_crop_frame_offered": int_field(
                sidecar_final,
                "producer_preview_crop_frame_offered",
            ),
            "producer_preview_crop_frame_accepted": int_field(
                sidecar_final,
                "producer_preview_crop_frame_accepted",
            ),
            "producer_preview_crop_frame_dropped": int_field(
                sidecar_final,
                "producer_preview_crop_frame_dropped",
            ),
            "producer_pose_crop_frame_offered": int_field(
                sidecar_final,
                "producer_pose_crop_frame_offered",
            ),
            "producer_pose_crop_frame_accepted": int_field(
                sidecar_final,
                "producer_pose_crop_frame_accepted",
            ),
            "producer_pose_crop_frame_dropped": int_field(
                sidecar_final,
                "producer_pose_crop_frame_dropped",
            ),
            "external_summary_path": str(summary_path),
            "external_stream_config": stream_config,
            "external_frames_received": external_frames_received,
            "external_frames_encoded": external_frames_encoded,
            "external_encode_dropped": external_encode_dropped,
            "external_frames_dropped": external_frames_dropped,
            "external_encode_queue_depth": external_encode_queue_depth,
            "external_encode_queue_high_water": external_encode_queue_high_water,
            "external_enqueue_age_p95_ms": external_enqueue_age_p95_ms,
            "external_encode_total_p95_ms": external_encode.get("encode_total_p95_ms"),
            "external_lock_bitstream_p95_ms": external_encode.get("lock_bitstream_p95_ms"),
        }
    return summaries


def summarize_ptp(recording_folder: Path) -> dict[str, Any]:
    summary: dict[str, Any] = {"source": None, "cameras": {}}
    ptp_summary = read_json(recording_folder / "ptp_sync_summary.json")
    cameras = ptp_summary.get("cameras")
    if isinstance(cameras, dict):
        summary["source"] = "ptp_sync_summary.json"
        for serial, camera in cameras.items():
            if not isinstance(camera, dict):
                continue
            summary["cameras"][serial] = {
                "ptp_register_read_decimate": camera.get("ptp_register_read_decimate"),
                "ptp_register_reads": camera.get("ptp_register_reads"),
                "last_ptp_register_read_frame": camera.get("last_ptp_register_read_frame"),
                "camera_frame_id_gaps": camera.get("camera_frame_id_gaps"),
                "get_frame_errors": camera.get("get_frame_errors"),
            }

    for path in sorted(recording_folder.glob("Cam*_acquisition_cadence_probe.csv")):
        serial = camera_serial_from_cadence(path)
        if serial is None:
            continue
        rows = read_csv_rows(path)
        if not rows:
            continue
        final = rows[-1]
        camera = summary["cameras"].setdefault(serial, {})
        if "ptp_register_read_decimate" in final:
            camera["ptp_register_read_decimate"] = int_field(final, "ptp_register_read_decimate")
            camera["ptp_register_reads_from_cadence"] = sum(
                1 for row in rows if int_field(row, "ptp_register_read") == 1
            )
            camera["ptp_register_read_age_frames_max"] = max(
                (
                    int_field(row, "ptp_register_read_age_frames")
                    for row in rows
                    if int_field(row, "ptp_register_read_age_frames") is not None
                ),
                default=None,
            )
        for field in ("camera_dropped_frames", "get_frame_errors", "enc_fail", "enc_slow"):
            if field in final:
                camera[field] = int_field(final, field)
    return summary


def summarize_models(snapshot: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    models = snapshot.get("models")
    if not isinstance(models, dict):
        return out
    for serial, camera_models in models.items():
        if not isinstance(camera_models, dict):
            continue
        detect = camera_models.get("detect")
        if not isinstance(detect, dict):
            continue
        runtime = detect.get("runtime")
        if not isinstance(runtime, dict):
            runtime = {}
        engine_path = runtime.get("engine_path")
        out[str(serial)] = {
            "enabled": detect.get("enabled"),
            "backend": runtime.get("backend"),
            "engine_path": engine_path,
            "engine_name": Path(engine_path).name if isinstance(engine_path, str) else None,
            "model_id": runtime.get("model_id"),
            "gpu_id": runtime.get("gpu_id"),
        }
    return out


def summarize_spatial_calibrations(snapshot: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    calibrations = snapshot.get("calibrations")
    if not isinstance(calibrations, dict):
        return out
    for serial, calibration in calibrations.items():
        if not isinstance(calibration, dict):
            continue
        entry: dict[str, Any] = {}
        dish_mask = calibration.get("dish_mask")
        if isinstance(dish_mask, dict):
            runtime = dish_mask.get("runtime")
            ref = dish_mask.get("calibration_ref")
            entry["dish_mask"] = {
                "artifact_id": ref.get("artifact_id") if isinstance(ref, dict) else None,
                "enabled": runtime.get("enabled") if isinstance(runtime, dict) else None,
                "source": runtime.get("source") if isinstance(runtime, dict) else None,
            }
        arena_layout = calibration.get("arena_layout")
        if isinstance(arena_layout, dict):
            runtime = arena_layout.get("runtime")
            ref = arena_layout.get("calibration_ref")
            entry["arena_layout"] = {
                "artifact_id": ref.get("artifact_id") if isinstance(ref, dict) else None,
                "layout_id": runtime.get("layout_id") if isinstance(runtime, dict) else None,
                "enabled": runtime.get("enabled") if isinstance(runtime, dict) else None,
                "zone_count": len(runtime.get("zones", [])) if isinstance(runtime, dict) else None,
            }
        out[str(serial)] = entry
    return out


def summarize_pose_events(recording_folder: Path) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for path in sorted(recording_folder.glob("Cam*_pose_events.jsonl")):
        serial = camera_serial_from_pose_events(path)
        if serial is None:
            continue
        rows = 0
        statuses: dict[str, int] = {}
        last_recording_frame_id: int | None = None
        parse_errors = 0
        try:
            with path.open("r", encoding="utf-8") as handle:
                for line in handle:
                    stripped = line.strip()
                    if not stripped:
                        continue
                    rows += 1
                    try:
                        event = json.loads(stripped)
                    except json.JSONDecodeError:
                        parse_errors += 1
                        continue
                    pose = event.get("pose")
                    if isinstance(pose, dict):
                        status = str(pose.get("status", "missing"))
                        statuses[status] = statuses.get(status, 0) + 1
                    frame = event.get("frame")
                    if isinstance(frame, dict) and isinstance(frame.get("recording_frame_id"), int):
                        last_recording_frame_id = frame["recording_frame_id"]
        except OSError:
            continue
        out[serial] = {
            "path": str(path),
            "rows": rows,
            "statuses": statuses,
            "last_recording_frame_id": last_recording_frame_id,
            "parse_errors": parse_errors,
        }
    return out


def ffprobe_video(path: Path, ffprobe: str) -> dict[str, Any]:
    if not Path(ffprobe).exists() and shutil.which(ffprobe) is None:
        return {"status": "present_unprobed", "size_bytes": path.stat().st_size}
    command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,nb_frames,nb_read_frames,avg_frame_rate,duration:format=duration,size,bit_rate",
        "-of",
        "json",
        str(path),
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=20, check=False)
    if result.returncode != 0:
        return {
            "status": "ffprobe_failed",
            "error": result.stderr.strip(),
            "size_bytes": path.stat().st_size,
        }
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        return {"status": "ffprobe_json_failed", "error": str(exc), "size_bytes": path.stat().st_size}
    streams = payload.get("streams", [])
    if not streams:
        return {"status": "no_video_stream", "size_bytes": path.stat().st_size}
    stream = streams[0]
    fmt = payload.get("format", {}) if isinstance(payload.get("format"), dict) else {}
    size = int(fmt.get("size") or path.stat().st_size)
    duration = float(stream.get("duration") or fmt.get("duration") or 0.0)
    bit_rate = fmt.get("bit_rate")
    bitrate_bps = int(bit_rate) if isinstance(bit_rate, str) and bit_rate.isdigit() else None
    if bitrate_bps is None and duration > 0:
        bitrate_bps = int((size * 8) / duration)
    return {
        "status": "ok",
        "width": int(stream.get("width") or 0),
        "height": int(stream.get("height") or 0),
        "frames": int(stream.get("nb_frames") or stream.get("nb_read_frames") or 0),
        "avg_frame_rate": stream.get("avg_frame_rate"),
        "duration_s": duration,
        "size_bytes": size,
        "bitrate_bps": bitrate_bps,
    }


def summarize_videos(recording_folder: Path, ffprobe: str) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for path in sorted(recording_folder.glob("Cam*.mp4")):
        serial = camera_serial_from_video(path)
        if serial is None:
            continue
        out[serial] = {"path": str(path), "source": "recording_folder", **ffprobe_video(path, ffprobe)}

    manifest = read_json(recording_folder / "recording_session.json")
    camera_artifacts = manifest.get("camera_artifacts")
    camera_artifacts = camera_artifacts if isinstance(camera_artifacts, dict) else {}
    for serial, artifact in sorted(camera_artifacts.items()):
        artifact = artifact if isinstance(artifact, dict) else {}
        video_path = path_from_recording_folder(recording_folder, artifact.get("video"))
        if not video_path.exists() or video_path.stat().st_size == 0:
            continue
        out[str(serial)] = {
            "path": str(video_path),
            "source": "recording_session",
            **ffprobe_video(video_path, ffprobe),
        }
    return out


def summarize(recording_folder: Path, steady_after_frame: int, ffprobe: str) -> dict[str, Any]:
    recording_folder = resolve_recording_folder(recording_folder)
    snapshot = read_json(recording_folder / "recording_snapshot.json")
    manifest = read_json(recording_folder / "recording_session.json")
    outputs = build_recording_output_summary(recording_folder, manifest, snapshot)
    gui_display_frame_rate = (
        snapshot.get("session", {}).get("gui_display_frame_rate")
        if isinstance(snapshot.get("session"), dict)
        and isinstance(snapshot.get("session", {}).get("gui_display_frame_rate"), dict)
        else {}
    )
    return {
        "recording_folder": str(recording_folder),
        "recording_id": snapshot.get("recording_id"),
        "timestamp_utc": snapshot.get("timestamp_utc"),
        "sync": snapshot.get("sync") if isinstance(snapshot.get("sync"), dict) else {},
        "gui_display_frame_rate": gui_display_frame_rate,
        "gui_display_diagnosis": summarize_gui_timing_diagnosis(gui_display_frame_rate),
        "models": summarize_models(snapshot),
        "ptp": summarize_ptp(recording_folder),
        "yolo": summarize_yolo(recording_folder, steady_after_frame),
        "pipeline": summarize_pipeline(recording_folder),
        "videos": summarize_videos(recording_folder, ffprobe),
        "outputs": outputs,
        "crop": summarize_crop_recording(recording_folder, outputs, manifest),
        "pose_events": summarize_pose_events(recording_folder),
        "spatial_calibrations": summarize_spatial_calibrations(snapshot),
    }


def fmt_ms(value: Any) -> str:
    return "n/a" if value is None else f"{float(value):.3f}"


def fmt_ms_unit(value: Any) -> str:
    return "n/a" if value is None else f"{float(value):.3f}ms"


def fmt_s_unit(value: Any) -> str:
    return "n/a" if value is None else f"{float(value):.3f}s"


def fmt_int(value: Any) -> str:
    return "n/a" if value is None else str(value)


def fmt_float_unit(value: Any, digits: int, unit: str) -> str:
    return "n/a" if value is None else f"{float(value):.{digits}f}{unit}"


def fmt_percent(value: Any, digits: int = 0) -> str:
    return "n/a" if value is None else f"{float(value) * 100.0:.{digits}f}%"


def print_human(summary: dict[str, Any]) -> None:
    print(f"Recording: {summary['recording_folder']}")
    sync = summary.get("sync", {})
    print(f"Sync: mode={sync.get('mode', 'unknown')} camera_sync_enabled={sync.get('camera_sync_enabled', 'unknown')}")

    print("\nDetect Engines")
    if summary["models"]:
        for serial, model in sorted(summary["models"].items()):
            print(
                f"  Cam{serial}: enabled={model.get('enabled')} gpu={model.get('gpu_id')} "
                f"engine={model.get('engine_name') or 'unknown'}"
            )
    else:
        print("  none found")

    print("\nPTP Register Reads")
    cameras = summary["ptp"].get("cameras", {})
    if cameras:
        for serial, camera in sorted(cameras.items()):
            print(
                f"  Cam{serial}: decimate={fmt_int(camera.get('ptp_register_read_decimate'))} "
                f"reads={fmt_int(camera.get('ptp_register_reads') or camera.get('ptp_register_reads_from_cadence'))} "
                f"gaps={fmt_int(camera.get('camera_frame_id_gaps'))} "
                f"get_frame_errors={fmt_int(camera.get('get_frame_errors'))}"
            )
    else:
        print("  no PTP register-read counters found")

    print("\nYOLO Latency")
    if summary["yolo"]:
        for serial, yolo in sorted(summary["yolo"].items()):
            metrics = yolo["metrics"]
            primary = metrics.get("acquisition_to_detect_done_ms") or metrics.get("capture_to_detect_done_ms") or {}
            queue = metrics.get("yolo_queue_wait_ms") or {}
            cpu_pre_sync = metrics.get("cpu_pre_sync_ms") or {}
            ptp = metrics.get("acquisition_to_ptp_done_ms") or {}
            print(
                f"  Cam{serial}: rows={yolo['rows']} "
                f"detect_p95={fmt_ms_unit(primary.get('p95'))} "
                f"steady_p95={fmt_ms_unit(primary.get('steady_p95'))} "
                f"queue_p95={fmt_ms_unit(queue.get('p95'))} "
                f"cpu_pre_sync_p95={fmt_ms_unit(cpu_pre_sync.get('p95'))} "
                f"ptp_done_p95={fmt_ms_unit(ptp.get('p95'))}"
            )
    else:
        print("  no Cam*_yolo_perf.csv files found")

    print("\nPipeline Health")
    if summary["pipeline"]:
        for serial, pipeline in sorted(summary["pipeline"].items()):
            final = pipeline.get("final", {})
            print(
                f"  Cam{serial}: dropped={fmt_int(final.get('camera_dropped_frames'))} "
                f"get_frame_errors={fmt_int(final.get('get_frame_errors'))} "
                f"enc_fail={fmt_int(final.get('enc_fail'))} "
                f"enc_slow={fmt_int(final.get('enc_slow'))}"
            )
    else:
        print("  no Cam*_pipeline_perf.csv files found")

    gui_fps = summary.get("gui_display_frame_rate")
    gui_fps = gui_fps if isinstance(gui_fps, dict) else {}
    if gui_fps:
        timings = gui_fps.get("timings")
        timings = timings if isinstance(timings, dict) else {}

        def fps_bucket_text(name: str) -> str:
            bucket = gui_fps.get(name)
            bucket = bucket if isinstance(bucket, dict) else {}
            return (
                f"samples={fmt_int(bucket.get('sample_count'))} "
                f"p05={fmt_float_unit(bucket.get('p05_fps'), 1, '')} "
                f"p50={fmt_float_unit(bucket.get('p50_fps'), 1, '')} "
                f"mean={fmt_float_unit(bucket.get('mean_fps'), 1, '')}"
            )

        def timing_text(name: str) -> str:
            bucket = timings.get(name)
            bucket = bucket if isinstance(bucket, dict) else {}
            return (
                f"p50={fmt_float_unit(bucket.get('p50_ms'), 2, 'ms')} "
                f"p95={fmt_float_unit(bucket.get('p95_ms'), 2, 'ms')} "
                f"samples={fmt_int(bucket.get('sample_count'))}"
            )

        print("\nGUI Display")
        print(
            f"  stream_downsample={fmt_int(gui_fps.get('stream_downsample'))} "
            f"display_preview_max_fps={fmt_int(gui_fps.get('display_preview_max_fps'))} "
            f"speed_graphs={gui_fps.get('yolo_speed_graphs_enabled', 'n/a')}"
        )
        print(f"  fps overall: {fps_bucket_text('overall')}")
        print(f"  fps crop-preview-visible: {fps_bucket_text('crop_preview_visible')}")
        print(f"  fps crop-preview-hidden: {fps_bucket_text('crop_preview_hidden')}")
        if timings:
            diagnosis = summary.get("gui_display_diagnosis")
            diagnosis = diagnosis if isinstance(diagnosis, dict) else {}
            print("  timings:")
            print(f"    frame-total: {timing_text('frame_total_ms')}")
            print(f"    main-texture-upload: {timing_text('main_texture_upload_ms')}")
            print(f"    crop-texture-upload: {timing_text('crop_texture_upload_ms')}")
            print(f"    camera-window-draw: {timing_text('camera_window_draw_ms')}")
            print(f"    crop-window-draw: {timing_text('crop_window_draw_ms')}")
            print(f"    speed-graph-draw: {timing_text('speed_graph_draw_ms')}")
            print(f"    render-present: {timing_text('render_present_ms')}")
            if diagnosis:
                print(
                    "    dominant-p95: "
                    f"{diagnosis.get('dominant_timing_label', 'n/a')}="
                    f"{fmt_float_unit(diagnosis.get('dominant_timing_p95_ms'), 2, 'ms')} "
                    f"frame-total={fmt_float_unit(diagnosis.get('frame_total_p95_ms'), 2, 'ms')} "
                    f"share={fmt_percent(diagnosis.get('dominant_timing_fraction_of_frame_total_p95'))}"
                )
            print(
                f"    upload-counts: main={fmt_int(timings.get('main_texture_upload_count'))} "
                f"crop={fmt_int(timings.get('crop_texture_upload_count'))}"
            )

    print("\nRecording Outputs")
    if summary["outputs"]:
        for serial, outputs in sorted(summary["outputs"].items()):
            parts = []
            for output_kind, output in sorted(outputs.items()):
                parts.append(
                    f"{output_kind}:role={output.get('role', 'unknown')} "
                    f"backend={output.get('backend', 'unknown')} "
                    f"status={output.get('status', 'unknown')} "
                    f"frames={fmt_int(output.get('frame_count'))}"
                )
            print(f"  Cam{serial}: {'; '.join(parts)}")
    else:
        print("  no recording outputs found")

    print("\nCrop Recording")
    if summary.get("crop"):
        def fanout_text(crop: dict[str, Any], prefix: str) -> str:
            accepted = crop.get(f"producer_{prefix}_crop_frame_accepted")
            offered = crop.get(f"producer_{prefix}_crop_frame_offered")
            dropped = crop.get(f"producer_{prefix}_crop_frame_dropped")
            if accepted is None and offered is None and dropped is None:
                return "n/a"
            return f"{fmt_int(accepted)}/{fmt_int(offered)} dropped={fmt_int(dropped)}"

        for serial, crop in sorted(summary["crop"].items()):
            preview_updated = crop.get("preview_frames_updated")
            preview_offered = crop.get("preview_frames_offered")
            print(
                f"  Cam{serial}: backend={crop.get('backend') or 'unknown'} "
                f"status={crop.get('status') or 'unknown'} "
                f"frames={fmt_int(crop.get('frame_count'))} "
                f"meta_rows={fmt_int(crop.get('metadata_rows'))} "
                f"detection_rows={fmt_int(crop.get('metadata_detection_rows'))} "
                f"perf_rows={fmt_int(crop.get('perf_rows'))} "
                f"perf_dropped={fmt_int(crop.get('perf_dropped_rows'))}"
            )
            print(
                f"    preview: max_fps={fmt_int(crop.get('preview_max_fps'))} "
                f"display_enabled={fmt_int(crop.get('preview_display_enabled_final'))} "
                f"disabled={fmt_int(crop.get('preview_disabled'))} "
                f"updated/offered={fmt_int(preview_updated)}/{fmt_int(preview_offered)} "
                f"skipped={fmt_int(crop.get('preview_frames_skipped_by_cadence'))} "
                f"queue_drops={fmt_int(crop.get('preview_queue_full_drops'))} "
                f"pool={fmt_int(crop.get('crop_frame_pool_size'))}"
            )
            print(
                f"    fanout: recording={fanout_text(crop, 'recording')} "
                f"preview={fanout_text(crop, 'preview')} "
                f"pose={fanout_text(crop, 'pose')}"
            )
            stream_config = crop.get("external_stream_config")
            stream_config = stream_config if isinstance(stream_config, dict) else {}
            if crop.get("external_frames_received") is not None:
                print(
                    f"    external: received={fmt_int(crop.get('external_frames_received'))} "
                    f"encoded={fmt_int(crop.get('external_frames_encoded'))} "
                    f"dropped={fmt_int(crop.get('external_frames_dropped'))} "
                    f"encode_dropped={fmt_int(crop.get('external_encode_dropped'))} "
                    f"queue_depth={fmt_int(crop.get('external_encode_queue_depth'))} "
                    f"queue_high_water={fmt_int(crop.get('external_encode_queue_high_water'))} "
                    f"enqueue_age_p95={fmt_float_unit(crop.get('external_enqueue_age_p95_ms'), 2, 'ms')} "
                    f"encode_total_p95={fmt_float_unit(crop.get('external_encode_total_p95_ms'), 2, 'ms')} "
                    f"lock_p95={fmt_float_unit(crop.get('external_lock_bitstream_p95_ms'), 2, 'ms')}"
                )
            if stream_config:
                print(
                    f"    external_config: stream={stream_config.get('stream_id') or 'n/a'} "
                    f"analytics_gpu={fmt_int(stream_config.get('analytics_gpu_id'))} "
                    f"recorder_gpu={fmt_int(stream_config.get('recorder_gpu_id'))} "
                    f"socket={stream_config.get('socket_path') or 'n/a'}"
                )
    else:
        print("  no crop recording artifacts found")

    print("\nMain Videos")
    if summary["videos"]:
        for serial, video in sorted(summary["videos"].items()):
            bitrate = video.get("bitrate_bps")
            bitrate_mbps = None if bitrate is None else float(bitrate) / 1_000_000.0
            bitrate_text = "n/a" if bitrate_mbps is None else f"{bitrate_mbps:.1f} Mbps"
            print(
                f"  Cam{serial}: status={video.get('status')} "
                f"{video.get('width', 'n/a')}x{video.get('height', 'n/a')} "
                f"frames={fmt_int(video.get('frames'))} "
                f"duration={fmt_s_unit(video.get('duration_s'))} "
                f"bitrate={bitrate_text}"
            )
    else:
        print("  no main Cam<serial>.mp4 files found")

    print("\nPose Events")
    if summary["pose_events"]:
        for serial, pose_events in sorted(summary["pose_events"].items()):
            statuses = ",".join(
                f"{status}:{count}" for status, count in sorted(pose_events.get("statuses", {}).items())
            )
            print(
                f"  Cam{serial}: rows={pose_events.get('rows', 0)} "
                f"statuses={statuses or 'none'} "
                f"last_recording_frame_id={fmt_int(pose_events.get('last_recording_frame_id'))} "
                f"parse_errors={fmt_int(pose_events.get('parse_errors'))}"
            )
    else:
        print("  none recorded")

    print("\nSpatial Calibrations")
    if summary["spatial_calibrations"]:
        for serial, calibration in sorted(summary["spatial_calibrations"].items()):
            arena = calibration.get("arena_layout", {})
            dish = calibration.get("dish_mask", {})
            print(
                f"  Cam{serial}: arena_layout={arena.get('layout_id') or 'missing'} "
                f"zones={fmt_int(arena.get('zone_count'))} "
                f"dish_mask={dish.get('artifact_id') or 'missing'}"
            )
    else:
        print("  none recorded")


def main() -> int:
    args = parse_args()
    summary = summarize(resolve_requested_recording_folder(args), args.steady_after_frame, args.ffprobe)
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print_human(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
