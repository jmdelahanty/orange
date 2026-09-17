#!/usr/bin/env python3
"""Overlay live pose results on clips cut from a run's crop video.

Step 1 of the pose overlay plan (2026-09-17): draw the keypoints the pose
worker produced during the run onto the 384 px crop video the external crop
recorder wrote for the same frames, so a reviewer can judge whether the
poses sit on the fish.

Inputs, all from one run folder (the ``run_0001__…`` directory, or the
experiment folder above it):

- ``Cam<serial>_crop_meta.csv``: one row per recording frame with the video
  crop origin in full-frame pixels and ``crop_video_frame_index``.
- ``external_crop_recorder/Cam<serial>_crop_external.mp4``: the crop video,
  one frame per recording frame, blanks included.
- ``Cam<serial>_pose_events.jsonl``: one record per frame with the pose crop
  origin (full-frame pixels) and the keypoints (pose-crop pixels).

Mapping: full = pose_crop.origin + keypoint; crop-video pixel = full -
video_crop.origin. The script refuses to draw unless the video's frame
count matches the meta rows and ``crop_video_frame_index`` is exactly
``recording_frame_id - 1`` everywhere.

Clips are chosen automatically (lowest pose confidence, largest keypoint
jump, around no-pose stretches, plus random controls) or given as second
ranges. Each clip is written at native rate and at quarter speed, with a
contact sheet, into ``<run>/pose_overlays/<serial>/``.

Usage:
  overlay_pose_clips.py --run <run dir> --camera 2010096 [--clips 6]
      [--clip-seconds 4] [--ranges 12.0-16.0,30.5-34.5] [--out <dir>]
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np
import pandas as pd

KEYPOINT_COLOURS = {
    "bladder": (60, 200, 255),   # amber (BGR)
    "eye_left": (80, 220, 80),   # green
    "eye_right": (255, 120, 60), # blue
}
DEFAULT_COLOUR = (200, 200, 200)
SCALE = 2  # 384 -> 768 output
FFMPEG_CANDIDATES = ["/usr/bin/ffmpeg", "ffmpeg", "/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg"]
NVENC_GPU = os.environ.get("ORANGE_OVERLAY_NVENC_GPU", "1")  # an A16 die, not the A6000 desktop GPU


@dataclass
class Clip:
    start_frame: int  # 0-based crop video frame index, inclusive
    end_frame: int    # exclusive
    reason: str
    anchor_frame: int
    stats: dict = field(default_factory=dict)

    @property
    def name(self) -> str:
        return f"clip_{self.start_frame:06d}_{self.reason}"


def find_run_dir(path: Path) -> Path:
    if (path / "external_crop_recorder").is_dir():
        return path
    runs = sorted(p for p in path.glob("run_*") if (p / "external_crop_recorder").is_dir())
    if runs:
        if len(runs) > 1:
            print(f"[overlay] {len(runs)} run dirs under {path}; using {runs[-1].name}", file=sys.stderr)
        return runs[-1]
    raise SystemExit(f"no run directory with external_crop_recorder under {path}")


def find_ffmpeg() -> tuple[str, list[str]] | None:
    """Return (ffmpeg path, H.264 encoder args): libx264 where available, else NVENC on an A16 die."""
    found = []
    for candidate in FFMPEG_CANDIDATES:
        path = candidate if os.path.isabs(candidate) else shutil.which(candidate)
        if path and os.path.exists(path) and path not in found:
            found.append(path)
    for path in found:
        try:
            enc = subprocess.run([path, "-hide_banner", "-encoders"], capture_output=True, text=True, check=False).stdout
        except OSError:
            continue
        if "libx264" in enc:
            return path, ["-c:v", "libx264", "-preset", "veryfast", "-crf", "18"]
    for path in found:
        enc = subprocess.run([path, "-hide_banner", "-encoders"], capture_output=True, text=True, check=False).stdout
        if "h264_nvenc" in enc:
            return path, ["-c:v", "h264_nvenc", "-gpu", NVENC_GPU, "-preset", "p4", "-rc", "vbr", "-cq", "19", "-b:v", "0"]
    return None


def load_pose_events(path: Path) -> pd.DataFrame:
    rows = []
    with path.open() as fh:
        for line in fh:
            if not line.strip():
                continue
            e = json.loads(line)
            if e.get("event_kind") != "pose_result":
                continue
            crop = e.get("crop") or {}
            det = e.get("detection") or {}
            poses = e.get("poses") or []
            best = max(poses, key=lambda p: p.get("confidence", 0.0)) if poses else None
            row = {
                "recording_frame_id": e["frame"]["recording_frame_id"],
                "pose_status": e.get("pose", {}).get("status"),
                "model_id": e.get("pose", {}).get("model_id", ""),
                "pose_crop_x": crop.get("x_px"),
                "pose_crop_y": crop.get("y_px"),
                "pose_crop_w": crop.get("width_px"),
                "pose_crop_h": crop.get("height_px"),
                "pose_blank": bool(crop.get("blank_frame", False)),
                "det_conf": det.get("confidence"),
                "has_detection": bool(det.get("has_detection", False)),
                "n_poses": len(poses),
                "pose_conf": best.get("confidence") if best else None,
                "capture_to_pose_done_ms": e.get("latency_ms", {}).get("capture_to_pose_done"),
                "keypoints": [
                    (k.get("label", ""), float(k["x_px"]), float(k["y_px"]), float(k.get("confidence", 0.0)))
                    for k in (best.get("keypoints") if best else [])
                ],
            }
            rows.append(row)
    if not rows:
        raise SystemExit(f"no pose_result events in {path}")
    return pd.DataFrame(rows)


def video_frame_count(path: Path) -> int:
    cap = cv2.VideoCapture(str(path))
    if not cap.isOpened():
        raise SystemExit(f"cannot open {path}")
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    cap.release()
    return n


def build_table(run_dir: Path, serial: str) -> tuple[pd.DataFrame, Path]:
    meta_path = run_dir / f"Cam{serial}_crop_meta.csv"
    events_path = run_dir / f"Cam{serial}_pose_events.jsonl"
    video_path = run_dir / "external_crop_recorder" / f"Cam{serial}_crop_external.mp4"
    for p in (meta_path, events_path, video_path):
        if not p.exists():
            raise SystemExit(f"missing {p}")
    meta = pd.read_csv(meta_path)
    events = load_pose_events(events_path)
    n_video = video_frame_count(video_path)

    # Alignment gates: refuse to draw anything off by a frame.
    problems = []
    if n_video != len(meta):
        problems.append(f"crop video has {n_video} frames, meta has {len(meta)} rows")
    expected = meta.recording_frame_id.values - 1
    if not np.array_equal(meta.crop_video_frame_index.values, expected):
        bad = int((meta.crop_video_frame_index.values != expected).sum())
        problems.append(f"crop_video_frame_index != recording_frame_id - 1 on {bad} rows")
    if not meta.recording_frame_id.is_monotonic_increasing:
        problems.append("recording_frame_id not monotonic in crop meta")
    if not meta.timestamp.is_monotonic_increasing:
        problems.append("timestamp not monotonic in crop meta")
    if events.recording_frame_id.duplicated().any():
        problems.append("duplicate recording_frame_id in pose events")
    missing_events = len(set(meta.recording_frame_id) - set(events.recording_frame_id))
    if problems:
        raise SystemExit("[overlay] alignment check failed:\n  " + "\n  ".join(problems))

    table = meta.merge(events, on="recording_frame_id", how="left")
    # Pose crop must sit inside the video crop on detected frames; report how often it does not.
    det = table[(table.has_detection_x == 1) & table.pose_crop_x.notna()]
    inside = (
        (det.pose_crop_x >= det.crop_x) & (det.pose_crop_y >= det.crop_y) &
        (det.pose_crop_x + det.pose_crop_w <= det.crop_x + det.crop_w) &
        (det.pose_crop_y + det.pose_crop_h <= det.crop_y + det.crop_h)
    )
    print(f"[overlay] {serial}: {len(table)} frames, {int(table.n_poses.fillna(0).gt(0).sum())} with a pose, "
          f"{missing_events} frames without a pose event, pose crop inside video crop on "
          f"{inside.mean() * 100:.1f}% of detected frames", file=sys.stderr)
    return table, video_path


def keypoints_in_video_crop(row) -> list[tuple[str, float, float, float]]:
    kps = row.keypoints if isinstance(row.keypoints, list) else []
    if not kps or pd.isna(row.pose_crop_x) or pd.isna(row.crop_x) or row.crop_w == 0:
        return []
    dx = float(row.pose_crop_x) - float(row.crop_x)
    dy = float(row.pose_crop_y) - float(row.crop_y)
    return [(label, x + dx, y + dy, conf) for (label, x, y, conf) in kps]


def full_frame_keypoints(row) -> dict[str, tuple[float, float]]:
    kps = row.keypoints if isinstance(row.keypoints, list) else []
    if not kps or pd.isna(row.pose_crop_x):
        return {}
    return {label: (float(row.pose_crop_x) + x, float(row.pose_crop_y) + y) for (label, x, y, _c) in kps}


def per_frame_metrics(table: pd.DataFrame) -> pd.DataFrame:
    """Per-frame pose confidence and keypoint jump (max keypoint displacement vs previous frame, full-frame px)."""
    conf = table.pose_conf.fillna(0.0).values
    jump = np.zeros(len(table))
    prev: dict[str, tuple[float, float]] = {}
    for i, row in enumerate(table.itertuples(index=False)):
        cur = full_frame_keypoints(row)
        if cur and prev:
            common = set(cur) & set(prev)
            if common:
                jump[i] = max(math.hypot(cur[k][0] - prev[k][0], cur[k][1] - prev[k][1]) for k in common)
        prev = cur if cur else prev
    out = table[["recording_frame_id"]].copy()
    out["pose_conf"] = conf
    out["jump_px"] = jump
    out["no_pose"] = (table.n_poses.fillna(0) == 0).values
    return out


def pick_clips(metrics: pd.DataFrame, fps: float, n_clips: int, clip_seconds: float, seed: int) -> list[Clip]:
    n = len(metrics)
    half = int(round(clip_seconds * fps / 2))
    length = 2 * half
    taken: list[tuple[int, int]] = []

    def free(start: int, end: int) -> bool:
        return all(end <= s or start >= e for (s, e) in taken)

    def add(anchor: int, reason: str, stats: dict) -> Clip | None:
        start = max(0, min(anchor - half, n - length))
        end = start + length
        if end <= start or not free(start, end):
            return None
        taken.append((start, end))
        return Clip(start, end, reason, anchor, stats)

    clips: list[Clip] = []
    conf = metrics.pose_conf.values
    jump = metrics.jump_px.values
    no_pose = metrics.no_pose.values
    # Rolling mean confidence over a clip length, pick the lowest windows.
    window = max(1, length)
    rolled = pd.Series(conf).rolling(window, min_periods=window // 2).mean().values
    order = np.argsort(np.nan_to_num(rolled, nan=1.0))
    # Interleave selection categories until n_clips.
    candidates: list[tuple[str, np.ndarray]] = [
        ("lowconf", order),
        ("jump", np.argsort(-jump)),
        ("nopose", np.flatnonzero(no_pose)),
    ]
    rng = random.Random(seed)
    random_anchors = np.array([rng.randrange(half, max(half + 1, n - half)) for _ in range(n_clips * 4)])
    candidates.append(("random", random_anchors))
    cursors = {name: 0 for name, _ in candidates}
    while len(clips) < n_clips:
        progressed = False
        for name, idx in candidates:
            if len(clips) >= n_clips:
                break
            while cursors[name] < len(idx):
                anchor = int(idx[cursors[name]])
                cursors[name] += 1
                if name == "jump" and jump[anchor] <= 0:
                    continue
                clip = add(anchor, name, {
                    "anchor_pose_conf": float(conf[anchor]),
                    "anchor_jump_px": float(jump[anchor]),
                })
                if clip:
                    clips.append(clip)
                    progressed = True
                    break
        if not progressed:
            break
    clips.sort(key=lambda c: c.start_frame)
    return clips


def parse_ranges(text: str, fps: float, n: int) -> list[Clip]:
    clips = []
    for part in text.split(","):
        a, b = part.strip().split("-")
        start = max(0, int(float(a) * fps))
        end = min(n, int(float(b) * fps))
        if end > start:
            clips.append(Clip(start, end, "range", start))
    return clips


def draw_frame(frame: np.ndarray, row, metrics_row, serial: str) -> np.ndarray:
    out = cv2.resize(frame, None, fx=SCALE, fy=SCALE, interpolation=cv2.INTER_NEAREST)
    h, w = out.shape[:2]
    kps = keypoints_in_video_crop(row)
    blank = bool(row.blank_frame) if not pd.isna(row.blank_frame) else False
    # Detection box in crop-video pixels.
    if not blank and row.crop_w and not pd.isna(row.detection_x):
        x0 = (float(row.detection_x) - float(row.crop_x)) * SCALE
        y0 = (float(row.detection_y) - float(row.crop_y)) * SCALE
        x1 = x0 + float(row.detection_w) * SCALE
        y1 = y0 + float(row.detection_h) * SCALE
        cv2.rectangle(out, (int(x0), int(y0)), (int(x1), int(y1)), (160, 160, 160), 1)
    pts = {label: (x * SCALE, y * SCALE, c) for (label, x, y, c) in kps}
    if "eye_left" in pts and "eye_right" in pts:
        cv2.line(out, (int(pts["eye_left"][0]), int(pts["eye_left"][1])),
                 (int(pts["eye_right"][0]), int(pts["eye_right"][1])), (255, 255, 255), 1, cv2.LINE_AA)
        if "bladder" in pts:
            mx = (pts["eye_left"][0] + pts["eye_right"][0]) / 2
            my = (pts["eye_left"][1] + pts["eye_right"][1]) / 2
            cv2.line(out, (int(mx), int(my)), (int(pts["bladder"][0]), int(pts["bladder"][1])),
                     (255, 255, 255), 1, cv2.LINE_AA)
    for label, (x, y, c) in pts.items():
        colour = KEYPOINT_COLOURS.get(label, DEFAULT_COLOUR)
        radius = max(2, int(round(3 + 4 * c)))
        cv2.circle(out, (int(round(x)), int(round(y))), radius, colour, -1, cv2.LINE_AA)
        cv2.circle(out, (int(round(x)), int(round(y))), radius, (0, 0, 0), 1, cv2.LINE_AA)
    # Text strip.
    strip = np.zeros((54, w, 3), dtype=np.uint8)
    conf = row.pose_conf if not pd.isna(row.pose_conf) else None
    lat = row.capture_to_pose_done_ms if not pd.isna(row.capture_to_pose_done_ms) else None
    model = (row.model_id or "")[:40] if isinstance(row.model_id, str) else ""
    line1 = f"Cam{serial} rec_frame {int(row.recording_frame_id)}  " \
            f"pose conf {conf:.2f}  " if conf is not None else f"Cam{serial} rec_frame {int(row.recording_frame_id)}  NO POSE  "
    if lat is not None:
        line1 += f"capture->pose {lat:.2f} ms"
    line2 = f"jump {metrics_row.jump_px:.1f} px  det conf {row.detection_confidence:.2f}  {model}" \
        if not pd.isna(row.detection_confidence) else f"jump {metrics_row.jump_px:.1f} px  {model}"
    cv2.putText(strip, line1, (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.putText(strip, line2, (8, 42), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (200, 200, 200), 1, cv2.LINE_AA)
    if not kps:
        cv2.putText(out, "NO POSE" if not blank else "NO DETECTION", (16, 40), cv2.FONT_HERSHEY_SIMPLEX,
                    1.0, (0, 0, 255), 2, cv2.LINE_AA)
    return np.vstack([out, strip])


class ClipWriter:
    """Writes BGR frames to H.264 through ffmpeg when available, else OpenCV mp4v."""

    def __init__(self, path: Path, fps: float, size: tuple[int, int], ffmpeg: tuple[str, list[str]] | None):
        self.path = path
        self.proc = None
        self.writer = None
        w, h = size
        if ffmpeg:
            binary, codec_args = ffmpeg
            cmd = [binary, "-hide_banner", "-loglevel", "error", "-y",
                   "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{w}x{h}", "-r", f"{fps:g}", "-i", "-",
                   *codec_args, "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(path)]
            try:
                self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
            except OSError:
                self.proc = None
        if self.proc is None:
            self.writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"mp4v"), fps, (w, h))

    def write(self, frame: np.ndarray) -> None:
        if self.proc is not None:
            self.proc.stdin.write(frame.tobytes())
        else:
            self.writer.write(frame)

    def close(self) -> bool:
        if self.proc is not None:
            self.proc.stdin.close()
            err = self.proc.stderr.read().decode(errors="replace")
            code = self.proc.wait()
            if code != 0:
                print(f"[overlay] ffmpeg failed for {self.path}: {err.strip()[:400]}", file=sys.stderr)
                return False
            return True
        self.writer.release()
        return True


def write_slow_copy(ffmpeg: tuple[str, list[str]] | None, src: Path, dst: Path, factor: int, fps: float) -> None:
    if not ffmpeg:
        return
    binary, codec_args = ffmpeg
    subprocess.run([binary, "-hide_banner", "-loglevel", "error", "-y", "-i", str(src),
                    "-filter:v", f"setpts={factor}*PTS", "-r", f"{fps / factor:g}",
                    *codec_args, "-pix_fmt", "yuv420p", str(dst)], check=False)


def contact_sheet(frames: list[np.ndarray], path: Path, columns: int = 5) -> None:
    if not frames:
        return
    thumbs = [cv2.resize(f, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA) for f in frames]
    h, w = thumbs[0].shape[:2]
    rows = math.ceil(len(thumbs) / columns)
    sheet = np.zeros((rows * h, columns * w, 3), dtype=np.uint8)
    for i, t in enumerate(thumbs):
        r, c = divmod(i, columns)
        sheet[r * h:(r + 1) * h, c * w:(c + 1) * w] = t
    cv2.imwrite(str(path), sheet)


def render(table: pd.DataFrame, metrics: pd.DataFrame, video_path: Path, clips: list[Clip],
           out_dir: Path, serial: str, fps: float, ffmpeg: tuple[str, list[str]] | None, sheet_every: int) -> list[dict]:
    out_dir.mkdir(parents=True, exist_ok=True)
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise SystemExit(f"cannot open {video_path}")
    by_start = {c.start_frame: c for c in clips}
    last_needed = max(c.end_frame for c in clips)
    active: Clip | None = None
    writer: ClipWriter | None = None
    sheet_frames: list[np.ndarray] = []
    results = []
    clip_stats: dict = {}
    idx = 0
    while idx < last_needed:
        ok, frame = cap.read()
        if not ok:
            print(f"[overlay] decode stopped at frame {idx}", file=sys.stderr)
            break
        if active is None and idx in by_start:
            active = by_start[idx]
            size = (frame.shape[1] * SCALE, frame.shape[0] * SCALE + 54)
            writer = ClipWriter(out_dir / f"{active.name}.mp4", fps, size, ffmpeg)
            sheet_frames = []
            clip_stats = {"frames": 0, "with_pose": 0, "conf_sum": 0.0, "max_jump": 0.0}
        if active is not None:
            row = table.iloc[idx]
            mrow = metrics.iloc[idx]
            drawn = draw_frame(frame, row, mrow, serial)
            writer.write(drawn)
            clip_stats["frames"] += 1
            if row.n_poses and row.n_poses > 0:
                clip_stats["with_pose"] += 1
                clip_stats["conf_sum"] += float(row.pose_conf)
            clip_stats["max_jump"] = max(clip_stats["max_jump"], float(mrow.jump_px))
            if (idx - active.start_frame) % sheet_every == 0:
                sheet_frames.append(drawn)
            if idx + 1 >= active.end_frame:
                ok_write = writer.close()
                native = out_dir / f"{active.name}.mp4"
                slow = out_dir / f"{active.name}_quarter_speed.mp4"
                if ok_write:
                    write_slow_copy(ffmpeg, native, slow, 4, fps)
                contact_sheet(sheet_frames, out_dir / f"{active.name}_sheet.png")
                results.append({
                    "clip": active.name,
                    "reason": active.reason,
                    "start_recording_frame_id": int(table.iloc[active.start_frame].recording_frame_id),
                    "end_recording_frame_id": int(table.iloc[active.end_frame - 1].recording_frame_id),
                    "start_s": active.start_frame / fps,
                    "end_s": active.end_frame / fps,
                    "anchor_recording_frame_id": int(table.iloc[active.anchor_frame].recording_frame_id),
                    "anchor": active.stats,
                    "frames": clip_stats["frames"],
                    "frames_with_pose": clip_stats["with_pose"],
                    "mean_pose_conf": (clip_stats["conf_sum"] / clip_stats["with_pose"]) if clip_stats["with_pose"] else None,
                    "max_keypoint_jump_px": clip_stats["max_jump"],
                    "files": {"native": native.name, "quarter_speed": slow.name if slow.exists() else None,
                              "sheet": f"{active.name}_sheet.png"},
                })
                print(f"[overlay] wrote {native.name} ({clip_stats['frames']} frames, "
                      f"{clip_stats['with_pose']} with pose)", file=sys.stderr)
                active = None
                writer = None
        idx += 1
    cap.release()
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", required=True, help="run_0001__… directory or the experiment folder above it")
    ap.add_argument("--camera", required=True, help="camera serial, e.g. 2010096")
    ap.add_argument("--clips", type=int, default=6, help="number of automatic clips (default 6)")
    ap.add_argument("--clip-seconds", type=float, default=4.0)
    ap.add_argument("--ranges", help="explicit clips as start-end seconds, comma separated (overrides --clips)")
    ap.add_argument("--out", help="output directory (default <run>/pose_overlays/<serial>)")
    ap.add_argument("--fps", type=float, default=100.0)
    ap.add_argument("--sheet-every", type=int, default=20)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    run_dir = find_run_dir(Path(args.run).resolve())
    table, video_path = build_table(run_dir, args.camera)
    metrics = per_frame_metrics(table)
    if args.ranges:
        clips = parse_ranges(args.ranges, args.fps, len(table))
    else:
        clips = pick_clips(metrics, args.fps, args.clips, args.clip_seconds, args.seed)
    if not clips:
        raise SystemExit("no clips selected")
    out_dir = Path(args.out) if args.out else run_dir / "pose_overlays" / args.camera
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        print("[overlay] ffmpeg not found; writing mp4v with OpenCV, no slow copies", file=sys.stderr)
    results = render(table, metrics, video_path, clips, out_dir, args.camera, args.fps, ffmpeg, args.sheet_every)
    summary = {
        "run_dir": str(run_dir),
        "camera_serial": args.camera,
        "crop_video": str(video_path),
        "frames_total": int(len(table)),
        "frames_with_pose": int(table.n_poses.fillna(0).gt(0).sum()),
        "frames_blank": int(table.blank_frame.fillna(0).sum()),
        "pose_conf_mean": float(table.pose_conf.dropna().mean()) if table.pose_conf.notna().any() else None,
        "pose_conf_p05": float(table.pose_conf.dropna().quantile(0.05)) if table.pose_conf.notna().any() else None,
        "keypoint_jump_p99_px": float(np.quantile(metrics.jump_px, 0.99)),
        "model_id": str(table.model_id.dropna().iloc[0]) if table.model_id.notna().any() else None,
        "clips": results,
    }
    (out_dir / "index.json").write_text(json.dumps(summary, indent=2))
    print(f"[overlay] {len(results)} clips in {out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
