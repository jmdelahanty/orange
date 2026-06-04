#!/usr/bin/env python3
"""Update Orange app-config GUI display and recording profile defaults."""

from __future__ import annotations

import argparse
import json
import os
import tempfile
from pathlib import Path
from typing import Any


PROFILE_DEFAULTS = {
    "default": {
        "display_preview_max_fps": None,
        "swap_interval": None,
        "frame_max_fps": None,
    },
    "fast": {
        "display_preview_max_fps": 15,
        "swap_interval": 0,
        "frame_max_fps": 60,
    },
    "citrus_safe": {
        "display_preview_max_fps": 10,
        "swap_interval": 1,
        "frame_max_fps": 30,
    },
}


def default_config_path() -> Path:
    return Path.home() / "orange_data" / "config" / "app" / "default.json"


def nonnegative_int_in_range(max_value: int):
    def parse(value: str) -> int:
        try:
            parsed = int(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError("must be an integer") from exc
        if parsed < 0 or parsed > max_value:
            raise argparse.ArgumentTypeError(f"must be in [0,{max_value}]")
        return parsed

    return parse


def int_in_range(min_value: int, max_value: int):
    def parse(value: str) -> int:
        try:
            parsed = int(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError("must be an integer") from exc
        if parsed < min_value or parsed > max_value:
            raise argparse.ArgumentTypeError(f"must be in [{min_value},{max_value}]")
        return parsed

    return parse


def gui_stream_downsample(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if parsed not in {1, 2, 4, 8, 16}:
        raise argparse.ArgumentTypeError("must be one of 1, 2, 4, 8, or 16")
    return parsed


def serial_gpu_assignment(value: str) -> tuple[str, int]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("must be SERIAL=GPU")
    serial, gpu_text = value.split("=", 1)
    serial = serial.strip()
    if not serial:
        raise argparse.ArgumentTypeError("serial must not be empty")
    try:
        gpu_id = int(gpu_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("GPU must be an integer") from exc
    if gpu_id < 0 or gpu_id > 255:
        raise argparse.ArgumentTypeError("GPU must be in [0,255]")
    return serial, gpu_id


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        type=Path,
        default=default_config_path(),
        help="App config path. Default: %(default)s",
    )
    parser.add_argument(
        "--profile",
        choices=sorted(PROFILE_DEFAULTS),
        required=True,
        help="Display pacing profile to store in gui.display.profile.",
    )
    parser.add_argument(
        "--display-preview-max-fps",
        type=nonnegative_int_in_range(10000),
        default=None,
        help="Optional explicit full-frame display preview cap.",
    )
    parser.add_argument(
        "--swap-interval",
        type=nonnegative_int_in_range(4),
        default=None,
        help="Optional explicit GLFW swap interval.",
    )
    parser.add_argument(
        "--gui-frame-max-fps",
        type=nonnegative_int_in_range(1000),
        default=None,
        help="Optional explicit GUI frame cap; 0 disables the cap.",
    )
    parser.add_argument(
        "--stream-downsample",
        type=gui_stream_downsample,
        default=None,
        help="Optional gui.stream.downsample value; must be one of 1, 2, 4, 8, or 16.",
    )
    speed_group = parser.add_mutually_exclusive_group()
    speed_group.add_argument(
        "--show-speed-graphs",
        dest="show_speed_graphs",
        action="store_true",
        default=None,
        help="Set gui.telemetry.show_speed_graphs=true.",
    )
    speed_group.add_argument(
        "--hide-speed-graphs",
        dest="show_speed_graphs",
        action="store_false",
        help="Set gui.telemetry.show_speed_graphs=false.",
    )
    start_group = parser.add_mutually_exclusive_group()
    start_group.add_argument(
        "--enable-local-control-recording-start",
        dest="local_control_recording_start",
        action="store_true",
        default=None,
        help="Set gui.local_control.recording_start_enabled=true.",
    )
    start_group.add_argument(
        "--disable-local-control-recording-start",
        dest="local_control_recording_start",
        action="store_false",
        help="Set gui.local_control.recording_start_enabled=false.",
    )
    stop_group = parser.add_mutually_exclusive_group()
    stop_group.add_argument(
        "--enable-local-control-recording-stop",
        dest="local_control_recording_stop",
        action="store_true",
        default=None,
        help="Set gui.local_control.recording_stop_enabled=true.",
    )
    stop_group.add_argument(
        "--disable-local-control-recording-stop",
        dest="local_control_recording_stop",
        action="store_false",
        help="Set gui.local_control.recording_stop_enabled=false.",
    )
    citrus_stop_group = parser.add_mutually_exclusive_group()
    citrus_stop_group.add_argument(
        "--enable-citrus-completion-stop",
        dest="local_control_citrus_completion_stop",
        action="store_true",
        default=None,
        help="Set gui.local_control.citrus_completion_stop_enabled=true.",
    )
    citrus_stop_group.add_argument(
        "--disable-citrus-completion-stop",
        dest="local_control_citrus_completion_stop",
        action="store_false",
        help="Set gui.local_control.citrus_completion_stop_enabled=false.",
    )
    exit_group = parser.add_mutually_exclusive_group()
    exit_group.add_argument(
        "--enable-local-control-exit-after-finalize",
        dest="local_control_exit_after_finalize",
        action="store_true",
        default=None,
        help="Set gui.local_control.exit_after_finalize=true.",
    )
    exit_group.add_argument(
        "--disable-local-control-exit-after-finalize",
        dest="local_control_exit_after_finalize",
        action="store_false",
        help="Set gui.local_control.exit_after_finalize=false.",
    )
    drain_timeout_group = parser.add_mutually_exclusive_group()
    drain_timeout_group.add_argument(
        "--local-control-drain-timeout-seconds",
        type=nonnegative_int_in_range(86400),
        default=None,
        help="Set gui.local_control.drain_timeout_seconds.",
    )
    drain_timeout_group.add_argument(
        "--clear-local-control-drain-timeout-seconds",
        action="store_true",
        help="Set gui.local_control.drain_timeout_seconds=null.",
    )
    parser.add_argument(
        "--manual-citrus-completion-control",
        action="store_true",
        help=(
            "Store the normal manual-GUI Citrus completion profile: disable "
            "socket recording start and generic stop_recording, enable only "
            "citrus_completion stop, keep the GUI open after finalization, "
            "and set a 60s drain timeout unless explicitly overridden."
        ),
    )
    parser.add_argument(
        "--crop-recording-sink-mode",
        choices=["in_process", "inprocess", "real", "external_ipc"],
        default=None,
        help="Optional recording.crop.sink_mode value.",
    )
    parser.add_argument(
        "--crop-external-encode-queue-depth",
        type=int_in_range(1, 4096),
        default=None,
        help="Optional recording.crop.external_ipc.encode_queue_depth value.",
    )
    crop_recorder_gpu_group = parser.add_mutually_exclusive_group()
    crop_recorder_gpu_group.add_argument(
        "--crop-external-recorder-gpu-id",
        type=nonnegative_int_in_range(255),
        default=None,
        help="Optional recording.crop.external_ipc.recorder_gpu_id global fallback.",
    )
    crop_recorder_gpu_group.add_argument(
        "--clear-crop-external-recorder-gpu-id",
        action="store_true",
        help="Set recording.crop.external_ipc.recorder_gpu_id=null.",
    )
    parser.add_argument(
        "--crop-external-recorder-gpu",
        action="append",
        type=serial_gpu_assignment,
        default=[],
        metavar="SERIAL=GPU",
        help="Set a recording.crop.external_ipc.recorder_gpu_ids_by_serial entry.",
    )
    parser.add_argument(
        "--clear-crop-external-recorder-gpus",
        action="store_true",
        help="Clear recording.crop.external_ipc.recorder_gpu_ids_by_serial before applying entries.",
    )
    crop_pool_group = parser.add_mutually_exclusive_group()
    crop_pool_group.add_argument(
        "--crop-frame-pool-size",
        type=int_in_range(1, 512),
        default=None,
        help="Optional recording.crop.frame_pool_size value.",
    )
    crop_pool_group.add_argument(
        "--clear-crop-frame-pool-size",
        action="store_true",
        help="Set recording.crop.frame_pool_size=null.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the updated JSON without writing it.",
    )
    return parser.parse_args()


def load_config(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {
            "schema_id": "orange.app.config",
            "schema_version": 1,
        }
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"failed to parse {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise SystemExit(f"app config root must be a JSON object: {path}")
    return payload


def update_display_config(payload: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    out = dict(payload)
    out.setdefault("schema_id", "orange.app.config")
    out.setdefault("schema_version", 1)

    gui = out.get("gui")
    if gui is None:
        gui = {}
    if not isinstance(gui, dict):
        raise SystemExit("gui must be a JSON object")

    display = gui.get("display")
    if display is None:
        display = {}
    if not isinstance(display, dict):
        raise SystemExit("gui.display must be a JSON object")

    display = dict(display)
    display["profile"] = args.profile
    for key, value in PROFILE_DEFAULTS[args.profile].items():
        display[key] = value

    overrides = {
        "display_preview_max_fps": args.display_preview_max_fps,
        "swap_interval": args.swap_interval,
        "frame_max_fps": args.gui_frame_max_fps,
    }
    for key, value in overrides.items():
        if value is not None:
            display[key] = value

    gui = dict(gui)
    gui["display"] = display

    if args.stream_downsample is not None:
        stream = gui.get("stream")
        if stream is None:
            stream = {}
        if not isinstance(stream, dict):
            raise SystemExit("gui.stream must be a JSON object")
        stream = dict(stream)
        stream["downsample"] = args.stream_downsample
        gui["stream"] = stream

    if args.show_speed_graphs is not None:
        telemetry = gui.get("telemetry")
        if telemetry is None:
            telemetry = {}
        if not isinstance(telemetry, dict):
            raise SystemExit("gui.telemetry must be a JSON object")
        telemetry = dict(telemetry)
        telemetry["show_speed_graphs"] = args.show_speed_graphs
        gui["telemetry"] = telemetry

    if (
        args.manual_citrus_completion_control
        or args.local_control_recording_start is not None
        or args.local_control_recording_stop is not None
        or args.local_control_citrus_completion_stop is not None
        or args.local_control_exit_after_finalize is not None
        or args.local_control_drain_timeout_seconds is not None
        or args.clear_local_control_drain_timeout_seconds
    ):
        local_control = gui.get("local_control")
        if local_control is None:
            local_control = {}
        if not isinstance(local_control, dict):
            raise SystemExit("gui.local_control must be a JSON object")
        local_control = dict(local_control)
        if args.manual_citrus_completion_control:
            local_control["recording_start_enabled"] = False
            local_control["recording_stop_enabled"] = False
            local_control["citrus_completion_stop_enabled"] = True
            local_control["exit_after_finalize"] = False
            local_control["drain_timeout_seconds"] = 60
        if args.local_control_recording_start is not None:
            local_control["recording_start_enabled"] = args.local_control_recording_start
        if args.local_control_recording_stop is not None:
            local_control["recording_stop_enabled"] = args.local_control_recording_stop
        if args.local_control_citrus_completion_stop is not None:
            local_control["citrus_completion_stop_enabled"] = (
                args.local_control_citrus_completion_stop
            )
        if args.local_control_exit_after_finalize is not None:
            local_control["exit_after_finalize"] = args.local_control_exit_after_finalize
        if args.local_control_drain_timeout_seconds is not None:
            local_control["drain_timeout_seconds"] = (
                args.local_control_drain_timeout_seconds
            )
        elif args.clear_local_control_drain_timeout_seconds:
            local_control["drain_timeout_seconds"] = None
        gui["local_control"] = local_control

    out["gui"] = gui

    if (
        args.crop_recording_sink_mode is not None
        or args.crop_external_encode_queue_depth is not None
        or args.crop_external_recorder_gpu_id is not None
        or args.clear_crop_external_recorder_gpu_id
        or args.crop_external_recorder_gpu
        or args.clear_crop_external_recorder_gpus
        or args.crop_frame_pool_size is not None
        or args.clear_crop_frame_pool_size
    ):
        recording = out.get("recording")
        if recording is None:
            recording = {}
        if not isinstance(recording, dict):
            raise SystemExit("recording must be a JSON object")

        crop = recording.get("crop")
        if crop is None:
            crop = {}
        if not isinstance(crop, dict):
            raise SystemExit("recording.crop must be a JSON object")
        crop = dict(crop)

        if args.crop_recording_sink_mode is not None:
            crop["sink_mode"] = args.crop_recording_sink_mode
        if args.crop_frame_pool_size is not None:
            crop["frame_pool_size"] = args.crop_frame_pool_size
        elif args.clear_crop_frame_pool_size:
            crop["frame_pool_size"] = None
        if (
            args.crop_external_encode_queue_depth is not None
            or args.crop_external_recorder_gpu_id is not None
            or args.clear_crop_external_recorder_gpu_id
            or args.crop_external_recorder_gpu
            or args.clear_crop_external_recorder_gpus
        ):
            external_ipc = crop.get("external_ipc")
            if external_ipc is None:
                external_ipc = {}
            if not isinstance(external_ipc, dict):
                raise SystemExit("recording.crop.external_ipc must be a JSON object")
            external_ipc = dict(external_ipc)
            if args.crop_external_encode_queue_depth is not None:
                external_ipc["encode_queue_depth"] = args.crop_external_encode_queue_depth
            if args.crop_external_recorder_gpu_id is not None:
                external_ipc["recorder_gpu_id"] = args.crop_external_recorder_gpu_id
            elif args.clear_crop_external_recorder_gpu_id:
                external_ipc["recorder_gpu_id"] = None
            if args.crop_external_recorder_gpu or args.clear_crop_external_recorder_gpus:
                by_serial = external_ipc.get("recorder_gpu_ids_by_serial")
                if args.clear_crop_external_recorder_gpus or by_serial is None:
                    by_serial = {}
                if not isinstance(by_serial, dict):
                    raise SystemExit(
                        "recording.crop.external_ipc.recorder_gpu_ids_by_serial "
                        "must be a JSON object"
                    )
                by_serial = dict(by_serial)
                for serial, gpu_id in args.crop_external_recorder_gpu:
                    by_serial[serial] = gpu_id
                external_ipc["recorder_gpu_ids_by_serial"] = by_serial
            crop["external_ipc"] = external_ipc

        recording = dict(recording)
        recording["crop"] = crop
        out["recording"] = recording

    return out


def atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rendered = json.dumps(payload, indent=2) + "\n"
    fd, tmp_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        suffix=".tmp",
        dir=path.parent,
        text=True,
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(rendered)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_name, path)
    except Exception:
        try:
            os.unlink(tmp_name)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    args = parse_args()
    payload = load_config(args.config)
    updated = update_display_config(payload, args)
    rendered = json.dumps(updated, indent=2) + "\n"
    if args.dry_run:
        print(rendered, end="")
    else:
        atomic_write_json(args.config, updated)
        print(f"updated {args.config}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
