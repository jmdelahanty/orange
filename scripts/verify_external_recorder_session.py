#!/usr/bin/env python3
"""Verify the diagnostic external-recorder session contract."""

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

from recording_output_validation import recording_clip_output_contract_errors


CONTRACT_SCHEMA_ID = "orange.external_recorder.contract"
SUMMARY_SCHEMA_ID = "orange.external_recorder.summary"
STATUS_SCHEMA_ID = "orange.external_recorder.status"
DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    default_ffprobe = (
        str(DEFAULT_FFPROBE)
        if DEFAULT_FFPROBE.exists()
        else shutil.which("ffprobe") or "ffprobe"
    )
    parser = argparse.ArgumentParser(
        description=(
            "Verify an external_recorder_ipc_probe artifact root against "
            "fixed.external_recorder_contract and analytics runs.json."
        )
    )
    parser.add_argument(
        "artifact_root",
        nargs="?",
        help=(
            "External recorder artifact root. May be omitted when "
            "--analytics-root points at a folder containing "
            "external_recorder_contract.json, external_recorder_supervisor_plan.json, "
            "recording_session.json recording backend metadata, or an "
            "experiment_spec.json with fixed.external_recorder_contract.artifact_root."
        ),
    )
    parser.add_argument(
        "--analytics-root",
        help="Analytics experiment root containing experiment_spec.json/runs.json or recording_session.json.",
    )
    parser.add_argument(
        "--spec",
        help="Experiment spec JSON. Defaults to <analytics-root>/experiment_spec.json when present.",
    )
    parser.add_argument(
        "--camera",
        action="append",
        help="Camera serial to verify. May be repeated. Defaults to contract streams or summaries found in artifact root.",
    )
    parser.add_argument(
        "--allow-missing-video-sanity",
        action="store_true",
        help="Allow legacy artifacts without Cam*_external_video_sanity.json.",
    )
    parser.add_argument(
        "--ffprobe",
        default=default_ffprobe,
        help="ffprobe executable path for a basic MP4 fallback check.",
    )
    parser.add_argument(
        "--expect-encode-queue-depth",
        type=int,
        default=None,
        help="Optional expected recorder summary encode_queue_depth for each selected stream.",
    )
    parser.add_argument(
        "--max-encode-queue-high-water",
        type=int,
        default=None,
        help=(
            "Optional maximum recorder encode queue high-water. Uses "
            "summary encode_queue_high_water when present and falls back to "
            "the detach CSV encode_queue_depth column for older summaries."
        ),
    )
    parser.add_argument(
        "--max-enqueue-age-p95-ms",
        type=float,
        default=None,
        help="Optional maximum recorder external_encode.enqueue_age_p95_ms for each selected stream.",
    )
    parser.add_argument(
        "--require-recorder-status",
        action="store_true",
        help=(
            "Require each selected stream to have a completed "
            "orange.external_recorder.status heartbeat sidecar. New contracts "
            "can also set require_status=true."
        ),
    )
    parser.add_argument(
        "--require-recorder-runtime-status",
        action="store_true",
        help=(
            "Require external_recorder_supervisor_runtime.json to contain a "
            "valid parsed recorder_status entry for each selected status sidecar."
        ),
    )
    parser.add_argument(
        "--require-recorder-storage-preflight",
        action="store_true",
        help=(
            "Require summary/status recorder storage_preflight payloads, and "
            "runtime parsed storage fields when runtime status is required. "
            "New contracts can also set require_storage_preflight=true."
        ),
    )
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except FileNotFoundError as exc:
        raise VerificationError(f"missing JSON file: {path}") from exc
    except json.JSONDecodeError as exc:
        raise VerificationError(f"invalid JSON file {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise VerificationError(f"expected object JSON in {path}")
    return payload


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def as_int(value: Any, field: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise VerificationError(f"invalid integer {field}={value!r}") from exc


def optional_int(value: Any, field: str) -> int | None:
    if value is None:
        return None
    return as_int(value, field)


def optional_float(value: Any, field: str) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError) as exc:
        raise VerificationError(f"invalid float {field}={value!r}") from exc


def recording_control_for(contract: dict[str, Any], stream: dict[str, Any]) -> dict[str, Any]:
    value = stream.get("recording_control")
    if isinstance(value, dict):
        return value
    value = contract.get("recording_control")
    return value if isinstance(value, dict) else {}


def recording_control_from_summary(summary: dict[str, Any]) -> dict[str, Any]:
    value = summary.get("recording_control")
    if isinstance(value, dict):
        return value
    rolling = summary.get("rolling_output")
    if isinstance(rolling, dict) and rolling.get("enabled") is True:
        return {
            "record_for_seconds": rolling.get("record_for_seconds", 0),
            "clip_seconds": rolling.get("clip_seconds", 0),
        }
    return {}


def path_from(value: Any, base: Path) -> Path:
    path = Path(str(value))
    return path if path.is_absolute() else base / path


def path_key(path: Path) -> str:
    return str(path.expanduser().resolve(strict=False))


def derive_status_path(summary_path: Path) -> Path:
    name = summary_path.name
    if name.endswith("_summary.json"):
        return summary_path.with_name(name[: -len("_summary.json")] + "_status.json")
    return summary_path.with_name(summary_path.stem + "_status.json")


def load_spec(args: argparse.Namespace) -> dict[str, Any] | None:
    if args.spec:
        return read_json(Path(args.spec).expanduser())
    if args.analytics_root:
        spec_path = Path(args.analytics_root).expanduser() / "experiment_spec.json"
        if spec_path.exists():
            return read_json(spec_path)
    return None


def contract_from_spec(spec: dict[str, Any] | None) -> dict[str, Any] | None:
    if spec is None:
        return None
    fixed = spec.get("fixed")
    if not isinstance(fixed, dict):
        return None
    contract = fixed.get("external_recorder_contract")
    if not isinstance(contract, dict):
        return None
    contract = dict(contract)
    recording_control = fixed.get("recording_control")
    if isinstance(recording_control, dict) and not isinstance(contract.get("recording_control"), dict):
        contract["recording_control"] = recording_control
    return contract


def artifact_root_from_payload(payload: dict[str, Any], base: Path) -> Path | None:
    value = payload.get("artifact_root")
    if isinstance(value, str) and value:
        return path_from(value, base)

    backend = payload.get("recording_backend")
    if isinstance(backend, dict):
        value = backend.get("artifact_root")
        if isinstance(value, str) and value:
            return path_from(value, base)
    return None


def resolve_artifact_root(
    args: argparse.Namespace,
    analytics_root: Path | None,
    contract: dict[str, Any] | None,
) -> Path:
    if args.artifact_root:
        return Path(args.artifact_root).expanduser()

    if analytics_root is not None:
        for filename in (
            "external_recorder_contract.json",
            "external_recorder_supervisor_plan.json",
            "recording_session.json",
        ):
            path = analytics_root / filename
            if not path.exists():
                continue
            root = artifact_root_from_payload(read_json(path), analytics_root)
            if root is not None:
                return root.expanduser()

    if contract is not None and analytics_root is not None:
        root = artifact_root_from_payload(contract, analytics_root)
        if root is not None:
            return root.expanduser()
    if contract is not None:
        root = artifact_root_from_payload(contract, Path.cwd())
        if root is not None:
            return root.expanduser()

    raise VerificationError("artifact_root is required unless it can be derived from --analytics-root")


def synthesize_contract(artifact_root: Path, cameras: list[str] | None) -> dict[str, Any]:
    summaries = sorted(artifact_root.glob("Cam*_external_summary.json"))
    streams: dict[str, Any] = {}
    for summary_path in summaries:
        serial = summary_path.name.removeprefix("Cam").removesuffix("_external_summary.json")
        if cameras and serial not in cameras:
            continue
        streams[serial] = {
            "stream_id": serial,
            "summary_json": str(summary_path),
            "status_json": str(derive_status_path(summary_path)),
            "video_sanity_json": str(artifact_root / f"Cam{serial}_external_video_sanity.json"),
            "mp4": str(artifact_root / f"Cam{serial}_external.mp4"),
            "gop_routing_csv": str(artifact_root / f"Cam{serial}_external_gop_routing.csv"),
            "routing_policy": "gop_modulo",
        }
    return {
        "schema_id": CONTRACT_SCHEMA_ID,
        "schema_version": 1,
        "mode": "diagnostic_ipc_v1",
        "artifact_root": str(artifact_root),
        "require_summary": True,
        "require_video_sanity": False,
        "require_merged_mp4": True,
        "require_gop_routing": True,
        "streams": streams,
    }


def selected_streams(contract: dict[str, Any], cameras: list[str] | None) -> dict[str, dict[str, Any]]:
    streams = contract.get("streams")
    require(isinstance(streams, dict) and bool(streams), "external recorder contract has no streams")
    selected: dict[str, dict[str, Any]] = {}
    for serial, stream in streams.items():
        if cameras and serial not in cameras:
            continue
        require(isinstance(stream, dict), f"contract stream {serial} is not an object")
        selected[str(serial)] = stream
    require(bool(selected), "no external recorder streams selected for verification")
    return selected


def ffprobe_video(path: Path, ffprobe: str) -> None:
    command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,duration:format=size,duration",
        "-of",
        "json",
        str(path),
    ]
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except FileNotFoundError as exc:
        raise VerificationError(f"ffprobe executable not found: {ffprobe}") from exc
    except subprocess.TimeoutExpired as exc:
        raise VerificationError(f"ffprobe timed out for {path}") from exc
    if result.returncode != 0:
        raise VerificationError(f"ffprobe failed for {path}: {result.stderr.strip()}")
    payload = json.loads(result.stdout)
    streams = payload.get("streams")
    require(isinstance(streams, list) and bool(streams), f"ffprobe found no video stream in {path}")
    stream = streams[0]
    require(as_int(stream.get("width"), "ffprobe width") > 0, f"invalid MP4 width: {path}")
    require(as_int(stream.get("height"), "ffprobe height") > 0, f"invalid MP4 height: {path}")


