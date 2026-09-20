#!/usr/bin/env python3
"""Decode an IPC replay MP4 and compare selected luma frames to raw sources."""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Any

try:
    import numpy as np
except ImportError as exc:  # pragma: no cover - exercised by deployment preflight
    raise SystemExit(
        "validate_ipc_replay_content.py requires NumPy; invoke it with a NumPy-enabled Python"
    ) from exc


DEFAULT_SAMPLES = "0,1,24,25,49,50,last"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def parse_samples(text: str, frame_count: int) -> list[int]:
    values: list[int] = []
    for token in text.split(","):
        token = token.strip().lower()
        if not token:
            raise ValueError("sample list contains an empty entry")
        value = frame_count - 1 if token == "last" else int(token)
        if value < 0 or value >= frame_count:
            raise ValueError(f"sample index {value} is outside [0,{frame_count - 1}]")
        if value not in values:
            values.append(value)
    require(bool(values), "at least one sample is required")
    require(values == sorted(values), "samples must be in increasing decode order")
    return values


def run_json(command: list[str], timeout_s: float) -> dict[str, Any]:
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout_s,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {command!r}\n{completed.stderr}"
        )
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"command did not produce JSON: {command!r}") from exc


def read_exact(pipe: Any, count: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < count:
        chunk = pipe.read(count - len(chunks))
        if not chunk:
            break
        chunks.extend(chunk)
    return bytes(chunks)


def load_reference_y(
    raw_file: Path,
    source_index: int,
    raw_pitch: int,
    raw_frame_bytes: int,
    width: int,
    height: int,
) -> Any:
    offset = source_index * raw_frame_bytes
    with raw_file.open("rb") as source:
        source.seek(offset)
        record = source.read(raw_pitch * height)
    require(
        len(record) == raw_pitch * height,
        f"short raw source frame {source_index}: {len(record)} bytes",
    )
    rows = np.frombuffer(record, dtype=np.uint8).reshape(height, raw_pitch)
    return np.ascontiguousarray(rows[:, :width]).reshape(-1)


def load_replay_rows(path: Path, frame_count: int, source_frames: int) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    require(len(rows) == frame_count, f"replay CSV has {len(rows)} rows, expected {frame_count}")
    for zero_based, row in enumerate(rows):
        frame_id = int(row["recording_frame_id"])
        source_index = int(row["source_index"])
        require(frame_id == zero_based + 1, f"replay CSV frame id mismatch at row {zero_based}")
        require(
            source_index == zero_based % source_frames,
            f"replay CSV source mapping mismatch at frame {zero_based}",
        )
    return rows


def validate(args: argparse.Namespace) -> dict[str, Any]:
    require(args.mp4.is_file(), f"missing MP4: {args.mp4}")
    require(args.mp4.stat().st_size > 0, f"empty MP4: {args.mp4}")
    require(args.raw_file.is_file(), f"missing raw source: {args.raw_file}")
    require(args.replay_csv.is_file(), f"missing replay CSV: {args.replay_csv}")
    require(args.width > 0 and args.height > 0, "width and height must be positive")
    require(args.raw_pitch >= args.width, "raw pitch is smaller than width")
    require(args.raw_frame_bytes >= args.raw_pitch * args.height, "raw record stride is too small")
    require(args.frames > 0 and args.source_frames > 0, "frame counts must be positive")

    samples = parse_samples(args.samples, args.frames)
    replay_rows = load_replay_rows(args.replay_csv, args.frames, args.source_frames)

    probe_command = [
        args.ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-count_frames",
        "-count_packets",
        "-show_entries",
        "stream=codec_name,width,height,pix_fmt,color_range,nb_read_frames,nb_read_packets",
        "-of",
        "json",
        str(args.mp4),
    ]
    probe = run_json(probe_command, args.timeout_s)
    streams = probe.get("streams", [])
    require(len(streams) == 1, f"expected one video stream, found {len(streams)}")
    stream = streams[0]
    require(stream.get("codec_name") == "hevc", f"unexpected codec: {stream}")
    require(int(stream.get("width", 0)) == args.width, f"unexpected width: {stream}")
    require(int(stream.get("height", 0)) == args.height, f"unexpected height: {stream}")
    require(stream.get("pix_fmt") in {"gray", "yuv420p", "yuvj420p"}, f"unexpected pixel format: {stream}")
    require(int(stream.get("nb_read_frames", -1)) == args.frames, f"decoded frame count mismatch: {stream}")
    require(int(stream.get("nb_read_packets", -1)) == args.frames, f"packet count mismatch: {stream}")

    expression = "+".join(f"eq(n\\,{sample})" for sample in samples)
    decode_command = [
        args.ffmpeg,
        "-v",
        "error",
        "-threads",
        str(args.decode_threads),
        "-i",
        str(args.mp4),
        "-map",
        "0:v:0",
        "-vf",
        f"select={expression},extractplanes=y",
        # The deployed FFmpeg predates -fps_mode; -vsync 0 preserves the
        # selected frames without synthesizing duplicates.
        "-vsync",
        "0",
        "-pix_fmt",
        "gray",
        "-f",
        "rawvideo",
        "pipe:1",
    ]
    plane_bytes = args.width * args.height
    metrics: list[dict[str, Any]] = []
    process = subprocess.Popen(
        decode_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert process.stdout is not None
        for frame_index in samples:
            decoded_bytes = read_exact(process.stdout, plane_bytes)
            require(
                len(decoded_bytes) == plane_bytes,
                f"decoded sample {frame_index} has {len(decoded_bytes)} bytes, expected {plane_bytes}",
            )
            decoded = np.frombuffer(decoded_bytes, dtype=np.uint8)
            source_index = int(replay_rows[frame_index]["source_index"])
            reference = load_reference_y(
                args.raw_file,
                source_index,
                args.raw_pitch,
                args.raw_frame_bytes,
                args.width,
                args.height,
            )
            difference = decoded.astype(np.float32) - reference.astype(np.float32)
            absolute = np.abs(difference)
            mse = float(np.mean(difference * difference))
            mae = float(np.mean(absolute))
            psnr = math.inf if mse == 0.0 else 10.0 * math.log10((255.0 * 255.0) / mse)
            metric = {
                "frame_index": frame_index,
                "recording_frame_id": int(replay_rows[frame_index]["recording_frame_id"]),
                "source_index": source_index,
                "decoded_luma_mean": float(decoded.mean()),
                "decoded_luma_stddev": float(decoded.std()),
                "reference_luma_mean": float(reference.mean()),
                "reference_luma_stddev": float(reference.std()),
                "mae": mae,
                "mse": mse,
                "psnr_db": None if math.isinf(psnr) else psnr,
                "max_abs_error": int(absolute.max()),
            }
            metric["pass"] = (
                mae <= args.max_mae
                and psnr >= args.min_psnr_db
                and metric["decoded_luma_stddev"] >= args.min_luma_stddev
            )
            metrics.append(metric)

        require(process.stdout.read(1) == b"", "ffmpeg emitted more selected frames than requested")
        _, stderr = process.communicate(timeout=args.timeout_s)
        require(
            process.returncode == 0,
            f"ffmpeg selected/full-stream decode failed ({process.returncode}):\n{stderr.decode(errors='replace')}",
        )
    except BaseException:
        if process.poll() is None:
            process.kill()
            process.wait()
        raise

    passed = all(metric["pass"] for metric in metrics)
    report = {
        "schema_id": "orange.nvenc_native.ipc_replay_content_validation",
        "schema_version": 1,
        "pass": passed,
        "scope": (
            "ffprobe counts every decoded frame and packet; ffmpeg decodes through EOS while "
            "emitting selected luma planes; selected luma is compared with exact raw source rows "
            "using lossy-codec thresholds"
        ),
        "mp4": str(args.mp4.resolve()),
        "replay_csv": str(args.replay_csv.resolve()),
        "raw_file": str(args.raw_file.resolve()),
        "stream": stream,
        "expected_frames": args.frames,
        "samples": samples,
        "thresholds": {
            "max_mae": args.max_mae,
            "min_psnr_db": args.min_psnr_db,
            "min_luma_stddev": args.min_luma_stddev,
        },
        "probe_command": probe_command,
        "decode_command": decode_command,
        "sample_metrics": metrics,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    require(passed, f"decoded content check failed; see {args.output}")
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mp4", type=Path, required=True)
    parser.add_argument("--replay-csv", type=Path, required=True)
    parser.add_argument("--raw-file", type=Path, required=True)
    parser.add_argument("--raw-pitch", type=int, required=True)
    parser.add_argument("--raw-frame-bytes", type=int, required=True)
    parser.add_argument("--source-frames", type=int, default=8)
    parser.add_argument("--frames", type=int, default=200)
    parser.add_argument("--width", type=int, default=4512)
    parser.add_argument("--height", type=int, default=4512)
    parser.add_argument("--samples", default=DEFAULT_SAMPLES)
    parser.add_argument("--max-mae", type=float, default=12.0)
    parser.add_argument("--min-psnr-db", type=float, default=25.0)
    parser.add_argument("--min-luma-stddev", type=float, default=20.0)
    parser.add_argument("--decode-threads", type=int, default=2)
    parser.add_argument("--timeout-s", type=float, default=300.0)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = validate(args)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"validate_ipc_replay_content: FAIL: {exc}", file=sys.stderr)
        return 1
    compact = {
        "pass": report["pass"],
        "frames": report["expected_frames"],
        "samples": [
            {
                "frame_index": metric["frame_index"],
                "source_index": metric["source_index"],
                "mae": metric["mae"],
                "psnr_db": metric["psnr_db"],
            }
            for metric in report["sample_metrics"]
        ],
        "output": str(args.output),
    }
    print(json.dumps(compact, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
