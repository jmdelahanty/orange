#!/usr/bin/env python3
"""Decode-sample sanity check for external recorder MP4 artifacts."""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Any


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")
DEFAULT_FFMPEG = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg")
DEFAULT_MAX_BLACK_FRACTION = 0.98
DEFAULT_MIN_STDDEV = 5.0


def default_tool(path: Path, fallback: str) -> str:
    return str(path) if path.exists() else fallback


def _integer(value: Any) -> int:
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return 0


def video_content_sanity(
    mp4_path: Path,
    ffprobe: str,
    ffmpeg: str,
    max_black_fraction: float = DEFAULT_MAX_BLACK_FRACTION,
    min_stddev: float = DEFAULT_MIN_STDDEV,
) -> dict[str, Any]:
    """Probe the MP4 and decode five sampled frames; the one video check.

    Used by this script (headless recordings: Cam*_external_video_sanity.json
    next to the recorder outputs) and by validate_gui_ptp_recording.py (GUI
    recordings, after the folder is sealed). Never raises for a bad video: the
    result's status says what failed and content_valid is False.
    """
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
    width = _integer(stream.get("width"))
    height = _integer(stream.get("height"))
    if width <= 0 or height <= 0:
        return {
            "status": "invalid_dimensions",
            "content_valid": False,
            "detail": f"width={width} height={height}",
            "width": width,
            "height": height,
        }

    frame_count = _integer(stream.get("nb_frames"))
    if frame_count > 0:
        sample_indices = sorted({0, frame_count // 4, frame_count // 2, (3 * frame_count) // 4, frame_count - 1})
    else:
        sample_indices = [0]

    # Seek to each sample instead of decoding the whole file through a
    # select filter: a 10 min 4512x4512 HEVC recording took longer than the
    # 120 s budget to decode linearly (2026-09-21). The seek lands on the
    # preceding keyframe and decodes forward to the requested frame.
    frame_rate_text = str(stream.get("r_frame_rate") or "100/1")
    try:
        num, den = frame_rate_text.split("/")
        fps = float(num) / float(den) if float(den) else 100.0
    except ValueError:
        fps = 100.0
    decoded = b""
    for index in sample_indices:
        decode_cmd = [
            ffmpeg,
            "-v",
            "error",
            "-ss",
            f"{index / fps:.6f}",
            "-i",
            str(mp4_path),
            "-frames:v",
            "1",
            "-pix_fmt",
            "gray",
            "-f",
            "rawvideo",
            "-",
        ]
        try:
            decoded += subprocess.run(
                decode_cmd,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=120,
            ).stdout
        except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as exc:
            return {"status": "decode_failed", "content_valid": False, "detail": f"frame {index}: {exc}"}

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
                "min": next(value for value, count in enumerate(hist) if count),
                "max": 255 - next(value for value, count in enumerate(reversed(hist)) if count),
                "black_fraction_lt8": sum(hist[:8]) / pixel_count,
                "white_fraction_gt247": sum(hist[248:]) / pixel_count,
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
        "container": metadata.get("format", {}),
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


def write_result(path: Path, result: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mp4", type=Path, help="MP4 file to inspect.")
    parser.add_argument("summary_json", type=Path, help="Output video sanity JSON.")
    parser.add_argument("--ffprobe", default=default_tool(DEFAULT_FFPROBE, "ffprobe"))
    parser.add_argument("--ffmpeg", default=default_tool(DEFAULT_FFMPEG, "ffmpeg"))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    sanity = video_content_sanity(args.mp4, args.ffprobe, args.ffmpeg)
    result: dict[str, Any] = {
        "schema_version": 1,
        "video_path": str(args.mp4),
        "content_checked": True,
        **sanity,
    }
    result.setdefault("sampled_frames", [])
    write_result(args.summary_json, result)
    if "detail" in sanity:
        print(f"{sanity['status']}: {sanity['detail']}", file=sys.stderr)
        return 1
    print(
        "external_video_sanity "
        f"status={sanity['status']} frames={sanity['nb_frames']} samples={sanity['sampled_frame_count']} "
        f"mean_luma={sanity['mean_luma']:.3f} max_stddev={sanity['max_stddev']:.3f} "
        f"max_black_fraction_lt8={sanity['max_black_fraction_lt8']:.6f}"
    )
    return 0 if sanity["content_valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
