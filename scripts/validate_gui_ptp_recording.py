#!/usr/bin/env python3
"""Validate a production-like GUI PTP recording folder."""

from __future__ import annotations

import argparse
import csv
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import summarize_gui_validation as gui_summary
from recording_output_validation import recording_output_contract_errors


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")
DEFAULT_FFMPEG = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg")
DEFAULT_GUI_RECORDING_ROOT = Path("/home/jeremy/orange_data/exp/unsorted")


class Reporter:
    def __init__(self, *, verbose: bool = True) -> None:
        self.verbose = verbose
        self.failures: list[str] = []
        self.warnings: list[str] = []
        self.passes: list[str] = []

    def pass_(self, message: str) -> None:
        self.passes.append(message)
        if self.verbose:
            print(f"[PASS] {message}")

    def warn(self, message: str) -> None:
        self.warnings.append(message)
        if self.verbose:
            print(f"[WARN] {message}")

    def fail(self, message: str) -> None:
        self.failures.append(message)
        if self.verbose:
            print(f"[FAIL] {message}")

    def check(self, condition: bool, pass_message: str, fail_message: str) -> None:
        if condition:
            self.pass_(pass_message)
        else:
            self.fail(fail_message)


def default_tool(path: Path, fallback: str) -> str:
    return str(path) if path.exists() else shutil.which(fallback) or fallback


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate a GUI recording folder from scripts/run_gui_aq_off_validation.sh: "
            "per-camera PTP config, PTP register-read decimation, pipeline health, "
            "main video sanity, and YOLO timing health."
        )
    )
    parser.add_argument(
        "recording_folder",
        nargs="?",
        help="GUI recording folder, or parent containing one recording folder.",
    )
    parser.add_argument(
        "--latest",
        nargs="?",
        const=str(DEFAULT_GUI_RECORDING_ROOT),
        metavar="ROOT",
        help=(
            "Validate the newest direct child of ROOT containing recording_snapshot.json. "
            f"With no ROOT, uses {DEFAULT_GUI_RECORDING_ROOT}."
        ),
    )
    parser.add_argument(
        "--latest-complete",
        nargs="?",
        const=str(DEFAULT_GUI_RECORDING_ROOT),
        metavar="ROOT",
        help=(
            "Validate the newest direct child of ROOT that looks like a real "
            "recording: recording_snapshot.json plus matching main MP4, "
            "pipeline perf CSV, and YOLO perf CSV for at least one camera. "
            f"With no ROOT, uses {DEFAULT_GUI_RECORDING_ROOT}."
        ),
    )
    parser.add_argument(
        "--expected-cameras",
        default="",
        help="Comma-separated camera serials to require. Defaults to cameras discovered in the artifact.",
    )
    parser.add_argument("--expected-sync-mode", default="ptp_gate")
    parser.add_argument("--expected-ptp-mode", default="TwoStep")
    parser.add_argument(
        "--expect-ptp-register-read-decimate",
        type=int,
        default=100,
        help="Expected ORANGE_PTP_REGISTER_READ_DECIMATE value. Default: 100.",
    )
    parser.add_argument(
        "--skip-ptp-register-decimate-check",
        action="store_true",
        help="Allow old artifacts that do not contain PTP register-read decimation counters.",
    )
    parser.add_argument(
        "--steady-after-frame",
        type=int,
        default=50,
        help="Frame id threshold for steady-state YOLO metrics. Default: 50.",
    )
    parser.add_argument(
        "--max-yolo-queue-p95-ms",
        type=float,
        default=1.0,
        help="Fail if YOLO queue wait p95 exceeds this value. Default: 1 ms.",
    )
    parser.add_argument(
        "--max-yolo-steady-p95-ms",
        type=float,
        default=None,
        help="Optional fail threshold for steady-state acquisition/detect p95.",
    )
    parser.add_argument(
        "--max-ptp-done-p95-ms",
        type=float,
        default=None,
        help="Optional fail threshold for acquisition_to_ptp_done_ms p95 when present.",
    )
    parser.add_argument(
        "--min-main-video-bitrate-mbps",
        type=float,
        default=50.0,
        help="Fail if a main camera MP4 bitrate is below this value. Default: 50 Mbps.",
    )
    parser.add_argument(
        "--max-video-black-fraction",
        type=float,
        default=0.98,
        help="Fail decoded video sanity if sampled frames exceed this black-pixel fraction.",
    )
    parser.add_argument(
        "--min-video-stddev",
        type=float,
        default=5.0,
        help="Fail decoded video sanity if sampled frames are flatter than this stddev.",
    )
    parser.add_argument(
        "--min-gui-recording-fps-p05",
        type=float,
        default=None,
        help="Optional minimum recording-window GUI FPS p05 from recording_snapshot session telemetry.",
    )
    parser.add_argument(
        "--min-gui-crop-preview-visible-fps-p05",
        type=float,
        default=None,
        help="Optional minimum GUI FPS p05 while crop preview windows were visible.",
    )
    parser.add_argument(
        "--min-gui-crop-preview-hidden-fps-p05",
        type=float,
        default=None,
        help="Optional minimum GUI FPS p05 while crop preview windows were hidden.",
    )
    parser.add_argument(
        "--expect-gui-stream-downsample",
        type=int,
        default=None,
        help="Optional expected recording_snapshot session.gui_display_frame_rate.stream_downsample value.",
    )
    parser.add_argument(
        "--expect-display-preview-max-fps",
        type=int,
        default=None,
        help="Optional expected main-camera display preview FPS cap from snapshot and pipeline perf.",
    )
    parser.add_argument(
        "--expect-yolo-speed-graphs-enabled",
        type=int,
        choices=[0, 1],
        default=None,
        help=(
            "Optional expected recording_snapshot "
            "session.gui_display_frame_rate.yolo_speed_graphs_enabled value."
        ),
    )
    parser.add_argument(
        "--require-gui-timing-telemetry",
        action="store_true",
        help=(
            "Require recording_snapshot session.gui_display_frame_rate.timings "
            "phase timing buckets to be present and sampled."
        ),
    )
    parser.add_argument(
        "--expect-crop-preview-max-fps",
        type=int,
        default=None,
        help="Optional expected Cam*_crop_sidecar_perf.csv preview_max_fps value.",
    )
    parser.add_argument(
        "--expect-crop-preview-display-enabled",
        type=int,
        choices=(0, 1),
        default=None,
        help=(
            "Optional expected Cam*_crop_sidecar_perf.csv "
            "preview_display_enabled_final value for crop-enabled cameras."
        ),
    )
    parser.add_argument(
        "--expect-crop-preview-disabled",
        type=int,
        choices=(0, 1),
        default=None,
        help=(
            "Optional expected Cam*_crop_sidecar_perf.csv preview_disabled "
            "value for crop-enabled cameras."
        ),
    )
    parser.add_argument(
        "--min-crop-frame-pool-size",
        type=int,
        default=None,
        help="Optional minimum Cam*_crop_sidecar_perf.csv crop_frame_pool_size value.",
    )
    parser.add_argument(
        "--expect-external-crop-encode-queue-depth",
        type=int,
        default=None,
        help=(
            "Optional expected external crop recorder summary encode_queue_depth "
            "for crop outputs using backend=external_ipc."
        ),
    )
    parser.add_argument(
        "--expect-external-crop-recorder-gpu-id",
        type=int,
        default=None,
        help=(
            "Optional expected recorder_gpu_id for every external crop stream. "
            "Per-camera --expect-external-crop-recorder-gpu values override this."
        ),
    )
    parser.add_argument(
        "--expect-external-crop-recorder-gpu",
        action="append",
        default=[],
        metavar="SERIAL=GPU",
        help=(
            "Optional per-camera expected recorder_gpu_id for an external crop stream. "
            "May be provided more than once."
        ),
    )
    parser.add_argument(
        "--max-external-crop-encode-queue-high-water",
        type=int,
        default=None,
        help=(
            "Optional maximum external crop recorder summary encode_queue_high_water "
            "for crop outputs using backend=external_ipc."
        ),
    )
    parser.add_argument(
        "--max-external-crop-enqueue-age-p95-ms",
        type=float,
        default=None,
        help=(
            "Optional maximum external crop recorder summary "
            "external_encode.enqueue_age_p95_ms for crop outputs using backend=external_ipc."
        ),
    )
    parser.add_argument(
        "--require-external-crop-backend-metadata",
        action="store_true",
        help=(
            "For crop outputs using backend=external_ipc, require "
            "recording_session.json recording_backend.crop_recording mode, "
            "stream_config, and per-camera telemetry maps."
        ),
    )
    parser.add_argument(
        "--require-crop-preview-counters",
        action="store_true",
        help="Fail if crop sidecar preview counters are missing for crop-enabled cameras.",
    )
    parser.add_argument(
        "--require-crop-preview-sampling",
        action="store_true",
        help=(
            "Fail unless crop preview counters prove a visible bounded preview "
            "run skipped at least one offered frame by cadence."
        ),
    )
    parser.add_argument(
        "--require-crop-recording-artifacts",
        action="store_true",
        help=(
            "Fail unless crop-enabled cameras have aligned crop MP4, metadata, "
            "keyframe, and perf artifacts with zero crop drops."
        ),
    )
    parser.add_argument("--skip-video-content-check", action="store_true")
    parser.add_argument("--ffprobe", default=default_tool(DEFAULT_FFPROBE, "ffprobe"))
    parser.add_argument("--ffmpeg", default=default_tool(DEFAULT_FFMPEG, "ffmpeg"))
    parser.add_argument("--json", action="store_true", help="Print only machine-readable JSON.")
    parser.add_argument("--json-out", type=Path, help="Optional path to write the validation JSON summary.")
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return {}
    return payload if isinstance(payload, dict) else {}


def json_file_parses_as_object(path: Path) -> bool:
    try:
        with path.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return False
    return isinstance(payload, dict)


