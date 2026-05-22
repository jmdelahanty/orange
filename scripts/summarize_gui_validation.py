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


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Summarize the fields used for two-camera GUI validation: selected "
            "YOLO engine, PTP register-read decimation, YOLO latency, pipeline "
            "health counters, spatial calibrations, and main MP4 sanity."
        )
    )
    parser.add_argument("recording_folder", help="Recording folder or parent containing one run folder")
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
    return {
        "recording_folder": str(recording_folder),
        "recording_id": snapshot.get("recording_id"),
        "timestamp_utc": snapshot.get("timestamp_utc"),
        "sync": snapshot.get("sync") if isinstance(snapshot.get("sync"), dict) else {},
        "models": summarize_models(snapshot),
        "ptp": summarize_ptp(recording_folder),
        "yolo": summarize_yolo(recording_folder, steady_after_frame),
        "pipeline": summarize_pipeline(recording_folder),
        "videos": summarize_videos(recording_folder, ffprobe),
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
    summary = summarize(Path(args.recording_folder), args.steady_after_frame, args.ffprobe)
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print_human(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
