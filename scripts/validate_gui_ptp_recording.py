#!/usr/bin/env python3
"""Validate a production-like GUI PTP recording folder."""

from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import summarize_gui_validation as gui_summary


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")
DEFAULT_FFMPEG = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg")


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
    parser.add_argument("recording_folder", help="GUI recording folder, or parent containing one recording folder.")
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


def parse_expected_cameras(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


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


def check_pipeline(reporter: Reporter, summary: dict[str, Any], cameras: list[str]) -> None:
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


def compact_camera_summary(summary: dict[str, Any], cameras: list[str], video_sanity: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for serial in cameras:
        detect = metric(summary, serial, "acquisition_to_detect_done_ms") or metric(summary, serial, "capture_to_detect_done_ms")
        queue = metric(summary, serial, "yolo_queue_wait_ms")
        ptp_done = metric(summary, serial, "acquisition_to_ptp_done_ms")
        pipeline = nested_dict(summary, "pipeline", serial).get("final", {})
        video = nested_dict(summary, "videos", serial)
        out[serial] = {
            "detect_steady_p95_ms": detect.get("steady_p95"),
            "detect_p95_ms": detect.get("p95"),
            "queue_p95_ms": queue.get("p95"),
            "ptp_done_p95_ms": ptp_done.get("p95"),
            "pipeline_final": pipeline,
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
        print(
            f"  Cam{serial}: detect_steady_p95={item.get('detect_steady_p95_ms')} ms "
            f"queue_p95={item.get('queue_p95_ms')} ms "
            f"ptp_done_p95={item.get('ptp_done_p95_ms')} ms "
            f"video_frames={video.get('frames')} "
            f"bitrate_mbps={video.get('bitrate_mbps')}"
        )


def main() -> int:
    args = parse_args()
    recording_folder = gui_summary.resolve_recording_folder(Path(args.recording_folder))
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
        check_pipeline(reporter, summary, cameras)
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
    if not cameras:
        video_sanity = {}

    camera_summary = compact_camera_summary(summary, cameras, video_sanity)
    result = {
        "schema_version": 1,
        "recording_folder": str(recording_folder),
        "status": "fail" if reporter.failures else "pass",
        "passes": reporter.passes,
        "warnings": reporter.warnings,
        "failures": reporter.failures,
        "summary": camera_summary,
    }

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_camera_summary(camera_summary)
        if reporter.failures:
            print(f"\nResult: FAIL ({len(reporter.failures)} failures, {len(reporter.warnings)} warnings)")
        else:
            print(f"\nResult: PASS ({len(reporter.warnings)} warnings)")
    return 1 if reporter.failures else 0


if __name__ == "__main__":
    sys.exit(main())