def camera_serials_with_complete_artifacts(recording_folder: Path) -> set[str]:
    videos = {
        serial
        for path in recording_folder.glob("Cam*.mp4")
        if path.stat().st_size > 0
        for serial in [gui_summary.camera_serial_from_video(path)]
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
        for serial in [gui_summary.camera_serial_from_pipeline_perf(path)]
        if serial is not None
    }
    yolo = {
        serial
        for path in recording_folder.glob("Cam*_yolo_perf.csv")
        if path.stat().st_size > 0
        for serial in [gui_summary.camera_serial_from_yolo_perf(path)]
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
    return gui_summary.resolve_recording_folder(Path(args.recording_folder))


def parse_expected_cameras(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def parse_expected_serial_int_map(values: list[str], option_name: str) -> dict[str, int]:
    parsed: dict[str, int] = {}
    for raw in values:
        if "=" not in raw:
            raise SystemExit(f"{option_name} must use SERIAL=VALUE, got {raw!r}")
        serial, value_text = raw.split("=", 1)
        serial = serial.strip()
        value_text = value_text.strip()
        if not serial:
            raise SystemExit(f"{option_name} has an empty serial in {raw!r}")
        try:
            value = int(value_text)
        except ValueError as exc:
            raise SystemExit(f"{option_name} value must be an integer, got {raw!r}") from exc
        if value < 0:
            raise SystemExit(f"{option_name} value must be >= 0, got {raw!r}")
        parsed[serial] = value
    return parsed


def artifact_cameras(summary: dict[str, Any], snapshot: dict[str, Any], expected: list[str]) -> list[str]:
    if expected:
        return expected
    cameras: set[str] = set()
    for section in ("videos", "pipeline", "yolo"):
        value = summary.get(section)
        if isinstance(value, dict):
            cameras.update(str(serial) for serial in value)
    camera_runtime = snapshot.get("camera_runtime")
    if isinstance(camera_runtime, dict):
        cameras.update(str(serial) for serial in camera_runtime)
    return sorted(cameras)


def nested_dict(value: Any, *keys: str) -> dict[str, Any]:
    current = value
    for key in keys:
        if not isinstance(current, dict):
            return {}
        current = current.get(key)
    return current if isinstance(current, dict) else {}


def path_from_recording_folder(recording_folder: Path, value: Any) -> Path:
    path = Path(str(value or ""))
    return path if path.is_absolute() else recording_folder / path


def count_csv_data_rows(path: Path) -> int | None:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return max(0, sum(1 for _ in handle) - 1)
    except OSError:
        return None


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
        for value in [int_csv_field(row, "encode_queue_depth")]
        if value is not None
    ]
    return max(values) if values else None


def check_optional_backend_int_map(
    reporter: Reporter,
    backend: dict[str, Any],
    serial: str,
    map_name: str,
    expected: int | None,
    label: str,
) -> None:
    if expected is None:
        return
    values = backend.get(map_name)
    if not isinstance(values, dict) or serial not in values:
        return
    actual = integer(values.get(serial))
    reporter.check(
        actual == expected,
        f"Cam{serial} recording_backend.crop_recording.{map_name} matches {label} ({actual})",
        (
            f"Cam{serial} recording_backend.crop_recording.{map_name} "
            f"({actual}) != {label} ({expected})"
        ),
    )


def check_optional_backend_float_map(
    reporter: Reporter,
    backend: dict[str, Any],
    serial: str,
    map_name: str,
    expected: float | None,
    label: str,
) -> None:
    if expected is None:
        return
    values = backend.get(map_name)
    if not isinstance(values, dict) or serial not in values:
        return
    actual = number(values.get(serial))
    reporter.check(
        actual is not None and math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-6),
        f"Cam{serial} recording_backend.crop_recording.{map_name} matches {label} ({actual})",
        (
            f"Cam{serial} recording_backend.crop_recording.{map_name} "
            f"({actual}) != {label} ({expected})"
        ),
    )


def require_backend_map_key(
    reporter: Reporter,
    backend: dict[str, Any],
    serial: str,
    map_name: str,
) -> bool:
    values = backend.get(map_name)
    ok = isinstance(values, dict) and serial in values
    reporter.check(
        ok,
        f"Cam{serial} recording_backend.crop_recording.{map_name} present",
        f"Cam{serial} recording_backend.crop_recording.{map_name} missing serial",
    )
    return ok


def require_stream_config_string(
    reporter: Reporter,
    serial: str,
    stream_config: dict[str, Any],
    key: str,
) -> str | None:
    value = stream_config.get(key)
    ok = isinstance(value, str) and bool(value)
    reporter.check(
        ok,
        f"Cam{serial} recording_backend.crop_recording.stream_config.{key} present",
        f"Cam{serial} recording_backend.crop_recording.stream_config.{key} missing",
    )
    return value if ok else None


def require_stream_config_int(
    reporter: Reporter,
    serial: str,
    stream_config: dict[str, Any],
    key: str,
    *,
    minimum: int = 0,
) -> int | None:
    value = integer(stream_config.get(key))
    ok = value is not None and value >= minimum
    reporter.check(
        ok,
        f"Cam{serial} recording_backend.crop_recording.stream_config.{key}={value}",
        (
            f"Cam{serial} recording_backend.crop_recording.stream_config.{key} "
            f"missing or below {minimum}: {stream_config.get(key)}"
        ),
    )
    return value if ok else None


def check_descriptor_detail_matches_string(
    reporter: Reporter,
    serial: str,
    details: dict[str, Any],
    key: str,
    expected: str | None,
) -> None:
    if expected is None:
        return
    actual = details.get(key)
    reporter.check(
        actual == expected,
        f"Cam{serial} recording_outputs.crop.details.{key} matches ({actual})",
        (
            f"Cam{serial} recording_outputs.crop.details.{key} "
            f"({actual}) != expected ({expected})"
        ),
    )


def check_descriptor_detail_matches_int(
    reporter: Reporter,
    serial: str,
    details: dict[str, Any],
    key: str,
    expected: int | None,
) -> None:
    if expected is None:
        return
    actual = integer(details.get(key))
    reporter.check(
        actual == expected,
        f"Cam{serial} recording_outputs.crop.details.{key} matches ({actual})",
        (
            f"Cam{serial} recording_outputs.crop.details.{key} "
            f"({actual}) != expected ({expected})"
        ),
    )


def check_recording_session_manifest(
    reporter: Reporter,
    recording_folder: Path,
    snapshot: dict[str, Any],
    cameras: list[str],
) -> None:
    manifest_path = recording_folder / "recording_session.json"
    manifest = read_json(manifest_path)
    reporter.check(
        manifest.get("schema_id") == "orange.recording_session",
        "recording_session.json present",
        f"recording_session.json missing or invalid at {manifest_path}",
    )
    if not manifest:
        return

    producer = str(manifest.get("producer", ""))
    backend = manifest.get("recording_backend")
    backend = backend if isinstance(backend, dict) else {}
    external_ipc = producer == "orange_gui_external_ipc" or backend.get("mode") == "external_ipc"
    reporter.check(
        producer in {"orange_gui", "orange_gui_external_ipc"},
        f"recording_session producer is {producer}",
        f"recording_session producer={manifest.get('producer')!r}",
    )
    reporter.check(
        manifest.get("mode") == "single_clip",
        "recording_session mode is single_clip",
        f"recording_session mode={manifest.get('mode')!r}",
    )

    snapshot_session = snapshot.get("session")
    snapshot_session = snapshot_session if isinstance(snapshot_session, dict) else {}
    reporter.check(
        Path(str(snapshot_session.get("recording_session_manifest_path", ""))).resolve()
        == manifest_path.resolve(),
        "recording_snapshot points at recording_session.json",
        "recording_snapshot session recording_session_manifest_path mismatch",
    )
    reporter.check(
        snapshot_session.get("recording_mode") == "single_clip",
        "recording_snapshot session mode is single_clip",
        f"recording_snapshot recording_mode={snapshot_session.get('recording_mode')!r}",
    )

    camera_artifacts = manifest.get("camera_artifacts")
    camera_artifacts = camera_artifacts if isinstance(camera_artifacts, dict) else {}
    output_errors = recording_output_contract_errors(
        recording_folder,
        manifest,
        snapshot,
        cameras,
    )
    if output_errors:
        for error in output_errors:
            reporter.fail(error)
    else:
        reporter.pass_("schema-v2 recording_outputs contract valid")

    for serial in cameras:
        artifact = camera_artifacts.get(serial)
        artifact = artifact if isinstance(artifact, dict) else {}
        if not artifact:
            reporter.fail(f"Cam{serial} missing recording_session camera_artifacts")
            continue

        metadata_path = path_from_recording_folder(recording_folder, artifact.get("metadata"))
        video_path = path_from_recording_folder(recording_folder, artifact.get("video"))
        frame_count = integer(artifact.get("frame_count"))
        packet_count = integer(artifact.get("packet_count"))
        packet_source = str(artifact.get("packet_count_source", ""))
        metadata_rows = count_csv_data_rows(metadata_path)

        reporter.check(
            metadata_path.exists(),
            f"Cam{serial} recording_session metadata present",
            f"Cam{serial} recording_session metadata missing: {metadata_path}",
        )
        reporter.check(
            video_path.exists() and video_path.stat().st_size > 0,
            f"Cam{serial} recording_session video present",
            f"Cam{serial} recording_session video missing: {video_path}",
        )
        if external_ipc:
            summary = read_json(metadata_path)
            frames_received = integer(summary.get("frames_received"))
            acks_sent = integer(summary.get("acks_sent"))
            frames_encoded = integer(summary.get("frames_encoded"))
            merged_output = summary.get("merged_output")
            merged_output = merged_output if isinstance(merged_output, dict) else {}
            packets_written = integer(merged_output.get("packets_written"))
            reporter.check(
                frame_count is not None and frames_received == frame_count,
                f"Cam{serial} recording_session frame_count matches external frames_received",
                (
                    f"Cam{serial} recording_session frame_count={frame_count}, "
                    f"external frames_received={frames_received}"
                ),
            )
            reporter.check(
                frame_count is not None and acks_sent == frame_count and frames_encoded == frame_count,
                f"Cam{serial} external ACK/encoded counts match frame_count",
                (
                    f"Cam{serial} external counts frame_count={frame_count}, "
                    f"acks_sent={acks_sent}, frames_encoded={frames_encoded}"
                ),
            )
            if packets_written is not None and packet_count is not None:
                reporter.check(
                    packets_written == packet_count,
                    f"Cam{serial} recording_session packet_count matches external packets",
                    (
                        f"Cam{serial} recording_session packet_count={packet_count}, "
                        f"external packets_written={packets_written}"
                    ),
                )
        else:
            reporter.check(
                frame_count is not None and metadata_rows == frame_count,
                f"Cam{serial} recording_session frame_count matches metadata",
                f"Cam{serial} recording_session frame_count={frame_count}, metadata_rows={metadata_rows}",
            )
        reporter.check(
            packet_count is not None and packet_count > 0,
            f"Cam{serial} recording_session packet_count present",
            f"Cam{serial} recording_session packet_count={packet_count}",
        )
        reporter.check(
            packet_source not in {"", "not_collected", "unavailable"},
            f"Cam{serial} recording_session packet_count_source={packet_source}",
            f"Cam{serial} recording_session packet_count_source={packet_source!r}",
        )


def metric(summary: dict[str, Any], serial: str, field: str) -> dict[str, Any]:
    yolo = nested_dict(summary, "yolo", serial)
    metrics = yolo.get("metrics")
    if not isinstance(metrics, dict):
        return {}
    value = metrics.get(field)
    return value if isinstance(value, dict) else {}


