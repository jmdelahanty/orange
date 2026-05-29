#!/usr/bin/env python3
"""Validate a production-like GUI PTP recording folder."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import summarize_gui_validation as gui_summary
from recording_output_validation import (
    recording_clip_output_contract_errors,
    recording_output_contract_errors,
)


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
        "--expect-recording-mode",
        choices=("single_clip", "rolling_clips"),
        help=(
            "Optional expected recording_session.json mode. Use rolling_clips "
            "for GUI external IPC rollover validation."
        ),
    )
    parser.add_argument(
        "--expect-record-for-seconds",
        type=int,
        help="Optional expected recording_control.record_for_seconds value.",
    )
    parser.add_argument(
        "--expect-clip-seconds",
        type=int,
        help="Optional expected recording_control.clip_seconds value.",
    )
    parser.add_argument(
        "--expect-local-control-stop-method",
        choices=("stop_recording", "citrus_completion"),
        help="Optional expected recording.control.method in recording_session.json.",
    )
    parser.add_argument(
        "--expect-local-control-stop-operation-id",
        help="Optional expected recording.control.operation_id in recording_session.json.",
    )
    parser.add_argument(
        "--expect-local-control-stop-command-source",
        help="Optional expected recording.control.command_source in recording_session.json.",
    )
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
        "--max-yolo-acquisition-to-worker-start-p95-ms",
        type=float,
        default=None,
        help="Optional fail threshold for acquisition_to_worker_start_ms p95.",
    )
    parser.add_argument(
        "--max-yolo-enqueue-to-dequeue-p95-ms",
        type=float,
        default=None,
        help="Optional fail threshold for yolo_enqueue_to_dequeue_ms p95.",
    )
    parser.add_argument(
        "--max-yolo-dequeue-to-worker-start-p95-ms",
        type=float,
        default=None,
        help="Optional fail threshold for yolo_dequeue_to_worker_start_ms p95.",
    )
    parser.add_argument(
        "--max-yolo-same-camera-service-gap-p95-ms",
        type=float,
        default=None,
        help="Optional fail threshold for same_camera_service_gap_ms p95.",
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
        "--allow-main-video-content-failure",
        action="append",
        default=[],
        metavar="SERIAL[,SERIAL...]",
        help=(
            "Allow low-bitrate or decoded-content sanity failures for known "
            "optically invalid main camera videos, such as a camera with no "
            "lens attached. This does not allow missing videos, ffprobe "
            "failures, invalid dimensions, recorder errors, or crop failures."
        ),
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
        "--expect-gui-swap-interval",
        type=int,
        default=None,
        help="Optional expected recording_snapshot session.gui_display_frame_rate.swap_interval value.",
    )
    parser.add_argument(
        "--expect-gui-frame-max-fps",
        type=int,
        default=None,
        help="Optional expected recording_snapshot session.gui_display_frame_rate.frame_max_fps value.",
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
        "--expect-yolo-affinity",
        action="append",
        default=[],
        metavar="SERIAL=CPU",
        help=(
            "Optional per-camera expected YOLO worker CPU affinity. Checks both "
            "recording_snapshot session.yolo_worker requested affinity and "
            "Cam*_yolo_perf.csv effective worker affinity columns."
        ),
    )
    parser.add_argument(
        "--require-isolated-cpus",
        default="",
        metavar="CPU[,CPU...]",
        help=(
            "Optional comma/range CPU list that must be present in "
            "recording_snapshot session.system_cpu.isolated_cpus. Use this "
            "to prove the kernel isolation set was active for affinity runs."
        ),
    )
    parser.add_argument(
        "--require-kernel-cmdline-cpus",
        action="append",
        default=[],
        metavar="OPTION=CPU[,CPU...]",
        help=(
            "Optional repeated /proc/cmdline CPU-list requirement, for example "
            "isolcpus=6,8,10,12 or nohz_full=6,8,10,12. Checks the value "
            "captured in recording_snapshot session.system_cpu.kernel_cmdline.options."
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
        "--require-imgui-glfw-size-cache",
        action="store_true",
        help=(
            "Require recording_snapshot session.gui_display_frame_rate."
            "imgui_glfw_size_cache telemetry showing cached main-window "
            "size queries with no fallback GLFW polling."
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
        "--require-external-crop-recorder-gpu-separate-from-analytics",
        action="store_true",
        help=(
            "For external crop streams, fail unless recorder_gpu_id differs from "
            "analytics_gpu_id. Use this to prove the crop recorder is not on "
            "the same CUDA device as the crop-production source GPU."
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
        "--require-external-recorder-status",
        action="store_true",
        help=(
            "For full-frame and crop external_ipc recorder contracts present in "
            "the artifact, require status_json heartbeat sidecars and parsed "
            "external_recorder_supervisor_runtime.json recorder_status entries."
        ),
    )
    parser.add_argument(
        "--require-external-recorder-storage-preflight",
        action="store_true",
        help=(
            "For full-frame and crop external_ipc recorder contracts present in "
            "the artifact, require storage_preflight payloads in recorder "
            "status/summary JSON and parsed runtime storage fields."
        ),
    )
    parser.add_argument(
        "--require-external-recorder-protocol-hello",
        action="store_true",
        help=(
            "For full-frame and crop external_ipc recorder contracts present in "
            "the artifact, require recorder status/summary/runtime evidence of "
            "the versioned IPC hello handshake."
        ),
    )
    parser.add_argument(
        "--require-source-version",
        action="store_true",
        help=(
            "Require recording_snapshot.json source_version Git provenance with "
            "commit, producer_version, usable git command metadata, and tracked "
            "dirty-state telemetry."
        ),
    )
    parser.add_argument(
        "--expect-source-git-command-user-mode",
        choices=("process_euid", "sudo_invoking_user"),
        default=None,
        help=(
            "Optional expected source_version.git_command_user.mode. Use "
            "sudo_invoking_user for sudo-launched GUI validation runs that should "
            "drop Git commands to SUDO_UID/SUDO_GID."
        ),
    )
    parser.add_argument(
        "--expect-source-dirty-tracked",
        type=int,
        choices=(0, 1),
        default=None,
        help="Optional expected source_version.dirty_tracked value.",
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


def keyframe_frame_values(payload: dict[str, Any]) -> list[int] | None:
    frames = payload.get("keyframe_frames")
    if not isinstance(frames, list):
        return None
    out: list[int] = []
    for frame in frames:
        value = integer(frame)
        if value is None:
            return None
        out.append(value)
    return out


def check_keyframe_sidecar_starts_at_zero(
    reporter: Reporter,
    payload: dict[str, Any],
    path: Path,
    label: str,
) -> list[int] | None:
    frames = keyframe_frame_values(payload)
    reporter.check(
        frames is not None and bool(frames),
        f"{label} keyframe frames present",
        f"{label} keyframe sidecar missing keyframe_frames or has no keyframes: {path}",
    )
    if frames:
        reporter.check(
            frames[0] == 0,
            f"{label} starts with keyframe frame 0",
            f"{label} first keyframe frame {frames[0]} != 0",
        )
    return frames


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
    clips = manifest.get("clips")
    clips = clips if isinstance(clips, list) else []
    for clip in clips:
        clip = clip if isinstance(clip, dict) else {}
        clip_artifacts = clip.get("camera_artifacts")
        clip_artifacts = clip_artifacts if isinstance(clip_artifacts, dict) else {}
        for serial, artifact in clip_artifacts.items():
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


def parse_serial_filter(values: list[str], option_name: str) -> set[str]:
    serials: set[str] = set()
    for raw in values:
        raw = raw.strip()
        if not raw:
            raise SystemExit(f"{option_name} has an empty value")
        for token in raw.split(","):
            serial = token.strip()
            if not serial:
                raise SystemExit(f"{option_name} has an empty serial in {raw!r}")
            serials.add(serial)
    return serials


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


def parse_cpu_list(value: str, option_name: str) -> list[int]:
    if not value.strip():
        return []
    cpus: set[int] = set()
    for token in value.split(","):
        token = token.strip()
        if not token:
            raise SystemExit(f"{option_name} has an empty CPU token in {value!r}")
        if "-" in token:
            first_text, last_text = token.split("-", 1)
            try:
                first = int(first_text)
                last = int(last_text)
            except ValueError as exc:
                raise SystemExit(f"{option_name} has an invalid CPU range {token!r}") from exc
            if first < 0 or last < 0 or first > last:
                raise SystemExit(f"{option_name} has an invalid CPU range {token!r}")
            cpus.update(range(first, last + 1))
            continue
        try:
            cpu = int(token)
        except ValueError as exc:
            raise SystemExit(f"{option_name} has an invalid CPU token {token!r}") from exc
        if cpu < 0:
            raise SystemExit(f"{option_name} CPU values must be >= 0, got {token!r}")
        cpus.add(cpu)
    return sorted(cpus)


def parse_expected_option_cpu_map(values: list[str], option_name: str) -> dict[str, list[int]]:
    parsed: dict[str, list[int]] = {}
    for raw in values:
        if "=" not in raw:
            raise SystemExit(f"{option_name} must use OPTION=CPU[,CPU...], got {raw!r}")
        option, value_text = raw.split("=", 1)
        option = option.strip()
        value_text = value_text.strip()
        if not option:
            raise SystemExit(f"{option_name} has an empty option name in {raw!r}")
        cpus = parse_cpu_list(value_text, option_name)
        if not cpus:
            raise SystemExit(f"{option_name} must require at least one CPU, got {raw!r}")
        parsed[option] = cpus
    return parsed


def parse_kernel_cmdline_cpu_option_value(value: Any) -> tuple[set[int], list[str]]:
    if not isinstance(value, str):
        return set(), ["<non-string>"]
    cpus: set[int] = set()
    invalid: list[str] = []
    for raw_token in value.split(","):
        token = raw_token.strip()
        if not token:
            continue
        if re.fullmatch(r"\d+", token):
            cpus.add(int(token))
            continue
        range_match = re.fullmatch(r"(\d+)-(\d+)", token)
        if range_match:
            first = int(range_match.group(1))
            last = int(range_match.group(2))
            if first <= last:
                cpus.update(range(first, last + 1))
            else:
                invalid.append(token)
            continue
        if token[0].isdigit():
            invalid.append(token)
            continue
        # isolcpus can include non-CPU flags such as "domain" or "managed_irq".
    return cpus, invalid


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


def first_present(*values: Any) -> Any:
    for value in values:
        if value is not None:
            return value
    return None


def external_crop_contract_stream_config(
    contract: dict[str, Any],
    serial: str,
    stream_id: str | None,
) -> dict[str, Any]:
    streams = nested_dict(contract, "streams")
    if not streams:
        return {}

    candidates = [
        value
        for value in (stream_id, f"{serial}_crop")
        if isinstance(value, str) and value
    ]
    for candidate in candidates:
        value = streams.get(candidate)
        if isinstance(value, dict):
            return value

    for value in streams.values():
        if not isinstance(value, dict):
            continue
        if value.get("stream_id") in candidates or value.get("camera_serial") in candidates:
            return value
    return {}


def descriptor_stream_config(details: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(details, dict):
        return {}
    fields = (
        "stream_id",
        "analytics_gpu_id",
        "recorder_gpu_id",
        "socket_path",
        "encode_queue_depth",
        "summary_json",
        "status_json",
    )
    return {field: details[field] for field in fields if field in details}


def merge_stream_config_with_fallbacks(*configs: dict[str, Any]) -> dict[str, Any]:
    merged: dict[str, Any] = {}
    for config in configs:
        if not isinstance(config, dict):
            continue
        for key, value in config.items():
            if key not in merged and value not in (None, ""):
                merged[key] = value
    return merged


def path_from_recording_folder(recording_folder: Path, value: Any) -> Path:
    path = Path(str(value or ""))
    return path if path.is_absolute() else recording_folder / path


def summary_path_from_stream(recording_folder: Path, stream: dict[str, Any]) -> Path | None:
    summary_json = stream.get("summary_json")
    if isinstance(summary_json, str) and summary_json:
        return path_from_recording_folder(recording_folder, summary_json)
    return None


def status_path_from_stream(recording_folder: Path, stream: dict[str, Any]) -> Path | None:
    status_json = stream.get("status_json")
    if isinstance(status_json, str) and status_json:
        return path_from_recording_folder(recording_folder, status_json)
    summary_path = summary_path_from_stream(recording_folder, stream)
    if summary_path is not None:
        name = summary_path.name
        if name.endswith("_summary.json"):
            return summary_path.with_name(name[: -len("_summary.json")] + "_status.json")
        return summary_path.with_name(summary_path.stem + "_status.json")
    return None


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


def check_crop_rollover_node(
    reporter: Reporter,
    node: dict[str, Any],
    label: str,
    *,
    require_present: bool = False,
    allow_rolling: bool = False,
) -> None:
    control = node.get("recording_control")
    control = control if isinstance(control, dict) else {}
    rollover = node.get("rollover")
    rollover = rollover if isinstance(rollover, dict) else {}

    if require_present or control:
        record_for_seconds = integer(control.get("record_for_seconds"))
        clip_seconds = integer(control.get("clip_seconds"))
        if allow_rolling:
            reporter.check(
                record_for_seconds is not None and record_for_seconds >= 0,
                f"{label} crop recording_control has nonnegative record_for_seconds",
                f"{label} crop recording_control invalid record_for_seconds={record_for_seconds}",
            )
            reporter.check(
                clip_seconds is not None and clip_seconds > 0,
                f"{label} crop recording_control declares rolling",
                (
                    f"{label} crop recording_control does not request rolling: "
                    f"record_for_seconds={record_for_seconds}, clip_seconds={clip_seconds}"
                ),
            )
        else:
            reporter.check(
                record_for_seconds == 0 and clip_seconds == 0,
                f"{label} crop recording_control declares single_clip",
                (
                    f"{label} crop recording_control requests rolling or timed control: "
                    f"record_for_seconds={record_for_seconds}, clip_seconds={clip_seconds}"
                ),
            )
    if require_present or rollover:
        requested = rollover.get("requested")
        status = rollover.get("status")
        implementation = rollover.get("implementation")
        if allow_rolling:
            reporter.check(
                requested is True,
                f"{label} crop rollover requested=true",
                f"{label} crop rollover requested={requested!r}; expected rolling crop output",
            )
            reporter.check(
                status in {"supported", "completed", "incomplete"} and
                implementation == "external_recorder_gop_boundary_writer_rotation",
                f"{label} crop rollover uses external recorder writer rotation",
                (
                    f"{label} crop rollover status/implementation unexpected: "
                    f"{status!r}/{implementation!r}"
                ),
            )
            reporter.check(
                rollover.get("seamless_writer_switch") is True,
                f"{label} crop rollover seamless writer switch=true",
                f"{label} crop rollover seamless_writer_switch={rollover.get('seamless_writer_switch')!r}",
            )
            reporter.check(
                rollover.get("records_during_rollover") is True,
                f"{label} crop rollover records during rollover=true",
                f"{label} crop rollover records_during_rollover={rollover.get('records_during_rollover')!r}",
            )
            if "output_kind" in rollover:
                reporter.check(
                    rollover.get("output_kind") == "crop",
                    f"{label} crop rollover output_kind=crop",
                    f"{label} crop rollover output_kind={rollover.get('output_kind')!r}",
                )
            if "supported_mode" in rollover:
                reporter.check(
                    rollover.get("supported_mode") == "rolling_clips",
                    f"{label} crop rollover supported_mode=rolling_clips",
                    f"{label} crop rollover supported_mode={rollover.get('supported_mode')!r}",
                )
            if "rolling_supported" in rollover:
                reporter.check(
                    rollover.get("rolling_supported") is True,
                    f"{label} crop rolling_supported=true",
                    f"{label} crop rolling_supported={rollover.get('rolling_supported')!r}",
                )
        else:
            reporter.check(
                requested is False,
                f"{label} crop rollover requested=false",
                f"{label} crop rollover requested={requested!r}; crop rolling requires rolling_clips metadata",
            )
            reporter.check(
                status == "not_requested" and implementation == "none",
                f"{label} crop rollover status is not_requested",
                (
                    f"{label} crop rollover status/implementation unexpected: "
                    f"{status!r}/{implementation!r}"
                ),
            )
            if "rolling_supported" in rollover:
                reporter.check(
                    isinstance(rollover.get("rolling_supported"), bool),
                    f"{label} crop rolling_supported is explicit",
                    f"{label} crop rolling_supported={rollover.get('rolling_supported')!r}",
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
    expected_recording_mode: str | None = None,
    expected_record_for_seconds: int | None = None,
    expected_clip_seconds: int | None = None,
    expected_local_control_stop_method: str | None = None,
    expected_local_control_stop_operation_id: str | None = None,
    expected_local_control_stop_command_source: str | None = None,
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
    mode = str(manifest.get("mode", ""))
    if expected_recording_mode:
        reporter.check(
            mode == expected_recording_mode,
            f"recording_session mode is expected {expected_recording_mode}",
            (
                f"recording_session mode={manifest.get('mode')!r}; "
                f"expected {expected_recording_mode!r}"
            ),
        )
    else:
        reporter.check(
            mode in {"single_clip", "rolling_clips"},
            f"recording_session mode is {mode}",
            f"recording_session mode={manifest.get('mode')!r}",
        )

    check_recording_control_expectations(
        reporter,
        manifest,
        mode,
        expected_record_for_seconds,
        expected_clip_seconds,
    )
    check_local_control_stop_expectations(
        reporter,
        manifest,
        expected_local_control_stop_method,
        expected_local_control_stop_operation_id,
        expected_local_control_stop_command_source,
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
        snapshot_session.get("recording_mode") == mode,
        f"recording_snapshot session mode is {mode}",
        f"recording_snapshot recording_mode={snapshot_session.get('recording_mode')!r}",
    )

    if mode == "rolling_clips":
        check_rolling_recording_session_manifest(
            reporter,
            recording_folder,
            manifest,
            snapshot_session,
            cameras,
        )
        return

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


def check_source_version(
    reporter: Reporter,
    snapshot: dict[str, Any],
    *,
    require_source_version: bool,
    expected_git_command_user_mode: str | None,
    expected_dirty_tracked: int | None,
) -> dict[str, Any]:
    source = snapshot.get("source_version")
    source = source if isinstance(source, dict) else {}
    if (
        not require_source_version
        and expected_git_command_user_mode is None
        and expected_dirty_tracked is None
    ):
        return source

    producer_version = snapshot.get("producer_version")
    commit = source.get("commit")
    commit_short = source.get("commit_short")
    git_user = source.get("git_command_user")
    git_user = git_user if isinstance(git_user, dict) else {}
    dirty_tracked_available = source.get("dirty_tracked_available")
    dirty_tracked = source.get("dirty_tracked")

    if require_source_version:
        reporter.check(
            bool(source),
            "recording_snapshot source_version present",
            "recording_snapshot source_version missing",
        )
        reporter.check(
            source.get("schema_version") == 1,
            "source_version schema_version=1",
            f"source_version schema_version={source.get('schema_version')!r}",
        )
        reporter.check(
            source.get("vcs") == "git",
            "source_version vcs=git",
            f"source_version vcs={source.get('vcs')!r}",
        )
        reporter.check(
            source.get("available") is True,
            "source_version Git provenance available",
            f"source_version available={source.get('available')!r}",
        )
        reporter.check(
            isinstance(source.get("worktree"), str) and bool(source.get("worktree")),
            "source_version worktree present",
            f"source_version worktree={source.get('worktree')!r}",
        )
        reporter.check(
            isinstance(commit, str) and len(commit) >= 7,
            "source_version commit present",
            f"source_version commit={commit!r}",
        )
        reporter.check(
            isinstance(commit_short, str)
            and bool(commit_short)
            and isinstance(commit, str)
            and commit.startswith(commit_short),
            "source_version commit_short matches commit",
            f"source_version commit_short={commit_short!r}, commit={commit!r}",
        )
        reporter.check(
            producer_version == commit_short,
            "producer_version matches source_version commit_short",
            (
                f"producer_version={producer_version!r}, "
                f"source_version.commit_short={commit_short!r}"
            ),
        )
        reporter.check(
            source.get("git_command_available") is True,
            "source_version git command available",
            f"source_version git_command_available={source.get('git_command_available')!r}",
        )
        reporter.check(
            bool(git_user.get("mode")),
            "source_version git_command_user mode present",
            f"source_version git_command_user.mode={git_user.get('mode')!r}",
        )
        reporter.check(
            dirty_tracked_available is True,
            "source_version tracked dirty-state telemetry available",
            f"source_version dirty_tracked_available={dirty_tracked_available!r}",
        )
        reporter.check(
            isinstance(dirty_tracked, bool),
            "source_version dirty_tracked present",
            f"source_version dirty_tracked={dirty_tracked!r}",
        )

    if expected_git_command_user_mode is not None:
        reporter.check(
            git_user.get("mode") == expected_git_command_user_mode,
            f"source_version git_command_user.mode={expected_git_command_user_mode}",
            (
                f"source_version git_command_user.mode={git_user.get('mode')!r}; "
                f"expected {expected_git_command_user_mode!r}"
            ),
        )
        if expected_git_command_user_mode == "sudo_invoking_user":
            reporter.check(
                git_user.get("uid") not in {None, 0},
                "source_version git_command_user uid is sudo invoking user",
                f"source_version git_command_user.uid={git_user.get('uid')!r}",
            )
            reporter.check(
                git_user.get("gid") not in {None, 0},
                "source_version git_command_user gid is sudo invoking user",
                f"source_version git_command_user.gid={git_user.get('gid')!r}",
            )

    if expected_dirty_tracked is not None:
        expected_bool = bool(expected_dirty_tracked)
        reporter.check(
            dirty_tracked == expected_bool,
            f"source_version dirty_tracked={expected_bool}",
            (
                f"source_version dirty_tracked={dirty_tracked!r}; "
                f"expected {expected_bool!r}"
            ),
        )
    return source


def index_path_from_session(
    recording_folder: Path,
    manifest_indexes: dict[str, Any],
    snapshot_indexes: dict[str, Any],
    absolute_key: str,
    relative_key: str,
) -> Path:
    value = first_present(snapshot_indexes.get(absolute_key), manifest_indexes.get(absolute_key))
    if isinstance(value, str) and value:
        return path_from_recording_folder(recording_folder, value)
    value = first_present(snapshot_indexes.get(relative_key), manifest_indexes.get(relative_key))
    if not isinstance(value, str) or not value:
        return recording_folder / f"__missing_{relative_key}"
    return path_from_recording_folder(recording_folder, value)


def check_recording_control_expectations(
    reporter: Reporter,
    manifest: dict[str, Any],
    mode: str,
    expected_record_for_seconds: int | None,
    expected_clip_seconds: int | None,
) -> None:
    control = manifest.get("recording_control")
    control = control if isinstance(control, dict) else None
    requires_control = (
        mode == "rolling_clips"
        or expected_record_for_seconds is not None
        or expected_clip_seconds is not None
    )
    if not requires_control:
        return

    reporter.check(
        control is not None,
        "recording_session recording_control present",
        "recording_session recording_control missing",
    )
    if control is None:
        return

    record_for_seconds = integer(control.get("record_for_seconds"))
    clip_seconds = integer(control.get("clip_seconds"))
    if mode == "rolling_clips":
        reporter.check(
            record_for_seconds is not None and record_for_seconds > 0,
            f"recording_session record_for_seconds={record_for_seconds}",
            (
                "recording_session rolling_clips requires positive "
                f"record_for_seconds, got {record_for_seconds}"
            ),
        )
        reporter.check(
            clip_seconds is not None and clip_seconds > 0,
            f"recording_session clip_seconds={clip_seconds}",
            (
                "recording_session rolling_clips requires positive "
                f"clip_seconds, got {clip_seconds}"
            ),
        )
    if expected_record_for_seconds is not None:
        reporter.check(
            record_for_seconds == expected_record_for_seconds,
            f"recording_session record_for_seconds={expected_record_for_seconds}",
            (
                f"recording_session record_for_seconds={record_for_seconds}; "
                f"expected {expected_record_for_seconds}"
            ),
        )
    if expected_clip_seconds is not None:
        reporter.check(
            clip_seconds == expected_clip_seconds,
            f"recording_session clip_seconds={expected_clip_seconds}",
            (
                f"recording_session clip_seconds={clip_seconds}; "
                f"expected {expected_clip_seconds}"
            ),
        )


def check_local_control_stop_expectations(
    reporter: Reporter,
    manifest: dict[str, Any],
    expected_method: str | None,
    expected_operation_id: str | None,
    expected_command_source: str | None,
) -> None:
    requires_control = any(
        value is not None
        for value in (
            expected_method,
            expected_operation_id,
            expected_command_source,
        )
    )
    if not requires_control:
        return

    recording = manifest.get("recording")
    recording = recording if isinstance(recording, dict) else {}
    control = recording.get("control")
    control = control if isinstance(control, dict) else None
    reporter.check(
        control is not None,
        "recording_session local-control stop metadata present",
        "recording_session recording.control missing",
    )
    if control is None:
        return

    source = control.get("source")
    reporter.check(
        source == "orange_gui_local_control",
        "recording_session recording.control source is orange_gui_local_control",
        f"recording_session recording.control source={source!r}",
    )
    request_id = control.get("request_id")
    reporter.check(
        isinstance(request_id, str) and bool(request_id),
        "recording_session recording.control request_id present",
        f"recording_session recording.control request_id={request_id!r}",
    )
    received_at_utc = control.get("received_at_utc")
    reporter.check(
        isinstance(received_at_utc, str) and bool(received_at_utc),
        "recording_session recording.control received_at_utc present",
        (
            "recording_session recording.control received_at_utc="
            f"{received_at_utc!r}"
        ),
    )
    stop_triggered_at_utc = control.get("stop_triggered_at_utc")
    reporter.check(
        isinstance(stop_triggered_at_utc, str) and bool(stop_triggered_at_utc),
        "recording_session recording.control stop_triggered_at_utc present",
        (
            "recording_session recording.control stop_triggered_at_utc="
            f"{stop_triggered_at_utc!r}"
        ),
    )

    ack_state = control.get("ack_state")
    reporter.check(
        isinstance(ack_state, str) and bool(ack_state),
        "recording_session recording.control ack_state present",
        f"recording_session recording.control ack_state={ack_state!r}",
    )

    drain_completed = control.get("drain_completed")
    if drain_completed is not None:
        reporter.check(
            isinstance(drain_completed, bool),
            "recording_session recording.control drain_completed is boolean",
            (
                "recording_session recording.control drain_completed="
                f"{drain_completed!r}"
            ),
        )
        if drain_completed is True:
            drain_completed_at_utc = control.get("drain_completed_at_utc")
            reporter.check(
                isinstance(drain_completed_at_utc, str)
                and bool(drain_completed_at_utc),
                "recording_session recording.control drain_completed_at_utc present",
                (
                    "recording_session recording.control drain_completed_at_utc="
                    f"{drain_completed_at_utc!r}"
                ),
            )

    drain_timed_out = control.get("drain_timed_out")
    if drain_timed_out is not None:
        reporter.check(
            isinstance(drain_timed_out, bool),
            "recording_session recording.control drain_timed_out is boolean",
            (
                "recording_session recording.control drain_timed_out="
                f"{drain_timed_out!r}"
            ),
        )
        if drain_timed_out is True:
            forced_finalize_requested = control.get("forced_finalize_requested")
            reporter.check(
                forced_finalize_requested is True,
                "recording_session recording.control forced_finalize_requested=true after drain timeout",
                (
                    "recording_session recording.control drain_timed_out=true "
                    "but forced_finalize_requested="
                    f"{forced_finalize_requested!r}"
                ),
            )
            reporter.check(
                control.get("error_code") == "drain_timeout",
                "recording_session recording.control error_code=drain_timeout",
                (
                    "recording_session recording.control error_code="
                    f"{control.get('error_code')!r}; expected 'drain_timeout'"
                ),
            )
            reporter.check(
                control.get("ack_state") == "failed_timeout",
                "recording_session recording.control ack_state=failed_timeout after drain timeout",
                (
                    "recording_session recording.control drain_timed_out=true "
                    f"but ack_state={control.get('ack_state')!r}"
                ),
            )
        elif drain_timed_out is False and control.get("drain_completed") is True:
            reporter.check(
                control.get("ack_state") == "executed",
                "recording_session recording.control ack_state=executed after finalized drain",
                (
                    "recording_session recording.control clean finalized drain "
                    f"but ack_state={control.get('ack_state')!r}"
                ),
            )

    forced_finalize_requested = control.get("forced_finalize_requested")
    if forced_finalize_requested is True:
        forced_at = control.get("forced_finalize_requested_at_utc")
        reporter.check(
            isinstance(forced_at, str) and bool(forced_at),
            (
                "recording_session recording.control "
                "forced_finalize_requested_at_utc present"
            ),
            (
                "recording_session recording.control "
                f"forced_finalize_requested_at_utc={forced_at!r}"
            ),
        )

    if control.get("forced_finalize_stream_stop_requested") is True:
        reporter.check(
            control.get("forced_finalize_requested") is True,
            (
                "recording_session recording.control forced stream-stop "
                "requires forced finalize"
            ),
            (
                "recording_session recording.control "
                "forced_finalize_stream_stop_requested=true but "
                f"forced_finalize_requested={control.get('forced_finalize_requested')!r}"
            ),
        )

    if control.get("last_event") == "finalized_after_drain_timeout":
        reporter.check(
            control.get("drain_timed_out") is True,
            (
                "recording_session recording.control finalized-after-timeout "
                "requires drain_timed_out=true"
            ),
            (
                "recording_session recording.control last_event="
                "'finalized_after_drain_timeout' but drain_timed_out="
                f"{control.get('drain_timed_out')!r}"
            ),
        )
        reporter.check(
            control.get("forced_finalize_stream_stop_requested") is True,
            (
                "recording_session recording.control finalized-after-timeout "
                "requires forced stream-stop"
            ),
            (
                "recording_session recording.control last_event="
                "'finalized_after_drain_timeout' but "
                "forced_finalize_stream_stop_requested="
                f"{control.get('forced_finalize_stream_stop_requested')!r}"
            ),
        )
        reporter.check(
            control.get("ack_state") == "failed_timeout",
            (
                "recording_session recording.control finalized-after-timeout "
                "requires failed-timeout ACK state"
            ),
            (
                "recording_session recording.control last_event="
                "'finalized_after_drain_timeout' but ack_state="
                f"{control.get('ack_state')!r}"
            ),
        )
    elif control.get("last_event") == "finalized":
        reporter.check(
            control.get("ack_state") == "executed",
            (
                "recording_session recording.control finalized "
                "requires executed ACK state"
            ),
            (
                "recording_session recording.control last_event='finalized' "
                f"but ack_state={control.get('ack_state')!r}"
            ),
        )

    if expected_method is not None:
        reporter.check(
            control.get("method") == expected_method,
            f"recording_session recording.control method={expected_method}",
            (
                "recording_session recording.control method="
                f"{control.get('method')!r}; expected {expected_method!r}"
            ),
        )
    if expected_operation_id is not None:
        reporter.check(
            control.get("operation_id") == expected_operation_id,
            (
                "recording_session recording.control "
                f"operation_id={expected_operation_id}"
            ),
            (
                "recording_session recording.control operation_id="
                f"{control.get('operation_id')!r}; "
                f"expected {expected_operation_id!r}"
            ),
        )
    if expected_command_source is not None:
        reporter.check(
            control.get("command_source") == expected_command_source,
            (
                "recording_session recording.control "
                f"command_source={expected_command_source}"
            ),
            (
                "recording_session recording.control command_source="
                f"{control.get('command_source')!r}; "
                f"expected {expected_command_source!r}"
            ),
        )


def sorted_rolling_artifacts_for_camera(
    manifest: dict[str, Any],
    serial: str,
) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    clips = manifest.get("clips")
    clips = clips if isinstance(clips, list) else []
    out: list[tuple[dict[str, Any], dict[str, Any]]] = []
    for clip in clips:
        clip = clip if isinstance(clip, dict) else {}
        artifacts = nested_dict(clip, "camera_artifacts")
        artifact = artifacts.get(serial)
        if isinstance(artifact, dict):
            out.append((clip, artifact))
    return sorted(out, key=lambda item: integer(item[0].get("clip_index")) or 0)


def rolling_crop_outputs_for_camera(
    clip: dict[str, Any],
    serial: str,
) -> dict[str, Any]:
    outputs = clip.get("recording_outputs")
    outputs = outputs if isinstance(outputs, dict) else {}
    camera_outputs = outputs.get(serial)
    camera_outputs = camera_outputs if isinstance(camera_outputs, dict) else {}
    crop_output = camera_outputs.get("crop")
    return crop_output if isinstance(crop_output, dict) else {}


def check_rolling_recording_session_manifest(
    reporter: Reporter,
    recording_folder: Path,
    manifest: dict[str, Any],
    snapshot_session: dict[str, Any],
    cameras: list[str],
) -> None:
    backend = manifest.get("recording_backend")
    backend = backend if isinstance(backend, dict) else {}
    rollover = manifest.get("rollover")
    rollover = rollover if isinstance(rollover, dict) else {}
    clips = manifest.get("clips")
    clips = clips if isinstance(clips, list) else []

    reporter.check(
        bool(clips),
        f"recording_session rolling clips present ({len(clips)})",
        "recording_session rolling clips missing",
    )
    reporter.check(
        backend.get("mode") == "external_ipc",
        "rolling recording_session backend is external_ipc",
        f"rolling recording_session backend mode={backend.get('mode')!r}",
    )
    reporter.check(
        rollover.get("implementation") == "external_recorder_gop_boundary_writer_rotation",
        "rolling rollover implementation is external_recorder_gop_boundary_writer_rotation",
        f"rolling rollover implementation={rollover.get('implementation')!r}",
    )

    manifest_indexes = manifest.get("indexes")
    manifest_indexes = manifest_indexes if isinstance(manifest_indexes, dict) else {}
    snapshot_indexes = snapshot_session.get("recording_session_index")
    snapshot_indexes = snapshot_indexes if isinstance(snapshot_indexes, dict) else {}
    index_json_path = index_path_from_session(
        recording_folder,
        manifest_indexes,
        snapshot_indexes,
        "clip_index_json_path",
        "clip_index_json",
    )
    index_csv_path = index_path_from_session(
        recording_folder,
        manifest_indexes,
        snapshot_indexes,
        "clip_index_csv_path",
        "clip_index_csv",
    )
    reporter.check(
        index_json_path.exists(),
        "recording_session rolling JSON index present",
        f"recording_session rolling JSON index missing: {index_json_path}",
    )
    reporter.check(
        index_csv_path.exists(),
        "recording_session rolling CSV index present",
        f"recording_session rolling CSV index missing: {index_csv_path}",
    )

    for clip in clips:
        clip = clip if isinstance(clip, dict) else {}
        output_errors = recording_clip_output_contract_errors(
            recording_folder,
            clip,
            cameras,
        )
        for error in output_errors:
            reporter.fail(error)

    crop_recording = backend.get("crop_recording")
    crop_recording = crop_recording if isinstance(crop_recording, dict) else {}
    crop_rolling_clips = crop_recording.get("rolling_clips")
    crop_rolling_clips = crop_rolling_clips if isinstance(crop_rolling_clips, dict) else {}
    if crop_rolling_clips:
        clips_by_index = {
            integer(clip.get("clip_index")): clip
            for clip in clips
            if isinstance(clip, dict) and integer(clip.get("clip_index")) is not None
        }
        for serial, serial_clips in sorted(crop_rolling_clips.items()):
            if not isinstance(serial_clips, list):
                reporter.fail(f"Cam{serial} crop rolling_clips entry is not a list")
                continue
            for crop_clip in serial_clips:
                crop_clip = crop_clip if isinstance(crop_clip, dict) else {}
                clip_index = integer(crop_clip.get("clip_index"))
                parent_clip = clips_by_index.get(clip_index)
                reporter.check(
                    parent_clip is not None,
                    f"Cam{serial} rolling crop clip {clip_index} has parent clip",
                    f"Cam{serial} rolling crop clip {clip_index} missing parent clip",
                )
                if parent_clip is None:
                    continue
                crop_output = rolling_crop_outputs_for_camera(parent_clip, serial)
                reporter.check(
                    bool(crop_output),
                    f"Cam{serial} rolling clip {clip_index} crop recording_output present",
                    f"Cam{serial} rolling clip {clip_index} crop recording_output missing",
                )
                if not crop_output:
                    continue
                for output_key, clip_key in (
                    ("video", "video"),
                    ("metadata", "metadata"),
                    ("perf", "perf"),
                    ("keyframes", "keyframes"),
                ):
                    expected = crop_clip.get(clip_key)
                    actual = crop_output.get(output_key)
                    if isinstance(expected, str) and expected:
                        reporter.check(
                            actual == expected,
                            (
                                f"Cam{serial} rolling clip {clip_index} crop "
                                f"{output_key} path matches backend"
                            ),
                            (
                                f"Cam{serial} rolling clip {clip_index} crop "
                                f"{output_key} path {actual!r} != backend {expected!r}"
                            ),
                        )
                frame_count = integer(crop_clip.get("frame_count"))
                output_frame_count = integer(crop_output.get("frame_count"))
                reporter.check(
                    output_frame_count == frame_count,
                    (
                        f"Cam{serial} rolling clip {clip_index} crop output "
                        f"frame_count matches backend ({output_frame_count})"
                    ),
                    (
                        f"Cam{serial} rolling clip {clip_index} crop output "
                        f"frame_count {output_frame_count} != backend {frame_count}"
                    ),
                )

    for serial in cameras:
        clip_artifacts = sorted_rolling_artifacts_for_camera(manifest, serial)
        reporter.check(
            bool(clip_artifacts),
            f"Cam{serial} rolling camera_artifacts present ({len(clip_artifacts)} clips)",
            f"Cam{serial} missing rolling camera_artifacts",
        )
        expected_next_frame: int | None = None
        total_frames = 0
        total_packets = 0
        for clip, artifact in clip_artifacts:
            clip_index = integer(clip.get("clip_index"))
            video_path = path_from_recording_folder(recording_folder, artifact.get("video"))
            metadata_path = path_from_recording_folder(recording_folder, artifact.get("metadata"))
            keyframe_path = path_from_recording_folder(recording_folder, artifact.get("keyframes"))
            frame_count = integer(artifact.get("frame_count"))
            first_frame = integer(artifact.get("first_recording_frame_id"))
            last_frame = integer(artifact.get("last_recording_frame_id"))
            frame_gaps = integer(artifact.get("recording_frame_id_gaps"))
            packet_count = integer(artifact.get("packet_count"))
            packet_source = str(artifact.get("packet_count_source", ""))
            metadata_rows = count_csv_data_rows(metadata_path)
            keyframes = read_json(keyframe_path) if keyframe_path.exists() else {}
            keyframe_total_frames = integer(keyframes.get("total_frames"))

            reporter.check(
                video_path.exists() and video_path.stat().st_size > 0,
                f"Cam{serial} rolling clip {clip_index} video present",
                f"Cam{serial} rolling clip {clip_index} video missing: {video_path}",
            )
            reporter.check(
                metadata_path.exists(),
                f"Cam{serial} rolling clip {clip_index} metadata present",
                f"Cam{serial} rolling clip {clip_index} metadata missing: {metadata_path}",
            )
            reporter.check(
                keyframe_path.exists(),
                f"Cam{serial} rolling clip {clip_index} keyframe present",
                f"Cam{serial} rolling clip {clip_index} keyframe missing: {keyframe_path}",
            )
            if keyframe_path.exists():
                check_keyframe_sidecar_starts_at_zero(
                    reporter,
                    keyframes,
                    keyframe_path,
                    f"Cam{serial} rolling clip {clip_index}",
                )
                reporter.check(
                    keyframe_total_frames == frame_count,
                    (
                        f"Cam{serial} rolling clip {clip_index} "
                        f"keyframe total_frames matches frame_count ({keyframe_total_frames})"
                    ),
                    (
                        f"Cam{serial} rolling clip {clip_index} "
                        f"keyframe total_frames ({keyframe_total_frames}) != "
                        f"frame_count ({frame_count})"
                    ),
                )
            reporter.check(
                frame_count is not None and frame_count > 0,
                f"Cam{serial} rolling clip {clip_index} frame_count={frame_count}",
                f"Cam{serial} rolling clip {clip_index} invalid frame_count={frame_count}",
            )
            reporter.check(
                frame_count is not None and metadata_rows == frame_count,
                f"Cam{serial} rolling clip {clip_index} metadata rows match frame_count",
                (
                    f"Cam{serial} rolling clip {clip_index} metadata_rows={metadata_rows}, "
                    f"frame_count={frame_count}"
                ),
            )
            reporter.check(
                first_frame is not None and last_frame is not None and frame_count is not None
                and last_frame == first_frame + frame_count - 1,
                f"Cam{serial} rolling clip {clip_index} frame range matches frame_count",
                (
                    f"Cam{serial} rolling clip {clip_index} invalid frame range: "
                    f"first={first_frame}, last={last_frame}, frame_count={frame_count}"
                ),
            )
            if expected_next_frame is not None:
                reporter.check(
                    first_frame == expected_next_frame,
                    f"Cam{serial} rolling clip {clip_index} continues at frame {first_frame}",
                    (
                        f"Cam{serial} rolling frame continuity break at clip {clip_index}: "
                        f"expected first={expected_next_frame}, got {first_frame}"
                    ),
                )
            if first_frame is not None and last_frame is not None and frame_count:
                expected_next_frame = last_frame + 1
                total_frames += frame_count
            if frame_gaps is not None:
                reporter.check(
                    frame_gaps == 0,
                    f"Cam{serial} rolling clip {clip_index} recording_frame_id_gaps=0",
                    f"Cam{serial} rolling clip {clip_index} recording_frame_id_gaps={frame_gaps}",
                )
            reporter.check(
                packet_count is not None and packet_count > 0,
                f"Cam{serial} rolling clip {clip_index} packet_count present",
                f"Cam{serial} rolling clip {clip_index} packet_count={packet_count}",
            )
            if packet_count is not None:
                total_packets += packet_count
            reporter.check(
                packet_source not in {"", "not_collected", "unavailable"},
                f"Cam{serial} rolling clip {clip_index} packet_count_source={packet_source}",
                f"Cam{serial} rolling clip {clip_index} packet_count_source={packet_source!r}",
            )

        reporter.check(
            total_frames > 0,
            f"Cam{serial} rolling total frame_count={total_frames}",
            f"Cam{serial} rolling total frame_count={total_frames}",
        )
        reporter.check(
            total_packets > 0,
            f"Cam{serial} rolling total packet_count={total_packets}",
            f"Cam{serial} rolling total packet_count={total_packets}",
        )


def stream_display_serial(stream_key: str, stream: dict[str, Any]) -> str:
    for value in (stream.get("stream_id"), stream.get("camera_serial"), stream_key):
        if not isinstance(value, str) or not value:
            continue
        if value.startswith("Cam"):
            value = value[3:]
        if value.endswith("_crop"):
            value = value[: -len("_crop")]
        return value
    return stream_key


def runtime_processes_by_status_path(runtime: dict[str, Any]) -> dict[str, dict[str, Any]]:
    processes = runtime.get("processes")
    processes = processes if isinstance(processes, list) else []
    by_path: dict[str, dict[str, Any]] = {}
    for process in processes:
        if not isinstance(process, dict):
            continue
        path = process.get("status_json_path")
        if isinstance(path, str) and path:
            process_path = Path(path)
            by_path[str(process_path)] = process
            by_path[str(process_path.resolve())] = process
    return by_path


def check_external_recorder_rolling_status(
    reporter: Reporter,
    prefix: str,
    status: dict[str, Any],
    summary: dict[str, Any],
) -> dict[str, Any]:
    rolling = summary.get("rolling_output")
    rolling = rolling if isinstance(rolling, dict) else {}
    if rolling.get("enabled") is not True:
        return {}

    status_rolling = status.get("rolling")
    status_rolling = status_rolling if isinstance(status_rolling, dict) else {}
    reporter.check(
        bool(status_rolling),
        f"{prefix} rolling status sidecar present",
        f"{prefix} rolling status sidecar missing",
    )
    if not status_rolling:
        return {}

    reporter.check(
        status_rolling.get("enabled") is True,
        f"{prefix} rolling status enabled=true",
        f"{prefix} rolling status enabled={status_rolling.get('enabled')!r}",
    )
    reporter.check(
        status_rolling.get("implementation") ==
        "external_recorder_gop_boundary_writer_rotation",
        f"{prefix} rolling status implementation valid",
        f"{prefix} rolling status implementation={status_rolling.get('implementation')!r}",
    )
    for field in (
        "record_for_seconds",
        "clip_seconds",
        "clip_span_frames",
        "target_frame_count",
    ):
        reporter.check(
            integer(status_rolling.get(field)) == integer(rolling.get(field)),
            f"{prefix} rolling status {field} matches summary",
            (
                f"{prefix} rolling status {field}={status_rolling.get(field)!r}, "
                f"summary={rolling.get(field)!r}"
            ),
        )

    clips = rolling.get("clips")
    clips = clips if isinstance(clips, list) else []
    reporter.check(
        bool(clips),
        f"{prefix} rolling summary clips present",
        f"{prefix} rolling summary clips missing",
    )
    if clips:
        last_clip = clips[-1] if isinstance(clips[-1], dict) else {}
        expected_status = "failed" if last_clip.get("failed") is True else "completed"
        reporter.check(
            integer(status_rolling.get("completed_clip_count")) == len(clips),
            f"{prefix} rolling completed clip count matches summary",
            (
                f"{prefix} rolling completed_clip_count="
                f"{status_rolling.get('completed_clip_count')!r}, summary={len(clips)}"
            ),
        )
        checks = (
            ("last_completed_clip_index", "clip_index"),
            ("last_completed_clip_last_recording_frame_id", "last_recording_frame_id"),
            ("last_completed_clip_frame_count", "frame_count"),
        )
        for status_field, summary_field in checks:
            reporter.check(
                integer(status_rolling.get(status_field)) ==
                integer(last_clip.get(summary_field)),
                f"{prefix} rolling {status_field} matches summary",
                (
                    f"{prefix} rolling {status_field}="
                    f"{status_rolling.get(status_field)!r}, "
                    f"summary {summary_field}={last_clip.get(summary_field)!r}"
                ),
            )
        reporter.check(
            status_rolling.get("last_rollover_status") == expected_status,
            f"{prefix} rolling last rollover status matches summary",
            (
                f"{prefix} rolling last_rollover_status="
                f"{status_rolling.get('last_rollover_status')!r}, "
                f"expected={expected_status!r}"
            ),
        )

    return {
        "rolling_current_clip_index": integer(status_rolling.get("current_clip_index")),
        "rolling_next_rollover_at_recording_frame_id": integer(
            status_rolling.get("next_rollover_at_recording_frame_id")
        ),
        "rolling_frames_until_next_rollover": integer(
            status_rolling.get("frames_until_next_rollover")
        ),
        "rolling_completed_clip_count": integer(status_rolling.get("completed_clip_count")),
        "rolling_last_completed_clip_index": integer(
            status_rolling.get("last_completed_clip_index")
        ),
        "rolling_last_rollover_status": status_rolling.get("last_rollover_status"),
    }


def check_runtime_rolling_status(
    reporter: Reporter,
    prefix: str,
    runtime_status: dict[str, Any],
    rolling_status_summary: dict[str, Any],
) -> None:
    reporter.check(
        runtime_status.get("rolling_enabled") is True,
        f"{prefix} runtime rolling enabled=true",
        f"{prefix} runtime rolling_enabled={runtime_status.get('rolling_enabled')!r}",
    )
    for field in (
        "rolling_current_clip_index",
        "rolling_next_rollover_at_recording_frame_id",
        "rolling_frames_until_next_rollover",
        "rolling_completed_clip_count",
        "rolling_last_completed_clip_index",
    ):
        reporter.check(
            integer(runtime_status.get(field)) == rolling_status_summary.get(field),
            f"{prefix} runtime {field} matches sidecar",
            (
                f"{prefix} runtime {field}={runtime_status.get(field)!r}, "
                f"sidecar={rolling_status_summary.get(field)!r}"
            ),
        )
    reporter.check(
        runtime_status.get("rolling_last_rollover_status") ==
        rolling_status_summary.get("rolling_last_rollover_status"),
        f"{prefix} runtime rolling_last_rollover_status matches sidecar",
        (
            f"{prefix} runtime rolling_last_rollover_status="
            f"{runtime_status.get('rolling_last_rollover_status')!r}, "
            f"sidecar={rolling_status_summary.get('rolling_last_rollover_status')!r}"
        ),
    )


def check_mp4_queue_overflow_payload(
    reporter: Reporter,
    prefix: str,
    payload: dict[str, Any],
) -> None:
    if "mp4_queue_overflowed" in payload:
        reporter.check(
            payload.get("mp4_queue_overflowed") is not True,
            f"{prefix} mp4_queue_overflowed=false",
            f"{prefix} mp4_queue_overflowed={payload.get('mp4_queue_overflowed')!r}",
        )
    overflow_events = integer(payload.get("mp4_queue_overflow_events"))
    if overflow_events is not None:
        reporter.check(
            overflow_events == 0,
            f"{prefix} mp4_queue_overflow_events=0",
            f"{prefix} mp4_queue_overflow_events={overflow_events}",
        )


def check_external_summary_mp4_queue_overflow(
    reporter: Reporter,
    prefix: str,
    summary: dict[str, Any],
) -> None:
    external_encode = summary.get("external_encode")
    if isinstance(external_encode, dict):
        check_mp4_queue_overflow_payload(reporter, f"{prefix} external_encode", external_encode)
    shards = summary.get("external_encode_shards")
    if isinstance(shards, list):
        for index, shard in enumerate(shards):
            if not isinstance(shard, dict):
                continue
            shard_id = shard.get("assigned_shard_id", index)
            check_mp4_queue_overflow_payload(
                reporter,
                f"{prefix} shard {shard_id}",
                shard,
            )
    merged_output = summary.get("merged_output")
    if isinstance(merged_output, dict):
        check_mp4_queue_overflow_payload(
            reporter,
            f"{prefix} merged_output",
            merged_output,
        )


def check_storage_preflight_payload(
    reporter: Reporter,
    prefix: str,
    payload: dict[str, Any],
) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    storage = payload.get("storage_preflight")
    if not isinstance(storage, dict):
        return summary
    paths = storage.get("paths")
    paths = paths if isinstance(paths, list) else []
    min_available_bytes: int | None = None
    paths_ok_count = 0
    paths_low_space_count = 0
    for path in paths:
        if not isinstance(path, dict):
            continue
        if path.get("ok") is True:
            paths_ok_count += 1
        if path.get("below_warning") is True:
            paths_low_space_count += 1
        available_bytes = integer(path.get("available_bytes"))
        if available_bytes is not None:
            min_available_bytes = (
                available_bytes
                if min_available_bytes is None
                else min(min_available_bytes, available_bytes)
            )
    summary = {
        "storage_checked": storage.get("checked"),
        "storage_ok": storage.get("ok"),
        "storage_low_space": storage.get("low_space"),
        "storage_min_free_bytes": integer(storage.get("min_free_bytes")),
        "storage_low_space_warning_bytes": integer(storage.get("low_space_warning_bytes")),
        "storage_path_count": len(paths),
        "storage_paths_ok_count": paths_ok_count,
        "storage_paths_low_space_count": paths_low_space_count,
        "storage_min_available_bytes": min_available_bytes,
    }
    reporter.check(
        storage.get("ok") is not False,
        f"{prefix} storage_preflight ok=true",
        f"{prefix} storage_preflight ok={storage.get('ok')!r}",
    )
    reporter.check(
        storage.get("low_space") is not True,
        f"{prefix} storage_preflight low_space=false",
        f"{prefix} storage_preflight low_space={storage.get('low_space')!r}",
    )
    paths = storage.get("paths")
    if isinstance(paths, list):
        for index, path in enumerate(paths):
            if not isinstance(path, dict):
                continue
            path_label = path.get("path") or f"path[{index}]"
            reporter.check(
                path.get("ok") is not False,
                f"{prefix} storage path {path_label} ok=true",
                (
                    f"{prefix} storage path {path_label} ok={path.get('ok')!r} "
                    f"error={path.get('error')!r}"
                ),
            )
            reporter.check(
                path.get("meets_min_free") is not False,
                f"{prefix} storage path {path_label} meets_min_free=true",
                (
                    f"{prefix} storage path {path_label} "
                    f"meets_min_free={path.get('meets_min_free')!r}"
                ),
            )
            reporter.check(
                path.get("below_warning") is not True,
                f"{prefix} storage path {path_label} below_warning=false",
                (
                    f"{prefix} storage path {path_label} "
                    f"below_warning={path.get('below_warning')!r}"
                ),
            )
    return summary


def check_ipc_protocol_payload(
    reporter: Reporter,
    prefix: str,
    payload: dict[str, Any],
    required: bool,
) -> dict[str, Any]:
    protocol = payload.get("ipc_protocol")
    if not isinstance(protocol, dict):
        if required:
            reporter.fail(f"{prefix} ipc_protocol missing")
        return {}
    summary = {
        "ipc_protocol_name": protocol.get("name"),
        "ipc_protocol_version": integer(protocol.get("version")),
        "recorder_hello_sent": protocol.get("recorder_hello_sent"),
        "client_hello_received": protocol.get("client_hello_received"),
        "recorder_status_messages_sent": integer(
            protocol.get("recorder_status_messages_sent")
        ),
        "recorder_status_send_failures": integer(
            protocol.get("recorder_status_send_failures")
        ),
        "client_control_messages_received": integer(
            protocol.get("client_control_messages_received")
        ),
        "client_drain_messages_received": integer(
            protocol.get("client_drain_messages_received")
        ),
        "client_finalize_messages_received": integer(
            protocol.get("client_finalize_messages_received")
        ),
        "client_drain_received": protocol.get("client_drain_received"),
        "client_finalize_received": protocol.get("client_finalize_received"),
        "client_control_state": protocol.get("client_control_state"),
        "descriptor_intake_end_reason": protocol.get("descriptor_intake_end_reason"),
        "descriptor_intake_completed_cleanly": protocol.get(
            "descriptor_intake_completed_cleanly"
        ),
        "client_drain_first_frame_count": integer(
            protocol.get("client_drain_first_frame_count")
        ),
        "client_finalize_frame_count": integer(
            protocol.get("client_finalize_frame_count")
        ),
    }
    reporter.check(
        protocol.get("name") == "orange.external_recorder.ipc",
        f"{prefix} ipc_protocol name valid",
        f"{prefix} ipc_protocol name={protocol.get('name')!r}",
    )
    reporter.check(
        integer(protocol.get("version")) == 1,
        f"{prefix} ipc_protocol version=1",
        f"{prefix} ipc_protocol version={protocol.get('version')!r}",
    )
    reporter.check(
        protocol.get("recorder_hello_sent") is True,
        f"{prefix} recorder_hello_sent=true",
        f"{prefix} recorder_hello_sent={protocol.get('recorder_hello_sent')!r}",
    )
    reporter.check(
        protocol.get("client_hello_received") is True,
        f"{prefix} client_hello_received=true",
        f"{prefix} client_hello_received={protocol.get('client_hello_received')!r}",
    )
    send_failures = integer(protocol.get("recorder_status_send_failures"))
    if send_failures is not None:
        reporter.check(
            send_failures == 0,
            f"{prefix} recorder status protocol sends succeeded",
            f"{prefix} recorder_status_send_failures={send_failures}",
        )
    control_count = integer(protocol.get("client_control_messages_received"))
    if control_count is not None:
        reporter.check(
            control_count > 0,
            f"{prefix} client control messages received",
            f"{prefix} client_control_messages_received={control_count}",
        )
    drain_count = integer(protocol.get("client_drain_messages_received"))
    if drain_count is not None:
        reporter.check(
            drain_count > 0,
            f"{prefix} client drain messages received",
            f"{prefix} client_drain_messages_received={drain_count}",
        )
    finalize_count = integer(protocol.get("client_finalize_messages_received"))
    if finalize_count is not None:
        reporter.check(
            finalize_count > 0,
            f"{prefix} client finalize messages received",
            f"{prefix} client_finalize_messages_received={finalize_count}",
        )
    if "client_drain_received" in protocol:
        reporter.check(
            protocol.get("client_drain_received") is True,
            f"{prefix} client drain control received",
            f"{prefix} client_drain_received={protocol.get('client_drain_received')!r}",
        )
    if "client_finalize_received" in protocol:
        reporter.check(
            protocol.get("client_finalize_received") is True,
            f"{prefix} client finalize control received",
            f"{prefix} client_finalize_received={protocol.get('client_finalize_received')!r}",
        )
    if "client_control_state" in protocol:
        reporter.check(
            protocol.get("client_control_state") == "finalize_requested",
            f"{prefix} client control state reached finalize_requested",
            f"{prefix} client_control_state={protocol.get('client_control_state')!r}",
        )
    if "descriptor_intake_completed_cleanly" in protocol:
        reporter.check(
            protocol.get("descriptor_intake_completed_cleanly") is True,
            f"{prefix} descriptor intake completed cleanly",
            (
                f"{prefix} descriptor_intake_completed_cleanly="
                f"{protocol.get('descriptor_intake_completed_cleanly')!r}"
            ),
        )
    if "descriptor_intake_end_reason" in protocol:
        reporter.check(
            protocol.get("descriptor_intake_end_reason") == "client_finalize",
            f"{prefix} descriptor intake ended by client finalize",
            (
                f"{prefix} descriptor_intake_end_reason="
                f"{protocol.get('descriptor_intake_end_reason')!r}"
            ),
        )
    drain_frame_count = integer(protocol.get("client_drain_first_frame_count"))
    finalize_frame_count = integer(protocol.get("client_finalize_frame_count"))
    if drain_frame_count is not None and finalize_frame_count is not None:
        reporter.check(
            drain_frame_count <= finalize_frame_count,
            f"{prefix} client drain frame count precedes finalize",
            (
                f"{prefix} client_drain_first_frame_count={drain_frame_count}, "
                f"client_finalize_frame_count={finalize_frame_count}"
            ),
        )
    return summary


def require_storage_preflight_summary(
    reporter: Reporter,
    prefix: str,
    storage_summary: dict[str, Any],
) -> None:
    reporter.check(
        storage_summary.get("storage_checked") is True,
        f"{prefix} storage_preflight checked=true",
        f"{prefix} storage_preflight checked={storage_summary.get('storage_checked')!r}",
    )
    reporter.check(
        storage_summary.get("storage_ok") is True,
        f"{prefix} storage_preflight ok=true",
        f"{prefix} storage_preflight ok={storage_summary.get('storage_ok')!r}",
    )
    reporter.check(
        storage_summary.get("storage_low_space") is not True,
        f"{prefix} storage_preflight low_space=false",
        f"{prefix} storage_preflight low_space={storage_summary.get('storage_low_space')!r}",
    )
    path_count = integer(storage_summary.get("storage_path_count"))
    paths_ok_count = integer(storage_summary.get("storage_paths_ok_count"))
    reporter.check(
        path_count is not None and path_count > 0,
        f"{prefix} storage_preflight paths present",
        f"{prefix} storage_preflight path_count={path_count}",
    )
    if path_count is not None and paths_ok_count is not None:
        reporter.check(
            paths_ok_count == path_count,
            f"{prefix} storage_preflight all paths ok ({paths_ok_count}/{path_count})",
            (
                f"{prefix} storage_preflight paths_ok_count={paths_ok_count}, "
                f"path_count={path_count}"
            ),
        )


def check_external_recorder_status_contract(
    reporter: Reporter,
    recording_folder: Path,
    contract_path: Path,
    label: str,
    require_storage_preflight: bool,
    require_protocol_hello: bool,
) -> dict[str, Any]:
    contract = read_json(contract_path)
    if not contract:
        return {}
    streams = contract.get("streams")
    streams = streams if isinstance(streams, dict) else {}
    if not streams:
        reporter.fail(f"{label} external recorder contract has no streams: {contract_path}")
        return {}
    reporter.check(
        contract.get("require_status") is True,
        f"{label} external recorder contract require_status=true",
        f"{label} external recorder contract require_status={contract.get('require_status')!r}",
    )
    reporter.check(
        contract.get("require_status_runtime") is True,
        f"{label} external recorder contract require_status_runtime=true",
        (
            f"{label} external recorder contract "
            f"require_status_runtime={contract.get('require_status_runtime')!r}"
        ),
    )
    if require_storage_preflight:
        reporter.check(
            contract.get("require_storage_preflight") is True,
            f"{label} external recorder contract require_storage_preflight=true",
            (
                f"{label} external recorder contract "
                f"require_storage_preflight={contract.get('require_storage_preflight')!r}"
            ),
        )
    if require_protocol_hello:
        reporter.check(
            contract.get("require_protocol_hello") is True,
            f"{label} external recorder contract require_protocol_hello=true",
            (
                f"{label} external recorder contract "
                f"require_protocol_hello={contract.get('require_protocol_hello')!r}"
            ),
        )
    storage_preflight_required = (
        require_storage_preflight or contract.get("require_storage_preflight") is True
    )
    protocol_hello_required = (
        require_protocol_hello or contract.get("require_protocol_hello") is True
    )

    artifact_root_value = contract.get("artifact_root")
    artifact_root = (
        path_from_recording_folder(recording_folder, artifact_root_value)
        if isinstance(artifact_root_value, str) and artifact_root_value
        else contract_path.parent
    )
    runtime_path = artifact_root / "external_recorder_supervisor_runtime.json"
    runtime = read_json(runtime_path)
    reporter.check(
        runtime.get("schema_id") == "orange.external_recorder.supervisor_runtime",
        f"{label} external recorder runtime present",
        f"{label} external recorder runtime missing or invalid: {runtime_path}",
    )
    runtime_by_status_path = runtime_processes_by_status_path(runtime)

    status_summary: dict[str, Any] = {}
    for stream_key, raw_stream in streams.items():
        stream = raw_stream if isinstance(raw_stream, dict) else {}
        serial = stream_display_serial(str(stream_key), stream)
        status_path = status_path_from_stream(recording_folder, stream)
        summary_path = summary_path_from_stream(recording_folder, stream)
        status = read_json(status_path) if status_path is not None else {}
        summary = read_json(summary_path) if summary_path is not None else {}

        prefix = f"{label} Cam{serial}"
        reporter.check(
            status_path is not None and status_path.exists(),
            f"{prefix} recorder status sidecar present",
            f"{prefix} recorder status sidecar missing: {status_path}",
        )
        reporter.check(
            status.get("schema_id") == "orange.external_recorder.status"
            and integer(status.get("schema_version")) == 1,
            f"{prefix} recorder status schema valid",
            f"{prefix} recorder status schema invalid: {status_path}",
        )
        status_value = str(status.get("status", ""))
        reporter.check(
            status_value == "completed",
            f"{prefix} recorder status completed",
            f"{prefix} recorder status={status_value!r}; expected completed",
        )
        heartbeat_sequence = integer(status.get("heartbeat_sequence"))
        reporter.check(
            heartbeat_sequence is not None and heartbeat_sequence > 0,
            f"{prefix} recorder heartbeat sequence={heartbeat_sequence}",
            f"{prefix} recorder heartbeat sequence missing or zero ({heartbeat_sequence})",
        )
        reporter.check(
            status.get("worker_failed") is False,
            f"{prefix} recorder worker_failed=false",
            f"{prefix} recorder worker_failed={status.get('worker_failed')!r}",
        )
        status_storage_summary = check_storage_preflight_payload(
            reporter,
            f"{prefix} status",
            status,
        )
        status_protocol_summary = check_ipc_protocol_payload(
            reporter,
            f"{prefix} status",
            status,
            protocol_hello_required,
        )
        if storage_preflight_required:
            require_storage_preflight_summary(
                reporter,
                f"{prefix} status",
                status_storage_summary,
            )
        reporter.check(
            not status.get("error"),
            f"{prefix} recorder status error empty",
            f"{prefix} recorder status error={status.get('error')!r}",
        )

        frames_received = integer(status.get("frames_received"))
        frames_encoded = integer(status.get("frames_encoded"))
        acks_sent = integer(status.get("acks_sent"))
        summary_frames_received = integer(summary.get("frames_received"))
        summary_frames_encoded = integer(summary.get("frames_encoded"))
        summary_acks_sent = integer(summary.get("acks_sent"))
        if summary:
            check_external_summary_mp4_queue_overflow(reporter, prefix, summary)
            summary_storage_summary = check_storage_preflight_payload(
                reporter,
                f"{prefix} summary",
                summary,
            )
            if storage_preflight_required:
                require_storage_preflight_summary(
                    reporter,
                    f"{prefix} summary",
                    summary_storage_summary,
                )
            if protocol_hello_required:
                check_ipc_protocol_payload(
                    reporter,
                    f"{prefix} summary",
                    summary,
                    True,
                )
            reporter.check(
                frames_received == summary_frames_received,
                f"{prefix} status frames_received matches summary ({frames_received})",
                (
                    f"{prefix} status frames_received={frames_received}, "
                    f"summary frames_received={summary_frames_received}"
                ),
            )
            reporter.check(
                frames_encoded == summary_frames_encoded,
                f"{prefix} status frames_encoded matches summary ({frames_encoded})",
                (
                    f"{prefix} status frames_encoded={frames_encoded}, "
                    f"summary frames_encoded={summary_frames_encoded}"
                ),
            )
            reporter.check(
                acks_sent == summary_acks_sent,
                f"{prefix} status acks_sent matches summary ({acks_sent})",
                f"{prefix} status acks_sent={acks_sent}, summary acks_sent={summary_acks_sent}",
            )

        rolling_status_summary = check_external_recorder_rolling_status(
            reporter,
            prefix,
            status,
            summary,
        )

        runtime_process = None
        if status_path is not None:
            runtime_process = runtime_by_status_path.get(str(status_path))
            if runtime_process is None:
                # Runtime paths are absolute in current artifacts, but accept a resolved
                # match when a test fixture writes equivalent relative paths.
                runtime_process = runtime_by_status_path.get(str(status_path.resolve()))
        reporter.check(
            runtime_process is not None,
            f"{prefix} runtime recorder_status process present",
            f"{prefix} runtime recorder_status process missing for {status_path}",
        )
        runtime_status = (
            runtime_process.get("recorder_status")
            if isinstance(runtime_process, dict)
            else {}
        )
        runtime_status = runtime_status if isinstance(runtime_status, dict) else {}
        reporter.check(
            runtime_status.get("present") is True and runtime_status.get("valid") is True,
            f"{prefix} runtime recorder_status valid",
            f"{prefix} runtime recorder_status invalid: {runtime_status}",
        )
        reporter.check(
            runtime_status.get("status") == status_value,
            f"{prefix} runtime recorder_status matches sidecar",
            (
                f"{prefix} runtime recorder_status={runtime_status.get('status')!r}, "
                f"sidecar status={status_value!r}"
            ),
        )
        reporter.check(
            integer(runtime_status.get("heartbeat_sequence")) == heartbeat_sequence,
            f"{prefix} runtime heartbeat matches sidecar",
            (
                f"{prefix} runtime heartbeat={runtime_status.get('heartbeat_sequence')!r}, "
                f"sidecar heartbeat={heartbeat_sequence!r}"
            ),
        )
        if rolling_status_summary:
            check_runtime_rolling_status(
                reporter,
                prefix,
                runtime_status,
                rolling_status_summary,
            )
        if storage_preflight_required:
            runtime_storage_summary = {
                "storage_checked": runtime_status.get("storage_checked"),
                "storage_ok": runtime_status.get("storage_ok"),
                "storage_low_space": runtime_status.get("storage_low_space"),
                "storage_path_count": runtime_status.get("storage_path_count"),
                "storage_paths_ok_count": runtime_status.get("storage_paths_ok_count"),
            }
            require_storage_preflight_summary(
                reporter,
                f"{prefix} runtime",
                runtime_storage_summary,
            )
        if protocol_hello_required:
            reporter.check(
                runtime_status.get("ipc_protocol_name") == "orange.external_recorder.ipc",
                f"{prefix} runtime ipc_protocol_name valid",
                (
                    f"{prefix} runtime ipc_protocol_name="
                    f"{runtime_status.get('ipc_protocol_name')!r}"
                ),
            )
            reporter.check(
                integer(runtime_status.get("ipc_protocol_version")) == 1,
                f"{prefix} runtime ipc_protocol_version=1",
                (
                    f"{prefix} runtime ipc_protocol_version="
                    f"{runtime_status.get('ipc_protocol_version')!r}"
                ),
            )
            reporter.check(
                runtime_status.get("recorder_hello_sent") is True,
                f"{prefix} runtime recorder_hello_sent=true",
                (
                    f"{prefix} runtime recorder_hello_sent="
                    f"{runtime_status.get('recorder_hello_sent')!r}"
                ),
            )
            reporter.check(
                runtime_status.get("client_hello_received") is True,
                f"{prefix} runtime client_hello_received=true",
                (
                    f"{prefix} runtime client_hello_received="
                    f"{runtime_status.get('client_hello_received')!r}"
                ),
            )
            runtime_send_failures = integer(
                runtime_status.get("recorder_status_send_failures")
            )
            if runtime_send_failures is not None:
                reporter.check(
                    runtime_send_failures == 0,
                    f"{prefix} runtime recorder status protocol sends succeeded",
                    (
                        f"{prefix} runtime recorder_status_send_failures="
                        f"{runtime_send_failures}"
                    ),
                )
            runtime_control_count = integer(
                runtime_status.get("client_control_messages_received")
            )
            if runtime_control_count is not None:
                reporter.check(
                    runtime_control_count > 0,
                    f"{prefix} runtime client control messages received",
                    (
                        f"{prefix} runtime client_control_messages_received="
                        f"{runtime_control_count}"
                    ),
                )
            runtime_drain_count = integer(
                runtime_status.get("client_drain_messages_received")
            )
            if runtime_drain_count is not None:
                reporter.check(
                    runtime_drain_count > 0,
                    f"{prefix} runtime client drain messages received",
                    (
                        f"{prefix} runtime client_drain_messages_received="
                        f"{runtime_drain_count}"
                    ),
                )
            runtime_finalize_count = integer(
                runtime_status.get("client_finalize_messages_received")
            )
            if runtime_finalize_count is not None:
                reporter.check(
                    runtime_finalize_count > 0,
                    f"{prefix} runtime client finalize messages received",
                    (
                        f"{prefix} runtime client_finalize_messages_received="
                        f"{runtime_finalize_count}"
                    ),
                )
            if "client_drain_received" in runtime_status:
                reporter.check(
                    runtime_status.get("client_drain_received") is True,
                    f"{prefix} runtime client drain control received",
                    (
                        f"{prefix} runtime client_drain_received="
                        f"{runtime_status.get('client_drain_received')!r}"
                    ),
                )
            if "client_control_state" in runtime_status:
                reporter.check(
                    runtime_status.get("client_control_state") == "finalize_requested",
                    f"{prefix} runtime client control state reached finalize_requested",
                    (
                        f"{prefix} runtime client_control_state="
                        f"{runtime_status.get('client_control_state')!r}"
                    ),
                )
            if "descriptor_intake_completed_cleanly" in runtime_status:
                reporter.check(
                    runtime_status.get("descriptor_intake_completed_cleanly") is True,
                    f"{prefix} runtime descriptor intake completed cleanly",
                    (
                        f"{prefix} runtime descriptor_intake_completed_cleanly="
                        f"{runtime_status.get('descriptor_intake_completed_cleanly')!r}"
                    ),
                )
            if "descriptor_intake_end_reason" in runtime_status:
                reporter.check(
                    runtime_status.get("descriptor_intake_end_reason")
                    == "client_finalize",
                    f"{prefix} runtime descriptor intake ended by client finalize",
                    (
                        f"{prefix} runtime descriptor_intake_end_reason="
                        f"{runtime_status.get('descriptor_intake_end_reason')!r}"
                    ),
                )
            runtime_drain_frame_count = integer(
                runtime_status.get("client_drain_first_frame_count")
            )
            runtime_finalize_frame_count = integer(
                runtime_status.get("client_finalize_frame_count")
            )
            if (
                runtime_drain_frame_count is not None
                and runtime_finalize_frame_count is not None
            ):
                reporter.check(
                    runtime_drain_frame_count <= runtime_finalize_frame_count,
                    f"{prefix} runtime client drain frame count precedes finalize",
                    (
                        f"{prefix} runtime client_drain_first_frame_count="
                        f"{runtime_drain_frame_count}, "
                        f"client_finalize_frame_count={runtime_finalize_frame_count}"
                    ),
                )
            if "client_finalize_received" in runtime_status:
                reporter.check(
                    runtime_status.get("client_finalize_received") is True,
                    f"{prefix} runtime client finalize control received",
                    (
                        f"{prefix} runtime client_finalize_received="
                        f"{runtime_status.get('client_finalize_received')!r}"
                    ),
                )

        status_summary[serial] = {
            "status_json": str(status_path) if status_path is not None else "",
            "summary_json": str(summary_path) if summary_path is not None else "",
            "status": status_value,
            "heartbeat_sequence": heartbeat_sequence,
            "frames_received": frames_received,
            "acks_sent": acks_sent,
            "frames_encoded": frames_encoded,
            "runtime_present": runtime_process is not None,
            "runtime_valid": runtime_status.get("valid") is True,
        }
        status_summary[serial].update(status_storage_summary)
        status_summary[serial].update(status_protocol_summary)
        status_summary[serial].update(
            {
                "runtime_storage_checked": runtime_status.get("storage_checked"),
                "runtime_storage_ok": runtime_status.get("storage_ok"),
                "runtime_storage_low_space": runtime_status.get("storage_low_space"),
                "runtime_storage_path_count": runtime_status.get("storage_path_count"),
                "runtime_storage_paths_ok_count": runtime_status.get("storage_paths_ok_count"),
                "runtime_storage_min_available_bytes": integer(
                    runtime_status.get("storage_min_available_bytes")
                ),
            }
        )
        status_summary[serial].update(rolling_status_summary)
    return status_summary


def check_external_recorder_status(
    reporter: Reporter,
    recording_folder: Path,
    require_status: bool,
    require_storage_preflight: bool = False,
    require_protocol_hello: bool = False,
) -> dict[str, Any]:
    if not require_status:
        return {}
    summary: dict[str, Any] = {}
    contract_specs = (
        ("full", recording_folder / "external_recorder_contract.json"),
        ("crop", recording_folder / "external_crop_recorder_contract.json"),
    )
    found = False
    for label, contract_path in contract_specs:
        if not contract_path.exists():
            continue
        found = True
        summary[label] = check_external_recorder_status_contract(
            reporter,
            recording_folder,
            contract_path,
            label,
            require_storage_preflight,
            require_protocol_hello,
        )
    reporter.check(
        found,
        "external recorder status contracts present",
        "external recorder status required but no external recorder contract files were found",
    )
    return summary


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


def crop_backend_artifact_path(
    recording_folder: Path,
    crop_recording_backend: dict[str, Any],
    serial: str,
    map_name: str,
) -> Path | None:
    values = crop_recording_backend.get(map_name)
    if not isinstance(values, dict):
        return None
    value = values.get(serial)
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


def resolve_crop_video_artifact_path(
    recording_folder: Path,
    crop_output: dict[str, Any],
    crop_descriptor: dict[str, Any],
    crop_recording_backend: dict[str, Any],
    serial: str,
) -> Path:
    descriptor_path = crop_descriptor_artifact_path(recording_folder, crop_descriptor, "video")
    if descriptor_path is not None:
        return descriptor_path
    backend_path = crop_backend_artifact_path(
        recording_folder,
        crop_recording_backend,
        serial,
        "merged_mp4",
    )
    if backend_path is not None:
        return backend_path
    return crop_artifact_path(recording_folder, crop_output, "video", f"Cam{serial}_crop.mp4")


def resolve_crop_keyframe_artifact_path(
    recording_folder: Path,
    crop_output: dict[str, Any],
    crop_descriptor: dict[str, Any],
    crop_recording_backend: dict[str, Any],
    serial: str,
) -> Path:
    descriptor_path = crop_descriptor_artifact_path(recording_folder, crop_descriptor, "keyframes")
    if descriptor_path is not None:
        return descriptor_path
    backend_path = crop_backend_artifact_path(
        recording_folder,
        crop_recording_backend,
        serial,
        "keyframes",
    )
    if backend_path is not None:
        return backend_path
    return crop_artifact_path(recording_folder, crop_output, "keyframes", f"Cam{serial}_crop_keyframe.json")


def resolve_crop_summary_artifact_path(
    recording_folder: Path,
    crop_descriptor: dict[str, Any],
    crop_recording_backend: dict[str, Any],
    serial: str,
) -> Path | None:
    descriptor_path = crop_descriptor_artifact_path(recording_folder, crop_descriptor, "summary")
    if descriptor_path is not None:
        return descriptor_path
    return crop_backend_artifact_path(
        recording_folder,
        crop_recording_backend,
        serial,
        "summary_json",
    )


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
    allowed_content_failure_serials: set[str],
    max_black_fraction: float,
    min_stddev: float,
) -> dict[str, Any]:
    video_sanity: dict[str, Any] = {}
    for serial in cameras:
        video = nested_dict(summary, "videos", serial)
        if not video:
            reporter.fail(f"Cam{serial} missing main MP4")
            continue
        video_label = (
            "rolling full-frame video"
            if video.get("source") == "recording_session_rolling_clips"
            else "main MP4"
        )
        reporter.check(
            video.get("status") == "ok",
            f"Cam{serial} {video_label} ffprobe status=ok",
            f"Cam{serial} {video_label} ffprobe status={video.get('status')!r}",
        )
        frames = integer(video.get("frames"))
        width = integer(video.get("width"))
        height = integer(video.get("height"))
        reporter.check(
            bool(frames and frames > 0 and width and width > 0 and height and height > 0),
            f"Cam{serial} {video_label} dimensions/frame count present ({width}x{height}, frames={frames})",
            f"Cam{serial} invalid {video_label} dimensions/frame count ({width}x{height}, frames={frames})",
        )
        bitrate_bps = number(video.get("bitrate_bps"))
        bitrate_mbps = None if bitrate_bps is None else bitrate_bps / 1_000_000.0
        bitrate_ok = bitrate_mbps is not None and bitrate_mbps >= min_bitrate_mbps
        if bitrate_ok:
            reporter.pass_(
                f"Cam{serial} {video_label} bitrate "
                f"{fmt_float(bitrate_mbps, 1)} Mbps >= {min_bitrate_mbps:.1f} Mbps"
            )
        elif serial in allowed_content_failure_serials:
            reporter.warn(
                f"Cam{serial} {video_label} bitrate {bitrate_mbps} Mbps below "
                f"{min_bitrate_mbps:.1f} Mbps (allowed main-video content failure)"
            )
        else:
            reporter.fail(
                f"Cam{serial} {video_label} bitrate {bitrate_mbps} Mbps below "
                f"{min_bitrate_mbps:.1f} Mbps"
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
        if sanity.get("content_valid"):
            reporter.pass_(
                f"Cam{serial} decoded video sanity pass "
                f"(mean_luma={fmt_float(sanity.get('mean_luma'), 1)}, "
                f"stddev={fmt_float(sanity.get('max_stddev'), 1)}, "
                f"black={fmt_float(sanity.get('max_black_fraction_lt8'), 6)})"
            )
        elif serial in allowed_content_failure_serials:
            reporter.warn(
                f"Cam{serial} decoded video sanity failed: {sanity.get('status')} "
                f"{sanity.get('detail', '')} (allowed main-video content failure)"
            )
        else:
            reporter.fail(
                f"Cam{serial} decoded video sanity failed: "
                f"{sanity.get('status')} {sanity.get('detail', '')}"
            )
    return video_sanity


def check_yolo(
    reporter: Reporter,
    summary: dict[str, Any],
    cameras: list[str],
    max_queue_p95_ms: float,
    max_acquisition_to_worker_start_p95_ms: float | None,
    max_enqueue_to_dequeue_p95_ms: float | None,
    max_dequeue_to_worker_start_p95_ms: float | None,
    max_same_camera_service_gap_p95_ms: float | None,
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
        acquisition_to_worker = metric(summary, serial, "acquisition_to_worker_start_ms")
        enqueue_to_dequeue = metric(summary, serial, "yolo_enqueue_to_dequeue_ms")
        dequeue_to_worker = metric(summary, serial, "yolo_dequeue_to_worker_start_ms")
        queue = metric(summary, serial, "yolo_queue_wait_ms")
        service_gap = metric(summary, serial, "same_camera_service_gap_ms")
        cpu_pre_sync = metric(summary, serial, "cpu_pre_sync_ms")
        ptp_done = metric(summary, serial, "acquisition_to_ptp_done_ms")

        detect_steady_p95 = number(detect.get("steady_p95"))
        acquisition_to_worker_p95 = number(acquisition_to_worker.get("p95"))
        enqueue_to_dequeue_p95 = number(enqueue_to_dequeue.get("p95"))
        dequeue_to_worker_p95 = number(dequeue_to_worker.get("p95"))
        queue_p95 = number(queue.get("p95"))
        service_gap_p95 = number(service_gap.get("p95"))
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
        if acquisition_to_worker_p95 is not None:
            if max_acquisition_to_worker_start_p95_ms is None:
                reporter.pass_(
                    f"Cam{serial} acquisition_to_worker_start p95={acquisition_to_worker_p95:.3f} ms"
                )
            else:
                reporter.check(
                    acquisition_to_worker_p95 <= max_acquisition_to_worker_start_p95_ms,
                    (
                        f"Cam{serial} acquisition_to_worker_start p95="
                        f"{acquisition_to_worker_p95:.3f} ms"
                    ),
                    (
                        f"Cam{serial} acquisition_to_worker_start p95="
                        f"{acquisition_to_worker_p95:.3f} ms > "
                        f"{max_acquisition_to_worker_start_p95_ms:.3f} ms"
                    ),
                )
        if enqueue_to_dequeue_p95 is not None:
            if max_enqueue_to_dequeue_p95_ms is None:
                reporter.pass_(
                    f"Cam{serial} yolo_enqueue_to_dequeue p95={enqueue_to_dequeue_p95:.3f} ms"
                )
            else:
                reporter.check(
                    enqueue_to_dequeue_p95 <= max_enqueue_to_dequeue_p95_ms,
                    f"Cam{serial} yolo_enqueue_to_dequeue p95={enqueue_to_dequeue_p95:.3f} ms",
                    (
                        f"Cam{serial} yolo_enqueue_to_dequeue p95={enqueue_to_dequeue_p95:.3f} ms "
                        f"> {max_enqueue_to_dequeue_p95_ms:.3f} ms"
                    ),
                )
        if dequeue_to_worker_p95 is not None:
            if max_dequeue_to_worker_start_p95_ms is None:
                reporter.pass_(
                    f"Cam{serial} yolo_dequeue_to_worker_start p95={dequeue_to_worker_p95:.3f} ms"
                )
            else:
                reporter.check(
                    dequeue_to_worker_p95 <= max_dequeue_to_worker_start_p95_ms,
                    (
                        f"Cam{serial} yolo_dequeue_to_worker_start p95="
                        f"{dequeue_to_worker_p95:.3f} ms"
                    ),
                    (
                        f"Cam{serial} yolo_dequeue_to_worker_start p95="
                        f"{dequeue_to_worker_p95:.3f} ms > "
                        f"{max_dequeue_to_worker_start_p95_ms:.3f} ms"
                    ),
                )
        if service_gap_p95 is not None:
            if max_same_camera_service_gap_p95_ms is None:
                reporter.pass_(
                    f"Cam{serial} same_camera_service_gap p95={service_gap_p95:.3f} ms"
                )
            else:
                reporter.check(
                    service_gap_p95 <= max_same_camera_service_gap_p95_ms,
                    f"Cam{serial} same_camera_service_gap p95={service_gap_p95:.3f} ms",
                    (
                        f"Cam{serial} same_camera_service_gap p95={service_gap_p95:.3f} ms "
                        f"> {max_same_camera_service_gap_p95_ms:.3f} ms"
                    ),
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


def pipe_cpu_set_contains_exactly(value: str | None, expected_cpu: int) -> bool:
    if value is None:
        return False
    parts = {part.strip() for part in str(value).split("|") if part.strip()}
    return parts == {str(expected_cpu)}


def int_set_from_json_list(value: Any) -> set[int]:
    if not isinstance(value, list):
        return set()
    cpus: set[int] = set()
    for item in value:
        try:
            cpu = int(item)
        except (TypeError, ValueError):
            continue
        cpus.add(cpu)
    return cpus


def normalized_system_cpu_kernel_cmdline_options(system_cpu: dict[str, Any]) -> list[str]:
    return gui_summary.normalized_kernel_cpu_options(system_cpu)


def check_system_cpu_isolation(
    reporter: Reporter,
    snapshot: dict[str, Any],
    required_cpus: list[int],
    required_kernel_cmdline_cpus_by_option: dict[str, list[int]] | None = None,
) -> dict[str, Any]:
    required_kernel_cmdline_cpus_by_option = required_kernel_cmdline_cpus_by_option or {}
    system_cpu = nested_dict(snapshot, "session", "system_cpu")
    isolated = nested_dict(system_cpu, "isolated_cpus")
    if not required_cpus and not required_kernel_cmdline_cpus_by_option:
        return system_cpu

    reporter.check(
        bool(system_cpu),
        "recording_snapshot session.system_cpu present",
        "recording_snapshot session.system_cpu missing",
    )
    if required_cpus:
        reporter.check(
            isolated.get("available") is True,
            "system_cpu isolated CPU telemetry available",
            f"system_cpu isolated CPU telemetry unavailable: {isolated.get('error')!r}",
        )
        reporter.check(
            isolated.get("parse_ok") is True,
            "system_cpu isolated CPU list parsed",
            (
                "system_cpu isolated CPU list failed to parse: "
                f"raw={isolated.get('raw')!r} error={isolated.get('error')!r}"
            ),
        )

        observed = int_set_from_json_list(isolated.get("cpus"))
        missing = sorted(set(required_cpus) - observed)
        required_text = ",".join(str(cpu) for cpu in required_cpus)
        observed_text = ",".join(str(cpu) for cpu in sorted(observed))
        reporter.check(
            not missing,
            f"system_cpu isolated CPUs include {required_text}",
            (
                f"system_cpu isolated CPUs {observed_text or '<empty>'} "
                f"missing required {','.join(str(cpu) for cpu in missing)}"
            ),
        )

    if required_kernel_cmdline_cpus_by_option:
        cmdline = nested_dict(system_cpu, "kernel_cmdline")
        options = nested_dict(cmdline, "options")
        reporter.check(
            cmdline.get("available") is True,
            "system_cpu kernel cmdline telemetry available",
            f"system_cpu kernel cmdline telemetry unavailable: {cmdline.get('error')!r}",
        )
        reporter.check(
            bool(options),
            "system_cpu kernel cmdline options present",
            "system_cpu kernel cmdline options missing",
        )
        for option, cpus in sorted(required_kernel_cmdline_cpus_by_option.items()):
            raw_value = options.get(option)
            reporter.check(
                isinstance(raw_value, str) and bool(raw_value.strip()),
                f"kernel cmdline {option} option present",
                f"kernel cmdline {option} option missing",
            )
            if not isinstance(raw_value, str) or not raw_value.strip():
                continue
            observed, invalid = parse_kernel_cmdline_cpu_option_value(raw_value)
            if invalid:
                reporter.fail(
                    f"kernel cmdline {option} has invalid CPU tokens: {','.join(invalid)}"
                )
            missing = sorted(set(cpus) - observed)
            required_text = ",".join(str(cpu) for cpu in cpus)
            observed_text = ",".join(str(cpu) for cpu in sorted(observed))
            reporter.check(
                not missing,
                f"kernel cmdline {option} CPUs include {required_text}",
                (
                    f"kernel cmdline {option} CPUs {observed_text or '<empty>'} "
                    f"missing required {','.join(str(cpu) for cpu in missing)}"
                ),
            )
    return system_cpu


def check_yolo_affinity(
    reporter: Reporter,
    recording_folder: Path,
    snapshot: dict[str, Any],
    expected_affinity_by_serial: dict[str, int],
) -> None:
    if not expected_affinity_by_serial:
        return
    yolo_worker = nested_dict(snapshot, "session", "yolo_worker")
    affinity_by_camera = nested_dict(yolo_worker, "affinity", "per_camera")
    for serial, expected_cpu in sorted(expected_affinity_by_serial.items()):
        snapshot_affinity = (
            affinity_by_camera.get(serial)
            if isinstance(affinity_by_camera, dict)
            else None
        )
        snapshot_affinity = snapshot_affinity if isinstance(snapshot_affinity, dict) else {}
        requested_cpus = snapshot_affinity.get("requested_cpus")
        reporter.check(
            snapshot_affinity.get("configured") is True and str(requested_cpus) == str(expected_cpu),
            f"Cam{serial} snapshot YOLO affinity requested CPU {expected_cpu}",
            (
                f"Cam{serial} snapshot YOLO affinity requested_cpus={requested_cpus!r}, "
                f"expected {expected_cpu}"
            ),
        )

        rows = read_csv_rows(recording_folder / f"Cam{serial}_yolo_perf.csv")
        if not rows:
            reporter.fail(f"Cam{serial} YOLO affinity check missing yolo perf rows")
            continue
        first = rows[0]
        configured = integer(first.get("yolo_affinity_configured"))
        applied = integer(first.get("yolo_affinity_applied"))
        requested_effective = first.get("yolo_affinity_requested_cpus")
        effective_cpus = first.get("yolo_affinity_effective_cpus")
        reporter.check(
            configured == 1,
            f"Cam{serial} YOLO affinity configured in perf CSV",
            f"Cam{serial} YOLO affinity configured={configured}, expected 1",
        )
        reporter.check(
            applied == 1,
            f"Cam{serial} YOLO affinity applied by worker",
            f"Cam{serial} YOLO affinity applied={applied}, expected 1",
        )
        reporter.check(
            pipe_cpu_set_contains_exactly(requested_effective, expected_cpu),
            f"Cam{serial} YOLO affinity requested_cpus={requested_effective}",
            (
                f"Cam{serial} YOLO affinity requested_cpus={requested_effective!r}, "
                f"expected CPU {expected_cpu}"
            ),
        )
        reporter.check(
            pipe_cpu_set_contains_exactly(effective_cpus, expected_cpu),
            f"Cam{serial} YOLO affinity effective_cpus={effective_cpus}",
            (
                f"Cam{serial} YOLO affinity effective_cpus={effective_cpus!r}, "
                f"expected CPU {expected_cpu}"
            ),
        )


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
    expected_swap_interval: int | None,
    expected_frame_max_fps: int | None,
    expected_yolo_speed_graphs_enabled: int | None,
    require_timing_telemetry: bool,
    require_imgui_glfw_size_cache: bool,
) -> dict[str, Any]:
    if (
        min_overall_p05 is None
        and min_visible_p05 is None
        and min_hidden_p05 is None
        and expected_stream_downsample is None
        and expected_display_preview_max_fps is None
        and expected_swap_interval is None
        and expected_frame_max_fps is None
        and expected_yolo_speed_graphs_enabled is None
        and not require_timing_telemetry
        and not require_imgui_glfw_size_cache
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
    if expected_swap_interval is not None:
        swap_interval = integer(metrics.get("swap_interval"))
        reporter.check(
            swap_interval == expected_swap_interval,
            f"GUI swap interval={swap_interval}",
            f"GUI swap interval={swap_interval}, expected {expected_swap_interval}",
        )
    if expected_frame_max_fps is not None:
        frame_max_fps = integer(metrics.get("frame_max_fps"))
        reporter.check(
            frame_max_fps == expected_frame_max_fps,
            f"GUI frame max FPS={frame_max_fps}",
            f"GUI frame max FPS={frame_max_fps}, expected {expected_frame_max_fps}",
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

    if require_imgui_glfw_size_cache:
        size_cache = metrics.get("imgui_glfw_size_cache")
        size_cache = size_cache if isinstance(size_cache, dict) else {}
        reporter.check(
            bool(size_cache),
            "GUI ImGui GLFW size-cache telemetry present",
            "GUI ImGui GLFW size-cache telemetry missing",
        )
        reporter.check(
            integer(size_cache.get("schema_version")) == 1,
            "GUI ImGui GLFW size-cache schema_version=1",
            (
                "GUI ImGui GLFW size-cache "
                f"schema_version={size_cache.get('schema_version')!r}"
            ),
        )
        source = size_cache.get("source")
        reporter.check(
            source == "orange_imgui_glfw_size_cache",
            "GUI ImGui GLFW size-cache source=orange_imgui_glfw_size_cache",
            f"GUI ImGui GLFW size-cache source={source!r}",
        )
        cache_context_registered = size_cache.get("cache_context_registered")
        reporter.check(
            cache_context_registered is True,
            "GUI ImGui GLFW size-cache context registered",
            (
                "GUI ImGui GLFW size-cache context not registered "
                f"({cache_context_registered!r})"
            ),
        )
        window_hits = integer(size_cache.get("window_size_cache_hits"))
        framebuffer_hits = integer(size_cache.get("framebuffer_size_cache_hits"))
        window_fallbacks = integer(size_cache.get("window_size_fallbacks"))
        framebuffer_fallbacks = integer(size_cache.get("framebuffer_size_fallbacks"))
        null_requests = integer(size_cache.get("null_window_requests"))
        total_size_requests = integer(size_cache.get("total_size_requests"))
        expected_total = None
        if (
            window_hits is not None
            and framebuffer_hits is not None
            and window_fallbacks is not None
            and framebuffer_fallbacks is not None
            and null_requests is not None
        ):
            expected_total = (
                window_hits
                + framebuffer_hits
                + window_fallbacks
                + framebuffer_fallbacks
                + null_requests
            )
        reporter.check(
            window_hits is not None and window_hits > 0,
            f"GUI ImGui GLFW window-size cache hits={window_hits}",
            f"GUI ImGui GLFW window-size cache hits missing or zero ({window_hits})",
        )
        reporter.check(
            framebuffer_hits is not None and framebuffer_hits > 0,
            f"GUI ImGui GLFW framebuffer-size cache hits={framebuffer_hits}",
            (
                "GUI ImGui GLFW framebuffer-size cache hits missing or zero "
                f"({framebuffer_hits})"
            ),
        )
        reporter.check(
            window_fallbacks == 0,
            "GUI ImGui GLFW window-size fallback calls=0",
            f"GUI ImGui GLFW window-size fallback calls={window_fallbacks}",
        )
        reporter.check(
            framebuffer_fallbacks == 0,
            "GUI ImGui GLFW framebuffer-size fallback calls=0",
            (
                "GUI ImGui GLFW framebuffer-size fallback calls="
                f"{framebuffer_fallbacks}"
            ),
        )
        reporter.check(
            null_requests == 0,
            "GUI ImGui GLFW null-window size requests=0",
            f"GUI ImGui GLFW null-window size requests={null_requests}",
        )
        reporter.check(
            expected_total is not None and total_size_requests == expected_total,
            f"GUI ImGui GLFW total size requests={total_size_requests}",
            (
                "GUI ImGui GLFW total size requests "
                f"{total_size_requests} != expected {expected_total}"
            ),
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


def crop_rolling_clips_for_serial(
    crop_recording_backend: dict[str, Any],
    serial: str,
) -> list[dict[str, Any]]:
    rolling_clips = crop_recording_backend.get("rolling_clips")
    if not isinstance(rolling_clips, dict):
        return []
    serial_clips = rolling_clips.get(serial)
    if not isinstance(serial_clips, list):
        return []
    return [clip for clip in serial_clips if isinstance(clip, dict)]


def crop_clip_artifact_path(
    recording_folder: Path,
    clip: dict[str, Any],
    *keys: str,
) -> Path | None:
    for key in keys:
        value = clip.get(key)
        if isinstance(value, str) and value:
            return path_from_recording_folder(recording_folder, value)
    return None


def check_crop_metadata_geometry(
    reporter: Reporter,
    serial: str,
    rows: list[dict[str, str]],
    crop_size: int | None,
    label: str,
) -> None:
    label_prefix = f"{label} " if label else ""
    if crop_size is None:
        reporter.fail(f"Cam{serial} {label_prefix}crop size missing from recording_snapshot")
        return
    bad_geometry_rows = []
    for index, row in enumerate(rows, start=2):
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
        f"Cam{serial} {label_prefix}crop metadata geometry matches crop_size_px={crop_size}",
        (
            f"Cam{serial} {label_prefix}crop metadata has {len(bad_geometry_rows)} row(s) "
            f"with geometry inconsistent with crop_size_px={crop_size}"
        ),
    )


def check_crop_rolling_clip_artifacts(
    reporter: Reporter,
    recording_folder: Path,
    serial: str,
    clip: dict[str, Any],
    crop_size: int | None,
    ffprobe: str,
    *,
    probe_video: bool,
) -> dict[str, Any]:
    clip_index = integer(clip.get("clip_index"))
    label = f"rolling crop clip {clip_index}" if clip_index is not None else "rolling crop clip"
    frame_count = integer(clip.get("frame_count"))
    first_frame = integer(clip.get("first_recording_frame_id"))
    last_frame = integer(clip.get("last_recording_frame_id"))
    video_path = crop_clip_artifact_path(recording_folder, clip, "video", "mp4")
    metadata_path = crop_clip_artifact_path(recording_folder, clip, "metadata")
    perf_path = crop_clip_artifact_path(recording_folder, clip, "perf")
    keyframes_path = crop_clip_artifact_path(recording_folder, clip, "keyframes", "mp4_keyframe")

    reporter.check(
        clip_index is not None and clip_index >= 0,
        f"Cam{serial} {label} index present",
        f"Cam{serial} rolling crop clip has invalid clip_index={clip_index}",
    )
    reporter.check(
        frame_count is not None and frame_count > 0,
        f"Cam{serial} {label} frame_count={frame_count}",
        f"Cam{serial} {label} invalid frame_count={frame_count}",
    )
    reporter.check(
        (
            first_frame is not None and last_frame is not None and
            frame_count is not None and
            first_frame > 0 and last_frame == first_frame + frame_count - 1
        ),
        f"Cam{serial} {label} frame range matches frame_count",
        (
            f"Cam{serial} {label} invalid frame range: first={first_frame}, "
            f"last={last_frame}, frame_count={frame_count}"
        ),
    )

    for path, artifact_label in (
        (video_path, "video"),
        (metadata_path, "metadata"),
        (perf_path, "perf"),
        (keyframes_path, "keyframes"),
    ):
        reporter.check(
            path is not None and path.exists() and (artifact_label != "video" or path.stat().st_size > 0),
            f"Cam{serial} {label} {artifact_label} present",
            f"Cam{serial} {label} {artifact_label} missing or empty: {path}",
        )

    keyframes_valid = keyframes_path is not None and json_file_parses_as_object(keyframes_path)
    keyframes = read_json(keyframes_path) if keyframes_valid and keyframes_path is not None else {}
    keyframe_total_frames = integer(keyframes.get("total_frames"))
    reporter.check(
        keyframes_valid,
        f"Cam{serial} {label} keyframe sidecar parses as JSON",
        f"Cam{serial} {label} keyframe sidecar missing or invalid JSON: {keyframes_path}",
    )
    if keyframes_valid and keyframes_path is not None:
        check_keyframe_sidecar_starts_at_zero(
            reporter,
            keyframes,
            keyframes_path,
            f"Cam{serial} {label}",
        )

    crop_rows = read_csv_rows(metadata_path) if metadata_path is not None else []
    crop_perf_rows = read_csv_rows(perf_path) if perf_path is not None else []
    metadata_rows_declared = integer(clip.get("metadata_rows"))
    perf_rows_declared = integer(clip.get("perf_rows"))
    reporter.check(
        bool(crop_rows),
        f"Cam{serial} {label} crop metadata rows={len(crop_rows)}",
        f"Cam{serial} {label} crop metadata has no data rows",
    )
    reporter.check(
        bool(crop_perf_rows),
        f"Cam{serial} {label} crop perf rows={len(crop_perf_rows)}",
        f"Cam{serial} {label} crop perf has no data rows",
    )
    reporter.check(
        frame_count is not None and len(crop_rows) == frame_count,
        f"Cam{serial} {label} crop metadata rows match frame_count ({len(crop_rows)})",
        (
            f"Cam{serial} {label} crop metadata rows ({len(crop_rows)}) != "
            f"frame_count ({frame_count})"
        ),
    )
    reporter.check(
        frame_count is not None and len(crop_perf_rows) == frame_count,
        f"Cam{serial} {label} crop perf rows match frame_count ({len(crop_perf_rows)})",
        (
            f"Cam{serial} {label} crop perf rows ({len(crop_perf_rows)}) != "
            f"frame_count ({frame_count})"
        ),
    )
    if metadata_rows_declared is not None:
        reporter.check(
            metadata_rows_declared == len(crop_rows),
            f"Cam{serial} {label} metadata_rows matches sidecar",
            (
                f"Cam{serial} {label} metadata_rows ({metadata_rows_declared}) != "
                f"sidecar rows ({len(crop_rows)})"
            ),
        )
    if perf_rows_declared is not None:
        reporter.check(
            perf_rows_declared == len(crop_perf_rows),
            f"Cam{serial} {label} perf_rows matches sidecar",
            (
                f"Cam{serial} {label} perf_rows ({perf_rows_declared}) != "
                f"sidecar rows ({len(crop_perf_rows)})"
            ),
        )
    reporter.check(
        keyframe_total_frames == frame_count,
        f"Cam{serial} {label} keyframe total_frames matches frame_count ({keyframe_total_frames})",
        (
            f"Cam{serial} {label} keyframe total_frames ({keyframe_total_frames}) != "
            f"frame_count ({frame_count})"
        ),
    )

    crop_ids, missing_crop_ids = recording_frame_ids_from_rows(crop_rows)
    perf_ids, missing_perf_ids = recording_frame_ids_from_rows(crop_perf_rows)
    reporter.check(
        missing_crop_ids == 0 and ids_are_positive_strictly_increasing(crop_ids),
        f"Cam{serial} {label} metadata recording_frame_id values are positive and increasing",
        (
            f"Cam{serial} {label} metadata recording_frame_id values are invalid "
            f"(missing={missing_crop_ids})"
        ),
    )
    reporter.check(
        missing_perf_ids == 0 and ids_are_positive_strictly_increasing(perf_ids),
        f"Cam{serial} {label} perf recording_frame_id values are positive and increasing",
        (
            f"Cam{serial} {label} perf recording_frame_id values are invalid "
            f"(missing={missing_perf_ids})"
        ),
    )
    reporter.check(
        crop_ids == perf_ids,
        f"Cam{serial} {label} perf and metadata recording_frame_id sequences match",
        f"Cam{serial} {label} perf and metadata recording_frame_id sequences differ",
    )
    if crop_ids and first_frame is not None and last_frame is not None and frame_count is not None:
        reporter.check(
            crop_ids[0] == first_frame and crop_ids[-1] == last_frame and
            len(crop_ids) == frame_count,
            f"Cam{serial} {label} recording_frame_id range matches clip",
            (
                f"Cam{serial} {label} recording_frame_id range "
                f"{crop_ids[0]}-{crop_ids[-1]} does not match clip "
                f"{first_frame}-{last_frame} frame_count={frame_count}"
            ),
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
        f"Cam{serial} {label} crop perf dropped column present",
        f"Cam{serial} {label} crop perf missing dropped value on {len(missing_dropped_rows)} row(s)",
    )
    reporter.check(
        not dropped_rows,
        f"Cam{serial} {label} crop perf reports no dropped crop frames",
        f"Cam{serial} {label} crop perf reports {len(dropped_rows)} dropped crop frame(s)",
    )
    check_crop_metadata_geometry(reporter, serial, crop_rows, crop_size, label)

    video_frames: int | None = None
    video_width: int | None = None
    video_height: int | None = None
    if video_path is not None and video_path.exists() and probe_video:
        video = gui_summary.ffprobe_video(video_path, ffprobe)
        video_frames = integer(video.get("frames"))
        video_width = integer(video.get("width"))
        video_height = integer(video.get("height"))
        reporter.check(
            video.get("status") == "ok",
            f"Cam{serial} {label} crop MP4 ffprobe status=ok",
            f"Cam{serial} {label} crop MP4 ffprobe status={video.get('status')!r}",
        )
        reporter.check(
            video_frames == frame_count,
            f"Cam{serial} {label} crop MP4 frame count matches frame_count ({video_frames})",
            (
                f"Cam{serial} {label} crop MP4 frames ({video_frames}) != "
                f"frame_count ({frame_count})"
            ),
        )
        if crop_size is not None:
            reporter.check(
                video_width == crop_size and video_height == crop_size,
                f"Cam{serial} {label} crop MP4 dimensions match crop_size_px ({crop_size})",
                (
                    f"Cam{serial} {label} crop MP4 dimensions {video_width}x{video_height} "
                    f"!= crop_size_px {crop_size}"
                ),
            )

    return {
        "clip_index": clip_index,
        "frame_count": frame_count,
        "first_recording_frame_id": first_frame,
        "last_recording_frame_id": last_frame,
        "metadata_rows": len(crop_rows),
        "perf_rows": len(crop_perf_rows),
        "keyframe_total_frames": keyframe_total_frames,
        "video_frames": video_frames,
        "video_width": video_width,
        "video_height": video_height,
        "dropped_rows": len(dropped_rows),
    }


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
    require_external_recorder_gpu_separate_from_analytics: bool = False,
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
    crop_backend_rolling_clips = crop_recording_backend.get("rolling_clips")
    crop_backend_rolling_clips = (
        crop_backend_rolling_clips
        if isinstance(crop_backend_rolling_clips, dict)
        else {}
    )
    crop_rolling_allowed = (
        recording_session_manifest.get("mode") == "rolling_clips" and
        bool(crop_backend_rolling_clips)
    )
    external_crop_contract = read_json(recording_folder / "external_crop_recorder_contract.json")
    check_crop_rollover_node(
        reporter,
        external_crop_contract,
        "external_crop_recorder_contract",
        require_present=require_external_crop_backend_metadata and bool(external_crop_contract),
        allow_rolling=crop_rolling_allowed,
    )
    check_crop_rollover_node(
        reporter,
        crop_recording_backend,
        "recording_backend.crop_recording",
        require_present=require_external_crop_backend_metadata and bool(crop_recording_backend),
        allow_rolling=crop_rolling_allowed,
    )

    for serial in target_cameras:
        serial_rolling_clips = crop_rolling_clips_for_serial(crop_recording_backend, serial)
        serial_crop_rolling_allowed = crop_rolling_allowed and bool(serial_rolling_clips)
        if crop_rolling_allowed:
            reporter.check(
                bool(serial_rolling_clips),
                f"Cam{serial} crop rolling clips present",
                f"Cam{serial} crop rolling clips missing from recording_backend.crop_recording.rolling_clips",
            )
        crop_output = crop_output_for(snapshot, serial)
        crop_descriptor = crop_recording_output_descriptor(snapshot, serial)
        crop_descriptor_details = nested_dict(crop_descriptor, "details")
        crop_size = crop_size_from_snapshot(snapshot, serial)
        backend_mode = crop_recording_backend.get("mode")
        prefer_external_backend_artifacts = (
            isinstance(backend_mode, str)
            and backend_mode == "external_ipc"
            and serial_crop_rolling_allowed
        )
        if require_external_crop_backend_metadata and serial_crop_rolling_allowed:
            reporter.check(
                bool(crop_descriptor),
                f"Cam{serial} session-aggregate crop recording_output present",
                f"Cam{serial} session-aggregate crop recording_output missing",
            )
            reporter.check(
                crop_descriptor.get("backend") == "external_ipc",
                f"Cam{serial} session-aggregate crop recording_output backend=external_ipc",
                (
                    f"Cam{serial} session-aggregate crop recording_output "
                    f"backend={crop_descriptor.get('backend')!r}"
                ),
            )
            reporter.check(
                crop_descriptor.get("status") in {"completed", "finalized"},
                f"Cam{serial} session-aggregate crop recording_output completed",
                (
                    f"Cam{serial} session-aggregate crop recording_output "
                    f"status={crop_descriptor.get('status')!r}"
                ),
            )
            reporter.check(
                crop_descriptor_details.get("scope") == "session_aggregate",
                f"Cam{serial} session-aggregate crop recording_output scope=session_aggregate",
                (
                    f"Cam{serial} session-aggregate crop recording_output "
                    f"scope={crop_descriptor_details.get('scope')!r}"
                ),
            )
        # In GUI external crop rolling, recording_snapshot.json can still carry
        # the in-process crop descriptor that was known before recorder
        # finalization. The final recording_session backend is authoritative for
        # external crop video/keyframe/summary artifacts unless the snapshot has
        # already been updated with the final external session-aggregate
        # descriptor.
        crop_descriptor_for_external_artifacts = (
            crop_descriptor
            if (
                not prefer_external_backend_artifacts or
                crop_descriptor.get("backend") == "external_ipc"
            )
            else {}
        )
        video_path = resolve_crop_video_artifact_path(
            recording_folder,
            crop_output,
            crop_descriptor_for_external_artifacts,
            crop_recording_backend,
            serial,
        )
        metadata_path = resolve_crop_artifact_path(
            recording_folder, crop_output, crop_descriptor, "metadata", f"Cam{serial}_crop_meta.csv"
        )
        keyframes_path = resolve_crop_keyframe_artifact_path(
            recording_folder,
            crop_output,
            crop_descriptor_for_external_artifacts,
            crop_recording_backend,
            serial,
        )
        perf_path = resolve_crop_artifact_path(
            recording_folder, crop_output, crop_descriptor, "perf", f"Cam{serial}_crop_perf.csv"
        )
        descriptor_backend = str(crop_descriptor.get("backend", ""))
        if prefer_external_backend_artifacts:
            descriptor_backend = "external_ipc"
        elif not descriptor_backend and isinstance(crop_recording_backend, dict):
            descriptor_backend = backend_mode if isinstance(backend_mode, str) else ""
        summary_path = resolve_crop_summary_artifact_path(
            recording_folder,
            crop_descriptor_for_external_artifacts,
            crop_recording_backend,
            serial,
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
        if require_external_crop_backend_metadata and serial_crop_rolling_allowed:
            descriptor_frame_count = integer(crop_descriptor.get("frame_count"))
            reporter.check(
                descriptor_frame_count == len(crop_rows),
                (
                    f"Cam{serial} session-aggregate crop recording_output "
                    f"frame_count matches crop metadata rows ({descriptor_frame_count})"
                ),
                (
                    f"Cam{serial} session-aggregate crop recording_output "
                    f"frame_count ({descriptor_frame_count}) != crop metadata rows ({len(crop_rows)})"
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
        external_stream_config_source: str | None = None
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
            if external_summary:
                check_external_summary_mp4_queue_overflow(
                    reporter,
                    f"Cam{serial} external crop",
                    external_summary,
                )
                check_storage_preflight_payload(
                    reporter,
                    f"Cam{serial} external crop summary",
                    external_summary,
                )
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
            backend_stream_config = nested_dict(crop_recording_backend, "stream_config").get(serial)
            backend_stream_config = backend_stream_config if isinstance(backend_stream_config, dict) else {}
            descriptor_details = nested_dict(crop_descriptor, "details")
            detail_stream_config = descriptor_stream_config(descriptor_details)
            contract_stream_config = external_crop_contract_stream_config(
                external_crop_contract,
                serial,
                str(first_present(
                    backend_stream_config.get("stream_id"),
                    detail_stream_config.get("stream_id"),
                    "",
                )) or None,
            )
            external_stream_config = merge_stream_config_with_fallbacks(
                backend_stream_config,
                contract_stream_config,
                detail_stream_config,
            )
            check_crop_rollover_node(
                reporter,
                contract_stream_config,
                f"Cam{serial} external_crop_recorder_contract.stream",
                require_present=False,
                allow_rolling=serial_crop_rolling_allowed,
            )
            check_crop_rollover_node(
                reporter,
                backend_stream_config,
                f"Cam{serial} recording_backend.crop_recording.stream_config",
                require_present=False,
                allow_rolling=serial_crop_rolling_allowed,
            )
            check_crop_rollover_node(
                reporter,
                descriptor_details,
                f"Cam{serial} recording_outputs.crop.details",
                require_present=False,
                allow_rolling=serial_crop_rolling_allowed,
            )
            external_stream_config_source = (
                "recording_backend.crop_recording.stream_config"
                if backend_stream_config else (
                    "external_crop_recorder_contract.json"
                    if contract_stream_config else (
                        "recording_outputs.crop.details"
                        if detail_stream_config else None
                    )
                )
            )
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
            if require_external_recorder_gpu_separate_from_analytics:
                reporter.check(
                    external_analytics_gpu_id is not None and external_recorder_gpu_id is not None,
                    (
                        f"Cam{serial} external crop analytics/recorder GPU metadata present "
                        f"({external_analytics_gpu_id}->{external_recorder_gpu_id})"
                    ),
                    (
                        f"Cam{serial} external crop analytics/recorder GPU metadata missing: "
                        f"analytics_gpu_id={external_analytics_gpu_id}, "
                        f"recorder_gpu_id={external_recorder_gpu_id}"
                    ),
                )
                if external_analytics_gpu_id is not None and external_recorder_gpu_id is not None:
                    reporter.check(
                        external_recorder_gpu_id != external_analytics_gpu_id,
                        (
                            f"Cam{serial} external crop recorder GPU "
                            f"{external_recorder_gpu_id} is separate from analytics GPU "
                            f"{external_analytics_gpu_id}"
                        ),
                        (
                            f"Cam{serial} external crop recorder_gpu_id "
                            f"({external_recorder_gpu_id}) matches analytics_gpu_id "
                            f"({external_analytics_gpu_id}); this uses the same CUDA device "
                            "as crop production"
                        ),
                    )
            if require_external_crop_backend_metadata:
                reporter.check(
                    bool(backend_stream_config),
                    f"Cam{serial} recording_backend.crop_recording.stream_config present",
                    f"Cam{serial} recording_backend.crop_recording.stream_config missing",
                )
                stream_id = require_stream_config_string(
                    reporter,
                    serial,
                    backend_stream_config,
                    "stream_id",
                )
                analytics_gpu_id = require_stream_config_int(
                    reporter,
                    serial,
                    backend_stream_config,
                    "analytics_gpu_id",
                    minimum=0,
                )
                recorder_gpu_id = require_stream_config_int(
                    reporter,
                    serial,
                    backend_stream_config,
                    "recorder_gpu_id",
                    minimum=0,
                )
                socket_path = require_stream_config_string(
                    reporter,
                    serial,
                    backend_stream_config,
                    "socket_path",
                )
                summary_json = require_stream_config_string(
                    reporter,
                    serial,
                    backend_stream_config,
                    "summary_json",
                )
                status_json = require_stream_config_string(
                    reporter,
                    serial,
                    backend_stream_config,
                    "status_json",
                )
                if summary_path is not None and summary_json is not None:
                    reporter.check(
                        summary_json == str(summary_path),
                        (
                            f"Cam{serial} recording_backend.crop_recording.stream_config "
                            "summary_json matches descriptor"
                        ),
                        (
                            f"Cam{serial} recording_backend.crop_recording.stream_config "
                            f"summary_json ({summary_json}) != descriptor summary ({summary_path})"
                        ),
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
                details = crop_descriptor_details
                reporter.check(
                    bool(details),
                    f"Cam{serial} recording_outputs.crop.details present",
                    f"Cam{serial} recording_outputs.crop.details missing",
                )
                if details:
                    if serial_crop_rolling_allowed:
                        check_descriptor_detail_matches_string(
                            reporter,
                            serial,
                            details,
                            "scope",
                            "session_aggregate",
                        )
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
                    check_descriptor_detail_matches_string(
                        reporter,
                        serial,
                        details,
                        "summary_json",
                        summary_json,
                    )
                    check_descriptor_detail_matches_string(
                        reporter,
                        serial,
                        details,
                        "status_json",
                        status_json,
                    )
            if backend_stream_config and external_encode_queue_depth is not None:
                stream_config_queue_depth = integer(backend_stream_config.get("encode_queue_depth"))
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

        check_crop_metadata_geometry(reporter, serial, crop_rows, crop_size, "")

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

        rolling_clip_summaries: list[dict[str, Any]] = []
        if serial_crop_rolling_allowed:
            for clip in serial_rolling_clips:
                rolling_clip_summaries.append(
                    check_crop_rolling_clip_artifacts(
                        reporter,
                        recording_folder,
                        serial,
                        clip,
                        crop_size,
                        ffprobe,
                        probe_video=probe_video,
                    )
                )
            rolling_metadata_rows = sum(
                item.get("metadata_rows") or 0 for item in rolling_clip_summaries
            )
            rolling_perf_rows = sum(
                item.get("perf_rows") or 0 for item in rolling_clip_summaries
            )
            rolling_frame_count = sum(
                item.get("frame_count") or 0 for item in rolling_clip_summaries
            )
            reporter.check(
                rolling_metadata_rows == len(crop_rows),
                (
                    f"Cam{serial} rolling crop metadata rows sum to root crop metadata rows "
                    f"({rolling_metadata_rows})"
                ),
                (
                    f"Cam{serial} rolling crop metadata rows ({rolling_metadata_rows}) != "
                    f"root crop metadata rows ({len(crop_rows)})"
                ),
            )
            reporter.check(
                rolling_perf_rows == len(crop_perf_rows),
                (
                    f"Cam{serial} rolling crop perf rows sum to root crop perf rows "
                    f"({rolling_perf_rows})"
                ),
                (
                    f"Cam{serial} rolling crop perf rows ({rolling_perf_rows}) != "
                    f"root crop perf rows ({len(crop_perf_rows)})"
                ),
            )
            reporter.check(
                rolling_frame_count == len(crop_rows),
                (
                    f"Cam{serial} rolling crop frame_count sums to crop metadata rows "
                    f"({rolling_frame_count})"
                ),
                (
                    f"Cam{serial} rolling crop frame_count ({rolling_frame_count}) != "
                    f"crop metadata rows ({len(crop_rows)})"
                ),
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
            "external_stream_config_source": external_stream_config_source,
            "rolling_clip_count": len(rolling_clip_summaries) if serial_crop_rolling_allowed else 0,
            "rolling_clips": rolling_clip_summaries,
        }

    return crop_summary


def compact_camera_summary(summary: dict[str, Any], cameras: list[str], video_sanity: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for serial in cameras:
        detect = metric(summary, serial, "acquisition_to_detect_done_ms") or metric(summary, serial, "capture_to_detect_done_ms")
        acquisition_to_worker = metric(summary, serial, "acquisition_to_worker_start_ms")
        enqueue_to_dequeue = metric(summary, serial, "yolo_enqueue_to_dequeue_ms")
        dequeue_to_worker = metric(summary, serial, "yolo_dequeue_to_worker_start_ms")
        queue = metric(summary, serial, "yolo_queue_wait_ms")
        service_gap = metric(summary, serial, "same_camera_service_gap_ms")
        ptp_done = metric(summary, serial, "acquisition_to_ptp_done_ms")
        yolo = nested_dict(summary, "yolo", serial)
        yolo_affinity = yolo.get("affinity")
        yolo_affinity = yolo_affinity if isinstance(yolo_affinity, dict) else {}
        yolo_events = yolo.get("events")
        yolo_events = yolo_events if isinstance(yolo_events, dict) else {}
        pipeline = nested_dict(summary, "pipeline", serial).get("final", {})
        video = nested_dict(summary, "videos", serial)
        outputs = nested_dict(summary, "outputs", serial)
        out[serial] = {
            "yolo_affinity": yolo_affinity,
            "yolo_event_rows": yolo_events.get("rows"),
            "yolo_detection_rows": yolo_events.get("detection_rows"),
            "yolo_zero_rows": yolo_events.get("zero_rows"),
            "yolo_failed_rows": yolo_events.get("failed_rows"),
            "yolo_timeout_rows": yolo_events.get("timeout_rows"),
            "yolo_event_parse_errors": yolo_events.get("parse_errors"),
            "detect_steady_p95_ms": detect.get("steady_p95"),
            "detect_p95_ms": detect.get("p95"),
            "acquisition_to_worker_start_p95_ms": acquisition_to_worker.get("p95"),
            "acquisition_to_worker_start_steady_p95_ms": acquisition_to_worker.get("steady_p95"),
            "yolo_enqueue_to_dequeue_p95_ms": enqueue_to_dequeue.get("p95"),
            "yolo_enqueue_to_dequeue_steady_p95_ms": enqueue_to_dequeue.get("steady_p95"),
            "yolo_dequeue_to_worker_start_p95_ms": dequeue_to_worker.get("p95"),
            "yolo_dequeue_to_worker_start_steady_p95_ms": dequeue_to_worker.get("steady_p95"),
            "queue_p95_ms": queue.get("p95"),
            "queue_steady_p95_ms": queue.get("steady_p95"),
            "same_camera_service_gap_p95_ms": service_gap.get("p95"),
            "same_camera_service_gap_steady_p95_ms": service_gap.get("steady_p95"),
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
            f"acq_worker_p95={item.get('acquisition_to_worker_start_p95_ms')} ms "
            f"acq_worker_steady_p95={item.get('acquisition_to_worker_start_steady_p95_ms')} ms "
            f"enqueue_dequeue_p95={item.get('yolo_enqueue_to_dequeue_p95_ms')} ms "
            f"enqueue_dequeue_steady_p95={item.get('yolo_enqueue_to_dequeue_steady_p95_ms')} ms "
            f"dequeue_worker_p95={item.get('yolo_dequeue_to_worker_start_p95_ms')} ms "
            f"queue_p95={item.get('queue_p95_ms')} ms "
            f"queue_steady_p95={item.get('queue_steady_p95_ms')} ms "
            f"service_gap_p95={item.get('same_camera_service_gap_p95_ms')} ms "
            f"service_gap_steady_p95={item.get('same_camera_service_gap_steady_p95_ms')} ms "
            f"ptp_done_p95={item.get('ptp_done_p95_ms')} ms "
            f"yolo_events={item.get('yolo_event_rows')} "
            f"yolo_det_rows={item.get('yolo_detection_rows')} "
            f"yolo_zero_rows={item.get('yolo_zero_rows')} "
            f"outputs={output_text} "
            f"video_frames={video.get('frames')} "
            f"bitrate_mbps={video.get('bitrate_mbps')}"
        )


def print_system_cpu_summary(system_cpu: dict[str, Any]) -> None:
    if not system_cpu:
        return
    isolated = system_cpu.get("isolated_cpus")
    isolated = isolated if isinstance(isolated, dict) else {}
    cpus = isolated.get("cpus")
    if isinstance(cpus, list):
        cpu_text = ",".join(str(cpu) for cpu in cpus)
    else:
        cpu_text = str(isolated.get("raw") or "")
    print("\nSystem CPU")
    print(
        f"  isolated: available={isolated.get('available')} "
        f"parse_ok={isolated.get('parse_ok')} cpus={cpu_text or '<empty>'}"
    )
    cmdline = system_cpu.get("kernel_cmdline")
    cmdline = cmdline if isinstance(cmdline, dict) else {}
    options = cmdline.get("options")
    options = options if isinstance(options, dict) else {}
    if cmdline:
        option_text = ", ".join(
            f"{key}={options.get(key)}"
            for key in ("isolcpus", "nohz_full", "rcu_nocbs")
            if options.get(key)
        )
        print(
            f"  kernel_cmdline: available={cmdline.get('available')} "
            f"options={option_text or '<none>'}"
        )
    normalized_options = normalized_system_cpu_kernel_cmdline_options(system_cpu)
    if normalized_options:
        print("  kernel_cmdline normalized: " + " ".join(normalized_options))


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
            f"rolling_clips={item.get('rolling_clip_count')} "
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
    if "swap_interval" in gui_fps:
        print(f"  swap-interval: {gui_fps.get('swap_interval')}")
    if "frame_max_fps" in gui_fps:
        print(f"  frame-max-fps: {gui_fps.get('frame_max_fps')}")
    if "yolo_speed_graphs_enabled" in gui_fps:
        print(f"  yolo-speed-graphs-enabled: {gui_fps.get('yolo_speed_graphs_enabled')}")
    size_cache = gui_fps.get("imgui_glfw_size_cache")
    size_cache = size_cache if isinstance(size_cache, dict) else {}
    if size_cache:
        print(
            "  imgui-glfw-size-cache: "
            f"registered={size_cache.get('cache_context_registered')} "
            f"window_hits={size_cache.get('window_size_cache_hits')} "
            f"framebuffer_hits={size_cache.get('framebuffer_size_cache_hits')} "
            f"fallbacks={size_cache.get('window_size_fallbacks')}/"
            f"{size_cache.get('framebuffer_size_fallbacks')} "
            f"null_requests={size_cache.get('null_window_requests')} "
            f"total={size_cache.get('total_size_requests')}"
        )
    print(f"  overall: {bucket_text('overall')}")
    print(f"  crop-preview-visible: {bucket_text('crop_preview_visible')}")
    print(f"  crop-preview-hidden: {bucket_text('crop_preview_hidden')}")
    if isinstance(gui_fps.get("timings"), dict):
        timings = gui_fps["timings"]
        diagnosis = gui_fps.get("timing_diagnosis")
        diagnosis = diagnosis if isinstance(diagnosis, dict) else gui_summary.summarize_gui_timing_diagnosis(gui_fps)
        print("  timings:")
        print(f"    frame-total: {timing_text('frame_total_ms')}")
        print(f"    pre-frame-maintenance: {timing_text('pre_frame_maintenance_ms')}")
        print(f"    imgui-new-frame: {timing_text('imgui_new_frame_ms')}")
        print(f"    orange-window-draw: {timing_text('orange_window_draw_ms')}")
        print(f"    recording-panel-draw: {timing_text('recording_panel_draw_ms')}")
        print(f"    camera-properties-draw: {timing_text('camera_properties_draw_ms')}")
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


def print_recording_session_summary(recording_session: dict[str, Any]) -> None:
    if not recording_session:
        return
    print("\nRecording Session")
    print(
        f"  producer={recording_session.get('producer', 'unknown')} "
        f"mode={recording_session.get('mode', 'unknown')} "
        f"status={recording_session.get('status', 'unknown')}"
    )
    recording_control = recording_session.get("recording_control")
    recording_control = recording_control if isinstance(recording_control, dict) else {}
    if recording_control:
        print(
            "  recording-control: "
            f"record_for_seconds={recording_control.get('record_for_seconds')} "
            f"clip_seconds={recording_control.get('clip_seconds')}"
        )
    local_control_stop = recording_session.get("local_control_stop")
    local_control_stop = local_control_stop if isinstance(local_control_stop, dict) else {}
    if local_control_stop:
        print(
            "  local-control-stop: "
            f"method={local_control_stop.get('method', 'unknown')} "
            f"operation_id={local_control_stop.get('operation_id', 'unknown')} "
            f"source={local_control_stop.get('command_source', 'unknown')} "
            f"reason={local_control_stop.get('reason', 'unknown')} "
            f"ack_state={local_control_stop.get('ack_state', 'unknown')} "
            f"drain_timed_out={local_control_stop.get('drain_timed_out', 'unknown')} "
            "forced_stream_stop="
            f"{local_control_stop.get('forced_finalize_stream_stop_requested', 'unknown')}"
        )


def main() -> int:
    args = parse_args()
    if (
        args.expect_external_crop_recorder_gpu_id is not None
        and args.expect_external_crop_recorder_gpu_id < 0
    ):
        raise SystemExit("--expect-external-crop-recorder-gpu-id must be >= 0")
    if args.expect_record_for_seconds is not None and args.expect_record_for_seconds < 0:
        raise SystemExit("--expect-record-for-seconds must be >= 0")
    if args.expect_clip_seconds is not None and args.expect_clip_seconds < 0:
        raise SystemExit("--expect-clip-seconds must be >= 0")
    expected_external_recorder_gpu_by_serial = parse_expected_serial_int_map(
        args.expect_external_crop_recorder_gpu,
        "--expect-external-crop-recorder-gpu",
    )
    allowed_main_video_content_failure_serials = parse_serial_filter(
        args.allow_main_video_content_failure,
        "--allow-main-video-content-failure",
    )
    expected_yolo_affinity_by_serial = parse_expected_serial_int_map(
        args.expect_yolo_affinity,
        "--expect-yolo-affinity",
    )
    required_isolated_cpus = parse_cpu_list(
        args.require_isolated_cpus,
        "--require-isolated-cpus",
    )
    required_kernel_cmdline_cpus_by_option = parse_expected_option_cpu_map(
        args.require_kernel_cmdline_cpus,
        "--require-kernel-cmdline-cpus",
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

    check_source_version(
        reporter,
        snapshot,
        require_source_version=args.require_source_version,
        expected_git_command_user_mode=args.expect_source_git_command_user_mode,
        expected_dirty_tracked=args.expect_source_dirty_tracked,
    )
    system_cpu_summary = check_system_cpu_isolation(
        reporter,
        snapshot,
        required_isolated_cpus,
        required_kernel_cmdline_cpus_by_option,
    )

    if not cameras:
        reporter.fail("no cameras discovered or requested")
    else:
        unknown_allowed_content_failure_serials = sorted(
            allowed_main_video_content_failure_serials.difference(cameras)
        )
        if unknown_allowed_content_failure_serials:
            reporter.fail(
                "allowed main-video content failure camera(s) not present: "
                + ", ".join(f"Cam{serial}" for serial in unknown_allowed_content_failure_serials)
            )
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
        check_recording_session_manifest(
            reporter,
            recording_folder,
            snapshot,
            cameras,
            args.expect_recording_mode,
            args.expect_record_for_seconds,
            args.expect_clip_seconds,
            args.expect_local_control_stop_method,
            args.expect_local_control_stop_operation_id,
            args.expect_local_control_stop_command_source,
        )
        external_recorder_status_summary = check_external_recorder_status(
            reporter,
            recording_folder,
            args.require_external_recorder_status,
            args.require_external_recorder_storage_preflight,
            args.require_external_recorder_protocol_hello,
        )
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
            allowed_main_video_content_failure_serials,
            args.max_video_black_fraction,
            args.min_video_stddev,
        )
        check_yolo(
            reporter,
            summary,
            cameras,
            args.max_yolo_queue_p95_ms,
            args.max_yolo_acquisition_to_worker_start_p95_ms,
            args.max_yolo_enqueue_to_dequeue_p95_ms,
            args.max_yolo_dequeue_to_worker_start_p95_ms,
            args.max_yolo_same_camera_service_gap_p95_ms,
            args.max_yolo_steady_p95_ms,
            args.max_ptp_done_p95_ms,
        )
        check_yolo_affinity(
            reporter,
            recording_folder,
            snapshot,
            expected_yolo_affinity_by_serial,
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
            require_external_recorder_gpu_separate_from_analytics=(
                args.require_external_crop_recorder_gpu_separate_from_analytics
            ),
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
            args.expect_gui_swap_interval,
            args.expect_gui_frame_max_fps,
            args.expect_yolo_speed_graphs_enabled,
            args.require_gui_timing_telemetry,
            args.require_imgui_glfw_size_cache,
        )
    if not cameras:
        video_sanity = {}
        crop_preview_summary = {}
        crop_recording_summary = {}
        gui_display_frame_rate_summary = {}
        external_recorder_status_summary = {}

    camera_summary = compact_camera_summary(summary, cameras, video_sanity)
    recording_session_summary = (
        summary.get("recording_session")
        if isinstance(summary.get("recording_session"), dict)
        else {}
    )
    result = {
        "schema_version": 1,
        "recording_folder": str(recording_folder),
        "producer_version": snapshot.get("producer_version"),
        "allowed_main_video_content_failure_cameras": sorted(
            allowed_main_video_content_failure_serials
        ),
        "source_version": (
            snapshot.get("source_version")
            if isinstance(snapshot.get("source_version"), dict)
            else {}
        ),
        "system_cpu": system_cpu_summary,
        "system_cpu_kernel_cmdline_cpu_option_values": (
            normalized_system_cpu_kernel_cmdline_options(system_cpu_summary)
        ),
        "status": "fail" if reporter.failures else "pass",
        "passes": reporter.passes,
        "warnings": reporter.warnings,
        "failures": reporter.failures,
        "recording_session": recording_session_summary,
        "summary": camera_summary,
        "crop_preview": crop_preview_summary,
        "crop_recording": crop_recording_summary,
        "external_recorder_status": external_recorder_status_summary,
        "gui_display_frame_rate": gui_display_frame_rate_summary,
    }

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_system_cpu_summary(system_cpu_summary)
        print_recording_session_summary(recording_session_summary)
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
