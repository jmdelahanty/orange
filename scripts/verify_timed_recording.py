#!/usr/bin/env python3
"""Verify the current headless timed-recording single-clip and rolling contracts."""

from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")
ACCEPTED_SCHEMA_IDS = {"orange.headless.recording_session", "orange.recording_session"}
HEALTH_COUNTERS = (
    "camera_frame_id_gaps",
    "get_frame_errors_final",
    "pre_drops_final",
    "enc_fail_final",
)


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
            "Verify a headless fixed.recording_control run: recording_session.json, "
            "session/clip artifact coherence, runs.json health/pass fields when "
            "available, and ffprobe media duration."
        )
    )
    parser.add_argument(
        "path",
        help="Experiment root containing runs.json, or a run folder containing recording_session.json.",
    )
    parser.add_argument("--run-id", help="Run id to verify when the experiment has multiple runs.")
    parser.add_argument(
        "--camera",
        help="Camera serial to verify. Defaults to every camera in the manifest.",
    )
    parser.add_argument(
        "--duration-tolerance-s",
        type=float,
        default=1.0,
        help="Allowed absolute difference between requested and encoded video duration.",
    )
    parser.add_argument(
        "--ffprobe",
        default=default_ffprobe,
        help="ffprobe executable path. Defaults to Orange ffprobe when available.",
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


def as_float(value: Any, field: str) -> float:
    try:
        return float(value)
    except (TypeError, ValueError) as exc:
        raise VerificationError(f"invalid numeric field {field}={value!r}") from exc


def as_int(value: Any, field: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise VerificationError(f"invalid integer field {field}={value!r}") from exc


def path_key(path: Path) -> str:
    return str(path.expanduser().resolve(strict=False))


def entries_from_runs_json(runs_json: dict[str, Any]) -> list[dict[str, Any]]:
    runs = runs_json.get("runs")
    if not isinstance(runs, list):
        raise VerificationError("runs.json missing array field: runs")
    entries: list[dict[str, Any]] = []
    for index, entry in enumerate(runs):
        if not isinstance(entry, dict):
            raise VerificationError(f"runs.json run entry {index} is not an object")
        entries.append(entry)
    return entries


def run_folder_from_entry(entry: dict[str, Any]) -> Path:
    folder = entry.get("recording_folder")
    if isinstance(folder, str) and folder:
        return Path(folder)
    camera_results = entry.get("camera_results")
    if isinstance(camera_results, list):
        for row in camera_results:
            if isinstance(row, dict):
                folder = row.get("recording_folder")
                if isinstance(folder, str) and folder:
                    return Path(folder)
    raise VerificationError(f"run {entry.get('run_id', '<unknown>')} has no recording_folder")


def find_run_entry(
    runs_json: dict[str, Any],
    run_folder: Path | None,
    run_id: str | None,
) -> dict[str, Any] | None:
    matches: list[dict[str, Any]] = []
    for entry in entries_from_runs_json(runs_json):
        if run_id is not None and entry.get("run_id") != run_id:
            continue
        if run_folder is not None and path_key(run_folder_from_entry(entry)) != path_key(run_folder):
            continue
        matches.append(entry)

    if run_id is not None and not matches:
        raise VerificationError(f"runs.json has no matching run_id: {run_id}")
    if len(matches) > 1:
        raise VerificationError("multiple runs matched; pass --run-id")
    return matches[0] if matches else None


def load_target(path: Path, run_id: str | None) -> tuple[Path, Path | None, dict[str, Any] | None]:
    if (path / "recording_session.json").exists():
        run_folder = path
        experiment_root = path.parent
        runs_path = experiment_root / "runs.json"
        runs_json = read_json(runs_path) if runs_path.exists() else None
        run_entry = find_run_entry(runs_json, run_folder, run_id) if runs_json else None
        return run_folder, experiment_root if runs_json else None, run_entry

    runs_path = path / "runs.json"
    if not runs_path.exists():
        raise VerificationError(
            f"{path} is neither an experiment root with runs.json nor a run folder with recording_session.json"
        )

    runs_json = read_json(runs_path)
    entries = entries_from_runs_json(runs_json)
    if run_id is not None:
        matches = [entry for entry in entries if entry.get("run_id") == run_id]
        require(bool(matches), f"runs.json has no run_id: {run_id}")
        require(len(matches) == 1, f"runs.json has duplicate run_id: {run_id}")
        run_entry = matches[0]
    else:
        require(bool(entries), "runs.json contains no runs")
        run_entry = entries[0]

    return run_folder_from_entry(run_entry), path, run_entry


def camera_results(entry: dict[str, Any] | None) -> list[dict[str, Any]]:
    if entry is None:
        return []
    rows = entry.get("camera_results")
    if not isinstance(rows, list):
        raise VerificationError(f"run {entry.get('run_id', '<unknown>')} missing camera_results")
    result: list[dict[str, Any]] = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise VerificationError(f"camera_results row {index} is not an object")
        result.append(row)
    return result


def manifest_camera_serials(manifest: dict[str, Any]) -> list[str]:
    cameras = manifest.get("cameras")
    if isinstance(cameras, list):
        camera_serials = [str(camera) for camera in cameras if str(camera)]
    elif isinstance(cameras, dict):
        camera_serials = [str(camera) for camera in cameras.keys() if str(camera)]
    else:
        raise VerificationError("recording_session.json has no cameras")
    require(bool(camera_serials), "recording_session.json has no cameras")
    require(
        len(camera_serials) == len(set(camera_serials)),
        f"recording_session.json has duplicate cameras: {camera_serials}",
    )
    return camera_serials


def select_cameras(manifest: dict[str, Any], requested_camera: str | None) -> list[str]:
    camera_serials = manifest_camera_serials(manifest)
    if requested_camera:
        require(
            requested_camera in camera_serials,
            f"camera {requested_camera} not listed in recording_session.json cameras={camera_serials}",
        )
        return [requested_camera]
    return camera_serials


def select_camera_row(entry: dict[str, Any] | None, camera: str) -> dict[str, Any] | None:
    for row in camera_results(entry):
        if str(row.get("camera_serial")) == camera:
            return row
    return None


def ffprobe_duration(path: Path, ffprobe: str) -> float:
    command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=duration,width,height:format=duration,size,bit_rate",
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
        stderr = result.stderr.strip()
        raise VerificationError(f"ffprobe failed for {path}: {stderr}")

    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise VerificationError(f"ffprobe returned invalid JSON for {path}: {exc}") from exc

    streams = payload.get("streams")
    require(isinstance(streams, list) and bool(streams), f"ffprobe found no video stream in {path}")
    stream = streams[0]
    if not isinstance(stream, dict):
        raise VerificationError(f"ffprobe stream payload is invalid for {path}")

    duration = stream.get("duration")
    if duration in (None, "N/A", ""):
        fmt = payload.get("format")
        if isinstance(fmt, dict):
            duration = fmt.get("duration")
    return as_float(duration, f"ffprobe duration for {path}")


def clip_artifact_value(clip: dict[str, Any], group: str, camera: str) -> str | None:
    artifacts = clip.get("artifacts")
    artifact_group = artifacts.get(group) if isinstance(artifacts, dict) else None
    value = artifact_group.get(camera) if isinstance(artifact_group, dict) else None
    return str(value) if value else None


def manifest_artifact_path(run_folder: Path, clip: dict[str, Any], group: str, camera: str) -> Path:
    fallback_suffix = {
        "videos": ".mp4",
        "metadata": "_meta.csv",
        "keyframes": "_keyframe.json",
    }
    value = clip_artifact_value(clip, group, camera)
    artifact_path = Path(value) if value else Path(f"Cam{camera}{fallback_suffix[group]}")
    if not artifact_path.is_absolute():
        artifact_path = run_folder / artifact_path
    return artifact_path


def verify_clip_artifact_coherence(run_folder: Path, manifest: dict[str, Any], clip: dict[str, Any], cameras: list[str]) -> None:
    manifest_folder = manifest.get("recording_folder")
    if isinstance(manifest_folder, str) and manifest_folder:
        require(
            path_key(Path(manifest_folder)) == path_key(run_folder),
            "recording_session.json recording_folder does not match selected run folder",
        )
    clip_folder = clip.get("recording_folder")
    if isinstance(clip_folder, str) and clip_folder:
        require(
            path_key(Path(clip_folder)) == path_key(run_folder),
            "clip recording_folder does not match selected run folder",
        )

    artifacts = clip.get("artifacts")
    require(isinstance(artifacts, dict), "clip missing artifacts object")
    for group in ("videos", "metadata", "keyframes"):
        artifact_group = artifacts.get(group)
        require(isinstance(artifact_group, dict), f"clip artifacts missing {group} object")
        for camera in cameras:
            require(
                camera in artifact_group,
                f"clip artifacts.{group} missing camera {camera}",
            )

    camera_artifacts = manifest.get("camera_artifacts")
    if isinstance(camera_artifacts, dict):
        for camera in cameras:
            row = camera_artifacts.get(camera)
            require(isinstance(row, dict), f"camera_artifacts missing camera {camera}")
            expected = {
                "video": clip_artifact_value(clip, "videos", camera),
                "metadata": clip_artifact_value(clip, "metadata", camera),
                "keyframes": clip_artifact_value(clip, "keyframes", camera),
            }
            for key, value in expected.items():
                require(
                    row.get(key) == value,
                    f"camera_artifacts.{camera}.{key} does not match clip artifacts",
                )


def verify_run_row(
    row: dict[str, Any],
    camera: str,
    record_for_seconds: float,
    clip_seconds: float,
    manifest_path: Path,
    ffprobe_duration_s: float,
    duration_tolerance_s: float,
) -> None:
    require(str(row.get("pass_fail")) == "pass", f"runs.json camera {camera} pass_fail is {row.get('pass_fail')!r}")
    require(str(row.get("status")) == "completed", f"runs.json camera {camera} status is {row.get('status')!r}")

    if "recording_control_record_for_seconds" in row:
        require(
            as_float(row.get("recording_control_record_for_seconds"), "recording_control_record_for_seconds")
            == record_for_seconds,
            "runs.json recording_control_record_for_seconds does not match recording_session.json",
        )
    if "recording_control_clip_seconds" in row:
        require(
            as_float(row.get("recording_control_clip_seconds"), "recording_control_clip_seconds")
            == clip_seconds,
            "runs.json recording_control_clip_seconds does not match recording_session.json",
        )
    if row.get("recording_session_manifest_path"):
        require(
            path_key(Path(str(row["recording_session_manifest_path"]))) == path_key(manifest_path),
            "runs.json recording_session_manifest_path does not match selected manifest",
        )

    for counter in HEALTH_COUNTERS:
        require(as_int(row.get(counter, 0), counter) == 0, f"runs.json {counter} is nonzero")

    if row.get("video_present") is not None:
        require(bool(row.get("video_present")), "runs.json video_present is false")
    if row.get("video_duration_s") is not None:
        row_video_duration = as_float(row.get("video_duration_s"), "video_duration_s")
        require(
            abs(row_video_duration - ffprobe_duration_s) <= 0.25,
            (
                "runs.json video_duration_s does not match ffprobe "
                f"({row_video_duration:.3f}s vs {ffprobe_duration_s:.3f}s)"
            ),
        )
    if row.get("recording_control_video_duration_error_s") is not None:
        duration_error = abs(as_float(row.get("recording_control_video_duration_error_s"), "recording_control_video_duration_error_s"))
        require(
            duration_error <= duration_tolerance_s,
            f"runs.json recording_control_video_duration_error_s exceeds tolerance: {duration_error:.3f}s",
        )
    if row.get("video_content_checked"):
        require(bool(row.get("video_content_valid")), "runs.json video_content_checked is true but video_content_valid is false")


def metadata_frame_ids(path: Path) -> list[int]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            lines = handle.readlines()
    except FileNotFoundError as exc:
        raise VerificationError(f"metadata artifact missing: {path}") from exc
    frame_ids: list[int] = []
    for line in lines[1:]:
        line = line.strip()
        if not line:
            continue
        frame_ids.append(as_int(line.split(",", 1)[0], f"metadata frame_id in {path}"))
    return frame_ids


def keyframe_frames(path: Path) -> list[int]:
    payload = read_json(path)
    frames = payload.get("keyframe_frames")
    require(isinstance(frames, list), f"keyframe sidecar missing keyframe_frames: {path}")
    return [as_int(frame, f"keyframe frame in {path}") for frame in frames]


def verify_common_manifest(
    manifest: dict[str, Any],
    duration_tolerance_s: float,
) -> tuple[list[str], dict[str, Any], float, float, dict[str, Any]]:
    recording_control = manifest.get("recording_control")
    require(isinstance(recording_control, dict), "recording_session.json missing recording_control")
    record_for_seconds = as_float(recording_control.get("record_for_seconds"), "recording_control.record_for_seconds")
    clip_seconds = as_float(recording_control.get("clip_seconds"), "recording_control.clip_seconds")
    require(record_for_seconds > 0, "recording_control.record_for_seconds must be positive")

    recording = manifest.get("recording")
    require(isinstance(recording, dict), "recording_session.json missing recording")
    require(bool(recording.get("started")), "recording.started is false")
    require(bool(recording.get("stop_requested")), "recording.stop_requested is false")
    require(recording.get("stop_reason") == "record_for_seconds_elapsed", f"unexpected stop_reason: {recording.get('stop_reason')!r}")
    require(bool(recording.get("drain_completed")), "recording.drain_completed is false")
    actual_recording_duration = as_float(
        recording.get("actual_recording_duration_s"),
        "recording.actual_recording_duration_s",
    )
    require(
        abs(actual_recording_duration - record_for_seconds) <= duration_tolerance_s,
        (
            "recording.actual_recording_duration_s outside tolerance "
            f"({actual_recording_duration:.3f}s vs requested {record_for_seconds:.3f}s)"
        ),
    )
    return manifest_camera_serials(manifest), recording_control, record_for_seconds, clip_seconds, recording


def verify_single_clip(
    args: argparse.Namespace,
    run_folder: Path,
    experiment_root: Path | None,
    run_entry: dict[str, Any] | None,
    manifest_path: Path,
    manifest: dict[str, Any],
    cameras: list[str],
    record_for_seconds: float,
    clip_seconds: float,
    recording: dict[str, Any],
) -> None:
    require(clip_seconds == 0, "single-clip timed verifier expects clip_seconds = 0")
    actual_recording_duration = as_float(
        recording.get("actual_recording_duration_s"),
        "recording.actual_recording_duration_s",
    )

    clips = manifest.get("clips")
    require(isinstance(clips, list) and len(clips) == 1, "expected exactly one manifest clip")
    clip = clips[0]
    require(isinstance(clip, dict), "clip entry is not an object")
    require(clip.get("clip_index") == 0, f"unexpected clip_index: {clip.get('clip_index')!r}")
    require(
        clip.get("clip_id") in {"clip_0000", "clip_000000"},
        f"unexpected clip_id: {clip.get('clip_id')!r}",
    )
    require(bool(clip.get("timed_stop_hit")), "clip.timed_stop_hit is false")
    require(bool(clip.get("drain_completed")), "clip.drain_completed is false")
    require(clip.get("stop_reason") == "record_for_seconds_elapsed", f"unexpected clip stop_reason: {clip.get('stop_reason')!r}")
    requested_clip_duration = as_float(clip.get("requested_duration_s"), "clip.requested_duration_s")
    require(
        requested_clip_duration == record_for_seconds,
        "clip.requested_duration_s does not match recording_control.record_for_seconds",
    )

    cameras = [camera for camera in cameras if not args.camera or camera == args.camera]
    verify_clip_artifact_coherence(run_folder, manifest, clip, cameras)

    ffprobe_durations: dict[str, float] = {}
    for camera in cameras:
        video_path = manifest_artifact_path(run_folder, clip, "videos", camera)
        metadata_path = manifest_artifact_path(run_folder, clip, "metadata", camera)
        keyframe_path = manifest_artifact_path(run_folder, clip, "keyframes", camera)
        require(video_path.exists(), f"video artifact missing for camera {camera}: {video_path}")
        require(metadata_path.exists(), f"metadata artifact missing for camera {camera}: {metadata_path}")
        require(keyframe_path.exists(), f"keyframe artifact missing for camera {camera}: {keyframe_path}")
        ffprobe_duration_s = ffprobe_duration(video_path, args.ffprobe)
        require(
            abs(ffprobe_duration_s - record_for_seconds) <= args.duration_tolerance_s,
            (
                f"encoded video duration outside tolerance for camera {camera} "
                f"({ffprobe_duration_s:.3f}s vs requested {record_for_seconds:.3f}s)"
            ),
        )
        ffprobe_durations[camera] = ffprobe_duration_s

        row = select_camera_row(run_entry, camera)
        if run_entry is not None:
            require(row is not None, f"runs.json has no camera_result for camera {camera}")
            verify_run_row(
                row,
                camera,
                record_for_seconds,
                clip_seconds,
                manifest_path,
                ffprobe_duration_s,
                args.duration_tolerance_s,
            )

    run_id = run_entry.get("run_id") if run_entry else run_folder.name
    experiment_text = f"\n  experiment: {experiment_root}" if experiment_root else ""
    print("Timed recording verification passed")
    print(f"  run: {run_id}")
    print(f"  folder: {run_folder}{experiment_text}")
    print(f"  cameras: {', '.join(cameras)}")
    print(f"  requested: {record_for_seconds:.3f}s")
    print(f"  manifest actual: {actual_recording_duration:.3f}s")
    for camera, duration in ffprobe_durations.items():
        print(f"  Cam{camera} ffprobe video: {duration:.3f}s")


def verify_rolling_clips(
    args: argparse.Namespace,
    run_folder: Path,
    experiment_root: Path | None,
    run_entry: dict[str, Any] | None,
    manifest_path: Path,
    manifest: dict[str, Any],
    cameras: list[str],
    record_for_seconds: float,
    clip_seconds: float,
    recording: dict[str, Any],
) -> None:
    require(clip_seconds > 0, "rolling-clip manifest requires clip_seconds > 0")
    selected_cameras = [camera for camera in cameras if not args.camera or camera == args.camera]
    require(bool(selected_cameras), f"camera {args.camera} not listed in recording_session.json cameras={cameras}")
    clips = manifest.get("clips")
    require(isinstance(clips, list) and bool(clips), "rolling manifest has no clips")
    if record_for_seconds > clip_seconds:
        require(len(clips) >= 2, "rolling manifest expected at least two clips")
    expected_min_clips = max(1, math.ceil((record_for_seconds - args.duration_tolerance_s) / clip_seconds))
    require(
        len(clips) >= expected_min_clips,
        f"rolling manifest has too few clips: {len(clips)} < {expected_min_clips}",
    )
    rollover_contract = manifest.get("rollover")
    require(isinstance(rollover_contract, dict), "rolling manifest missing rollover object")
    require(
        rollover_contract.get("implementation") == "headless_gop_boundary_writer_switch",
        f"unexpected rollover implementation: {rollover_contract.get('implementation')!r}",
    )
    require(bool(rollover_contract.get("seamless_writer_switch")), "rollover.seamless_writer_switch is false")
    require(bool(rollover_contract.get("next_writer_preopened")), "rollover.next_writer_preopened is false")
    if "records_during_rollover" in rollover_contract:
        require(bool(rollover_contract.get("records_during_rollover")), "rollover.records_during_rollover is false")

    ffprobe_totals: dict[str, float] = {camera: 0.0 for camera in selected_cameras}
    previous_last_frame_id: dict[str, int] = {}
    previous_rollover_at_frame_id: dict[str, int] = {}
    for expected_index, clip in enumerate(clips):
        require(isinstance(clip, dict), f"clip {expected_index} is not an object")
        require(clip.get("clip_index") == expected_index, f"unexpected clip_index: {clip.get('clip_index')!r}")
        require(clip.get("clip_id") == f"clip_{expected_index:06d}", f"unexpected clip_id: {clip.get('clip_id')!r}")
        require(bool(clip.get("drain_completed")), f"clip {expected_index} drain_completed is false")
        require(clip.get("status") == "completed", f"clip {expected_index} status is {clip.get('status')!r}")
        if expected_index < len(clips) - 1:
            require(clip.get("stop_reason") == "clip_seconds_elapsed", f"unexpected rollover stop_reason: {clip.get('stop_reason')!r}")
        else:
            require(clip.get("stop_reason") == "record_for_seconds_elapsed", f"unexpected final stop_reason: {clip.get('stop_reason')!r}")

        clip_folder = Path(str(clip.get("recording_folder", "")))
        require(bool(str(clip_folder)), f"clip {expected_index} missing recording_folder")
        require(clip_folder.exists(), f"clip folder missing: {clip_folder}")
        clip_manifest_path = clip_folder / "clip_manifest.json"
        clip_manifest = read_json(clip_manifest_path)
        require(clip_manifest.get("schema_id") == "orange.recording_clip", f"unexpected clip manifest schema: {clip_manifest.get('schema_id')!r}")
        require(clip_manifest.get("clip_id") == clip.get("clip_id"), "clip manifest clip_id mismatch")
        rollover = clip.get("rollover")
        require(isinstance(rollover, dict), f"clip {expected_index} missing rollover object")
        clip_manifest_rollover = clip_manifest.get("rollover")
        require(isinstance(clip_manifest_rollover, dict), f"clip manifest {expected_index} missing rollover object")
        for field in (
            "request_id",
            "rollover_at_recording_frame_id",
            "first_recording_frame_id",
            "last_recording_frame_id",
        ):
            require(
                as_int(clip_manifest_rollover.get(field, 0), f"clip_manifest.rollover.{field}")
                == as_int(rollover.get(field, 0), f"clip.rollover.{field}"),
                f"clip manifest rollover.{field} mismatch for clip {expected_index}",
            )

        for camera in selected_cameras:
            video_path = manifest_artifact_path(run_folder, clip, "videos", camera)
            metadata_path = manifest_artifact_path(run_folder, clip, "metadata", camera)
            keyframe_path = manifest_artifact_path(run_folder, clip, "keyframes", camera)
            require(video_path.exists(), f"video artifact missing for camera {camera}: {video_path}")
            require(metadata_path.exists(), f"metadata artifact missing for camera {camera}: {metadata_path}")
            require(keyframe_path.exists(), f"keyframe artifact missing for camera {camera}: {keyframe_path}")
            duration = ffprobe_duration(video_path, args.ffprobe)
            require(duration > 0, f"ffprobe duration is zero for camera {camera}: {video_path}")
            ffprobe_totals[camera] += duration
            keyframes = keyframe_frames(keyframe_path)
            require(bool(keyframes), f"keyframe sidecar has no keyframes for camera {camera}: {keyframe_path}")
            require(
                keyframes[0] == 0,
                f"clip {expected_index} for camera {camera} does not start on keyframe 0: {keyframes[:3]}",
            )

            frame_ids = metadata_frame_ids(metadata_path)
            require(bool(frame_ids), f"metadata has no frame rows for camera {camera}: {metadata_path}")
            for left, right in zip(frame_ids, frame_ids[1:]):
                require(right == left + 1, f"metadata frame_id gap inside {metadata_path}: {left} -> {right}")
            if camera in previous_last_frame_id:
                require(
                    frame_ids[0] == previous_last_frame_id[camera] + 1,
                    (
                        f"recording_frame_id is not continuous across clips for camera {camera}: "
                        f"{previous_last_frame_id[camera]} -> {frame_ids[0]}"
                    ),
                )
                if camera in previous_rollover_at_frame_id:
                    require(
                        frame_ids[0] == previous_rollover_at_frame_id[camera],
                        (
                            f"clip {expected_index} for camera {camera} did not start at prior "
                            f"rollover frame {previous_rollover_at_frame_id[camera]}: {frame_ids[0]}"
                        ),
                    )
            manifest_first = as_int(rollover.get("first_recording_frame_id", 0), "rollover.first_recording_frame_id")
            manifest_last = as_int(rollover.get("last_recording_frame_id", 0), "rollover.last_recording_frame_id")
            require(
                manifest_first == frame_ids[0],
                (
                    f"clip {expected_index} first_recording_frame_id mismatch for camera {camera}: "
                    f"{manifest_first} vs metadata {frame_ids[0]}"
                ),
            )
            require(
                manifest_last == frame_ids[-1],
                (
                    f"clip {expected_index} last_recording_frame_id mismatch for camera {camera}: "
                    f"{manifest_last} vs metadata {frame_ids[-1]}"
                ),
            )
            if expected_index < len(clips) - 1:
                rollover_at = as_int(
                    rollover.get("rollover_at_recording_frame_id", 0),
                    "rollover.rollover_at_recording_frame_id",
                )
                require(rollover_at > 0, f"clip {expected_index} missing rollover_at_recording_frame_id")
                require(
                    frame_ids[-1] == rollover_at - 1,
                    (
                        f"clip {expected_index} last frame does not end before rollover frame "
                        f"for camera {camera}: {frame_ids[-1]} vs {rollover_at}"
                    ),
                )
                previous_rollover_at_frame_id[camera] = rollover_at
            previous_last_frame_id[camera] = frame_ids[-1]

    for camera, total_duration in ffprobe_totals.items():
        require(
            abs(total_duration - record_for_seconds) <= args.duration_tolerance_s,
            (
                f"total rolling video duration outside tolerance for camera {camera} "
                f"({total_duration:.3f}s vs requested {record_for_seconds:.3f}s)"
            ),
        )
        row = select_camera_row(run_entry, camera)
        if run_entry is not None:
            require(row is not None, f"runs.json has no camera_result for camera {camera}")
            verify_run_row(
                row,
                camera,
                record_for_seconds,
                clip_seconds,
                manifest_path,
                total_duration,
                args.duration_tolerance_s,
            )

    run_id = run_entry.get("run_id") if run_entry else run_folder.name
    experiment_text = f"\n  experiment: {experiment_root}" if experiment_root else ""
    print("Rolling timed recording verification passed")
    print(f"  run: {run_id}")
    print(f"  folder: {run_folder}{experiment_text}")
    print(f"  cameras: {', '.join(selected_cameras)}")
    print(f"  requested: {record_for_seconds:.3f}s")
    print(f"  clips: {len(clips)}")
    for camera, duration in ffprobe_totals.items():
        print(f"  Cam{camera} total ffprobe video: {duration:.3f}s")


def verify(args: argparse.Namespace) -> None:
    target_path = Path(args.path).expanduser()
    run_folder, experiment_root, run_entry = load_target(target_path, args.run_id)
    manifest_path = run_folder / "recording_session.json"
    manifest = read_json(manifest_path)

    schema_id = manifest.get("schema_id")
    require(schema_id in ACCEPTED_SCHEMA_IDS, f"unexpected recording_session schema_id: {schema_id!r}")
    require(manifest.get("schema_version") == 1, f"unexpected schema_version: {manifest.get('schema_version')!r}")
    require(manifest.get("status") == "completed", f"expected completed manifest, got {manifest.get('status')!r}")
    cameras, _recording_control, record_for_seconds, clip_seconds, recording = verify_common_manifest(
        manifest,
        args.duration_tolerance_s,
    )
    if args.camera:
        require(args.camera in cameras, f"camera {args.camera} not listed in recording_session.json cameras={cameras}")

    mode = manifest.get("mode")
    if mode == "single_clip":
        verify_single_clip(
            args,
            run_folder,
            experiment_root,
            run_entry,
            manifest_path,
            manifest,
            cameras,
            record_for_seconds,
            clip_seconds,
            recording,
        )
    elif mode == "rolling_clips":
        verify_rolling_clips(
            args,
            run_folder,
            experiment_root,
            run_entry,
            manifest_path,
            manifest,
            cameras,
            record_for_seconds,
            clip_seconds,
            recording,
        )
    else:
        raise VerificationError(f"unexpected recording_session mode: {mode!r}")


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
