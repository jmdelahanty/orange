#!/usr/bin/env python3
"""Validate internal consistency of an Orange recording folder."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Any

from recording_output_validation import (
    mp4_key_sample_flag_errors,
    mp4_source_pixel_tag_errors,
)


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")
CROP_META_CONTRACT_COLUMNS = [
    "crop_video_frame_index",
    "crop_state",
    "crop_rect_valid",
    "crop_rect_coordinate_space",
    "crop_rect_layout",
    "crop_rect_semantics",
    "detection_rect_valid",
    "detection_rect_coordinate_space",
    "detection_rect_layout",
    "detection_rect_semantics",
    "detection_source",
    "selection_policy",
]
CROP_META_EXPECTED_TEXT = {
    "crop_rect_coordinate_space": "full_frame_pixels",
    "crop_rect_layout": "xywh_top_left",
    "crop_rect_semantics": "actual_clamped_source_roi",
    "detection_rect_coordinate_space": "full_frame_pixels",
    "detection_rect_layout": "xywh_top_left",
    "detection_rect_semantics": "selected_postprocessed_model_detection",
    "selection_policy": "largest_detection_by_confidence",
}


class ValidationError(RuntimeError):
    pass


class Reporter:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.warnings: list[str] = []

    def pass_(self, message: str) -> None:
        print(f"[PASS] {message}")

    def warn(self, message: str) -> None:
        self.warnings.append(message)
        print(f"[WARN] {message}")

    def fail(self, message: str) -> None:
        self.failures.append(message)
        print(f"[FAIL] {message}")

    def check(self, condition: bool, pass_message: str, fail_message: str) -> None:
        if condition:
            self.pass_(pass_message)
        else:
            self.fail(fail_message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate Orange recording artifacts: main video/metadata, optional "
            "crop video/metadata, and current full-rate YOLO event alignment."
        )
    )
    parser.add_argument("recording_folder", help="Path to one Orange recording folder")
    parser.add_argument(
        "--ffprobe",
        default=str(DEFAULT_FFPROBE if DEFAULT_FFPROBE.exists() else "ffprobe"),
        help="ffprobe executable path. Defaults to Orange ffprobe when available.",
    )
    parser.add_argument(
        "--allow-yolo-decimation",
        action="store_true",
        help=(
            "Do not require YOLO event rows to match crop metadata rows. "
            "Use only for future runs where YOLO/crop cadence is intentionally decimated."
        ),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print a machine-readable summary after human-readable checks.",
    )
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except FileNotFoundError as exc:
        raise ValidationError(f"missing JSON file: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ValidationError(f"invalid JSON file {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise ValidationError(f"expected object JSON in {path}")
    return payload


def sha256_reference(data: bytes) -> str:
    return f"sha256:{hashlib.sha256(data).hexdigest()}"


def recording_local_path(root: Path, relative_value: Any, label: str) -> Path:
    relative = Path(str(relative_value or ""))
    if not relative_value or relative.is_absolute():
        raise ValidationError(f"{label} must be a non-empty recording-relative path")
    resolved_root = root.resolve()
    resolved = (resolved_root / relative).resolve()
    try:
        resolved.relative_to(resolved_root)
    except ValueError as exc:
        raise ValidationError(f"{label} escapes the recording folder: {relative}") from exc
    return resolved


def read_exact_json(path: Path) -> tuple[bytes, dict[str, Any]]:
    try:
        data = path.read_bytes()
    except FileNotFoundError as exc:
        raise ValidationError(f"missing JSON file: {path}") from exc
    try:
        payload = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"invalid JSON file {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise ValidationError(f"expected object JSON in {path}")
    return data, payload


def _camera_circle(value: Any) -> tuple[float, float, float] | None:
    if not isinstance(value, dict) or value.get("type") != "circle":
        return None
    center = value.get("center_px")
    if not isinstance(center, dict):
        return None
    try:
        x = float(center["x"])
        y = float(center["y"])
        radius = float(value["radius_px"])
    except (KeyError, TypeError, ValueError):
        return None
    if not all(math.isfinite(component) for component in (x, y, radius)) or radius <= 0:
        return None
    return x, y, radius


def validate_daily_registered_masks(
    contract: dict[str, Any],
    snapshot: dict[str, Any],
    scoped_camera_set: set[str],
    files: list[Any],
    reporter: Reporter,
) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "status": "not_configured",
        "resolved_mask_count": 0,
    }
    daily = contract.get("daily_registration_geometry")
    if not isinstance(daily, dict):
        return summary
    status = str(daily.get("status", "invalid"))
    mode = str(daily.get("mode", "base_only"))
    summary.update({"status": status, "mode": mode})
    if mode != "selected_daily_registration":
        return summary
    if status != "selected_resolved":
        reporter.warn(
            f"selected daily registration is {status!r}; no complete mask set is claimed"
        )
        return summary

    daily_cameras = daily.get("cameras")
    calibrations = snapshot.get("calibrations")
    if not isinstance(daily_cameras, dict):
        reporter.fail("selected daily registration has no camera dictionary")
        return summary
    if not isinstance(calibrations, dict):
        reporter.fail("selected daily registration is absent from snapshot calibrations")
        calibrations = {}

    required_roles = {
        "daily_rim_observation",
        "daily_rim_manifest",
        "daily_rim_image_set",
        "daily_rim_spatial_mask_export",
        "daily_rim_palette_mask_export",
    }
    roles_by_camera: dict[str, set[str]] = {}
    for row in files:
        if not isinstance(row, dict):
            continue
        context = row.get("context")
        if not isinstance(context, dict):
            continue
        camera = context.get("camera_serial")
        role = row.get("role")
        if camera is not None and isinstance(role, str):
            roles_by_camera.setdefault(str(camera), set()).add(role)

    cameras_to_validate = scoped_camera_set or {
        str(camera) for camera in daily_cameras
    }
    for camera in sorted(cameras_to_validate):
        camera_daily = daily_cameras.get(camera)
        if not isinstance(camera_daily, dict) or camera_daily.get("status") != "resolved":
            reporter.fail(f"daily registration has no resolved mask for camera {camera}")
            continue
        entry = camera_daily.get("recording_snapshot_entry")
        if not isinstance(entry, dict):
            reporter.fail(f"daily registration mask entry is missing for camera {camera}")
            continue
        inner = entry.get("accepted_inner_rim_boundary")
        valid = entry.get("valid_detection_region")
        inner_circle = _camera_circle(
            inner.get("geometry") if isinstance(inner, dict) else None
        )
        valid_circle = _camera_circle(
            valid.get("geometry") if isinstance(valid, dict) else None
        )
        geometry_ok = (
            isinstance(inner, dict)
            and inner.get("coordinate_space") == "camera_native_pixels"
            and inner.get("target_plane") == "dish_top_rim"
            and isinstance(valid, dict)
            and valid.get("coordinate_space") == "camera_native_pixels"
            and valid.get("purpose") == "bounding_box_centroid_detection_gating"
            and valid.get("offset_direction") == "outward"
            and inner_circle is not None
            and valid_circle is not None
            and abs(inner_circle[0] - valid_circle[0]) <= 1e-6
            and abs(inner_circle[1] - valid_circle[1]) <= 1e-6
            and valid_circle[2] >= inner_circle[2]
        )
        reporter.check(
            geometry_ok,
            f"camera {camera} registered rim and outward centroid gate are coherent",
            f"camera {camera} registered rim/gate geometry is invalid or contradictory",
        )
        snapshot_entry = (
            calibrations.get(camera, {}).get("dish_top_rim_observation")
            if isinstance(calibrations.get(camera), dict)
            else None
        )
        reporter.check(
            isinstance(snapshot_entry, dict)
            and snapshot_entry.get("artifact_id") == entry.get("artifact_id")
            and snapshot_entry.get("valid_detection_region") == valid,
            f"camera {camera} direct recording-snapshot mask matches the contract",
            f"camera {camera} recording-snapshot mask differs from the contract",
        )
        missing_roles = required_roles - roles_by_camera.get(camera, set())
        reporter.check(
            not missing_roles,
            f"camera {camera} recording-local daily mask package is complete",
            f"camera {camera} daily mask package lacks roles {sorted(missing_roles)}",
        )
        if geometry_ok and not missing_roles:
            summary["resolved_mask_count"] += 1
    return summary


def validate_recording_geometry_artifacts(
    recording_folder: Path,
    snapshot: dict[str, Any],
    reporter: Reporter,
) -> dict[str, Any]:
    summary: dict[str, Any] = {"status": "not_referenced", "files": 0}
    reference = snapshot.get("recording_geometry_contract")
    if not isinstance(reference, dict):
        reporter.warn("recording snapshot has no recording geometry contract reference")
        return summary

    try:
        contract_path = recording_local_path(
            recording_folder,
            reference.get("relative_path"),
            "recording geometry contract path",
        )
        contract_bytes, contract = read_exact_json(contract_path)
    except ValidationError as exc:
        reporter.fail(str(exc))
        summary["status"] = "invalid"
        return summary

    expected_contract_sha256 = reference.get("sha256")
    actual_contract_sha256 = sha256_reference(contract_bytes)
    reporter.check(
        expected_contract_sha256 == actual_contract_sha256,
        "recording geometry contract checksum matches recording_snapshot.json",
        (
            "recording geometry contract checksum mismatch: "
            f"expected {expected_contract_sha256!r}, got {actual_contract_sha256!r}"
        ),
    )
    reporter.check(
        contract.get("schema_id") == "orange.recording.geometry_contract"
        and contract.get("schema_version") == 1,
        "recording geometry contract schema identity is valid",
        "recording geometry contract schema identity is invalid",
    )
    summary.update(
        {
            "status": str(contract.get("status", "unknown")),
            "contract_path": str(contract_path),
            "contract_sha256": actual_contract_sha256,
        }
    )

    assets = contract.get("materialized_assets")
    if not isinstance(assets, dict):
        reporter.warn("recording geometry contract predates materialized assets")
        summary["asset_status"] = "legacy_not_referenced"
        return summary
    summary["asset_status"] = str(assets.get("status", "unknown"))
    if assets.get("status") == "unavailable":
        reporter.warn(
            "recording-local geometry assets are explicitly unavailable; "
            "numerical geometry remains in the contract"
        )
        return summary

    try:
        manifest_path = recording_local_path(
            recording_folder,
            assets.get("relative_path"),
            "recording geometry asset manifest path",
        )
        manifest_bytes, manifest = read_exact_json(manifest_path)
    except ValidationError as exc:
        reporter.fail(str(exc))
        summary["asset_status"] = "invalid"
        return summary

    actual_manifest_sha256 = sha256_reference(manifest_bytes)
    reporter.check(
        assets.get("sha256") == actual_manifest_sha256,
        "recording geometry asset manifest checksum matches the contract",
        (
            "recording geometry asset manifest checksum mismatch: "
            f"expected {assets.get('sha256')!r}, got {actual_manifest_sha256!r}"
        ),
    )
    reporter.check(
        manifest.get("schema_id") == "orange.recording.geometry_assets"
        and manifest.get("schema_version") == 1,
        "recording geometry asset manifest schema identity is valid",
        "recording geometry asset manifest schema identity is invalid",
    )
    reporter.check(
        assets.get("request_sha256") == manifest.get("request_sha256"),
        "recording geometry asset request identity matches the contract",
        "recording geometry asset request identity differs from the contract",
    )
    reporter.check(
        assets.get("status") == manifest.get("status"),
        "recording geometry asset status matches the contract",
        "recording geometry asset status differs from the contract",
    )

    files = manifest.get("files")
    if not isinstance(files, list):
        reporter.fail("recording geometry asset manifest files is not an array")
        return summary
    reporter.check(
        assets.get("file_count") == len(files)
        and manifest.get("materialized_file_count") == len(files),
        f"recording geometry asset file counts agree ({len(files)})",
        "recording geometry asset file counts disagree",
    )
    failures = manifest.get("failures")
    if not isinstance(failures, list):
        reporter.fail("recording geometry asset manifest failures is not an array")
        failures = []
    required_failures = sum(
        1 for failure in failures
        if isinstance(failure, dict) and failure.get("required") is True
    )
    optional_failures = sum(
        1 for failure in failures
        if isinstance(failure, dict) and failure.get("required") is False
    )
    reporter.check(
        manifest.get("required_failure_count") == required_failures
        and assets.get("required_failure_count") == required_failures,
        f"recording geometry required-failure counts agree ({required_failures})",
        "recording geometry required-failure counts disagree",
    )
    reporter.check(
        manifest.get("optional_failure_count") == optional_failures,
        f"recording geometry optional-failure count agrees ({optional_failures})",
        "recording geometry optional-failure count disagrees",
    )
    status = manifest.get("status")
    status_consistent = (
        (status == "complete" and required_failures == 0)
        or (status == "partial" and required_failures > 0)
        or (
            status == "empty"
            and manifest.get("required_requested_file_count") == 0
            and not files
        )
    )
    reporter.check(
        status_consistent,
        f"recording geometry asset status {status!r} matches its failures",
        f"recording geometry asset status {status!r} contradicts its failures/counts",
    )

    bundle_root = manifest_path.parent.resolve()
    scope = manifest.get("scope") if isinstance(manifest.get("scope"), dict) else {}
    scoped_cameras = scope.get("camera_serials")
    scoped_camera_set = {
        str(value) for value in scoped_cameras
    } if isinstance(scoped_cameras, list) else set()
    arena_by_camera = scope.get("arena_by_camera")
    if not isinstance(arena_by_camera, dict):
        arena_by_camera = {}
    file_failure_count_before = len(reporter.failures)
    total_bytes = 0
    for index, file_entry in enumerate(files):
        if not isinstance(file_entry, dict):
            reporter.fail(f"geometry asset file row {index} is not an object")
            continue
        relative = file_entry.get("relative_path")
        try:
            asset_path = recording_local_path(
                bundle_root,
                relative,
                f"geometry asset file row {index} path",
            )
            asset_bytes = asset_path.read_bytes()
        except (ValidationError, FileNotFoundError) as exc:
            reporter.fail(str(exc))
            continue
        actual_sha256 = sha256_reference(asset_bytes)
        if actual_sha256 != file_entry.get("sha256"):
            reporter.fail(
                f"geometry asset checksum mismatch for {relative}: "
                f"expected {file_entry.get('sha256')!r}, got {actual_sha256!r}"
            )
            continue
        if file_entry.get("size_bytes") != len(asset_bytes):
            reporter.fail(
                f"geometry asset byte count mismatch for {relative}: "
                f"expected {file_entry.get('size_bytes')!r}, got {len(asset_bytes)}"
            )
            continue
        context = file_entry.get("context")
        if isinstance(context, dict):
            camera = context.get("camera_serial")
            arena = context.get("arena_id")
            if camera is not None and str(camera) not in scoped_camera_set:
                reporter.fail(
                    f"geometry asset {relative} names unscoped camera {camera!r}"
                )
                continue
            if (
                camera is not None
                and arena is not None
                and str(arena_by_camera.get(str(camera), "")) != str(arena)
            ):
                reporter.fail(
                    f"geometry asset {relative} arena {arena!r} does not match "
                    f"camera {camera!r} scope"
                )
                continue
        total_bytes += len(asset_bytes)

    summary["daily_registration"] = validate_daily_registered_masks(
        contract, snapshot, scoped_camera_set, files, reporter
    )

    reporter.check(
        assets.get("total_bytes") == total_bytes
        and manifest.get("total_bytes") == total_bytes,
        f"recording geometry asset byte totals agree ({total_bytes})",
        "recording geometry asset byte totals disagree",
    )
    summary.update(
        {
            "asset_manifest_path": str(manifest_path),
            "asset_manifest_sha256": actual_manifest_sha256,
            "files": len(files),
            "total_bytes": total_bytes,
        }
    )
    if manifest.get("status") == "partial":
        reporter.warn(
            "recording geometry asset bundle is explicitly partial; inspect its failures[]"
        )
    elif manifest.get("status") == "empty":
        reporter.pass_("recording geometry asset bundle is explicitly empty")
    elif len(reporter.failures) == file_failure_count_before:
        reporter.pass_("all listed recording geometry assets passed checksum validation")
    return summary


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            return list(csv.DictReader(handle))
    except FileNotFoundError as exc:
        raise ValidationError(f"missing CSV file: {path}") from exc


def read_yolo_events(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    try:
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                stripped = line.strip()
                if not stripped:
                    continue
                try:
                    payload = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise ValidationError(
                        f"invalid JSONL row {line_number} in {path}: {exc}"
                    ) from exc
                if not isinstance(payload, dict):
                    raise ValidationError(f"expected object JSONL row {line_number} in {path}")
                events.append(payload)
    except FileNotFoundError as exc:
        raise ValidationError(f"missing YOLO event JSONL file: {path}") from exc
    return events


def int_field(row: dict[str, Any], field: str, path: Path) -> int:
    value = row.get(field)
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise ValidationError(f"invalid integer field {field}={value!r} in {path}") from exc


def check_crop_metadata_contract(
    rows: list[dict[str, str]],
    path: Path,
    serial: str,
    reporter: Reporter,
) -> None:
    if not rows:
        reporter.fail(f"Cam{serial} crop metadata has no rows for contract check")
        return

    header_fields = set(rows[0].keys())
    missing_columns = [
        column for column in CROP_META_CONTRACT_COLUMNS if column not in header_fields
    ]
    reporter.check(
        not missing_columns,
        f"Cam{serial} crop metadata has self-describing contract columns",
        (
            f"Cam{serial} crop metadata missing contract columns in {path}: "
            f"{','.join(missing_columns)}"
        ),
    )
    if missing_columns:
        return

    # session_crop_video_frame_index is optional (appended by newer recorders);
    # when present it must be contiguous ascending. In session-aggregate CSVs
    # it starts at 0 and equals crop_video_frame_index; in per-clip split CSVs
    # it continues the session-global count while crop_video_frame_index
    # restarts at 0.
    has_session_index = "session_crop_video_frame_index" in header_fields
    session_index_base = None
    bad_session_index_rows = []

    bad_index_rows = []
    bad_state_rows = []
    bad_validity_rows = []
    bad_text_rows = []
    for data_index, row in enumerate(rows):
        line_number = data_index + 2
        has_detection = int_field(row, "has_detection", path) != 0
        blank_frame = int_field(row, "blank_frame", path) != 0
        if int_field(row, "crop_video_frame_index", path) != data_index:
            bad_index_rows.append(line_number)
        if has_session_index:
            session_index = int_field(row, "session_crop_video_frame_index", path)
            if session_index_base is None:
                session_index_base = session_index
            if session_index != session_index_base + data_index:
                bad_session_index_rows.append(line_number)

        expected_state = "detected_crop" if has_detection else "blank_no_detection"
        expected_crop_valid = 1 if has_detection and not blank_frame else 0
        expected_detection_valid = 1 if has_detection and not blank_frame else 0
        expected_detection_source = "model" if has_detection else "none"
        if row.get("crop_state") != expected_state:
            bad_state_rows.append(line_number)
        if (
            int_field(row, "crop_rect_valid", path) != expected_crop_valid or
            int_field(row, "detection_rect_valid", path) != expected_detection_valid or
            row.get("detection_source") != expected_detection_source
        ):
            bad_validity_rows.append(line_number)
        if any(row.get(field) != expected for field, expected in CROP_META_EXPECTED_TEXT.items()):
            bad_text_rows.append(line_number)

    reporter.check(
        not bad_index_rows,
        f"Cam{serial} crop_video_frame_index is contiguous from 0",
        f"Cam{serial} crop metadata has {len(bad_index_rows)} invalid crop_video_frame_index row(s)",
    )
    if has_session_index:
        reporter.check(
            not bad_session_index_rows,
            f"Cam{serial} session_crop_video_frame_index is contiguous ascending",
            (
                f"Cam{serial} crop metadata has {len(bad_session_index_rows)} "
                "invalid session_crop_video_frame_index row(s)"
            ),
        )
    reporter.check(
        not bad_state_rows,
        f"Cam{serial} crop_state matches has_detection/blank_frame",
        f"Cam{serial} crop metadata has {len(bad_state_rows)} invalid crop_state row(s)",
    )
    reporter.check(
        not bad_validity_rows,
        f"Cam{serial} crop/detection validity flags match row state",
        f"Cam{serial} crop metadata has {len(bad_validity_rows)} invalid validity row(s)",
    )
    reporter.check(
        not bad_text_rows,
        f"Cam{serial} crop metadata coordinate/layout semantics are explicit",
        f"Cam{serial} crop metadata has {len(bad_text_rows)} invalid semantic row(s)",
    )


def ffprobe_video(path: Path, ffprobe: str) -> dict[str, Any]:
    stream = ffprobe_stream(path, ffprobe, count_frames=False)
    frame_count = stream.get("nb_frames")
    if not usable_frame_count(frame_count):
        stream = ffprobe_stream(path, ffprobe, count_frames=True)
        frame_count = stream.get("nb_read_frames") or stream.get("nb_frames")

    try:
        parsed_frame_count = int(frame_count)
    except (TypeError, ValueError) as exc:
        raise ValidationError(f"could not read frame count from ffprobe for {path}") from exc

    return {
        "width": int(stream["width"]),
        "height": int(stream["height"]),
        "frames": parsed_frame_count,
        "avg_frame_rate": stream.get("avg_frame_rate", ""),
        "duration": float(stream.get("duration") or 0.0),
        "tags": stream.get("_format_tags", {}),
    }


def usable_frame_count(value: Any) -> bool:
    try:
        return int(value) >= 0
    except (TypeError, ValueError):
        return False


def ffprobe_stream(path: Path, ffprobe: str, count_frames: bool) -> dict[str, Any]:
    command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,nb_frames,nb_read_frames,avg_frame_rate,duration:format_tags=title,comment",
        "-of",
        "json",
        str(path),
    ]
    if count_frames:
        command.insert(5, "-count_frames")

    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except FileNotFoundError as exc:
        raise ValidationError(f"ffprobe executable not found: {ffprobe}") from exc
    except subprocess.TimeoutExpired as exc:
        raise ValidationError(f"ffprobe timed out for {path}") from exc

    if result.returncode != 0:
        stderr = result.stderr.strip()
        raise ValidationError(f"ffprobe failed for {path}: {stderr}")

    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ValidationError(f"ffprobe returned invalid JSON for {path}: {exc}") from exc

    streams = payload.get("streams", [])
    if not streams:
        raise ValidationError(f"ffprobe found no video stream in {path}")

    stream = streams[0]
    fmt = payload.get("format", {}) if isinstance(payload.get("format"), dict) else {}
    stream["_format_tags"] = fmt.get("tags", {}) if isinstance(fmt.get("tags"), dict) else {}
    return stream


def ffprobe_packet_key_flags(path: Path, ffprobe: str) -> list[str]:
    command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "packet=flags",
        "-of",
        "csv=p=0",
        str(path),
    ]
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except FileNotFoundError as exc:
        raise ValidationError(f"ffprobe executable not found: {ffprobe}") from exc
    except subprocess.TimeoutExpired as exc:
        raise ValidationError(f"ffprobe packet key flag probe timed out for {path}") from exc

    if result.returncode != 0:
        stderr = result.stderr.strip()
        raise ValidationError(f"ffprobe packet key flag probe failed for {path}: {stderr}")

    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def camera_serial_from_main_video(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith(".mp4"):
        return None
    if name.endswith("_crop.mp4"):
        return None
    return name[len("Cam") : -len(".mp4")]


def camera_serial_from_crop_video(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith("_crop.mp4"):
        return None
    return name[len("Cam") : -len("_crop.mp4")]


def get_nested_crop_size(snapshot: dict[str, Any], serial: str) -> int | None:
    crop_output = get_crop_output(snapshot, serial)
    if crop_output is not None:
        runtime = crop_output.get("runtime")
        if isinstance(runtime, dict):
            crop_size = runtime.get("crop_size_px")
            if isinstance(crop_size, int) and crop_size > 0:
                return crop_size

    runtime = snapshot.get("camera_runtime", {}).get(serial, {})
    if isinstance(runtime, dict):
        runtime_config = runtime.get("runtime", {})
        if isinstance(runtime_config, dict):
            crop_size = crop_size_from_config(runtime_config)
            if crop_size is not None:
                return crop_size

    camera_config = snapshot.get("cameras", {}).get(serial, {})
    if isinstance(camera_config, dict):
        crop_size = crop_size_from_config(camera_config)
        if crop_size is not None:
            return crop_size

    return None


def get_crop_output(snapshot: dict[str, Any], serial: str) -> dict[str, Any] | None:
    crop_outputs = snapshot.get("crop_outputs")
    if not isinstance(crop_outputs, dict):
        return None
    crop_output = crop_outputs.get(serial)
    if not isinstance(crop_output, dict):
        return None
    return crop_output


def crop_size_from_config(config: dict[str, Any]) -> int | None:
    crop_pipeline = config.get("crop_pipeline")
    if not isinstance(crop_pipeline, dict):
        return None
    crop_size = crop_pipeline.get("crop_size_px")
    if isinstance(crop_size, int) and crop_size > 0:
        return crop_size
    return None


def recording_frame_ids_from_csv(rows: list[dict[str, str]], path: Path) -> list[int]:
    ids: list[int] = []
    for row in rows:
        ids.append(int_field(row, "recording_frame_id", path))
    return ids


def recording_frame_ids_from_yolo_events(events: list[dict[str, Any]], path: Path) -> list[int]:
    ids: list[int] = []
    for index, event in enumerate(events, start=1):
        frame = event.get("frame")
        if not isinstance(frame, dict):
            raise ValidationError(f"missing frame object in YOLO event row {index} of {path}")
        ids.append(int_field(frame, "recording_frame_id", path))
    return ids


def validate_monotonic_positive(ids: list[int], label: str, reporter: Reporter) -> None:
    positive = all(value > 0 for value in ids)
    monotonic = all(ids[index] < ids[index + 1] for index in range(len(ids) - 1))
    reporter.check(
        positive and monotonic,
        f"{label} recording_frame_id values are positive and strictly increasing",
        f"{label} recording_frame_id values are not positive/strictly increasing",
    )


def summarize_yolo_statuses(events: list[dict[str, Any]]) -> dict[str, int]:
    statuses: Counter[str] = Counter()
    for event in events:
        yolo = event.get("yolo")
        if isinstance(yolo, dict):
            statuses[str(yolo.get("status", "missing"))] += 1
        else:
            statuses["missing"] += 1
    return dict(statuses)


def validate_main_artifacts(
    recording_folder: Path,
    ffprobe: str,
    reporter: Reporter,
) -> dict[str, dict[str, Any]]:
    summaries: dict[str, dict[str, Any]] = {}
    main_videos = sorted(
        path for path in recording_folder.glob("Cam*.mp4")
        if camera_serial_from_main_video(path) is not None
    )
    if not main_videos:
        reporter.warn("no main Cam<serial>.mp4 videos found")
        return summaries

    for video_path in main_videos:
        serial = camera_serial_from_main_video(video_path)
        if serial is None:
            continue
        meta_path = recording_folder / f"Cam{serial}_meta.csv"
        try:
            video = ffprobe_video(video_path, ffprobe)
            meta_rows = read_csv_rows(meta_path)
        except ValidationError as exc:
            reporter.fail(str(exc))
            continue

        reporter.check(
            video["frames"] == len(meta_rows),
            (
                f"Cam{serial} main video frame count matches metadata rows "
                f"({video['frames']})"
            ),
            (
                f"Cam{serial} main video frames ({video['frames']}) != "
                f"metadata rows ({len(meta_rows)})"
            ),
        )
        for error in mp4_source_pixel_tag_errors(
            video.get("tags", {}),
            output_kind="full",
            label=f"Cam{serial} main video",
        ):
            reporter.fail(error)
        summaries[serial] = {
            "main_video": str(video_path),
            "main_frames": video["frames"],
            "main_width": video["width"],
            "main_height": video["height"],
            "main_metadata_rows": len(meta_rows),
        }
    return summaries


def validate_crop_artifacts(
    recording_folder: Path,
    snapshot: dict[str, Any],
    ffprobe: str,
    allow_yolo_decimation: bool,
    reporter: Reporter,
) -> dict[str, dict[str, Any]]:
    summaries: dict[str, dict[str, Any]] = {}
    crop_videos = sorted(recording_folder.glob("Cam*_crop.mp4"))
    if not crop_videos:
        reporter.warn("no crop videos found; crop validation skipped")
        return summaries

    for crop_video_path in crop_videos:
        serial = camera_serial_from_crop_video(crop_video_path)
        if serial is None:
            continue
        crop_meta_path = recording_folder / f"Cam{serial}_crop_meta.csv"
        crop_perf_path = recording_folder / f"Cam{serial}_crop_perf.csv"
        crop_keyframe_path = recording_folder / f"Cam{serial}_crop_keyframe.json"
        yolo_events_path = recording_folder / f"Cam{serial}_yolo_events.jsonl"

        try:
            crop_video = ffprobe_video(crop_video_path, ffprobe)
            crop_rows = read_csv_rows(crop_meta_path)
            crop_perf_rows = read_csv_rows(crop_perf_path)
            read_json(crop_keyframe_path)
            crop_size = get_nested_crop_size(snapshot, serial)
            if crop_size is None:
                raise ValidationError(
                    f"Cam{serial} crop artifacts exist but snapshot has no "
                    "crop_pipeline.crop_size_px"
                )
        except ValidationError as exc:
            reporter.fail(str(exc))
            continue

        crop_output = get_crop_output(snapshot, serial)
        reporter.check(
            crop_output is not None,
            f"Cam{serial} crop output snapshot exists",
            f"Cam{serial} crop artifacts exist but recording_snapshot.json has no crop_outputs entry",
        )
        if crop_output is not None:
            runtime = crop_output.get("runtime")
            if not isinstance(runtime, dict):
                reporter.fail(f"Cam{serial} crop output snapshot runtime is missing or not an object")
                runtime = {}
            files = runtime.get("files")
            if not isinstance(files, dict):
                files = {}
            reporter.check(
                crop_output.get("enabled") is True,
                f"Cam{serial} crop output snapshot marks crop enabled",
                f"Cam{serial} crop output snapshot does not mark crop enabled",
            )
            reporter.check(
                runtime.get("crop_size_px") == crop_size
                and runtime.get("width") == crop_size
                and runtime.get("height") == crop_size,
                f"Cam{serial} crop output snapshot geometry matches crop_size_px",
                f"Cam{serial} crop output snapshot geometry does not match crop_size_px",
            )
            reporter.check(
                files.get("video") == crop_video_path.name
                and files.get("metadata") == crop_meta_path.name
                and files.get("keyframes") == crop_keyframe_path.name
                and files.get("perf") == crop_perf_path.name,
                f"Cam{serial} crop output snapshot file names match artifacts",
                f"Cam{serial} crop output snapshot file names do not match artifacts",
            )
        reporter.pass_(f"Cam{serial} crop keyframe sidecar exists and parses as JSON")

        reporter.check(
            crop_video["width"] == crop_size and crop_video["height"] == crop_size,
            f"Cam{serial} crop video dimensions match crop_size_px ({crop_size})",
            (
                f"Cam{serial} crop video dimensions "
                f"{crop_video['width']}x{crop_video['height']} != crop_size_px {crop_size}"
            ),
        )
        reporter.check(
            crop_video["frames"] == len(crop_rows),
            (
                f"Cam{serial} crop video frame count matches crop metadata rows "
                f"({crop_video['frames']})"
            ),
            (
                f"Cam{serial} crop video frames ({crop_video['frames']}) != "
                f"crop metadata rows ({len(crop_rows)})"
            ),
        )
        try:
            crop_key_flags = ffprobe_packet_key_flags(crop_video_path, ffprobe)
        except ValidationError as exc:
            reporter.fail(str(exc))
            crop_key_flags = []
        key_sample_errors = mp4_key_sample_flag_errors(
            crop_key_flags,
            label=f"Cam{serial} crop video",
            require_all_key_samples=True,
            expected_packet_count=len(crop_rows),
        )
        if key_sample_errors:
            for error in key_sample_errors:
                reporter.fail(error)
        else:
            reporter.pass_(
                f"Cam{serial} crop video MP4 key/sync samples cover every packet "
                f"({len(crop_key_flags)})"
            )
        for error in mp4_source_pixel_tag_errors(
            crop_video.get("tags", {}),
            output_kind="crop",
            label=f"Cam{serial} crop video",
        ):
            reporter.fail(error)

        bad_geometry_rows = []
        for index, row in enumerate(crop_rows, start=2):
            crop_w = int_field(row, "crop_w", crop_meta_path)
            crop_h = int_field(row, "crop_h", crop_meta_path)
            blank_frame = int_field(row, "blank_frame", crop_meta_path) != 0
            has_detection = int_field(row, "has_detection", crop_meta_path) != 0

            if blank_frame and not has_detection:
                if crop_w != 0 or crop_h != 0:
                    bad_geometry_rows.append(index)
                continue

            if crop_w != crop_size or crop_h != crop_size:
                bad_geometry_rows.append(index)

        reporter.check(
            not bad_geometry_rows,
            (
                f"Cam{serial} crop metadata geometry matches crop_size_px "
                "for detections and 0x0 for blank frames"
            ),
            (
                f"Cam{serial} crop metadata has {len(bad_geometry_rows)} rows "
                "with invalid crop_w/crop_h geometry"
            ),
        )
        check_crop_metadata_contract(crop_rows, crop_meta_path, serial, reporter)

        crop_ids = recording_frame_ids_from_csv(crop_rows, crop_meta_path)
        validate_monotonic_positive(crop_ids, f"Cam{serial} crop metadata", reporter)

        crop_perf_ids = recording_frame_ids_from_csv(crop_perf_rows, crop_perf_path)
        validate_monotonic_positive(crop_perf_ids, f"Cam{serial} crop perf", reporter)
        reporter.check(
            len(crop_perf_rows) == len(crop_rows),
            f"Cam{serial} crop perf rows match crop metadata rows ({len(crop_perf_rows)})",
            (
                f"Cam{serial} crop perf rows ({len(crop_perf_rows)}) != "
                f"crop metadata rows ({len(crop_rows)})"
            ),
        )
        reporter.check(
            crop_perf_ids == crop_ids,
            f"Cam{serial} crop perf and crop metadata recording_frame_id sequences match",
            f"Cam{serial} crop perf and crop metadata recording_frame_id sequences differ",
        )
        dropped_perf_rows = [
            index
            for index, row in enumerate(crop_perf_rows, start=2)
            if int_field(row, "dropped", crop_perf_path) != 0
        ]
        reporter.check(
            not dropped_perf_rows,
            f"Cam{serial} crop perf reports no dropped crop frames",
            f"Cam{serial} crop perf reports {len(dropped_perf_rows)} dropped crop frame(s)",
        )

        yolo_statuses: dict[str, int] = {}
        try:
            yolo_events = read_yolo_events(yolo_events_path)
            yolo_ids = recording_frame_ids_from_yolo_events(yolo_events, yolo_events_path)
            yolo_statuses = summarize_yolo_statuses(yolo_events)
            validate_monotonic_positive(yolo_ids, f"Cam{serial} YOLO events", reporter)

            if not allow_yolo_decimation:
                reporter.check(
                    len(yolo_events) == len(crop_rows),
                    (
                        f"Cam{serial} YOLO event rows match crop metadata rows "
                        f"({len(yolo_events)})"
                    ),
                    (
                        f"Cam{serial} YOLO event rows ({len(yolo_events)}) != "
                        f"crop metadata rows ({len(crop_rows)})"
                    ),
                )
                reporter.check(
                    yolo_ids == crop_ids,
                    f"Cam{serial} YOLO and crop recording_frame_id sequences match",
                    f"Cam{serial} YOLO and crop recording_frame_id sequences differ",
                )
        except ValidationError as exc:
            reporter.fail(str(exc))

        summaries[serial] = {
            "crop_video": str(crop_video_path),
            "crop_size_px": crop_size,
            "crop_frames": crop_video["frames"],
            "crop_metadata_rows": len(crop_rows),
            "crop_perf_rows": len(crop_perf_rows),
            "yolo_statuses": yolo_statuses,
        }

    return summaries


def validate_recording(args: argparse.Namespace) -> tuple[int, dict[str, Any]]:
    recording_folder = Path(args.recording_folder).expanduser().resolve()
    reporter = Reporter()
    summary: dict[str, Any] = {
        "recording_folder": str(recording_folder),
        "geometry": {},
        "main": {},
        "crop": {},
        "warnings": reporter.warnings,
        "failures": reporter.failures,
    }

    if not recording_folder.is_dir():
        reporter.fail(f"recording folder does not exist: {recording_folder}")
        return 1, summary

    snapshot_path = recording_folder / "recording_snapshot.json"
    try:
        snapshot = read_json(snapshot_path)
        reporter.pass_(f"found recording snapshot: {snapshot_path}")
    except ValidationError as exc:
        reporter.fail(str(exc))
        snapshot = {}

    summary["geometry"] = validate_recording_geometry_artifacts(
        recording_folder, snapshot, reporter
    )
    summary["main"] = validate_main_artifacts(recording_folder, args.ffprobe, reporter)
    summary["crop"] = validate_crop_artifacts(
        recording_folder,
        snapshot,
        args.ffprobe,
        args.allow_yolo_decimation,
        reporter,
    )

    summary["warnings"] = reporter.warnings
    summary["failures"] = reporter.failures

    if reporter.failures:
        print(f"\nValidation failed with {len(reporter.failures)} failure(s).", file=sys.stderr)
        return 1, summary

    print("\nValidation passed.")
    return 0, summary


def main() -> int:
    args = parse_args()
    exit_code, summary = validate_recording(args)
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