def number(value: Any) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def integer(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def int_csv_field(row: dict[str, str], field: str) -> int | None:
    value = row.get(field)
    try:
        return int(value) if value is not None and value != "" else None
    except ValueError:
        return None


def crop_enabled_cameras(snapshot: dict[str, Any], cameras: list[str]) -> list[str]:
    crop_outputs = snapshot.get("crop_outputs")
    crop_outputs = crop_outputs if isinstance(crop_outputs, dict) else {}
    return [
        serial
        for serial in cameras
        if isinstance(crop_outputs.get(serial), dict)
        and crop_outputs[serial].get("enabled") is True
    ]


def crop_output_for(snapshot: dict[str, Any], serial: str) -> dict[str, Any]:
    crop_outputs = snapshot.get("crop_outputs")
    crop_outputs = crop_outputs if isinstance(crop_outputs, dict) else {}
    crop_output = crop_outputs.get(serial)
    return crop_output if isinstance(crop_output, dict) else {}


def crop_runtime_for(crop_output: dict[str, Any]) -> dict[str, Any]:
    runtime = crop_output.get("runtime")
    return runtime if isinstance(runtime, dict) else {}


def crop_size_from_snapshot(snapshot: dict[str, Any], serial: str) -> int | None:
    crop_output = crop_output_for(snapshot, serial)
    runtime = crop_runtime_for(crop_output)
    for field in ("crop_size_px", "width", "height"):
        value = integer(runtime.get(field))
        if value is not None and value > 0:
            return value

    camera_runtime = nested_dict(snapshot, "camera_runtime", serial, "runtime")
    crop_pipeline = camera_runtime.get("crop_pipeline")
    crop_pipeline = crop_pipeline if isinstance(crop_pipeline, dict) else {}
    value = integer(crop_pipeline.get("crop_size_px"))
    if value is not None and value > 0:
        return value

    camera_config = nested_dict(snapshot, "cameras", serial)
    crop_pipeline = camera_config.get("crop_pipeline")
    crop_pipeline = crop_pipeline if isinstance(crop_pipeline, dict) else {}
    value = integer(crop_pipeline.get("crop_size_px"))
    return value if value is not None and value > 0 else None


def crop_artifact_path(
    recording_folder: Path,
    crop_output: dict[str, Any],
    key: str,
    default_name: str,
) -> Path:
    runtime = crop_runtime_for(crop_output)
    files = runtime.get("files")
    files = files if isinstance(files, dict) else {}
    return path_from_recording_folder(recording_folder, files.get(key) or default_name)


def crop_recording_output_descriptor(snapshot: dict[str, Any], serial: str) -> dict[str, Any]:
    recording_outputs = snapshot.get("recording_outputs")
    recording_outputs = recording_outputs if isinstance(recording_outputs, dict) else {}
    camera_outputs = recording_outputs.get(serial)
    camera_outputs = camera_outputs if isinstance(camera_outputs, dict) else {}
    crop_output = camera_outputs.get("crop")
    return crop_output if isinstance(crop_output, dict) else {}


def crop_descriptor_artifact_path(
    recording_folder: Path,
    crop_descriptor: dict[str, Any],
    key: str,
) -> Path | None:
    value = crop_descriptor.get(key)
    if not isinstance(value, str) or not value:
        return None
    return path_from_recording_folder(recording_folder, value)


def resolve_crop_artifact_path(
    recording_folder: Path,
    crop_output: dict[str, Any],
    crop_descriptor: dict[str, Any],
    key: str,
    default_name: str,
) -> Path:
    descriptor_path = crop_descriptor_artifact_path(recording_folder, crop_descriptor, key)
    if descriptor_path is not None:
        return descriptor_path
    return crop_artifact_path(recording_folder, crop_output, key, default_name)


def crop_metadata_row_count(recording_folder: Path, snapshot: dict[str, Any], serial: str) -> int | None:
    crop_output = crop_output_for(snapshot, serial)
    crop_descriptor = crop_recording_output_descriptor(snapshot, serial)
    metadata_path = resolve_crop_artifact_path(
        recording_folder,
        crop_output,
        crop_descriptor,
        "metadata",
        f"Cam{serial}_crop_meta.csv",
    )
    return count_csv_data_rows(metadata_path)


def crop_metadata_detection_row_count(recording_folder: Path, snapshot: dict[str, Any], serial: str) -> int | None:
    crop_output = crop_output_for(snapshot, serial)
    crop_descriptor = crop_recording_output_descriptor(snapshot, serial)
    metadata_path = resolve_crop_artifact_path(
        recording_folder,
        crop_output,
        crop_descriptor,
        "metadata",
        f"Cam{serial}_crop_meta.csv",
    )
    try:
        with metadata_path.open("r", encoding="utf-8", newline="") as handle:
            return sum(
                1
                for row in csv.DictReader(handle)
                if int_csv_field(row, "has_detection") == 1
            )
    except OSError:
        return None


def recording_frame_ids_from_rows(rows: list[dict[str, str]]) -> tuple[list[int], int]:
    ids: list[int] = []
    missing = 0
    for row in rows:
        value = int_csv_field(row, "recording_frame_id")
        if value is None:
            missing += 1
        else:
            ids.append(value)
    return ids, missing


def ids_are_positive_strictly_increasing(ids: list[int]) -> bool:
    return all(value > 0 for value in ids) and all(
        ids[index] < ids[index + 1] for index in range(len(ids) - 1)
    )


def fmt_float(value: Any, precision: int = 3) -> str:
    parsed = number(value)
    return "n/a" if parsed is None else f"{parsed:.{precision}f}"


def check_sync_config(
    reporter: Reporter,
    snapshot: dict[str, Any],
    summary: dict[str, Any],
    cameras: list[str],
    expected_sync_mode: str,
    expected_ptp_mode: str,
) -> None:
    sync = summary.get("sync") if isinstance(summary.get("sync"), dict) else {}
    reporter.check(
        bool(sync.get("camera_sync_enabled")),
        "session sync reports camera_sync_enabled=true",
        f"session sync camera_sync_enabled is {sync.get('camera_sync_enabled')!r}",
    )

    for serial in cameras:
        runtime = nested_dict(snapshot, "camera_runtime", serial, "runtime")
        if not runtime:
            reporter.fail(f"Cam{serial} missing recording_snapshot camera runtime")
            continue
        sync_mode = runtime.get("sync_mode")
        reporter.check(
            sync_mode == expected_sync_mode,
            f"Cam{serial} sync_mode={expected_sync_mode}",
            f"Cam{serial} sync_mode is {sync_mode!r}; expected {expected_sync_mode!r}",
        )
        ptp = runtime.get("ptp")
        if not isinstance(ptp, dict):
            reporter.fail(f"Cam{serial} runtime ptp config missing")
            continue
        reporter.check(
            ptp.get("enabled") is True,
            f"Cam{serial} ptp.enabled=true",
            f"Cam{serial} ptp.enabled is {ptp.get('enabled')!r}",
        )
        if expected_ptp_mode:
            reporter.check(
                ptp.get("mode") == expected_ptp_mode,
                f"Cam{serial} ptp.mode={expected_ptp_mode}",
                f"Cam{serial} ptp.mode is {ptp.get('mode')!r}; expected {expected_ptp_mode!r}",
            )


def check_ptp_counters(
    reporter: Reporter,
    summary: dict[str, Any],
    ptp_sync_summary: dict[str, Any],
    cameras: list[str],
    expected_decimate: int,
    skip_decimate: bool,
) -> None:
    ptp_cameras = nested_dict(summary, "ptp", "cameras")
    raw_ptp_cameras = ptp_sync_summary.get("cameras")
    raw_ptp_cameras = raw_ptp_cameras if isinstance(raw_ptp_cameras, dict) else {}

    for serial in cameras:
        camera = ptp_cameras.get(serial) if isinstance(ptp_cameras, dict) else None
        camera = camera if isinstance(camera, dict) else {}
        raw_camera = raw_ptp_cameras.get(serial)
        raw_camera = raw_camera if isinstance(raw_camera, dict) else {}
        if not camera and not raw_camera:
            reporter.fail(f"Cam{serial} missing PTP/acquisition summary")
            continue

        gaps = integer(camera.get("camera_frame_id_gaps", raw_camera.get("camera_frame_id_gaps")))
        get_frame_errors = integer(camera.get("get_frame_errors", raw_camera.get("get_frame_errors")))
        reporter.check(gaps == 0, f"Cam{serial} PTP frame gaps=0", f"Cam{serial} PTP frame gaps={gaps}")
        reporter.check(
            get_frame_errors == 0,
            f"Cam{serial} GetFrame errors=0",
            f"Cam{serial} GetFrame errors={get_frame_errors}",
        )
        if "finalized" in raw_camera:
            reporter.check(
                raw_camera.get("finalized") is True,
                f"Cam{serial} PTP summary finalized",
                f"Cam{serial} PTP summary finalized={raw_camera.get('finalized')!r}",
            )

        decimate = integer(camera.get("ptp_register_read_decimate"))
        reads = integer(camera.get("ptp_register_reads") or camera.get("ptp_register_reads_from_cadence"))
        if skip_decimate:
            if decimate is None:
                reporter.warn(f"Cam{serial} PTP register-read decimation field missing; check skipped")
            else:
                reporter.pass_(f"Cam{serial} PTP register-read decimate={decimate}")
        else:
            reporter.check(
                decimate == expected_decimate,
                f"Cam{serial} PTP register-read decimate={expected_decimate}",
                f"Cam{serial} PTP register-read decimate={decimate}; expected {expected_decimate}",
            )
            reporter.check(
                reads is not None and reads > 0,
                f"Cam{serial} PTP register reads sampled ({reads})",
                f"Cam{serial} PTP register reads missing or zero ({reads})",
            )


def check_pipeline(
    reporter: Reporter,
    summary: dict[str, Any],
    cameras: list[str],
    expected_display_preview_max_fps: int | None = None,
) -> None:
    for serial in cameras:
        pipeline = nested_dict(summary, "pipeline", serial)
        if not pipeline:
            reporter.fail(f"Cam{serial} missing pipeline perf CSV")
            continue
        final = pipeline.get("final")
        final = final if isinstance(final, dict) else {}
        checks = [
            ("camera_dropped_frames", "camera dropped frames"),
            ("camera_frame_id_gaps", "camera frame-id gaps"),
            ("get_frame_errors", "GetFrame errors"),
            ("enc_fail", "encode failures"),
            ("external_ipc_failures", "external IPC failures"),
            ("external_ipc_ack_timeouts", "external IPC ACK timeouts"),
        ]
        for field, label in checks:
            if field not in final or final.get(field) is None:
                continue
            value = integer(final.get(field))
            reporter.check(value == 0, f"Cam{serial} {label}=0", f"Cam{serial} {label}={value}")
        enc_slow = integer(final.get("enc_slow"))
        if enc_slow is not None:
            reporter.pass_(f"Cam{serial} enc_slow={enc_slow} (reported, not a failure)")
        if expected_display_preview_max_fps is not None:
            display_preview_max_fps = integer(final.get("display_preview_max_fps"))
            reporter.check(
                display_preview_max_fps == expected_display_preview_max_fps,
                f"Cam{serial} display preview max FPS={display_preview_max_fps}",
                (
                    f"Cam{serial} display preview max FPS={display_preview_max_fps}, "
                    f"expected {expected_display_preview_max_fps}"
                ),
            )


def video_content_sanity(
    mp4_path: Path,
    ffprobe: str,
    ffmpeg: str,
    max_black_fraction: float,
    min_stddev: float,
) -> dict[str, Any]:
    if not mp4_path.exists() or mp4_path.stat().st_size == 0:
        return {"status": "missing_video", "content_valid": False, "detail": "MP4 is missing or empty"}

    probe_cmd = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,nb_frames,avg_frame_rate,duration",
        "-show_entries",
        "format=size,duration",
        "-of",
        "json",
        str(mp4_path),
    ]
    try:
        probe = subprocess.run(
            probe_cmd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return {"status": "ffprobe_failed", "content_valid": False, "detail": str(exc)}

    try:
        metadata = json.loads(probe.stdout)
    except json.JSONDecodeError as exc:
        return {"status": "ffprobe_json_failed", "content_valid": False, "detail": str(exc)}

    streams = metadata.get("streams") or []
    if not streams:
        return {"status": "no_video_stream", "content_valid": False, "detail": "ffprobe found no video stream"}
    stream = streams[0]
    width = integer(stream.get("width")) or 0
    height = integer(stream.get("height")) or 0
    if width <= 0 or height <= 0:
        return {"status": "invalid_dimensions", "content_valid": False, "width": width, "height": height}

    frame_count = integer(stream.get("nb_frames")) or 0
    if frame_count > 0:
        sample_indices = sorted({0, frame_count // 4, frame_count // 2, (3 * frame_count) // 4, frame_count - 1})
    else:
        sample_indices = [0]

    select_expr = "+".join(f"eq(n\\,{index})" for index in sample_indices)
    decode_cmd = [
        ffmpeg,
        "-v",
        "error",
        "-i",
        str(mp4_path),
        "-vf",
        f"select='{select_expr}'",
        "-vsync",
        "0",
        "-pix_fmt",
        "gray",
        "-f",
        "rawvideo",
        "-",
    ]
    try:
        decoded = subprocess.run(
            decode_cmd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=120,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return {"status": "decode_failed", "content_valid": False, "detail": str(exc)}

    frame_bytes = width * height
    decoded_frames = len(decoded) // frame_bytes if frame_bytes else 0
    if decoded_frames == 0:
        return {"status": "decode_empty", "content_valid": False, "detail": "ffmpeg returned no sample frames"}

    measurements: list[dict[str, Any]] = []
    for index in range(decoded_frames):
        frame = decoded[index * frame_bytes : (index + 1) * frame_bytes]
        hist = [0] * 256
        for value in frame:
            hist[value] += 1
        pixel_count = sum(hist)
        total = sum(value * count for value, count in enumerate(hist))
        total_sq = sum(value * value * count for value, count in enumerate(hist))
        mean = total / pixel_count
        variance = max(0.0, total_sq / pixel_count - mean * mean)
        measurements.append(
            {
                "requested_frame_index": sample_indices[min(index, len(sample_indices) - 1)],
                "mean": mean,
                "stddev": math.sqrt(variance),
                "black_fraction_lt8": sum(hist[:8]) / pixel_count,
                "decoded_bytes": pixel_count,
            }
        )

    max_black = max(item["black_fraction_lt8"] for item in measurements)
    max_stddev = max(item["stddev"] for item in measurements)
    mean_luma = sum(item["mean"] for item in measurements) / len(measurements)
    if max_black >= max_black_fraction:
        status = "black_frame"
    elif max_stddev < min_stddev:
        status = "flat_frame"
    else:
        status = "pass"
    return {
        "status": status,
        "content_valid": status == "pass",
        "width": width,
        "height": height,
        "nb_frames": frame_count,
        "sampled_frame_count": len(measurements),
        "mean_luma": mean_luma,
        "max_stddev": max_stddev,
        "max_black_fraction_lt8": max_black,
        "thresholds": {
            "max_black_fraction_lt8": max_black_fraction,
            "min_max_stddev": min_stddev,
        },
        "sampled_frames": measurements,
    }


def check_videos(
    reporter: Reporter,
    summary: dict[str, Any],
    cameras: list[str],
    ffprobe: str,
    ffmpeg: str,
    min_bitrate_mbps: float,
    skip_content_check: bool,
    max_black_fraction: float,
    min_stddev: float,
) -> dict[str, Any]:
    video_sanity: dict[str, Any] = {}
    for serial in cameras:
        video = nested_dict(summary, "videos", serial)
        if not video:
            reporter.fail(f"Cam{serial} missing main MP4")
            continue
        reporter.check(
            video.get("status") == "ok",
            f"Cam{serial} main MP4 ffprobe status=ok",
            f"Cam{serial} main MP4 ffprobe status={video.get('status')!r}",
        )
        frames = integer(video.get("frames"))
        width = integer(video.get("width"))
        height = integer(video.get("height"))
        reporter.check(
            bool(frames and frames > 0 and width and width > 0 and height and height > 0),
            f"Cam{serial} main MP4 dimensions/frame count present ({width}x{height}, frames={frames})",
            f"Cam{serial} invalid main MP4 dimensions/frame count ({width}x{height}, frames={frames})",
        )
        bitrate_bps = number(video.get("bitrate_bps"))
        bitrate_mbps = None if bitrate_bps is None else bitrate_bps / 1_000_000.0
        reporter.check(
            bitrate_mbps is not None and bitrate_mbps >= min_bitrate_mbps,
            f"Cam{serial} main MP4 bitrate {fmt_float(bitrate_mbps, 1)} Mbps >= {min_bitrate_mbps:.1f} Mbps",
            f"Cam{serial} main MP4 bitrate {bitrate_mbps} Mbps below {min_bitrate_mbps:.1f} Mbps",
        )
        if skip_content_check:
            reporter.warn(f"Cam{serial} decoded video-content check skipped")
            continue
        sanity = video_content_sanity(
            Path(str(video.get("path"))),
            ffprobe,
            ffmpeg,
            max_black_fraction,
            min_stddev,
        )
        video_sanity[serial] = sanity
        reporter.check(
            bool(sanity.get("content_valid")),
            (
                f"Cam{serial} decoded video sanity pass "
                f"(mean_luma={fmt_float(sanity.get('mean_luma'), 1)}, "
                f"stddev={fmt_float(sanity.get('max_stddev'), 1)}, "
                f"black={fmt_float(sanity.get('max_black_fraction_lt8'), 6)})"
            ),
            f"Cam{serial} decoded video sanity failed: {sanity.get('status')} {sanity.get('detail', '')}",
        )
    return video_sanity


def check_yolo(
    reporter: Reporter,
    summary: dict[str, Any],
    cameras: list[str],
    max_queue_p95_ms: float,
    max_steady_p95_ms: float | None,
    max_ptp_done_p95_ms: float | None,
) -> None:
    for serial in cameras:
        yolo = nested_dict(summary, "yolo", serial)
        if not yolo:
            reporter.fail(f"Cam{serial} missing YOLO perf CSV")
            continue
        rows = integer(yolo.get("rows")) or 0
        ok_rows = integer(yolo.get("ok_rows")) or 0
        reporter.check(rows > 0, f"Cam{serial} YOLO rows={rows}", f"Cam{serial} YOLO rows missing")
        reporter.check(ok_rows == rows, f"Cam{serial} YOLO ok rows={ok_rows}/{rows}", f"Cam{serial} YOLO ok rows={ok_rows}/{rows}")

        detect = metric(summary, serial, "acquisition_to_detect_done_ms") or metric(summary, serial, "capture_to_detect_done_ms")
        queue = metric(summary, serial, "yolo_queue_wait_ms")
        cpu_pre_sync = metric(summary, serial, "cpu_pre_sync_ms")
        ptp_done = metric(summary, serial, "acquisition_to_ptp_done_ms")

        detect_steady_p95 = number(detect.get("steady_p95"))
        queue_p95 = number(queue.get("p95"))
        cpu_pre_sync_p95 = number(cpu_pre_sync.get("p95"))
        ptp_done_p95 = number(ptp_done.get("p95"))

        reporter.check(
            detect_steady_p95 is not None,
            f"Cam{serial} YOLO steady detect p95={fmt_float(detect_steady_p95)} ms",
            f"Cam{serial} YOLO steady detect p95 missing",
        )
        if max_steady_p95_ms is not None and detect_steady_p95 is not None:
            reporter.check(
                detect_steady_p95 <= max_steady_p95_ms,
                f"Cam{serial} YOLO steady detect p95 <= {max_steady_p95_ms:.3f} ms",
                f"Cam{serial} YOLO steady detect p95 {detect_steady_p95:.3f} ms > {max_steady_p95_ms:.3f} ms",
            )
        reporter.check(
            queue_p95 is not None and queue_p95 <= max_queue_p95_ms,
            f"Cam{serial} YOLO queue p95={fmt_float(queue_p95)} ms",
            f"Cam{serial} YOLO queue p95={queue_p95} ms exceeds {max_queue_p95_ms:.3f} ms",
        )
        if cpu_pre_sync_p95 is not None:
            reporter.pass_(f"Cam{serial} YOLO cpu_pre_sync p95={cpu_pre_sync_p95:.3f} ms")
        if ptp_done_p95 is None:
            reporter.warn(f"Cam{serial} acquisition_to_ptp_done_ms missing from YOLO perf")
        elif max_ptp_done_p95_ms is not None:
            reporter.check(
                ptp_done_p95 <= max_ptp_done_p95_ms,
                f"Cam{serial} acquisition_to_ptp_done p95={ptp_done_p95:.3f} ms",
                f"Cam{serial} acquisition_to_ptp_done p95={ptp_done_p95:.3f} ms > {max_ptp_done_p95_ms:.3f} ms",
            )
        else:
            reporter.pass_(f"Cam{serial} acquisition_to_ptp_done p95={ptp_done_p95:.3f} ms")


def check_crop_preview_counters(
    reporter: Reporter,
    recording_folder: Path,
    snapshot: dict[str, Any],
    cameras: list[str],
    expected_preview_max_fps: int | None,
    expected_preview_display_enabled: int | None,
    expected_preview_disabled: int | None,
    min_crop_frame_pool_size: int | None,
    require_sampling: bool,
    require_counters: bool,
) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    if (
        expected_preview_max_fps is None
        and expected_preview_display_enabled is None
        and expected_preview_disabled is None
        and min_crop_frame_pool_size is None
        and not require_sampling
        and not require_counters
    ):
        return summary

    crop_outputs = snapshot.get("crop_outputs")
    crop_outputs = crop_outputs if isinstance(crop_outputs, dict) else {}
    if crop_outputs:
        target_cameras = crop_enabled_cameras(snapshot, cameras)
    else:
        target_cameras = cameras

    if (require_counters or require_sampling) and not target_cameras:
        reporter.fail("crop preview counters required but no crop-enabled cameras were found")

    for serial in target_cameras:
        path = recording_folder / f"Cam{serial}_crop_sidecar_perf.csv"
        rows = read_csv_rows(path)
        if not rows:
            if require_counters or require_sampling:
                reporter.fail(f"Cam{serial} crop sidecar preview counters missing at {path}")
            else:
                reporter.warn(f"Cam{serial} crop sidecar preview counters absent")
            continue

        row = rows[-1]
        preview_max_fps = int_csv_field(row, "preview_max_fps")
        preview_disabled = int_csv_field(row, "preview_disabled")
        preview_display_enabled = int_csv_field(row, "preview_display_enabled_final")
        crop_frame_pool_size = int_csv_field(row, "crop_frame_pool_size")
        producer_recording_crop_frame_offered = int_csv_field(
            row, "producer_recording_crop_frame_offered")
        producer_recording_crop_frame_accepted = int_csv_field(
            row, "producer_recording_crop_frame_accepted")
        producer_recording_crop_frame_dropped = int_csv_field(
            row, "producer_recording_crop_frame_dropped")
        producer_preview_crop_frame_offered = int_csv_field(
            row, "producer_preview_crop_frame_offered")
        producer_preview_crop_frame_accepted = int_csv_field(
            row, "producer_preview_crop_frame_accepted")
        producer_preview_crop_frame_dropped = int_csv_field(
            row, "producer_preview_crop_frame_dropped")
        producer_pose_crop_frame_offered = int_csv_field(
            row, "producer_pose_crop_frame_offered")
        producer_pose_crop_frame_accepted = int_csv_field(
            row, "producer_pose_crop_frame_accepted")
        producer_pose_crop_frame_dropped = int_csv_field(
            row, "producer_pose_crop_frame_dropped")
        producer_crop_frame_pool_misses_total = int_csv_field(
            row, "producer_crop_frame_pool_misses_total")
        offered = int_csv_field(row, "preview_frames_offered")
        updated = int_csv_field(row, "preview_frames_updated")
        skipped = int_csv_field(row, "preview_frames_skipped_by_cadence")
        clears = int_csv_field(row, "preview_clears_updated")
        preview_queue_full_drops = int_csv_field(row, "preview_queue_full_drops")
        preview_queue_high_water = int_csv_field(row, "preview_queue_high_water")
        serial_final = int_csv_field(row, "preview_serial_final")
        crop_metadata_rows = crop_metadata_row_count(recording_folder, snapshot, serial)
        crop_metadata_detection_rows = crop_metadata_detection_row_count(
            recording_folder,
            snapshot,
            serial,
        )

        summary[serial] = {
            "preview_max_fps": preview_max_fps,
            "preview_disabled": preview_disabled,
            "preview_display_enabled_final": preview_display_enabled,
            "crop_frame_pool_size": crop_frame_pool_size,
            "producer_recording_crop_frame_offered": producer_recording_crop_frame_offered,
            "producer_recording_crop_frame_accepted": producer_recording_crop_frame_accepted,
            "producer_recording_crop_frame_dropped": producer_recording_crop_frame_dropped,
            "producer_preview_crop_frame_offered": producer_preview_crop_frame_offered,
            "producer_preview_crop_frame_accepted": producer_preview_crop_frame_accepted,
            "producer_preview_crop_frame_dropped": producer_preview_crop_frame_dropped,
            "producer_pose_crop_frame_offered": producer_pose_crop_frame_offered,
            "producer_pose_crop_frame_accepted": producer_pose_crop_frame_accepted,
            "producer_pose_crop_frame_dropped": producer_pose_crop_frame_dropped,
            "producer_crop_frame_pool_misses_total": producer_crop_frame_pool_misses_total,
            "preview_frames_offered": offered,
            "preview_frames_updated": updated,
            "preview_frames_skipped_by_cadence": skipped,
            "preview_clears_updated": clears,
            "preview_queue_full_drops": preview_queue_full_drops,
            "preview_queue_high_water": preview_queue_high_water,
            "preview_serial_final": serial_final,
            "crop_metadata_rows": crop_metadata_rows,
            "crop_metadata_detection_rows": crop_metadata_detection_rows,
        }

        required_fields_present = all(
            value is not None
            for value in (
                preview_max_fps,
                preview_disabled,
                preview_display_enabled,
                offered,
                updated,
                skipped,
                clears,
                serial_final,
            )
        )
        reporter.check(
            required_fields_present,
            f"Cam{serial} crop preview counters present",
            f"Cam{serial} crop preview counters incomplete in {path}",
        )
        if expected_preview_max_fps is not None:
            reporter.check(
                preview_max_fps == expected_preview_max_fps,
                f"Cam{serial} crop preview max FPS={preview_max_fps}",
                (
                    f"Cam{serial} crop preview max FPS={preview_max_fps}, "
                    f"expected {expected_preview_max_fps}"
                ),
            )
        if expected_preview_display_enabled is not None:
            reporter.check(
                preview_display_enabled == expected_preview_display_enabled,
                f"Cam{serial} crop preview display_enabled_final={preview_display_enabled}",
                (
                    f"Cam{serial} crop preview display_enabled_final={preview_display_enabled}, "
                    f"expected {expected_preview_display_enabled}"
                ),
            )
        if expected_preview_disabled is not None:
            reporter.check(
                preview_disabled == expected_preview_disabled,
                f"Cam{serial} crop preview disabled={preview_disabled}",
                (
                    f"Cam{serial} crop preview disabled={preview_disabled}, "
                    f"expected {expected_preview_disabled}"
                ),
            )
        if min_crop_frame_pool_size is not None:
            reporter.check(
                crop_frame_pool_size is not None and crop_frame_pool_size >= min_crop_frame_pool_size,
                f"Cam{serial} crop frame pool size={crop_frame_pool_size}",
                (
                    f"Cam{serial} crop frame pool size={crop_frame_pool_size}, "
                    f"expected >= {min_crop_frame_pool_size}"
                ),
            )
        if offered is not None and updated is not None:
            reporter.check(
                updated <= offered,
                f"Cam{serial} crop preview updated/offered={updated}/{offered}",
                f"Cam{serial} crop preview updated frames {updated} exceed offered {offered}",
            )
        for label, fanout_offered, fanout_accepted, fanout_dropped in (
            (
                "recording",
                producer_recording_crop_frame_offered,
                producer_recording_crop_frame_accepted,
                producer_recording_crop_frame_dropped,
            ),
            (
                "preview",
                producer_preview_crop_frame_offered,
                producer_preview_crop_frame_accepted,
                producer_preview_crop_frame_dropped,
            ),
            (
                "pose",
                producer_pose_crop_frame_offered,
                producer_pose_crop_frame_accepted,
                producer_pose_crop_frame_dropped,
            ),
        ):
            if (
                fanout_offered is not None
                and fanout_accepted is not None
                and fanout_dropped is not None
            ):
                reporter.check(
                    fanout_accepted + fanout_dropped == fanout_offered,
                    (
                        f"Cam{serial} crop-frame {label} fanout "
                        f"{fanout_accepted}/{fanout_offered} accepted, "
                        f"dropped={fanout_dropped}"
                    ),
                    (
                        f"Cam{serial} crop-frame {label} fanout counters do not balance: "
                        f"accepted={fanout_accepted}, dropped={fanout_dropped}, "
                        f"offered={fanout_offered}"
                    ),
                )
        if (
            producer_recording_crop_frame_accepted is not None
            and crop_metadata_detection_rows is not None
        ):
            reporter.check(
                producer_recording_crop_frame_accepted == crop_metadata_detection_rows,
                (
                    f"Cam{serial} recording crop-frame fanout matches detection crop rows "
                    f"({producer_recording_crop_frame_accepted})"
                ),
                (
                    f"Cam{serial} recording crop-frame accepted "
                    f"({producer_recording_crop_frame_accepted}) != crop metadata "
                    f"has_detection rows ({crop_metadata_detection_rows})"
                ),
            )
        if producer_recording_crop_frame_dropped is not None:
            reporter.check(
                producer_recording_crop_frame_dropped == 0,
                f"Cam{serial} recording crop-frame fanout drops=0",
                (
                    f"Cam{serial} recording crop-frame fanout drops="
                    f"{producer_recording_crop_frame_dropped}"
                ),
            )
        if (
            producer_preview_crop_frame_accepted is not None
            and updated is not None
        ):
            reporter.check(
                producer_preview_crop_frame_accepted <= updated,
                (
                    f"Cam{serial} preview crop-frame accepted <= preview updates "
                    f"({producer_preview_crop_frame_accepted}/{updated})"
                ),
                (
                    f"Cam{serial} preview crop-frame accepted "
                    f"({producer_preview_crop_frame_accepted}) exceeds preview updates "
                    f"({updated})"
                ),
            )
        if (
            producer_preview_crop_frame_offered is not None
            and offered is not None
        ):
            reporter.check(
                producer_preview_crop_frame_offered <= offered,
                (
                    f"Cam{serial} preview crop-frame offered <= preview offered "
                    f"({producer_preview_crop_frame_offered}/{offered})"
                ),
                (
                    f"Cam{serial} preview crop-frame offered "
                    f"({producer_preview_crop_frame_offered}) exceeds preview offered "
                    f"({offered})"
                ),
            )
        if offered and skipped is not None:
            reporter.pass_(f"Cam{serial} crop preview skipped_by_cadence={skipped}")
        if require_sampling:
            reporter.check(
                preview_disabled == 0,
                f"Cam{serial} crop preview sampling: preview enabled",
                f"Cam{serial} crop preview sampling cannot be proven with preview_disabled={preview_disabled}",
            )
            reporter.check(
                preview_display_enabled == 1,
                f"Cam{serial} crop preview sampling: display enabled at finalization",
                (
                    f"Cam{serial} crop preview sampling cannot be proven with "
                    f"preview_display_enabled_final={preview_display_enabled}"
                ),
            )
            reporter.check(
                preview_max_fps is not None and preview_max_fps > 0,
                f"Cam{serial} crop preview sampling: bounded max FPS={preview_max_fps}",
                (
                    f"Cam{serial} crop preview sampling requires bounded preview_max_fps; "
                    f"got {preview_max_fps}"
                ),
            )
            reporter.check(
                crop_metadata_rows is not None and crop_metadata_rows > 1,
                f"Cam{serial} crop preview sampling: crop rows={crop_metadata_rows}",
                (
                    f"Cam{serial} crop preview sampling needs more than one crop row; "
                    f"got {crop_metadata_rows}"
                ),
            )
            reporter.check(
                offered is not None and offered > 1,
                f"Cam{serial} crop preview sampling: offered preview frames={offered}",
                f"Cam{serial} crop preview sampling needs more than one offered frame; got {offered}",
            )
            reporter.check(
                skipped is not None and skipped > 0,
                f"Cam{serial} crop preview sampling skipped frames by cadence ({skipped})",
                f"Cam{serial} crop preview sampling did not skip any offered frames by cadence ({skipped})",
            )
            reporter.check(
                offered is not None and updated is not None and updated < offered,
                f"Cam{serial} crop preview sampling updated fewer frames than offered ({updated}/{offered})",
                f"Cam{serial} crop preview sampling updated/offered={updated}/{offered}",
            )

    return summary


def check_gui_display_frame_rate(
    reporter: Reporter,
    snapshot: dict[str, Any],
    min_overall_p05: float | None,
    min_visible_p05: float | None,
    min_hidden_p05: float | None,
    expected_stream_downsample: int | None,
    expected_display_preview_max_fps: int | None,
    expected_yolo_speed_graphs_enabled: int | None,
    require_timing_telemetry: bool,
) -> dict[str, Any]:
    if (
        min_overall_p05 is None
        and min_visible_p05 is None
        and min_hidden_p05 is None
        and expected_stream_downsample is None
        and expected_display_preview_max_fps is None
        and expected_yolo_speed_graphs_enabled is None
        and not require_timing_telemetry
    ):
        return {}

    metrics = nested_dict(snapshot, "session", "gui_display_frame_rate")
    reporter.check(
        bool(metrics),
        "GUI display frame-rate telemetry present",
        "GUI display frame-rate telemetry missing from recording_snapshot session",
    )
    if not metrics:
        return {}

    if expected_stream_downsample is not None:
        stream_downsample = integer(metrics.get("stream_downsample"))
        reporter.check(
            stream_downsample == expected_stream_downsample,
            f"GUI stream downsample={stream_downsample}",
            f"GUI stream downsample={stream_downsample}, expected {expected_stream_downsample}",
        )
    if expected_display_preview_max_fps is not None:
        display_preview_max_fps = integer(metrics.get("display_preview_max_fps"))
        reporter.check(
            display_preview_max_fps == expected_display_preview_max_fps,
            f"GUI display preview max FPS={display_preview_max_fps}",
            (
                f"GUI display preview max FPS={display_preview_max_fps}, "
                f"expected {expected_display_preview_max_fps}"
            ),
        )
    if expected_yolo_speed_graphs_enabled is not None:
        yolo_speed_graphs_enabled = integer(metrics.get("yolo_speed_graphs_enabled"))
        reporter.check(
            yolo_speed_graphs_enabled == expected_yolo_speed_graphs_enabled,
            f"GUI YOLO speed graphs enabled={yolo_speed_graphs_enabled}",
            (
                f"GUI YOLO speed graphs enabled={yolo_speed_graphs_enabled}, "
                f"expected {expected_yolo_speed_graphs_enabled}"
            ),
        )
    if require_timing_telemetry:
        timings = metrics.get("timings")
        timings = timings if isinstance(timings, dict) else {}
        reporter.check(
            bool(timings),
            "GUI timing telemetry present",
            "GUI timing telemetry missing from session.gui_display_frame_rate.timings",
        )
        required_timing_buckets = [
            "frame_total_ms",
            "main_texture_upload_ms",
            "crop_texture_upload_ms",
            "camera_window_draw_ms",
            "crop_window_draw_ms",
            "speed_graph_draw_ms",
            "render_present_ms",
        ]
        for bucket_name in required_timing_buckets:
            bucket = timings.get(bucket_name)
            bucket = bucket if isinstance(bucket, dict) else {}
            sample_count = integer(bucket.get("sample_count"))
            reporter.check(
                sample_count is not None and sample_count > 0,
                f"GUI timing {bucket_name} samples={sample_count}",
                f"GUI timing {bucket_name} samples missing or zero ({sample_count})",
            )
        main_upload_count = integer(timings.get("main_texture_upload_count"))
        crop_upload_count = integer(timings.get("crop_texture_upload_count"))
        reporter.check(
            main_upload_count is not None,
            f"GUI timing main texture upload count={main_upload_count}",
            "GUI timing main_texture_upload_count missing",
        )
        reporter.check(
            crop_upload_count is not None,
            f"GUI timing crop texture upload count={crop_upload_count}",
            "GUI timing crop_texture_upload_count missing",
        )

    def check_bucket(bucket_name: str, label: str, threshold: float | None) -> None:
        if threshold is None:
            return
        bucket = metrics.get(bucket_name)
        bucket = bucket if isinstance(bucket, dict) else {}
        sample_count = integer(bucket.get("sample_count"))
        p05_fps = number(bucket.get("p05_fps"))
        reporter.check(
            sample_count is not None and sample_count > 0,
            f"GUI {label} FPS samples={sample_count}",
            f"GUI {label} FPS samples missing or zero ({sample_count})",
        )
        reporter.check(
            p05_fps is not None and p05_fps >= threshold,
            f"GUI {label} FPS p05={fmt_float(p05_fps, 1)} >= {threshold:.1f}",
            f"GUI {label} FPS p05={p05_fps} below {threshold:.1f}",
        )

    check_bucket("overall", "recording", min_overall_p05)
    check_bucket("crop_preview_visible", "crop-preview-visible", min_visible_p05)
    check_bucket("crop_preview_hidden", "crop-preview-hidden", min_hidden_p05)
    out = dict(metrics)
    diagnosis = gui_summary.summarize_gui_timing_diagnosis(metrics)
    if diagnosis:
        out["timing_diagnosis"] = diagnosis
    return out


def check_crop_recording_artifacts(
    reporter: Reporter,
    recording_folder: Path,
    snapshot: dict[str, Any],
    summary: dict[str, Any],
    cameras: list[str],
    require_crop_artifacts: bool,
    ffprobe: str,
    *,
    probe_video: bool = True,
    expected_external_queue_depth: int | None = None,
    expected_external_recorder_gpu_id: int | None = None,
    expected_external_recorder_gpu_by_serial: dict[str, int] | None = None,
    max_external_queue_high_water: int | None = None,
    max_external_enqueue_age_p95_ms: float | None = None,
    require_external_crop_backend_metadata: bool = False,
) -> dict[str, Any]:
    crop_summary: dict[str, Any] = {}
    if not require_crop_artifacts:
        return crop_summary

    target_cameras = crop_enabled_cameras(snapshot, cameras)
    if not target_cameras:
        reporter.fail("crop recording artifacts required but no crop-enabled cameras were found")
        return crop_summary
    recording_session_manifest = read_json(recording_folder / "recording_session.json")
    crop_recording_backend = nested_dict(
        recording_session_manifest,
        "recording_backend",
        "crop_recording",
    )

    for serial in target_cameras:
        crop_output = crop_output_for(snapshot, serial)
        crop_descriptor = crop_recording_output_descriptor(snapshot, serial)
        crop_size = crop_size_from_snapshot(snapshot, serial)
        video_path = resolve_crop_artifact_path(
            recording_folder, crop_output, crop_descriptor, "video", f"Cam{serial}_crop.mp4"
        )
        metadata_path = resolve_crop_artifact_path(
            recording_folder, crop_output, crop_descriptor, "metadata", f"Cam{serial}_crop_meta.csv"
        )
        keyframes_path = resolve_crop_artifact_path(
            recording_folder, crop_output, crop_descriptor, "keyframes", f"Cam{serial}_crop_keyframe.json"
        )
        perf_path = resolve_crop_artifact_path(
            recording_folder, crop_output, crop_descriptor, "perf", f"Cam{serial}_crop_perf.csv"
        )
        descriptor_backend = str(crop_descriptor.get("backend", ""))
        summary_path = crop_descriptor_artifact_path(
            recording_folder, crop_descriptor, "summary"
        )

        video_exists = video_path.exists() and video_path.stat().st_size > 0
        reporter.check(
            video_exists,
            f"Cam{serial} crop MP4 present",
            f"Cam{serial} crop MP4 missing or empty: {video_path}",
        )
        reporter.check(
            metadata_path.exists(),
            f"Cam{serial} crop metadata present",
            f"Cam{serial} crop metadata missing: {metadata_path}",
        )
        reporter.check(
            keyframes_path.exists(),
            f"Cam{serial} crop keyframe sidecar present",
            f"Cam{serial} crop keyframe sidecar missing: {keyframes_path}",
        )
        reporter.check(
            perf_path.exists(),
            f"Cam{serial} crop perf present",
            f"Cam{serial} crop perf missing: {perf_path}",
        )

        keyframes_valid = json_file_parses_as_object(keyframes_path)
        keyframes = read_json(keyframes_path) if keyframes_valid else {}
        keyframe_total_frames = integer(keyframes.get("total_frames"))
        reporter.check(
            keyframes_valid,
            f"Cam{serial} crop keyframe sidecar parses as JSON",
            f"Cam{serial} crop keyframe sidecar missing or invalid JSON: {keyframes_path}",
        )

        crop_rows = read_csv_rows(metadata_path)
        crop_perf_rows = read_csv_rows(perf_path)
        reporter.check(
            bool(crop_rows),
            f"Cam{serial} crop metadata rows={len(crop_rows)}",
            f"Cam{serial} crop metadata has no data rows",
        )
        reporter.check(
            bool(crop_perf_rows),
            f"Cam{serial} crop perf rows={len(crop_perf_rows)}",
            f"Cam{serial} crop perf has no data rows",
        )
        reporter.check(
            len(crop_perf_rows) == len(crop_rows),
            f"Cam{serial} crop perf rows match crop metadata rows ({len(crop_perf_rows)})",
            (
                f"Cam{serial} crop perf rows ({len(crop_perf_rows)}) != "
                f"crop metadata rows ({len(crop_rows)})"
            ),
        )
        reporter.check(
            keyframe_total_frames == len(crop_rows),
            f"Cam{serial} crop keyframe total_frames matches crop metadata rows ({keyframe_total_frames})",
            (
                f"Cam{serial} crop keyframe total_frames ({keyframe_total_frames}) != "
                f"crop metadata rows ({len(crop_rows)})"
            ),
        )

        crop_ids, missing_crop_ids = recording_frame_ids_from_rows(crop_rows)
        perf_ids, missing_perf_ids = recording_frame_ids_from_rows(crop_perf_rows)
        reporter.check(
            missing_crop_ids == 0 and ids_are_positive_strictly_increasing(crop_ids),
            f"Cam{serial} crop metadata recording_frame_id values are positive and strictly increasing",
            (
                f"Cam{serial} crop metadata recording_frame_id values are invalid "
                f"(missing={missing_crop_ids})"
            ),
        )
        reporter.check(
            missing_perf_ids == 0 and ids_are_positive_strictly_increasing(perf_ids),
            f"Cam{serial} crop perf recording_frame_id values are positive and strictly increasing",
            (
                f"Cam{serial} crop perf recording_frame_id values are invalid "
                f"(missing={missing_perf_ids})"
            ),
        )
        reporter.check(
            crop_ids == perf_ids,
            f"Cam{serial} crop perf and metadata recording_frame_id sequences match",
            f"Cam{serial} crop perf and metadata recording_frame_id sequences differ",
        )

        dropped_rows = [
            index
            for index, row in enumerate(crop_perf_rows, start=2)
            if int_csv_field(row, "dropped") not in (0, None)
        ]
        missing_dropped_rows = [
            index
            for index, row in enumerate(crop_perf_rows, start=2)
            if int_csv_field(row, "dropped") is None
        ]
        reporter.check(
            not missing_dropped_rows,
            f"Cam{serial} crop perf dropped column present",
            f"Cam{serial} crop perf missing dropped value on {len(missing_dropped_rows)} row(s)",
        )
        reporter.check(
            not dropped_rows,
            f"Cam{serial} crop perf reports no dropped crop frames",
            f"Cam{serial} crop perf reports {len(dropped_rows)} dropped crop frame(s)",
        )

        external_frames_received: int | None = None
        external_frames_encoded: int | None = None
        external_frames_dropped: int | None = None
        external_encode_dropped: int | None = None
        external_encode_queue_depth: int | None = None
        external_encode_queue_high_water: int | None = None
        external_enqueue_age_p95_ms: float | None = None
        external_stream_config: dict[str, Any] = {}
        external_stream_id: str | None = None
        external_analytics_gpu_id: int | None = None
        external_recorder_gpu_id: int | None = None
        external_socket_path: str | None = None
        if descriptor_backend == "external_ipc":
            if require_external_crop_backend_metadata:
                backend_mode = str(crop_recording_backend.get("mode", ""))
                reporter.check(
                    backend_mode == "external_ipc",
                    "recording_backend.crop_recording mode is external_ipc",
                    (
                        "recording_backend.crop_recording.mode "
                        f"({backend_mode or 'missing'}) != external_ipc"
                    ),
                )
            reporter.check(
                summary_path is not None and summary_path.exists(),
                f"Cam{serial} external crop summary present",
                f"Cam{serial} external crop summary missing: {summary_path}",
            )
            external_summary = read_json(summary_path) if summary_path and summary_path.exists() else {}
            external_encode = external_summary.get("external_encode")
            external_encode = external_encode if isinstance(external_encode, dict) else {}
            external_frames_received = integer(external_summary.get("frames_received"))
            external_frames_encoded = integer(external_summary.get("frames_encoded"))
            external_frames_dropped = integer(external_encode.get("frames_dropped"))
            external_encode_dropped = integer(external_summary.get("encode_dropped"))
            external_encode_queue_depth = integer(external_summary.get("encode_queue_depth"))
            external_encode_queue_high_water = integer(external_summary.get("encode_queue_high_water"))
            if external_encode_queue_high_water is None and summary_path and summary_path.exists():
                external_encode_queue_high_water = external_detach_queue_high_water(summary_path)
            external_enqueue_age_p95_ms = number(external_encode.get("enqueue_age_p95_ms"))
            reporter.check(
                external_frames_received == len(crop_rows),
                (
                    f"Cam{serial} external crop received count matches crop metadata rows "
                    f"({external_frames_received})"
                ),
                (
                    f"Cam{serial} external crop frames_received ({external_frames_received}) != "
                    f"crop metadata rows ({len(crop_rows)})"
                ),
            )
            reporter.check(
                external_frames_encoded == len(crop_rows),
                (
                    f"Cam{serial} external crop encoded count matches crop metadata rows "
                    f"({external_frames_encoded})"
                ),
                (
                    f"Cam{serial} external crop frames_encoded ({external_frames_encoded}) != "
                    f"crop metadata rows ({len(crop_rows)})"
                ),
            )
            reporter.check(
                (external_frames_dropped or 0) == 0 and (external_encode_dropped or 0) == 0,
                f"Cam{serial} external crop recorder reports no dropped frames",
                (
                    f"Cam{serial} external crop recorder dropped frames: "
                    f"external_encode.frames_dropped={external_frames_dropped}, "
                    f"encode_dropped={external_encode_dropped}"
                ),
            )
            if expected_external_queue_depth is not None:
                reporter.check(
                    external_encode_queue_depth == expected_external_queue_depth,
                    f"Cam{serial} external crop encode queue depth={expected_external_queue_depth}",
                    (
                        f"Cam{serial} external crop encode_queue_depth "
                        f"({external_encode_queue_depth}) != {expected_external_queue_depth}"
                    ),
                )
            if external_encode_queue_depth is not None and external_encode_queue_high_water is not None:
                reporter.check(
                    external_encode_queue_high_water <= external_encode_queue_depth,
                    (
                        f"Cam{serial} external crop encode queue high-water "
                        f"{external_encode_queue_high_water} <= depth {external_encode_queue_depth}"
                    ),
                    (
                        f"Cam{serial} external crop encode_queue_high_water "
                        f"({external_encode_queue_high_water}) exceeds encode_queue_depth "
                        f"({external_encode_queue_depth})"
                    ),
                )
            if max_external_queue_high_water is not None:
                reporter.check(
                    external_encode_queue_high_water is not None and
                    external_encode_queue_high_water <= max_external_queue_high_water,
                    (
                        f"Cam{serial} external crop encode queue high-water "
                        f"{external_encode_queue_high_water} <= {max_external_queue_high_water}"
                    ),
                    (
                        f"Cam{serial} external crop encode_queue_high_water "
                        f"({external_encode_queue_high_water}) > {max_external_queue_high_water}"
                    ),
                )
            if max_external_enqueue_age_p95_ms is not None:
                reporter.check(
                    external_enqueue_age_p95_ms is not None and
                    external_enqueue_age_p95_ms <= max_external_enqueue_age_p95_ms,
                    (
                        f"Cam{serial} external crop enqueue age p95 "
                        f"{fmt_float(external_enqueue_age_p95_ms)} ms <= "
                        f"{max_external_enqueue_age_p95_ms:.3f} ms"
                    ),
                    (
                        f"Cam{serial} external crop enqueue_age_p95_ms "
                        f"({external_enqueue_age_p95_ms}) > {max_external_enqueue_age_p95_ms:.3f} ms"
                    ),
                )
            check_optional_backend_int_map(
                reporter,
                crop_recording_backend,
                serial,
                "frames_received",
                external_frames_received,
                "external crop frames_received",
            )
            check_optional_backend_int_map(
                reporter,
                crop_recording_backend,
                serial,
                "frames_encoded",
                external_frames_encoded,
                "external crop frames_encoded",
            )
            check_optional_backend_int_map(
                reporter,
                crop_recording_backend,
                serial,
                "encode_dropped",
                external_encode_dropped,
                "external crop encode_dropped",
            )
            check_optional_backend_int_map(
                reporter,
                crop_recording_backend,
                serial,
                "external_frames_dropped",
                external_frames_dropped,
                "external crop external_encode.frames_dropped",
            )
            check_optional_backend_int_map(
                reporter,
                crop_recording_backend,
                serial,
                "encode_queue_depth",
                external_encode_queue_depth,
                "external crop encode_queue_depth",
            )
            check_optional_backend_int_map(
                reporter,
                crop_recording_backend,
                serial,
                "encode_queue_high_water",
                external_encode_queue_high_water,
                "external crop encode_queue_high_water",
            )
            check_optional_backend_float_map(
                reporter,
                crop_recording_backend,
                serial,
                "enqueue_age_p95_ms",
                external_enqueue_age_p95_ms,
                "external crop external_encode.enqueue_age_p95_ms",
            )
            stream_config = nested_dict(crop_recording_backend, "stream_config").get(serial)
            external_stream_config = stream_config if isinstance(stream_config, dict) else {}
            stream_id_value = external_stream_config.get("stream_id")
            external_stream_id = stream_id_value if isinstance(stream_id_value, str) and stream_id_value else None
            external_analytics_gpu_id = integer(external_stream_config.get("analytics_gpu_id"))
            external_recorder_gpu_id = integer(external_stream_config.get("recorder_gpu_id"))
            socket_path_value = external_stream_config.get("socket_path")
            external_socket_path = (
                socket_path_value if isinstance(socket_path_value, str) and socket_path_value else None
            )
            expected_recorder_gpu = (
                expected_external_recorder_gpu_by_serial.get(serial)
                if (
                    expected_external_recorder_gpu_by_serial
                    and serial in expected_external_recorder_gpu_by_serial
                )
                else expected_external_recorder_gpu_id
            )
            if expected_recorder_gpu is not None:
                reporter.check(
                    external_recorder_gpu_id == expected_recorder_gpu,
                    f"Cam{serial} external crop recorder_gpu_id={expected_recorder_gpu}",
                    (
                        f"Cam{serial} external crop recorder_gpu_id "
                        f"({external_recorder_gpu_id}) != {expected_recorder_gpu}"
                    ),
                )
            if require_external_crop_backend_metadata:
                reporter.check(
                    bool(external_stream_config),
                    f"Cam{serial} recording_backend.crop_recording.stream_config present",
                    f"Cam{serial} recording_backend.crop_recording.stream_config missing",
                )
                stream_id = require_stream_config_string(
                    reporter,
                    serial,
                    external_stream_config,
                    "stream_id",
                )
                analytics_gpu_id = require_stream_config_int(
                    reporter,
                    serial,
                    external_stream_config,
                    "analytics_gpu_id",
                    minimum=0,
                )
                recorder_gpu_id = require_stream_config_int(
                    reporter,
                    serial,
                    external_stream_config,
                    "recorder_gpu_id",
                    minimum=0,
                )
                socket_path = require_stream_config_string(
                    reporter,
                    serial,
                    external_stream_config,
                    "socket_path",
                )
                for map_name in (
                    "frames_received",
                    "frames_encoded",
                    "encode_dropped",
                    "external_frames_dropped",
                    "encode_queue_depth",
                    "encode_queue_high_water",
                ):
                    require_backend_map_key(
                        reporter,
                        crop_recording_backend,
                        serial,
                        map_name,
                    )
                if external_enqueue_age_p95_ms is not None:
                    require_backend_map_key(
                        reporter,
                        crop_recording_backend,
                        serial,
                        "enqueue_age_p95_ms",
                    )
                details = nested_dict(crop_descriptor, "details")
                reporter.check(
                    bool(details),
                    f"Cam{serial} recording_outputs.crop.details present",
                    f"Cam{serial} recording_outputs.crop.details missing",
                )
                if details:
                    check_descriptor_detail_matches_string(
                        reporter,
                        serial,
                        details,
                        "stream_id",
                        stream_id,
                    )
                    check_descriptor_detail_matches_string(
                        reporter,
                        serial,
                        details,
                        "socket_path",
                        socket_path,
                    )
                    check_descriptor_detail_matches_int(
                        reporter,
                        serial,
                        details,
                        "analytics_gpu_id",
                        analytics_gpu_id,
                    )
                    check_descriptor_detail_matches_int(
                        reporter,
                        serial,
                        details,
                        "recorder_gpu_id",
                        recorder_gpu_id,
                    )
                    check_descriptor_detail_matches_int(
                        reporter,
                        serial,
                        details,
                        "encode_queue_depth",
                        external_encode_queue_depth,
                    )
                    if summary_path is not None:
                        check_descriptor_detail_matches_string(
                            reporter,
                            serial,
                            details,
                            "summary_json",
                            str(summary_path),
                        )
            if external_stream_config and external_encode_queue_depth is not None:
                stream_config_queue_depth = integer(external_stream_config.get("encode_queue_depth"))
                reporter.check(
                    stream_config_queue_depth == external_encode_queue_depth,
                    (
                        f"Cam{serial} recording_backend.crop_recording.stream_config "
                        f"encode_queue_depth matches summary ({stream_config_queue_depth})"
                    ),
                    (
                        f"Cam{serial} recording_backend.crop_recording.stream_config "
                        f"encode_queue_depth ({stream_config_queue_depth}) != "
                        f"external crop encode_queue_depth ({external_encode_queue_depth})"
                    ),
                )

        if crop_size is None:
            reporter.fail(f"Cam{serial} crop size missing from recording_snapshot")
        else:
            bad_geometry_rows = []
            for index, row in enumerate(crop_rows, start=2):
                crop_w = int_csv_field(row, "crop_w")
                crop_h = int_csv_field(row, "crop_h")
                blank_frame = int_csv_field(row, "blank_frame") == 1
                has_detection = int_csv_field(row, "has_detection") == 1
                if blank_frame and not has_detection:
                    if crop_w != 0 or crop_h != 0:
                        bad_geometry_rows.append(index)
                elif crop_w != crop_size or crop_h != crop_size:
                    bad_geometry_rows.append(index)
            reporter.check(
                not bad_geometry_rows,
                f"Cam{serial} crop metadata geometry matches crop_size_px={crop_size}",
                (
                    f"Cam{serial} crop metadata has {len(bad_geometry_rows)} row(s) "
                    f"with geometry inconsistent with crop_size_px={crop_size}"
                ),
            )

        video_frames: int | None = None
        video_width: int | None = None
        video_height: int | None = None
        if video_exists and probe_video:
            video = gui_summary.ffprobe_video(video_path, ffprobe)
            video_frames = integer(video.get("frames"))
            video_width = integer(video.get("width"))
            video_height = integer(video.get("height"))
            reporter.check(
                video.get("status") == "ok",
                f"Cam{serial} crop MP4 ffprobe status=ok",
                f"Cam{serial} crop MP4 ffprobe status={video.get('status')!r}",
            )
            reporter.check(
                video_frames == len(crop_rows),
                f"Cam{serial} crop MP4 frame count matches crop metadata rows ({video_frames})",
                (
                    f"Cam{serial} crop MP4 frames ({video_frames}) != "
                    f"crop metadata rows ({len(crop_rows)})"
                ),
            )
            if crop_size is not None:
                reporter.check(
                    video_width == crop_size and video_height == crop_size,
                    f"Cam{serial} crop MP4 dimensions match crop_size_px ({crop_size})",
                    (
                        f"Cam{serial} crop MP4 dimensions {video_width}x{video_height} "
                        f"!= crop_size_px {crop_size}"
                    ),
                )

        yolo_rows = integer(nested_dict(summary, "yolo", serial).get("rows"))
        if yolo_rows is not None and yolo_rows > 0:
            reporter.check(
                len(crop_rows) == yolo_rows,
                f"Cam{serial} crop metadata rows match YOLO rows ({len(crop_rows)})",
                f"Cam{serial} crop metadata rows ({len(crop_rows)}) != YOLO rows ({yolo_rows})",
            )

        crop_summary[serial] = {
            "backend": descriptor_backend or None,
            "video": str(video_path),
            "metadata": str(metadata_path),
            "keyframes": str(keyframes_path),
            "perf": str(perf_path),
            "crop_size_px": crop_size,
            "video_frames": video_frames,
            "video_width": video_width,
            "video_height": video_height,
            "keyframe_total_frames": keyframe_total_frames,
            "metadata_rows": len(crop_rows),
            "perf_rows": len(crop_perf_rows),
            "dropped_rows": len(dropped_rows),
            "yolo_rows": yolo_rows,
            "external_frames_received": external_frames_received,
            "external_frames_encoded": external_frames_encoded,
            "external_frames_dropped": external_frames_dropped,
            "external_encode_dropped": external_encode_dropped,
            "external_encode_queue_depth": external_encode_queue_depth,
            "external_encode_queue_high_water": external_encode_queue_high_water,
            "external_enqueue_age_p95_ms": external_enqueue_age_p95_ms,
            "external_stream_id": external_stream_id,
            "external_analytics_gpu_id": external_analytics_gpu_id,
            "external_recorder_gpu_id": external_recorder_gpu_id,
            "external_socket_path": external_socket_path,
            "external_stream_config": external_stream_config or None,
        }

    return crop_summary


def compact_camera_summary(summary: dict[str, Any], cameras: list[str], video_sanity: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for serial in cameras:
        detect = metric(summary, serial, "acquisition_to_detect_done_ms") or metric(summary, serial, "capture_to_detect_done_ms")
        queue = metric(summary, serial, "yolo_queue_wait_ms")
        ptp_done = metric(summary, serial, "acquisition_to_ptp_done_ms")
        pipeline = nested_dict(summary, "pipeline", serial).get("final", {})
        video = nested_dict(summary, "videos", serial)
        outputs = nested_dict(summary, "outputs", serial)
        out[serial] = {
            "detect_steady_p95_ms": detect.get("steady_p95"),
            "detect_p95_ms": detect.get("p95"),
            "queue_p95_ms": queue.get("p95"),
            "ptp_done_p95_ms": ptp_done.get("p95"),
            "pipeline_final": pipeline,
            "outputs": outputs,
            "video": {
                "status": video.get("status"),
                "frames": video.get("frames"),
                "duration_s": video.get("duration_s"),
                "bitrate_mbps": None
                if video.get("bitrate_bps") is None
                else float(video["bitrate_bps"]) / 1_000_000.0,
            },
            "video_sanity": video_sanity.get(serial),
        }
    return out


def print_camera_summary(camera_summary: dict[str, Any]) -> None:
    print("\nSummary")
    for serial, item in sorted(camera_summary.items()):
        video = item.get("video", {})
        outputs = item.get("outputs") or {}
        output_text = ",".join(sorted(outputs.keys())) if isinstance(outputs, dict) and outputs else "none"
        print(
            f"  Cam{serial}: detect_steady_p95={item.get('detect_steady_p95_ms')} ms "
            f"queue_p95={item.get('queue_p95_ms')} ms "
            f"ptp_done_p95={item.get('ptp_done_p95_ms')} ms "
            f"outputs={output_text} "
            f"video_frames={video.get('frames')} "
            f"bitrate_mbps={video.get('bitrate_mbps')}"
        )


def print_crop_preview_summary(crop_preview: dict[str, Any]) -> None:
    if not crop_preview:
        return
    print("\nCrop Preview")
    for serial, item in sorted(crop_preview.items()):
        print(
            f"  Cam{serial}: max_fps={item.get('preview_max_fps')} "
            f"disabled={item.get('preview_disabled')} "
            f"display_enabled_final={item.get('preview_display_enabled_final')} "
            f"pool={item.get('crop_frame_pool_size')} "
            f"updated/offered={item.get('preview_frames_updated')}/"
            f"{item.get('preview_frames_offered')} "
            f"skipped={item.get('preview_frames_skipped_by_cadence')} "
            f"queue_drops={item.get('preview_queue_full_drops')} "
            f"crop_rows={item.get('crop_metadata_rows')} "
            f"detection_rows={item.get('crop_metadata_detection_rows')}"
        )
        if item.get("producer_recording_crop_frame_offered") is not None:
            print(
                f"    crop-frame fanout: "
                f"recording={item.get('producer_recording_crop_frame_accepted')}/"
                f"{item.get('producer_recording_crop_frame_offered')} "
                f"dropped={item.get('producer_recording_crop_frame_dropped')} "
                f"preview={item.get('producer_preview_crop_frame_accepted')}/"
                f"{item.get('producer_preview_crop_frame_offered')} "
                f"dropped={item.get('producer_preview_crop_frame_dropped')} "
                f"pose={item.get('producer_pose_crop_frame_accepted')}/"
                f"{item.get('producer_pose_crop_frame_offered')} "
                f"dropped={item.get('producer_pose_crop_frame_dropped')} "
                f"pool_misses_total={item.get('producer_crop_frame_pool_misses_total')}"
            )


def print_crop_recording_summary(crop_recording: dict[str, Any]) -> None:
    if not crop_recording:
        return
    print("\nCrop Recording")
    for serial, item in sorted(crop_recording.items()):
        print(
            f"  Cam{serial}: backend={item.get('backend') or 'unknown'} "
            f"rows={item.get('metadata_rows')} "
            f"perf_rows={item.get('perf_rows')} "
            f"keyframes={item.get('keyframe_total_frames')} "
            f"video_frames={item.get('video_frames')} "
            f"dropped_rows={item.get('dropped_rows')} "
            f"external_received={item.get('external_frames_received')} "
            f"external_encoded={item.get('external_frames_encoded')} "
            f"external_dropped={item.get('external_frames_dropped')} "
            f"external_queue={item.get('external_encode_queue_depth')} "
            f"external_q_high={item.get('external_encode_queue_high_water')} "
            f"external_enqueue_p95={item.get('external_enqueue_age_p95_ms')} "
            f"yolo_rows={item.get('yolo_rows')}"
        )


def print_gui_display_frame_rate_summary(gui_fps: dict[str, Any]) -> None:
    if not gui_fps:
        return

    def bucket_text(bucket_name: str) -> str:
        bucket = gui_fps.get(bucket_name)
        bucket = bucket if isinstance(bucket, dict) else {}
        return (
            f"samples={bucket.get('sample_count')} "
            f"p05={fmt_float(bucket.get('p05_fps'), 1)} "
            f"p50={fmt_float(bucket.get('p50_fps'), 1)} "
            f"mean={fmt_float(bucket.get('mean_fps'), 1)}"
        )

    def timing_text(bucket_name: str) -> str:
        timings = gui_fps.get("timings")
        timings = timings if isinstance(timings, dict) else {}
        bucket = timings.get(bucket_name)
        bucket = bucket if isinstance(bucket, dict) else {}
        return (
            f"samples={bucket.get('sample_count')} "
            f"p50={fmt_float(bucket.get('p50_ms'), 3)}ms "
            f"p95={fmt_float(bucket.get('p95_ms'), 3)}ms "
            f"mean={fmt_float(bucket.get('mean_ms'), 3)}ms"
        )

    def percent_text(value: Any) -> str:
        return "n/a" if value is None else f"{float(value) * 100.0:.0f}%"

    print("\nGUI FPS")
    if "yolo_speed_graphs_enabled" in gui_fps:
        print(f"  yolo-speed-graphs-enabled: {gui_fps.get('yolo_speed_graphs_enabled')}")
    print(f"  overall: {bucket_text('overall')}")
    print(f"  crop-preview-visible: {bucket_text('crop_preview_visible')}")
    print(f"  crop-preview-hidden: {bucket_text('crop_preview_hidden')}")
    if isinstance(gui_fps.get("timings"), dict):
        timings = gui_fps["timings"]
        diagnosis = gui_fps.get("timing_diagnosis")
        diagnosis = diagnosis if isinstance(diagnosis, dict) else gui_summary.summarize_gui_timing_diagnosis(gui_fps)
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
                f"{fmt_float(diagnosis.get('dominant_timing_p95_ms'), 3)}ms "
                f"frame-total={fmt_float(diagnosis.get('frame_total_p95_ms'), 3)}ms "
                f"share={percent_text(diagnosis.get('dominant_timing_fraction_of_frame_total_p95'))}"
            )
        print(
            f"    upload-counts: main={timings.get('main_texture_upload_count')} "
            f"crop={timings.get('crop_texture_upload_count')}"
        )


def main() -> int:
    args = parse_args()
    if (
        args.expect_external_crop_recorder_gpu_id is not None
        and args.expect_external_crop_recorder_gpu_id < 0
    ):
        raise SystemExit("--expect-external-crop-recorder-gpu-id must be >= 0")
    expected_external_recorder_gpu_by_serial = parse_expected_serial_int_map(
        args.expect_external_crop_recorder_gpu,
        "--expect-external-crop-recorder-gpu",
    )
    recording_folder = resolve_requested_recording_folder(args)
    summary = gui_summary.summarize(recording_folder, args.steady_after_frame, args.ffprobe)
    snapshot = read_json(recording_folder / "recording_snapshot.json")
    ptp_sync_summary = read_json(recording_folder / "ptp_sync_summary.json")
    cameras = artifact_cameras(summary, snapshot, parse_expected_cameras(args.expected_cameras))

    reporter = Reporter(verbose=not args.json)
    if not args.json:
        print(f"GUI PTP recording validation: {recording_folder}")
        print(f"Cameras: {', '.join('Cam' + serial for serial in cameras) if cameras else 'none'}")

    if not cameras:
        reporter.fail("no cameras discovered or requested")
    else:
        check_sync_config(
            reporter,
            snapshot,
            summary,
            cameras,
            args.expected_sync_mode,
            args.expected_ptp_mode,
        )
        check_ptp_counters(
            reporter,
            summary,
            ptp_sync_summary,
            cameras,
            args.expect_ptp_register_read_decimate,
            args.skip_ptp_register_decimate_check,
        )
        check_recording_session_manifest(reporter, recording_folder, snapshot, cameras)
        check_pipeline(
            reporter,
            summary,
            cameras,
            args.expect_display_preview_max_fps,
        )
        video_sanity = check_videos(
            reporter,
            summary,
            cameras,
            args.ffprobe,
            args.ffmpeg,
            args.min_main_video_bitrate_mbps,
            args.skip_video_content_check,
            args.max_video_black_fraction,
            args.min_video_stddev,
        )
        check_yolo(
            reporter,
            summary,
            cameras,
            args.max_yolo_queue_p95_ms,
            args.max_yolo_steady_p95_ms,
            args.max_ptp_done_p95_ms,
        )
        crop_preview_summary = check_crop_preview_counters(
            reporter,
            recording_folder,
            snapshot,
            cameras,
            args.expect_crop_preview_max_fps,
            args.expect_crop_preview_display_enabled,
            args.expect_crop_preview_disabled,
            args.min_crop_frame_pool_size,
            args.require_crop_preview_sampling,
            args.require_crop_preview_counters,
        )
        crop_recording_summary = check_crop_recording_artifacts(
            reporter,
            recording_folder,
            snapshot,
            summary,
            cameras,
            args.require_crop_recording_artifacts,
            args.ffprobe,
            expected_external_queue_depth=args.expect_external_crop_encode_queue_depth,
            expected_external_recorder_gpu_id=args.expect_external_crop_recorder_gpu_id,
            expected_external_recorder_gpu_by_serial=expected_external_recorder_gpu_by_serial,
            max_external_queue_high_water=args.max_external_crop_encode_queue_high_water,
            max_external_enqueue_age_p95_ms=args.max_external_crop_enqueue_age_p95_ms,
            require_external_crop_backend_metadata=args.require_external_crop_backend_metadata,
        )
        gui_display_frame_rate_summary = check_gui_display_frame_rate(
            reporter,
            snapshot,
            args.min_gui_recording_fps_p05,
            args.min_gui_crop_preview_visible_fps_p05,
            args.min_gui_crop_preview_hidden_fps_p05,
            args.expect_gui_stream_downsample,
            args.expect_display_preview_max_fps,
            args.expect_yolo_speed_graphs_enabled,
            args.require_gui_timing_telemetry,
        )
    if not cameras:
        video_sanity = {}
        crop_preview_summary = {}
        crop_recording_summary = {}
        gui_display_frame_rate_summary = {}

    camera_summary = compact_camera_summary(summary, cameras, video_sanity)
    result = {
        "schema_version": 1,
        "recording_folder": str(recording_folder),
        "status": "fail" if reporter.failures else "pass",
        "passes": reporter.passes,
        "warnings": reporter.warnings,
        "failures": reporter.failures,
        "summary": camera_summary,
        "crop_preview": crop_preview_summary,
        "crop_recording": crop_recording_summary,
        "gui_display_frame_rate": gui_display_frame_rate_summary,
    }

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_camera_summary(camera_summary)
        print_crop_preview_summary(crop_preview_summary)
        print_crop_recording_summary(crop_recording_summary)
        print_gui_display_frame_rate_summary(gui_display_frame_rate_summary)
        if reporter.failures:
            print(f"\nResult: FAIL ({len(reporter.failures)} failures, {len(reporter.warnings)} warnings)")
        else:
            print(f"\nResult: PASS ({len(reporter.warnings)} warnings)")
    return 1 if reporter.failures else 0


if __name__ == "__main__":
    sys.exit(main())