def verify_video_sanity(path: Path, allow_missing: bool) -> str:
    if not path.exists():
        if allow_missing:
            return "missing_allowed"
        raise VerificationError(f"missing video sanity JSON: {path}")
    payload = read_json(path)
    require(payload.get("content_checked") is True, f"video sanity did not run: {path}")
    require(payload.get("content_valid") is True, f"video sanity failed: {path}")
    require(payload.get("status") == "pass", f"video sanity status is not pass: {path}")
    return "pass"


def count_csv_data_rows(path: Path) -> int:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            return sum(1 for _ in csv.DictReader(handle))
    except FileNotFoundError as exc:
        raise VerificationError(f"missing CSV file: {path}") from exc


def read_csv_rows(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            rows = list(reader)
            return list(reader.fieldnames or []), rows
    except FileNotFoundError as exc:
        raise VerificationError(f"missing CSV file: {path}") from exc


def csv_int(row: dict[str, str], field: str, path: Path, row_index: int) -> int | None:
    value = row.get(field)
    if value in (None, ""):
        return None
    try:
        return int(value)
    except ValueError as exc:
        raise VerificationError(f"invalid integer {field} row {row_index} in {path}: {value!r}") from exc


def detach_queue_high_water(summary: dict[str, Any], summary_path: Path, artifact_root: Path) -> int | None:
    outputs = summary.get("outputs")
    outputs = outputs if isinstance(outputs, dict) else {}
    detach_csv = outputs.get("detach_csv")
    if isinstance(detach_csv, str) and detach_csv:
        detach_path = path_from(detach_csv, artifact_root)
    elif summary_path.name.endswith("_summary.json"):
        detach_path = summary_path.with_name(
            summary_path.name[: -len("_summary.json")] + "_detach.csv"
        )
    else:
        return None
    if not detach_path.exists():
        return None
    _, rows = read_csv_rows(detach_path)
    values = [
        value
        for index, row in enumerate(rows, start=2)
        for value in [csv_int(row, "encode_queue_depth", detach_path, index)]
        if value is not None
    ]
    return max(values) if values else None


def require_no_mp4_queue_overflow(payload: dict[str, Any], label: str) -> None:
    if payload.get("mp4_queue_overflowed") is True:
        raise VerificationError(f"{label} reports mp4_queue_overflowed=true")
    events = optional_int(payload.get("mp4_queue_overflow_events"), f"{label}.mp4_queue_overflow_events")
    if events is not None:
        require(events == 0, f"{label} mp4_queue_overflow_events={events}")


def require_storage_preflight_ok(
    payload: dict[str, Any],
    label: str,
    require_present: bool = False,
) -> None:
    storage = payload.get("storage_preflight")
    if not isinstance(storage, dict):
        require(not require_present, f"{label} missing storage_preflight")
        return
    if require_present:
        require(storage.get("checked") is True, f"{label} storage_preflight.checked is not true")
        require(storage.get("ok") is True, f"{label} storage_preflight.ok is not true")
    else:
        require(storage.get("ok") is not False, f"{label} storage_preflight.ok=false")
    require(storage.get("low_space") is not True, f"{label} storage_preflight.low_space=true")
    paths = storage.get("paths")
    if require_present:
        require(isinstance(paths, list) and len(paths) > 0, f"{label} storage_preflight.paths missing")
    if isinstance(paths, list):
        for index, path in enumerate(paths):
            if not isinstance(path, dict):
                continue
            path_label = path.get("path") or f"path[{index}]"
            require(
                path.get("ok") is not False,
                f"{label} storage path {path_label} ok=false: {path.get('error')}",
            )
            require(
                path.get("meets_min_free") is not False,
                f"{label} storage path {path_label} below min_free_bytes",
            )
            require(
                path.get("below_warning") is not True,
                f"{label} storage path {path_label} below low_space_warning_bytes",
            )


def read_metadata_frame_rows(path: Path) -> list[dict[str, int]]:
    fieldnames, rows = read_csv_rows(path)
    field_set = set(fieldnames)
    if "recording_frame_id" in field_set:
        frame_id_column = "recording_frame_id"
    elif "frame_id" in field_set:
        frame_id_column = "frame_id"
    else:
        raise VerificationError(
            f"metadata CSV missing recording_frame_id/frame_id column: {path}"
        )

    for required_column in ("timestamp", "timestamp_sys"):
        require(
            required_column in field_set,
            f"metadata CSV missing {required_column} column: {path}",
        )

    parsed_rows: list[dict[str, int]] = []
    for row_index, row in enumerate(rows, start=2):
        parsed_rows.append(
            {
                "recording_frame_id": as_int(
                    row.get(frame_id_column),
                    f"{frame_id_column} row {row_index} in {path}",
                ),
                "timestamp": as_int(
                    row.get("timestamp"),
                    f"timestamp row {row_index} in {path}",
                ),
                "timestamp_sys": as_int(
                    row.get("timestamp_sys"),
                    f"timestamp_sys row {row_index} in {path}",
                ),
            }
        )
    return parsed_rows


def verify_rolling_output(
    artifact_root: Path,
    serial: str,
    summary: dict[str, Any],
    stream: dict[str, Any],
    contract: dict[str, Any],
    frames_encoded: int,
    ffprobe: str,
) -> list[dict[str, Any]]:
    recording_control = recording_control_for(contract, stream)
    summary_recording_control = recording_control_from_summary(summary)
    if summary_recording_control:
        merged_recording_control = dict(summary_recording_control)
        merged_recording_control.update(recording_control)
        recording_control = merged_recording_control
    clip_seconds = as_int(recording_control.get("clip_seconds", 0), "recording_control.clip_seconds")
    record_for_seconds = as_int(
        recording_control.get("record_for_seconds", 0),
        "recording_control.record_for_seconds",
    )
    if clip_seconds <= 0:
        return []
    require(record_for_seconds > 0, f"rolling output for {serial} requires record_for_seconds > 0")

    rolling = summary.get("rolling_output")
    require(isinstance(rolling, dict), f"summary missing rolling_output for {serial}")
    require(rolling.get("enabled") is True, f"rolling_output disabled for {serial}")
    require(
        rolling.get("implementation") == "external_recorder_gop_boundary_writer_rotation",
        f"unexpected rolling implementation for {serial}: {rolling.get('implementation')!r}",
    )
    require(as_int(rolling.get("clip_seconds"), "rolling_output.clip_seconds") == clip_seconds, f"clip_seconds mismatch for {serial}")
    clips = rolling.get("clips")
    require(isinstance(clips, list) and bool(clips), f"rolling_output has no clips for {serial}")
    require(as_int(rolling.get("clip_count"), "rolling_output.clip_count") == len(clips), f"clip_count mismatch for {serial}")
    clip_span_frames = as_int(rolling.get("clip_span_frames", 0), "rolling_output.clip_span_frames")
    target_frame_count = as_int(rolling.get("target_frame_count", 0), "rolling_output.target_frame_count")
    terminal_tail_coalesce_frames = as_int(
        rolling.get("terminal_tail_coalesce_frames", 0),
        "rolling_output.terminal_tail_coalesce_frames",
    )
    terminal_tail_coalesced_frames = as_int(
        rolling.get("terminal_tail_coalesced_frames", 0),
        "rolling_output.terminal_tail_coalesced_frames",
    )
    if record_for_seconds > clip_seconds:
        require(len(clips) >= 2, f"expected multiple rolling clips for {serial}")
    if (
        target_frame_count > 0
        and clip_span_frames > 0
        and frames_encoded > target_frame_count
        and frames_encoded - target_frame_count <= terminal_tail_coalesce_frames
    ):
        expected_clip_count = math.ceil(target_frame_count / clip_span_frames)
        require(
            len(clips) == expected_clip_count,
            f"terminal tail was not coalesced for {serial}: clips={len(clips)} expected={expected_clip_count}",
        )
        require(
            terminal_tail_coalesced_frames == frames_encoded - target_frame_count,
            f"terminal tail coalesced frame count mismatch for {serial}: "
            f"{terminal_tail_coalesced_frames} vs {frames_encoded - target_frame_count}",
        )

    expected_next_frame = 1
    total_clip_frames = 0
    verified_clips: list[dict[str, Any]] = []
    for expected_index, clip in enumerate(clips):
        require(isinstance(clip, dict), f"rolling clip {expected_index} is not an object for {serial}")
        require(clip.get("clip_index") == expected_index, f"unexpected clip_index for {serial}: {clip.get('clip_index')!r}")
        require(clip.get("clip_id") == f"clip_{expected_index:06d}", f"unexpected clip_id for {serial}: {clip.get('clip_id')!r}")
        require(clip.get("failed") is False, f"rolling clip failed for {serial}: {clip.get('clip_id')}")

        mp4_path = path_from(clip.get("mp4"), artifact_root)
        metadata_path = path_from(clip.get("metadata"), artifact_root)
        keyframe_path = path_from(clip.get("keyframes"), artifact_root)
        require(mp4_path.exists() and mp4_path.stat().st_size > 0, f"missing rolling clip MP4 for {serial}: {mp4_path}")
        require(metadata_path.exists() and metadata_path.stat().st_size > 0, f"missing rolling metadata for {serial}: {metadata_path}")
        require(keyframe_path.exists() and keyframe_path.stat().st_size > 0, f"missing rolling keyframe sidecar for {serial}: {keyframe_path}")
        ffprobe_video(mp4_path, ffprobe)

        frame_count = as_int(clip.get("frame_count"), "rolling clip frame_count")
        packet_count = as_int(clip.get("packets_written"), "rolling clip packets_written")
        first_frame = as_int(clip.get("first_recording_frame_id"), "rolling clip first_recording_frame_id")
        last_frame = as_int(clip.get("last_recording_frame_id"), "rolling clip last_recording_frame_id")
        require(frame_count > 0, f"rolling clip has no frames for {serial}: {clip.get('clip_id')}")
        require(packet_count > 0, f"rolling clip has no packets for {serial}: {clip.get('clip_id')}")
        require(first_frame == expected_next_frame, f"rolling frame continuity break for {serial}: expected {expected_next_frame}, got {first_frame}")
        require(last_frame == first_frame + frame_count - 1, f"rolling frame range mismatch for {serial}: {clip.get('clip_id')}")
        metadata_rows = read_metadata_frame_rows(metadata_path)
        require(
            len(metadata_rows) == frame_count,
            f"rolling metadata rows mismatch for {serial}: {metadata_path}",
        )
        require(
            metadata_rows[0]["recording_frame_id"] == first_frame,
            f"rolling metadata first frame mismatch for {serial}: {metadata_path}",
        )
        require(
            metadata_rows[-1]["recording_frame_id"] == last_frame,
            f"rolling metadata last frame mismatch for {serial}: {metadata_path}",
        )
        for left, right in zip(metadata_rows, metadata_rows[1:]):
            left_frame = left["recording_frame_id"]
            right_frame = right["recording_frame_id"]
            require(
                right_frame == left_frame + 1,
                (
                    f"rolling metadata frame gap inside {metadata_path}: "
                    f"{left_frame} -> {right_frame}"
                ),
            )
        verified_clips.append(
            {
                "clip_index": expected_index,
                "clip_id": str(clip.get("clip_id")),
                "first_recording_frame_id": first_frame,
                "last_recording_frame_id": last_frame,
                "frame_count": frame_count,
                "packet_count": packet_count,
                "mp4": str(mp4_path),
                "metadata": str(metadata_path),
                "keyframes": str(keyframe_path),
            }
        )
        total_clip_frames += frame_count
        expected_next_frame = last_frame + 1

    require(total_clip_frames == frames_encoded, f"rolling clip frames != frames_encoded for {serial}: {total_clip_frames} vs {frames_encoded}")
    return verified_clips


def runtime_processes_by_status_path(runtime_path: Path) -> dict[str, dict[str, Any]]:
    runtime = read_json(runtime_path)
    processes = runtime.get("processes")
    require(isinstance(processes, list), f"external recorder runtime missing processes: {runtime_path}")
    by_path: dict[str, dict[str, Any]] = {}
    for process in processes:
        if not isinstance(process, dict):
            continue
        status_json_path = process.get("status_json_path")
        if isinstance(status_json_path, str) and status_json_path:
            by_path[path_key(Path(status_json_path))] = process
    return by_path


def verify_status_sidecar(
    artifact_root: Path,
    serial: str,
    stream: dict[str, Any],
    summary_path: Path,
    summary: dict[str, Any],
    require_status: bool,
    require_runtime_status: bool,
    require_storage_preflight: bool,
) -> dict[str, Any] | None:
    status_path = path_from(
        stream.get("status_json") or derive_status_path(summary_path),
        artifact_root,
    )
    status_exists = status_path.exists()
    if not status_exists and not (require_status or require_runtime_status):
        return None
    require(status_exists, f"missing external recorder status JSON: {status_path}")

    status = read_json(status_path)
    require(status.get("schema_id") == STATUS_SCHEMA_ID, f"unexpected status schema_id in {status_path}")
    require(status.get("schema_version") == 1, f"unexpected status schema_version in {status_path}")
    require(status.get("tool") == "external_recorder_ipc_probe", f"unexpected status tool in {status_path}")
    require(str(status.get("stream_id")) == str(stream.get("stream_id", serial)), f"status stream_id mismatch in {status_path}")
    require(status.get("status") == "completed", f"recorder status is not completed in {status_path}")
    require(status.get("worker_failed") is False, f"recorder status worker_failed=true in {status_path}")
    require_storage_preflight_ok(
        status,
        f"status for {serial}",
        require_storage_preflight,
    )
    error = status.get("error")
    require(error in (None, ""), f"recorder status reports error in {status_path}: {error}")

    heartbeat_sequence = as_int(status.get("heartbeat_sequence"), "status heartbeat_sequence")
    require(heartbeat_sequence > 0, f"recorder status heartbeat missing or zero in {status_path}")

    for field in (
        "frames_received",
        "acks_sent",
        "detach_copied",
        "encode_enqueued",
        "encode_skipped",
        "encode_dropped",
        "frames_encoded",
    ):
        if field in status and field in summary:
            require(
                as_int(status.get(field), f"status {field}") == as_int(summary.get(field), field),
                f"status {field} does not match summary for {serial}",
            )

    status_rolling_summary = verify_status_rolling_progress(serial, status, summary)

    runtime_path = artifact_root / "external_recorder_supervisor_runtime.json"
    runtime_present = runtime_path.exists()
    runtime_status: dict[str, Any] | None = None
    if runtime_present or require_runtime_status:
        require(runtime_present, f"missing external recorder supervisor runtime: {runtime_path}")
        process = runtime_processes_by_status_path(runtime_path).get(path_key(status_path))
        require(process is not None, f"runtime missing process for status JSON: {status_path}")
        status_json_path = process.get("status_json_path")
        require(
            isinstance(status_json_path, str) and path_key(Path(status_json_path)) == path_key(status_path),
            f"runtime status_json_path mismatch for {serial}",
        )
        candidate = process.get("recorder_status")
        require(isinstance(candidate, dict), f"runtime missing recorder_status for {serial}")
        require(candidate.get("present") is True, f"runtime recorder_status not present for {serial}")
        require(candidate.get("valid") is True, f"runtime recorder_status invalid for {serial}")
        require(candidate.get("status") == status.get("status"), f"runtime recorder status mismatch for {serial}")
        require(
            as_int(candidate.get("heartbeat_sequence"), "runtime heartbeat_sequence") == heartbeat_sequence,
            f"runtime heartbeat does not match status sidecar for {serial}",
        )
        for field in ("frames_received", "acks_sent", "frames_encoded"):
            if field in candidate and field in status:
                require(
                    as_int(candidate.get(field), f"runtime {field}") ==
                    as_int(status.get(field), f"status {field}"),
                    f"runtime {field} does not match status sidecar for {serial}",
                )
        if status_rolling_summary:
            compare_runtime_rolling_status(serial, candidate, status_rolling_summary)
        if require_storage_preflight:
            require(candidate.get("storage_checked") is True,
                    f"runtime storage_checked is not true for {serial}")
            require(candidate.get("storage_ok") is True,
                    f"runtime storage_ok is not true for {serial}")
            require(candidate.get("storage_low_space") is not True,
                    f"runtime storage_low_space=true for {serial}")
            require(as_int(candidate.get("storage_path_count"), "runtime storage_path_count") > 0,
                    f"runtime storage_path_count missing for {serial}")
            require(
                as_int(candidate.get("storage_paths_ok_count"), "runtime storage_paths_ok_count") ==
                as_int(candidate.get("storage_path_count"), "runtime storage_path_count"),
                f"runtime storage path ok count mismatch for {serial}",
            )
        runtime_status = candidate

    return {
        "status_path": str(status_path),
        "status": status.get("status"),
        "heartbeat_sequence": heartbeat_sequence,
        **status_rolling_summary,
        "runtime_path": str(runtime_path) if runtime_present else "",
        "runtime_status": runtime_status.get("status") if runtime_status is not None else None,
        "runtime_heartbeat_sequence": (
            as_int(runtime_status.get("heartbeat_sequence"), "runtime heartbeat_sequence")
            if runtime_status is not None else None
        ),
    }


def verify_status_rolling_progress(
    serial: str,
    status: dict[str, Any],
    summary: dict[str, Any],
) -> dict[str, Any]:
    rolling = summary.get("rolling_output")
    rolling = rolling if isinstance(rolling, dict) else {}
    if rolling.get("enabled") is not True:
        return {}

    status_rolling = status.get("rolling")
    require(isinstance(status_rolling, dict), f"status missing rolling progress for {serial}")
    require(status_rolling.get("enabled") is True, f"status rolling progress disabled for {serial}")
    require(
        status_rolling.get("implementation") == "external_recorder_gop_boundary_writer_rotation",
        f"unexpected status rolling implementation for {serial}: {status_rolling.get('implementation')!r}",
    )
    for field in (
        "record_for_seconds",
        "clip_seconds",
        "clip_span_frames",
        "target_frame_count",
    ):
        require(
            as_int(status_rolling.get(field), f"status rolling {field}") ==
            as_int(rolling.get(field), f"rolling_output {field}"),
            f"status rolling {field} does not match summary for {serial}",
        )

    clips = rolling.get("clips")
    require(isinstance(clips, list) and bool(clips), f"rolling_output has no clips for {serial}")
    require(
        as_int(status_rolling.get("completed_clip_count"), "status rolling completed_clip_count") ==
        len(clips),
        f"status rolling completed_clip_count does not match summary for {serial}",
    )
    last_clip = clips[-1]
    require(isinstance(last_clip, dict), f"rolling_output final clip is invalid for {serial}")
    expected_status = "failed" if last_clip.get("failed") is True else "completed"
    checks = (
        (
            "last_completed_clip_index",
            "clip_index",
            "status rolling last_completed_clip_index",
        ),
        (
            "last_completed_clip_last_recording_frame_id",
            "last_recording_frame_id",
            "status rolling last_completed_clip_last_recording_frame_id",
        ),
        (
            "last_completed_clip_frame_count",
            "frame_count",
            "status rolling last_completed_clip_frame_count",
        ),
    )
    for status_field, summary_field, label in checks:
        require(
            as_int(status_rolling.get(status_field), label) ==
            as_int(last_clip.get(summary_field), f"rolling_output final {summary_field}"),
            f"status rolling {status_field} does not match summary for {serial}",
        )
    require(
        status_rolling.get("last_rollover_status") == expected_status,
        f"status rolling last_rollover_status does not match summary for {serial}",
    )

    return {
        "rolling_current_clip_index": as_int(
            status_rolling.get("current_clip_index"),
            "status rolling current_clip_index",
        ),
        "rolling_next_rollover_at_recording_frame_id": as_int(
            status_rolling.get("next_rollover_at_recording_frame_id"),
            "status rolling next_rollover_at_recording_frame_id",
        ),
        "rolling_frames_until_next_rollover": as_int(
            status_rolling.get("frames_until_next_rollover"),
            "status rolling frames_until_next_rollover",
        ),
        "rolling_completed_clip_count": as_int(
            status_rolling.get("completed_clip_count"),
            "status rolling completed_clip_count",
        ),
        "rolling_last_completed_clip_index": as_int(
            status_rolling.get("last_completed_clip_index"),
            "status rolling last_completed_clip_index",
        ),
        "rolling_last_rollover_status": status_rolling.get("last_rollover_status"),
    }


def compare_runtime_rolling_status(
    serial: str,
    runtime_status: dict[str, Any],
    status_rolling_summary: dict[str, Any],
) -> None:
    require(
        runtime_status.get("rolling_enabled") is True,
        f"runtime rolling_enabled does not match status sidecar for {serial}",
    )
    for field in (
        "rolling_current_clip_index",
        "rolling_next_rollover_at_recording_frame_id",
        "rolling_frames_until_next_rollover",
        "rolling_completed_clip_count",
        "rolling_last_completed_clip_index",
    ):
        require(
            as_int(runtime_status.get(field), f"runtime {field}") ==
            as_int(status_rolling_summary.get(field), f"status {field}"),
            f"runtime {field} does not match status sidecar for {serial}",
        )
    require(
        runtime_status.get("rolling_last_rollover_status") ==
        status_rolling_summary.get("rolling_last_rollover_status"),
        f"runtime rolling_last_rollover_status does not match status sidecar for {serial}",
    )


def verify_summary(
    artifact_root: Path,
    serial: str,
    stream: dict[str, Any],
    contract: dict[str, Any],
    ffprobe: str,
    allow_missing_video_sanity: bool,
    expected_encode_queue_depth: int | None,
    max_encode_queue_high_water: int | None,
    max_enqueue_age_p95_ms: float | None,
    require_recorder_status: bool,
    require_recorder_runtime_status: bool,
    require_recorder_storage_preflight: bool,
) -> dict[str, Any]:
    summary_path = path_from(
        stream.get("summary_json") or artifact_root / f"Cam{serial}_external_summary.json",
        artifact_root,
    )
    summary = read_json(summary_path)
    status_summary = verify_status_sidecar(
        artifact_root,
        serial,
        stream,
        summary_path,
        summary,
        require_recorder_status or bool(contract.get("require_status", False)),
        require_recorder_runtime_status or bool(contract.get("require_status_runtime", False)),
        require_recorder_storage_preflight or bool(contract.get("require_storage_preflight", False)),
    )
    schema_id = summary.get("schema_id")
    require(
        schema_id in (None, SUMMARY_SCHEMA_ID),
        f"unexpected external summary schema_id={schema_id!r} in {summary_path}",
    )
    require(summary.get("schema_version") == 1, f"unexpected summary schema_version in {summary_path}")
    require(summary.get("tool") == "external_recorder_ipc_probe", f"unexpected recorder tool in {summary_path}")
    require(str(summary.get("stream_id")) == str(stream.get("stream_id", serial)), f"stream_id mismatch in {summary_path}")
    require(summary.get("encode") is True, f"summary encode=false in {summary_path}")
    require(summary.get("worker_failed") is False, f"recorder worker_failed=true in {summary_path}")
    require_storage_preflight_ok(
        summary,
        f"summary for {serial}",
        require_recorder_storage_preflight or bool(contract.get("require_storage_preflight", False)),
    )

    frames_received = as_int(summary.get("frames_received"), "frames_received")
    acks_sent = as_int(summary.get("acks_sent"), "acks_sent")
    detach_copied = as_int(summary.get("detach_copied"), "detach_copied")
    encode_enqueued = as_int(summary.get("encode_enqueued"), "encode_enqueued")
    encode_skipped = as_int(summary.get("encode_skipped"), "encode_skipped")
    encode_dropped = as_int(summary.get("encode_dropped"), "encode_dropped")
    encode_queue_depth = optional_int(summary.get("encode_queue_depth"), "encode_queue_depth")
    encode_queue_high_water = optional_int(summary.get("encode_queue_high_water"), "encode_queue_high_water")
    if encode_queue_high_water is None:
        encode_queue_high_water = detach_queue_high_water(summary, summary_path, artifact_root)
    external_encode = summary.get("external_encode")
    external_encode = external_encode if isinstance(external_encode, dict) else {}
    require_no_mp4_queue_overflow(external_encode, f"external_encode for {serial}")
    enqueue_age_p95_ms = optional_float(
        external_encode.get("enqueue_age_p95_ms"),
        "external_encode.enqueue_age_p95_ms",
    )
    frames_encoded = as_int(summary.get("frames_encoded"), "frames_encoded")
    require(frames_received > 0, f"no frames received in {summary_path}")
    require(acks_sent == frames_received, f"acks_sent != frames_received in {summary_path}")
    require(
        encode_enqueued + encode_skipped + encode_dropped == frames_received,
        f"encode accounting does not sum to frames_received in {summary_path}",
    )
    require(detach_copied == encode_enqueued, f"detach_copied != encode_enqueued in {summary_path}")
    require(encode_dropped == 0, f"encode_dropped is nonzero in {summary_path}")
    require(frames_encoded == encode_enqueued, f"frames_encoded != encode_enqueued in {summary_path}")
    require(frames_encoded > 0, f"no frames encoded in {summary_path}")
    if encode_queue_depth is not None and encode_queue_high_water is not None:
        require(
            encode_queue_high_water <= encode_queue_depth,
            (
                f"encode_queue_high_water exceeds encode_queue_depth in {summary_path}: "
                f"{encode_queue_high_water} > {encode_queue_depth}"
            ),
        )
    if expected_encode_queue_depth is not None:
        require(
            encode_queue_depth == expected_encode_queue_depth,
            (
                f"encode_queue_depth mismatch in {summary_path}: "
                f"{encode_queue_depth} != {expected_encode_queue_depth}"
            ),
        )
    if max_encode_queue_high_water is not None:
        require(
            encode_queue_high_water is not None and
            encode_queue_high_water <= max_encode_queue_high_water,
            (
                f"encode_queue_high_water too high in {summary_path}: "
                f"{encode_queue_high_water} > {max_encode_queue_high_water}"
            ),
        )
    if max_enqueue_age_p95_ms is not None:
        require(
            enqueue_age_p95_ms is not None and enqueue_age_p95_ms <= max_enqueue_age_p95_ms,
            (
                f"enqueue_age_p95_ms too high in {summary_path}: "
                f"{enqueue_age_p95_ms} > {max_enqueue_age_p95_ms}"
            ),
        )

    expected_routing_policy = stream.get("routing_policy")
    if expected_routing_policy:
        require(
            summary.get("routing_policy") == expected_routing_policy,
            f"routing_policy mismatch in {summary_path}",
        )

    shards = summary.get("external_encode_shards")
    require(isinstance(shards, list) and bool(shards), f"summary has no external_encode_shards in {summary_path}")
    expected_gpus = stream.get("expected_shard_gpu_ids")
    if expected_gpus is not None:
        actual_gpus = [as_int(shard.get("assigned_gpu_id"), "assigned_gpu_id") for shard in shards]
        require(
            actual_gpus == [int(value) for value in expected_gpus],
            f"shard GPU ids mismatch for {serial}: expected {expected_gpus}, got {actual_gpus}",
        )
    require(as_int(summary.get("shard_count"), "shard_count") == len(shards), f"shard_count mismatch in {summary_path}")
    for shard in shards:
        require(shard.get("worker_failed") is False, f"shard worker_failed=true for {serial}")
        require(as_int(shard.get("frames_dropped"), "shard frames_dropped") == 0, f"shard dropped frames for {serial}")
        require(as_int(shard.get("frames_encoded"), "shard frames_encoded") > 0, f"shard encoded no frames for {serial}")
        require_no_mp4_queue_overflow(shard, f"shard {shard.get('assigned_shard_id')} for {serial}")

    merged = summary.get("merged_output")
    require(isinstance(merged, dict), f"summary missing merged_output in {summary_path}")
    require_no_mp4_queue_overflow(merged, f"merged_output for {serial}")
    if bool(contract.get("require_merged_mp4", True)) and len(shards) > 1:
        require(merged.get("enabled") is True, f"merged output disabled for {serial}")
        require(merged.get("failed") is False, f"merged output failed for {serial}")
        require(as_int(merged.get("pending_gops"), "merged pending_gops") == 0, f"merged output has pending GOPs for {serial}")
        require(as_int(merged.get("packets_written"), "merged packets_written") > 0, f"merged output wrote no packets for {serial}")

    mp4_path = path_from(stream.get("mp4") or summary.get("outputs", {}).get("mp4"), artifact_root)
    require(mp4_path.exists() and mp4_path.stat().st_size > 0, f"missing or empty external MP4: {mp4_path}")
    ffprobe_video(mp4_path, ffprobe)

    if bool(contract.get("require_gop_routing", True)):
        routing_path = path_from(
            stream.get("gop_routing_csv") or summary.get("outputs", {}).get("gop_routing_csv"),
            artifact_root,
        )
        require(count_csv_data_rows(routing_path) == frames_received, f"GOP routing rows do not match frames_received for {serial}")

    video_sanity_path = path_from(
        stream.get("video_sanity_json") or artifact_root / f"Cam{serial}_external_video_sanity.json",
        artifact_root,
    )
    sanity_status = verify_video_sanity(
        video_sanity_path,
        allow_missing_video_sanity or not bool(contract.get("require_video_sanity", True)),
    )
    rolling_clips = verify_rolling_output(
        artifact_root,
        serial,
        summary,
        stream,
        contract,
        frames_encoded,
        ffprobe,
    )

    return {
        "serial": serial,
        "summary_path": str(summary_path),
        "mp4_path": str(mp4_path),
        "frames_received": frames_received,
        "frames_encoded": frames_encoded,
        "encode_queue_depth": encode_queue_depth,
        "encode_queue_high_water": encode_queue_high_water,
        "enqueue_age_p95_ms": enqueue_age_p95_ms,
        "shard_count": len(shards),
        "routing_policy": summary.get("routing_policy"),
        "video_sanity": sanity_status,
        "rolling_clip_count": len(rolling_clips),
        "rolling_clips": rolling_clips,
        "recorder_status": status_summary,
    }


def verify_analytics_root(analytics_root: Path, serials: list[str]) -> list[Path]:
    runs_path = analytics_root / "runs.json"
    if not runs_path.exists():
        manifest_path = analytics_root / "recording_session.json"
        require(
            manifest_path.exists(),
            f"analytics root has neither runs.json nor recording_session.json: {analytics_root}",
        )
        return [analytics_root]

    runs_json = read_json(runs_path)
    rows: list[dict[str, Any]] = []
    recording_folders: list[Path] = []
    for run in runs_json.get("runs", []):
        for row in run.get("camera_results", []):
            if str(row.get("camera_serial")) in serials:
                rows.append(row)
                folder = run.get("recording_folder") or row.get("recording_folder")
                if isinstance(folder, str) and folder:
                    path = Path(folder)
                    if path not in recording_folders:
                        recording_folders.append(path)
    require(len(rows) == len(serials), f"runs.json did not contain one row per verified camera in {analytics_root}")
    for row in rows:
        serial = str(row.get("camera_serial"))
        require(row.get("recording_sink_mode") == "external_ipc", f"analytics row {serial} is not external_ipc")
        require(row.get("pass_fail") == "pass", f"analytics row {serial} pass_fail={row.get('pass_fail')!r}")
        require(as_int(row.get("external_ipc_failures_final", 0), "external_ipc_failures_final") == 0, f"external IPC failures for {serial}")
        require(as_int(row.get("external_ipc_ack_timeouts_final", 0), "external_ipc_ack_timeouts_final") == 0, f"external IPC ACK timeouts for {serial}")
        acked = as_int(row.get("external_ipc_frames_acked_final", 0), "external_ipc_frames_acked_final")
        submitted = as_int(row.get("submitted_frames_final", 0), "submitted_frames_final")
        require(submitted == 0 or acked >= submitted, f"external IPC ACKed fewer frames than submitted for {serial}")
    return recording_folders


def index_path_from_manifest(recording_folder: Path, indexes: dict[str, Any], field: str) -> Path:
    value = indexes.get(field)
    require(isinstance(value, str) and value, f"recording_session.indexes missing {field}")
    path = Path(value)
    return path if path.is_absolute() else recording_folder / path


def verify_analytics_recording_session_indexes(
    recording_folder: Path,
    manifest: dict[str, Any],
    rolling_summaries: list[dict[str, Any]],
) -> None:
    indexes = manifest.get("indexes")
    require(isinstance(indexes, dict), f"recording_session missing indexes: {recording_folder}")
    require(
        indexes.get("schema_id") == "orange.recording_session.indexes",
        f"recording_session indexes schema mismatch: {indexes.get('schema_id')!r}",
    )
    require(indexes.get("row_granularity") == "clip_camera", "recording_session indexes row_granularity mismatch")
    index_json_path = index_path_from_manifest(recording_folder, indexes, "clip_index_json")
    index_csv_path = index_path_from_manifest(recording_folder, indexes, "clip_index_csv")
    index = read_json(index_json_path)
    require(index.get("schema_id") == "orange.recording_clip_index", f"unexpected clip index schema: {index_json_path}")
    require(index.get("schema_version") == 1, f"unexpected clip index schema_version: {index_json_path}")
    require(index.get("session_id") == manifest.get("session_id"), "clip index session_id mismatch")
    require(index.get("mode") == "rolling_clips", "clip index mode mismatch")
    rows = index.get("rows")
    require(isinstance(rows, list), "clip index missing rows array")
    columns = index.get("columns")
    require(isinstance(columns, list) and columns, "clip index missing columns")
    csv_columns, csv_rows = read_csv_rows(index_csv_path)
    require(csv_columns == [str(column) for column in columns], "clip index CSV header does not match JSON columns")

    expected: dict[tuple[int, str], dict[str, Any]] = {}
    clips = manifest.get("clips")
    require(isinstance(clips, list), "recording_session clips is not an array")
    for item in rolling_summaries:
        serial = str(item["serial"])
        for expected_clip in item["rolling_clips"]:
            clip_index = as_int(expected_clip.get("clip_index"), "expected clip_index")
            require(clip_index < len(clips), f"recording_session missing clip {clip_index} for {serial}")
            manifest_clip = clips[clip_index]
            require(isinstance(manifest_clip, dict), f"recording_session clip {clip_index} is not an object")
            camera_artifacts = manifest_clip.get("camera_artifacts")
            require(isinstance(camera_artifacts, dict), f"recording_session clip {clip_index} missing camera_artifacts")
            camera_artifact = camera_artifacts.get(serial)
            require(isinstance(camera_artifact, dict), f"recording_session clip {clip_index} missing camera {serial}")
            expected[(clip_index, serial)] = {
                "clip_id": manifest_clip.get("clip_id"),
                "status": manifest_clip.get("status"),
                "stop_reason": manifest_clip.get("stop_reason"),
                "frame_count": as_int(expected_clip.get("frame_count"), "expected frame_count"),
                "first_recording_frame_id": as_int(expected_clip.get("first_recording_frame_id"), "expected first_recording_frame_id"),
                "last_recording_frame_id": as_int(expected_clip.get("last_recording_frame_id"), "expected last_recording_frame_id"),
                "packet_count": as_int(expected_clip.get("packet_count"), "expected packet_count"),
                "video": str(expected_clip.get("mp4")),
                "metadata": str(expected_clip.get("metadata")),
                "keyframes": str(expected_clip.get("keyframes")),
            }

    require(len(rows) == len(expected), f"clip index row count mismatch: {len(rows)} vs {len(expected)}")
    require(as_int(index.get("row_count"), "clip index row_count") == len(expected), "clip index row_count mismatch")
    require(as_int(indexes.get("row_count"), "manifest indexes row_count") == len(expected), "manifest indexes row_count mismatch")
    require(as_int(index.get("clip_count"), "clip index clip_count") == len(clips), "clip index clip_count mismatch")
    require(as_int(indexes.get("clip_count"), "manifest indexes clip_count") == len(clips), "manifest indexes clip_count mismatch")

    rows_by_key: dict[tuple[int, str], dict[str, Any]] = {}
    for row in rows:
        require(isinstance(row, dict), "clip index row is not an object")
        key = (as_int(row.get("clip_index"), "clip index row clip_index"), str(row.get("camera_serial")))
        require(key not in rows_by_key, f"duplicate clip index row for {key}")
        rows_by_key[key] = row
    require(set(rows_by_key.keys()) == set(expected.keys()), "clip index rows do not match external summary keys")

    csv_by_key: dict[tuple[int, str], dict[str, str]] = {}
    for row in csv_rows:
        key = (as_int(row.get("clip_index"), "clip index CSV clip_index"), str(row.get("camera_serial")))
        csv_by_key[key] = row
    require(set(csv_by_key.keys()) == set(expected.keys()), "clip index CSV rows do not match external summary keys")

    for key, expected_row in expected.items():
        row = rows_by_key[key]
        for field in ("clip_id", "status", "stop_reason", "video", "metadata", "keyframes"):
            require(str(row.get(field, "")) == str(expected_row.get(field, "")), f"clip index {field} mismatch for {key}")
        for field in ("frame_count", "first_recording_frame_id", "last_recording_frame_id"):
            require(as_int(row.get(field), f"clip index {field}") == expected_row[field], f"clip index {field} mismatch for {key}")
        require(as_int(row.get("packet_count"), "clip index packet_count") == expected_row["packet_count"], f"clip index packet_count mismatch for {key}")
        require(str(row.get("packet_count_source")) == "external_recorder_summary.packets_written", f"unexpected packet_count_source for {key}")
        clip_manifest_path = Path(str(row.get("clip_manifest_path", "")))
        require(clip_manifest_path.exists(), f"clip index clip_manifest_path missing for {key}: {clip_manifest_path}")
        csv_row = csv_by_key[key]
        require(csv_row.get("video") == str(row.get("video")), f"clip index CSV video mismatch for {key}")
        require(as_int(csv_row.get("frame_count"), "clip index CSV frame_count") == expected_row["frame_count"], f"clip index CSV frame_count mismatch for {key}")
        require(as_int(csv_row.get("packet_count"), "clip index CSV packet_count") == expected_row["packet_count"], f"clip index CSV packet_count mismatch for {key}")

    camera_ranges = index.get("camera_ranges")
    require(isinstance(camera_ranges, dict), "clip index missing camera_ranges")
    for camera in {camera for _, camera in expected.keys()}:
        camera_rows = [row for key, row in rows_by_key.items() if key[1] == camera]
        camera_range = camera_ranges.get(camera)
        require(isinstance(camera_range, dict), f"clip index missing camera range for {camera}")
        require(
            as_int(camera_range.get("total_packet_count"), "camera total_packet_count")
            == sum(as_int(row.get("packet_count"), "row packet_count") for row in camera_rows),
            f"camera total_packet_count mismatch for {camera}",
        )

    snapshot = read_json(recording_folder / "recording_snapshot.json")
    snapshot_session = snapshot.get("session")
    require(isinstance(snapshot_session, dict), "recording_snapshot.json missing session object")
    require(snapshot_session.get("recording_mode") == "rolling_clips", "recording_snapshot session recording_mode mismatch")
    require(
        path_key(Path(str(snapshot_session.get("recording_session_manifest_path"))))
        == path_key(recording_folder / "recording_session.json"),
        "recording_snapshot recording_session_manifest_path mismatch",
    )
    snapshot_index = snapshot_session.get("recording_session_index")
    require(isinstance(snapshot_index, dict), "recording_snapshot missing recording_session_index")
    require(
        path_key(Path(str(snapshot_index.get("clip_index_json_path")))) == path_key(index_json_path),
        "recording_snapshot clip_index_json_path mismatch",
    )
    require(
        path_key(Path(str(snapshot_index.get("clip_index_csv_path")))) == path_key(index_csv_path),
        "recording_snapshot clip_index_csv_path mismatch",
    )


def verify_analytics_recording_session_manifests(
    recording_folders: list[Path],
    summaries: list[dict[str, Any]],
) -> None:
    rolling_summaries = [item for item in summaries if item.get("rolling_clip_count", 0) > 0]
    if not rolling_summaries:
        return
    require(bool(recording_folders), "rolling external recorder verification has no analytics recording folder")
    for recording_folder in recording_folders:
        manifest_path = recording_folder / "recording_session.json"
        manifest = read_json(manifest_path)
        require(manifest.get("mode") == "rolling_clips", f"analytics recording_session.json is not rolling_clips: {manifest_path}")
        require(
            manifest.get("producer") in ("orange_headless_external_ipc", "orange_gui_external_ipc"),
            f"unexpected recording_session producer for external IPC rolling: {manifest.get('producer')!r}",
        )
        rollover = manifest.get("rollover")
        require(isinstance(rollover, dict), f"recording_session missing rollover object: {manifest_path}")
        require(
            rollover.get("implementation") == "external_recorder_gop_boundary_writer_rotation",
            f"recording_session rollover implementation mismatch: {rollover.get('implementation')!r}",
        )
        backend = manifest.get("recording_backend")
        require(isinstance(backend, dict), f"recording_session missing recording_backend: {manifest_path}")
        require(backend.get("mode") == "external_ipc", f"recording_session backend is not external_ipc: {manifest_path}")
        clips = manifest.get("clips")
        require(isinstance(clips, list) and bool(clips), f"recording_session has no clips: {manifest_path}")
        verify_analytics_recording_session_indexes(recording_folder, manifest, rolling_summaries)

        for item in rolling_summaries:
            serial = str(item["serial"])
            for expected in item["rolling_clips"]:
                clip_index = as_int(expected.get("clip_index"), "rolling clip index")
                require(clip_index < len(clips), f"recording_session missing clip {clip_index} for {serial}")
                manifest_clip = clips[clip_index]
                require(isinstance(manifest_clip, dict), f"recording_session clip {clip_index} is not an object")
                camera_artifacts = manifest_clip.get("camera_artifacts")
                require(isinstance(camera_artifacts, dict), f"recording_session clip {clip_index} missing camera_artifacts")
                camera_artifact = camera_artifacts.get(serial)
                require(isinstance(camera_artifact, dict), f"recording_session clip {clip_index} missing camera {serial}")
                output_errors = recording_clip_output_contract_errors(
                    recording_folder,
                    manifest_clip,
                    [serial],
                )
                require(
                    not output_errors,
                    "recording output descriptor contract failed: " + "; ".join(output_errors),
                )
                require(
                    as_int(camera_artifact.get("first_recording_frame_id"), "camera first_recording_frame_id") ==
                    as_int(expected.get("first_recording_frame_id"), "expected first_recording_frame_id"),
                    f"recording_session first frame mismatch for {serial} clip {clip_index}",
                )
                require(
                    as_int(camera_artifact.get("last_recording_frame_id"), "camera last_recording_frame_id") ==
                    as_int(expected.get("last_recording_frame_id"), "expected last_recording_frame_id"),
                    f"recording_session last frame mismatch for {serial} clip {clip_index}",
                )
                require(
                    as_int(camera_artifact.get("frame_count"), "camera frame_count") ==
                    as_int(expected.get("frame_count"), "expected frame_count"),
                    f"recording_session frame_count mismatch for {serial} clip {clip_index}",
                )
                video_path = path_from(camera_artifact.get("video"), recording_folder)
                metadata_path = path_from(camera_artifact.get("metadata"), recording_folder)
                keyframe_path = path_from(camera_artifact.get("keyframes"), recording_folder)
                require(video_path.exists() and video_path.stat().st_size > 0, f"recording_session video path missing: {video_path}")
                require(metadata_path.exists() and metadata_path.stat().st_size > 0, f"recording_session metadata path missing: {metadata_path}")
                require(keyframe_path.exists() and keyframe_path.stat().st_size > 0, f"recording_session keyframe path missing: {keyframe_path}")


def verify(args: argparse.Namespace) -> None:
    analytics_root = Path(args.analytics_root).expanduser() if args.analytics_root else None
    requested_cameras = args.camera

    spec = load_spec(args)
    contract = contract_from_spec(spec)
    artifact_root = resolve_artifact_root(args, analytics_root, contract)
    require(artifact_root.exists(), f"artifact root does not exist: {artifact_root}")
    if contract is None:
        contract = synthesize_contract(artifact_root, requested_cameras)
    require(contract.get("schema_id") in (None, CONTRACT_SCHEMA_ID), "unexpected external recorder contract schema_id")
    require(contract.get("schema_version", 1) == 1, "unexpected external recorder contract schema_version")
    require(contract.get("mode") == "diagnostic_ipc_v1", "external recorder contract mode must be diagnostic_ipc_v1")

    streams = selected_streams(contract, requested_cameras)
    summaries = [
        verify_summary(
            artifact_root,
            serial,
            stream,
            contract,
            args.ffprobe,
            args.allow_missing_video_sanity,
            args.expect_encode_queue_depth,
            args.max_encode_queue_high_water,
            args.max_enqueue_age_p95_ms,
            args.require_recorder_status,
            args.require_recorder_runtime_status,
            args.require_recorder_storage_preflight,
        )
        for serial, stream in streams.items()
    ]

    recording_folders: list[Path] = []
    if analytics_root is not None:
        recording_folders = verify_analytics_root(analytics_root, list(streams.keys()))
        verify_analytics_recording_session_manifests(recording_folders, summaries)

    total_frames = sum(item["frames_received"] for item in summaries)
    print("External recorder verification passed")
    print(f"  artifact_root: {artifact_root}")
    if analytics_root is not None:
        print(f"  analytics_root: {analytics_root}")
    print(f"  streams: {len(summaries)}")
    print(f"  frames_received: {total_frames}")
    for item in summaries:
        recorder_status = item.get("recorder_status")
        status_text = "none"
        heartbeat_text = "none"
        if isinstance(recorder_status, dict):
            status_text = str(recorder_status.get("status"))
            heartbeat_text = str(recorder_status.get("heartbeat_sequence"))
        print(
            "  "
            f"camera={item['serial']} frames={item['frames_received']} "
            f"encoded={item['frames_encoded']} shards={item['shard_count']} "
            f"queue_depth={item['encode_queue_depth']} "
            f"queue_high_water={item['encode_queue_high_water']} "
            f"enqueue_age_p95_ms={item['enqueue_age_p95_ms']} "
            f"routing={item['routing_policy']} video_sanity={item['video_sanity']} "
            f"rolling_clips={item['rolling_clip_count']} "
            f"recorder_status={status_text} heartbeat={heartbeat_text}"
        )


def main() -> int:
    args = parse_args()
    try:
        verify(args)
    except VerificationError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
