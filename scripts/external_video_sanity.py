#!/usr/bin/env python3
"""Decode-sample sanity check for external recorder MP4 artifacts."""

from __future__ import annotations

import argparse
import json
import math
import subprocess
from pathlib import Path
from typing import Any


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")
DEFAULT_FFMPEG = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg")


def default_tool(path: Path, fallback: str) -> str:
    return str(path) if path.exists() else fallback


def write_result(path: Path, result: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


def fail(summary_path: Path, mp4_path: Path, status: str, detail: str) -> None:
    write_result(
        summary_path,
        {
            "schema_version": 1,
            "video_path": str(mp4_path),
            "content_checked": True,
            "content_valid": False,
            "status": status,
            "detail": detail,
            "sampled_frames": [],
        },
    )
    raise SystemExit(f"{status}: {detail}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mp4", type=Path, help="MP4 file to inspect.")
    parser.add_argument("summary_json", type=Path, help="Output video sanity JSON.")
    parser.add_argument("--ffprobe", default=default_tool(DEFAULT_FFPROBE, "ffprobe"))
    parser.add_argument("--ffmpeg", default=default_tool(DEFAULT_FFMPEG, "ffmpeg"))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    mp4_path = args.mp4
    summary_path = args.summary_json

    if not mp4_path.exists() or mp4_path.stat().st_size == 0:
        fail(summary_path, mp4_path, "missing_video", "MP4 output is missing or empty")

    probe_cmd = [
        args.ffprobe,
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
        )
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        fail(summary_path, mp4_path, "ffprobe_failed", str(exc))

    metadata = json.loads(probe.stdout)
    streams = metadata.get("streams") or []
    if not streams:
        fail(summary_path, mp4_path, "no_video_stream", "ffprobe found no video stream")

    stream = streams[0]
    width = int(stream.get("width") or 0)
    height = int(stream.get("height") or 0)
    if width <= 0 or height <= 0:
        fail(summary_path, mp4_path, "invalid_dimensions", f"width={width} height={height}")

    try:
        frame_count = int(stream.get("nb_frames") or 0)
    except (TypeError, ValueError):
        frame_count = 0

    if frame_count > 0:
        sample_indices = sorted(
            {
                0,
                max(0, frame_count // 4),
                max(0, frame_count // 2),
                max(0, (3 * frame_count) // 4),
                max(0, frame_count - 1),
            }
        )
    else:
        sample_indices = [0]

    select_expr = "+".join(f"eq(n\\,{index})" for index in sample_indices)
    decode_cmd = [
        args.ffmpeg,
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
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        fail(summary_path, mp4_path, "decode_failed", str(exc))

    frame_bytes = width * height
    decoded_frames = len(decoded) // frame_bytes if frame_bytes else 0
    if decoded_frames == 0:
        fail(summary_path, mp4_path, "decode_empty", "ffmpeg returned no decoded sample frames")

    measurements: list[dict[str, Any]] = []
    for i in range(decoded_frames):
        frame = decoded[i * frame_bytes : (i + 1) * frame_bytes]
        hist = [0] * 256
        for value in frame:
            hist[value] += 1
        pixel_count = sum(hist)
        total = sum(value * count for value, count in enumerate(hist))
        total_sq = sum(value * value * count for value, count in enumerate(hist))
        mean = total / pixel_count
        variance = max(0.0, total_sq / pixel_count - mean * mean)
        min_value = next(value for value, count in enumerate(hist) if count)
        max_value = 255 - next(value for value, count in enumerate(reversed(hist)) if count)
        measurements.append(
            {
                "requested_frame_index": sample_indices[min(i, len(sample_indices) - 1)],
                "mean": mean,
                "stddev": math.sqrt(variance),
                "min": min_value,
                "max": max_value,
                "black_fraction_lt8": sum(hist[:8]) / pixel_count,
                "white_fraction_gt247": sum(hist[248:]) / pixel_count,
                "decoded_bytes": pixel_count,
            }
        )

    max_black_fraction = max(item["black_fraction_lt8"] for item in measurements)
    max_stddev = max(item["stddev"] for item in measurements)
    mean_luma = sum(item["mean"] for item in measurements) / len(measurements)
    content_valid = max_black_fraction < 0.98 and max_stddev >= 5.0
    if max_black_fraction >= 0.98:
        status = "black_frame"
    elif max_stddev < 5.0:
        status = "flat_frame"
    else:
        status = "pass"

    result = {
        "schema_version": 1,
        "video_path": str(mp4_path),
        "content_checked": True,
        "content_valid": content_valid,
        "status": status,
        "width": width,
        "height": height,
        "nb_frames": frame_count,
        "container": metadata.get("format", {}),
        "sampled_frame_count": len(measurements),
        "mean_luma": mean_luma,
        "max_stddev": max_stddev,
        "max_black_fraction_lt8": max_black_fraction,
        "thresholds": {
            "max_black_fraction_lt8": 0.98,
            "min_max_stddev": 5.0,
        },
        "sampled_frames": measurements,
    }
    write_result(summary_path, result)
    print(
        "external_video_sanity "
        f"status={status} frames={frame_count} samples={len(measurements)} "
        f"mean_luma={mean_luma:.3f} max_stddev={max_stddev:.3f} "
        f"max_black_fraction_lt8={max_black_fraction:.6f}"
    )
    return 0 if content_valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
