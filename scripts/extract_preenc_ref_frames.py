#!/usr/bin/env python3
"""Turn pre-encoder reference dumps into Mono8 PGM frames for INT8 calibration.

The in-process recording path's ``pre_encoder_reference_capture`` writes, per
camera, ``Cam<serial>_preenc_ref.bin`` (raw NV12 frames, pitch-aligned),
``Cam<serial>_preenc_ref_index.csv`` (reference_frame_index,
recording_frame_id, timestamp, timestamp_sys, byte_offset, byte_size) and
``Cam<serial>_preenc_ref.json`` (width, height, pitch, frame_size,
pixel_format). For a Mono8 camera the luma plane is the sensor frame, so this
takes the first ``height`` rows of ``width`` bytes from each frame and writes
them as binary PGMs, plus a manifest carrying the PTP timestamps.

Usage:
  extract_preenc_ref_frames.py --run <run dir or capture output dir> --out <dir>
      [--camera 2010096 ...] [--every 1] [--max-per-camera 0] [--label fish|empty]
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

import numpy as np


def find_dumps(root: Path) -> list[Path]:
    return sorted(set(root.rglob("Cam*_preenc_ref.json")))


def write_pgm(path: Path, luma: np.ndarray) -> None:
    with path.open("wb") as fh:
        fh.write(f"P5\n{luma.shape[1]} {luma.shape[0]}\n255\n".encode())
        fh.write(np.ascontiguousarray(luma).tobytes())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", required=True, help="directory searched recursively for Cam*_preenc_ref.json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--camera", action="append", help="serial filter, repeatable")
    ap.add_argument("--every", type=int, default=1, help="keep every Nth captured frame")
    ap.add_argument("--max-per-camera", type=int, default=0)
    ap.add_argument("--label", default="", help="scene label recorded in the manifest, e.g. fish or empty")
    args = ap.parse_args()

    dumps = find_dumps(Path(args.run))
    if not dumps:
        raise SystemExit(f"no Cam*_preenc_ref.json under {args.run}")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"source": str(Path(args.run).resolve()), "label": args.label, "frames": []}
    for meta_path in dumps:
        meta = json.loads(meta_path.read_text())
        serial = meta.get("camera_serial") or meta_path.name[3:].split("_")[0]
        if args.camera and serial not in args.camera:
            continue
        info = meta.get("frame", meta)
        width = int(info.get("width", meta.get("width", 0)))
        height = int(info.get("height", meta.get("height", 0)))
        pitch = int(info.get("pitch", meta.get("pitch", 0)))
        frame_size = int(info.get("frame_size", meta.get("frame_size", 0)))
        pixel_format = str(info.get("pixel_format", meta.get("pixel_format", ""))).lower()
        if pixel_format != "nv12":
            raise SystemExit(f"{meta_path}: pixel_format {pixel_format!r}, expected nv12")
        if not (width > 0 and height > 0 and pitch >= width and frame_size == pitch * height * 3 // 2):
            raise SystemExit(f"{meta_path}: inconsistent geometry width={width} height={height} pitch={pitch} frame_size={frame_size}")
        bin_path = meta_path.with_name(f"Cam{serial}_preenc_ref.bin")
        index_path = meta_path.with_name(f"Cam{serial}_preenc_ref_index.csv")
        if not bin_path.exists() or not index_path.exists():
            raise SystemExit(f"missing {bin_path} or {index_path}")
        bin_size = bin_path.stat().st_size
        rows = list(csv.DictReader(index_path.open()))
        kept = 0
        with bin_path.open("rb") as fh:
            for i, row in enumerate(rows):
                if i % args.every:
                    continue
                if args.max_per_camera and kept >= args.max_per_camera:
                    break
                offset = int(row["byte_offset"]); size = int(row["byte_size"])
                if size != frame_size or offset + size > bin_size:
                    raise SystemExit(f"{index_path}: row {i} offset {offset} size {size} does not fit the dump")
                fh.seek(offset)
                luma = np.frombuffer(fh.read(pitch * height), dtype=np.uint8).reshape(height, pitch)[:, :width]
                rec = int(row["recording_frame_id"])
                name = f"Cam{serial}_rec{rec:06d}.pgm"
                write_pgm(out / name, luma)
                manifest["frames"].append({"camera": serial, "recording_frame_id": rec, "timestamp": int(row["timestamp"]),
                                           "timestamp_sys": int(row["timestamp_sys"]), "file": name, "label": args.label,
                                           "luma_mean": float(luma.mean())})
                kept += 1
        print(f"[preenc] {serial}: {kept} frames from {len(rows)} captured ({meta_path.parent})", file=sys.stderr)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"[preenc] {len(manifest['frames'])} frames in {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
