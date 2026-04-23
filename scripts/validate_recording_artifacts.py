#!/usr/bin/env python3
"""Validate internal consistency of an Orange recording folder."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Any


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")


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
        "stream=width,height,nb_frames,nb_read_frames,avg_frame_rate,duration",
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

    return streams[0]


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

        bad_geometry_rows = [
            index
            for index, row in enumerate(crop_rows, start=2)
            if int_field(row, "crop_w", crop_meta_path) != crop_size
            or int_field(row, "crop_h", crop_meta_path) != crop_size
        ]
        reporter.check(
            not bad_geometry_rows,
            f"Cam{serial} crop metadata crop_w/crop_h match crop_size_px",
            (
                f"Cam{serial} crop metadata has {len(bad_geometry_rows)} rows "
                "with crop_w/crop_h not matching crop_size_px"
            ),
        )

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
