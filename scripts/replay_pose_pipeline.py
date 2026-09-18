#!/usr/bin/env python3
"""Offline replay of the two-stage detect + crop + pose pipeline on recorded frames.

Runs one or more pipeline configurations over the same frames and compares
them frame by frame, with side-by-side overlay clips. A configuration is a
detect engine, a pose engine, the pose input size, the pose crop size and a
crop mode:

  detect  the crop for frame N comes from the detector run on frame N
          (today's pipeline);
  track   the crop for frame N comes from the pose of frame N-1 (centroid of
          the keypoints, plus constant-velocity prediction with --velocity);
          the detector still runs on every frame as the trailing check, and
          re-acquires the crop on the first frame, after a loss (no pose,
          pose confidence below --min-pose-conf, a keypoint within
          --edge-margin px of the crop edge, or the detector's box centre
          further than --reacquire-px from the tracked centre) and on the
          watchdog cadence (--watchdog-every frames).

Everything mirrors the pipeline: detector preprocessing from
calibrate_tensorrt_int8.LetterboxSampler, crop origin = int(centre) - crop/2
clamped to the frame (src/detect_roi.cu), pose preprocessing = the same
letterbox from the crop to the pose input (identity when equal), pose decode
= best candidate by channel 4, keypoints at channels 5 + 3k mapped back to
crop pixels (src/pose_worker.cpp decode_best_pose_from).

Frame sources: a directory of Mono8 PGMs with a manifest.json (from
extract_preenc_ref_frames.py or the calibration extractor), or a full-frame
mp4 with a recording-frame range.

Usage:
  replay_pose_pipeline.py --frames <dir> [--camera 2010096] --out <dir> \
      --config fp16_256=<detect.engine>:<pose.engine>:256:256:detect \
      --config int8_192=<detect_int8.engine>:<pose192.engine>:192:192:detect \
      --config track_384=<detect.engine>:<pose.engine>:256:384:track
  replay_pose_pipeline.py --video <Cam.mp4> --video-range 5300-5700 --camera 2010096 ...
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from calibrate_tensorrt_int8 import LetterboxSampler, read_pgm, _cudart, cuda_check  # noqa: E402
from compare_tensorrt_engines import Engine  # noqa: E402
from overlay_pose_clips import ClipWriter, contact_sheet, find_ffmpeg, write_slow_copy, KEYPOINT_COLOURS  # noqa: E402

import tensorrt as trt  # noqa: E402

DEFAULT_LABELS = ["bladder", "eye_left", "eye_right"]


@dataclass
class Config:
    name: str
    detect_engine: Path
    pose_engine: Path
    pose_input: int
    pose_crop: int
    mode: str = "detect"


@dataclass
class TrackState:
    valid: bool = False
    centre: tuple[float, float] | None = None
    prev_centre: tuple[float, float] | None = None
    frames_since_acquire: int = 0
    reacquires: int = 0
    reasons: dict = field(default_factory=dict)


def parse_config(text: str) -> Config:
    name, _, rest = text.partition("=")
    parts = rest.split(":")
    if len(parts) < 4:
        raise SystemExit(f"--config {text!r}: expected name=detect:pose:pose_input:pose_crop[:mode]")
    mode = parts[4] if len(parts) > 4 else "detect"
    if mode not in ("detect", "track"):
        raise SystemExit(f"--config {name}: mode must be detect or track")
    return Config(name, Path(parts[0]), Path(parts[1]), int(parts[2]), int(parts[3]), mode)


class Stage:
    """Detector + pose engines with the pipeline's arithmetic around them."""

    def __init__(self, cfg: Config, logger: trt.Logger, src_w: int, src_h: int, labels: list[str],
                 pose_conf: float, kpt_conf: float):
        self.cfg = cfg
        self.detect = Engine(cfg.detect_engine, logger)
        self.pose = Engine(cfg.pose_engine, logger)
        self.det_sampler = LetterboxSampler(src_w, src_h, 640)
        self.det_scale = src_w / self.det_sampler.scaled_w
        self.pose_sampler = LetterboxSampler(cfg.pose_crop, cfg.pose_crop, cfg.pose_input)
        self.src_w, self.src_h = src_w, src_h
        self.labels = labels
        self.pose_conf, self.kpt_conf = pose_conf, kpt_conf
        out_name = next(n for n in self.pose.buffers if n != self.pose.input_name)
        shape = self.pose.buffers[out_name][1].shape  # 1 x C x N
        self.pose_out = out_name
        self.channels, self.candidates = int(shape[1]), int(shape[2])
        self.kpt_count = (self.channels - 5) // 3
        if self.kpt_count != len(labels):
            print(f"[replay] {cfg.name}: engine has {self.kpt_count} keypoints, labels give {len(labels)}; "
                  f"using generic labels", file=sys.stderr)
            self.labels = [f"kp{k}" for k in range(self.kpt_count)]

    def run_detect(self, mono: np.ndarray):
        out = self.detect.run(self.det_sampler(mono))
        n = int(out["num_dets"].reshape(-1)[0])
        if n <= 0:
            return None
        score = float(out["scores"].reshape(-1)[0])
        b = out["bboxes"].reshape(-1, 4)[0].astype(float)
        # letterbox inverse: 640-space -> source pixels (pad is 0 for a square source)
        x0 = (b[0] - self.det_sampler.pad_left) * self.det_scale
        y0 = (b[1] - self.det_sampler.pad_top) * self.det_scale
        x1 = (b[2] - self.det_sampler.pad_left) * self.det_scale
        y1 = (b[3] - self.det_sampler.pad_top) * self.det_scale
        return {"score": score, "box": [x0, y0, x1 - x0, y1 - y0], "centre": ((x0 + x1) / 2, (y0 + y1) / 2)}

    def crop_origin(self, centre: tuple[float, float]) -> tuple[int, int]:
        s = self.cfg.pose_crop
        hi_x = max(self.src_w - s, 0)
        hi_y = max(self.src_h - s, 0)
        return (min(max(int(centre[0]) - s // 2, 0), hi_x), min(max(int(centre[1]) - s // 2, 0), hi_y))

    def run_pose(self, mono: np.ndarray, origin: tuple[int, int]):
        s = self.cfg.pose_crop
        crop = np.zeros((s, s), dtype=np.uint8)
        y0, x0 = origin[1], origin[0]
        src = mono[y0:y0 + s, x0:x0 + s]
        crop[:src.shape[0], :src.shape[1]] = src
        out = self.pose.run(self.pose_sampler(crop))[self.pose_out].reshape(self.channels, self.candidates)
        scores = out[4]
        best = int(np.argmax(scores))
        if not np.isfinite(scores[best]) or scores[best] <= self.pose_conf:
            return None, crop
        ratio = min(self.cfg.pose_input / s, self.cfg.pose_input / s)
        dw = (self.cfg.pose_input - s * ratio) * 0.5
        dh = (self.cfg.pose_input - s * ratio) * 0.5
        kps = []
        for k in range(self.kpt_count):
            base = 5 + 3 * k
            mx, my, kc = float(out[base, best]), float(out[base + 1, best]), float(out[base + 2, best])
            x = min(max((mx - dw) / max(1.0, ratio), 0.0), float(s))
            y = min(max((my - dh) / max(1.0, ratio), 0.0), float(s))
            kps.append({"label": self.labels[k], "x": x, "y": y, "conf": min(max(kc, 0.0), 1.0),
                        "visible": kc >= self.kpt_conf})
        return {"conf": float(min(max(scores[best], 0.0), 1.0)), "keypoints": kps}, crop


def load_frames_dir(path: Path, camera: str | None):
    manifest = json.loads((path / "manifest.json").read_text()) if (path / "manifest.json").exists() else None
    if manifest:
        rows = [f for f in manifest["frames"] if not camera or f.get("camera") == camera]
        rows.sort(key=lambda f: (f.get("camera", ""), f.get("recording_frame_id", 0)))
        for f in rows:
            yield f.get("camera", ""), int(f.get("recording_frame_id", 0)), read_pgm(path / f["file"])
    else:
        for p in sorted(path.glob("*.pgm")):
            cam = p.name[3:10] if p.name.startswith("Cam") else ""
            if camera and cam != camera:
                continue
            rec = int(p.stem.split("rec")[-1]) if "rec" in p.stem else 0
            yield cam, rec, read_pgm(p)


def load_video_range(path: Path, camera: str, start: int, end: int):
    import cv2
    cap = cv2.VideoCapture(str(path))
    if not cap.isOpened():
        raise SystemExit(f"cannot open {path}")
    cap.set(cv2.CAP_PROP_POS_FRAMES, max(0, start - 1))
    idx = int(cap.get(cv2.CAP_PROP_POS_FRAMES))
    while idx < end:
        ok, f = cap.read()
        if not ok:
            break
        idx += 1
        if idx >= start:
            yield camera, idx, (f[:, :, 0] if f.ndim == 3 else f)
    cap.release()


def kp_full(kps, origin):
    return {k["label"]: (origin[0] + k["x"], origin[1] + k["y"], k["conf"]) for k in kps}


def draw_panel(crop: np.ndarray, pose, cfg_name: str, text: str, scale: int) -> np.ndarray:
    import cv2
    img = cv2.cvtColor(cv2.resize(crop, None, fx=scale, fy=scale, interpolation=cv2.INTER_NEAREST), cv2.COLOR_GRAY2BGR)
    if pose:
        pts = {k["label"]: (k["x"] * scale, k["y"] * scale, k["conf"]) for k in pose["keypoints"]}
        if "eye_left" in pts and "eye_right" in pts:
            cv2.line(img, (int(pts["eye_left"][0]), int(pts["eye_left"][1])), (int(pts["eye_right"][0]), int(pts["eye_right"][1])), (255, 255, 255), 1, cv2.LINE_AA)
            if "bladder" in pts:
                mx = (pts["eye_left"][0] + pts["eye_right"][0]) / 2
                my = (pts["eye_left"][1] + pts["eye_right"][1]) / 2
                cv2.line(img, (int(mx), int(my)), (int(pts["bladder"][0]), int(pts["bladder"][1])), (255, 255, 255), 1, cv2.LINE_AA)
        for label, (x, y, c) in pts.items():
            col = KEYPOINT_COLOURS.get(label, (200, 200, 200))
            r = max(2, int(round(3 + 4 * c)))
            cv2.circle(img, (int(round(x)), int(round(y))), r, col, -1, cv2.LINE_AA)
            cv2.circle(img, (int(round(x)), int(round(y))), r, (0, 0, 0), 1, cv2.LINE_AA)
    else:
        cv2.putText(img, "NO POSE", (12, 36), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 0, 255), 2, cv2.LINE_AA)
    strip = np.zeros((44, img.shape[1], 3), dtype=np.uint8)
    cv2.putText(strip, cfg_name, (8, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.putText(strip, text, (8, 38), cv2.FONT_HERSHEY_SIMPLEX, 0.42, (200, 200, 200), 1, cv2.LINE_AA)
    return np.vstack([img, strip])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", help="directory of PGMs (+ manifest.json)")
    ap.add_argument("--video", help="full-frame mp4 instead of --frames")
    ap.add_argument("--video-range", help="start-end recording frame ids for --video")
    ap.add_argument("--camera", help="camera serial filter (required with --video)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--config", action="append", required=True, help="name=detect:pose:pose_input:pose_crop[:mode]")
    ap.add_argument("--device", type=int, default=5)
    ap.add_argument("--labels", default=",".join(DEFAULT_LABELS))
    ap.add_argument("--pose-conf", type=float, default=0.25)
    ap.add_argument("--kpt-conf", type=float, default=0.25)
    ap.add_argument("--velocity", action="store_true", help="track mode: constant-velocity prediction")
    ap.add_argument("--watchdog-every", type=int, default=25)
    ap.add_argument("--edge-margin", type=float, default=16.0)
    ap.add_argument("--min-pose-conf", type=float, default=0.5)
    ap.add_argument("--reacquire-px", type=float, default=96.0)
    ap.add_argument("--clip", action="store_true", help="write side-by-side overlay clips per camera")
    ap.add_argument("--panel-px", type=int, default=512)
    ap.add_argument("--fps", type=float, default=100.0)
    ap.add_argument("--slow-fps", default="25,10")
    ap.add_argument("--max-frames", type=int, default=0)
    ap.add_argument("--every", type=int, default=1, help="run the engines on every Nth frame (statistics runs; not for track mode)")
    args = ap.parse_args()

    cfgs = [parse_config(c) for c in args.config]
    labels = [s.strip() for s in args.labels.split(",") if s.strip()]
    cuda_check(_cudart.cudaSetDevice(args.device), "cudaSetDevice")
    logger = trt.Logger(trt.Logger.ERROR)
    trt.init_libnvinfer_plugins(logger, "")

    if args.video:
        if not args.camera or not args.video_range:
            raise SystemExit("--video needs --camera and --video-range start-end")
        a, b = args.video_range.split("-")
        frames = load_video_range(Path(args.video), args.camera, int(a), int(b))
    elif args.frames:
        frames = load_frames_dir(Path(args.frames), args.camera)
    else:
        raise SystemExit("give --frames or --video")

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stages: dict[str, Stage] = {}
    states: dict[tuple[str, str], TrackState] = {}
    writers = {c.name: (out_dir / f"{c.name}.jsonl").open("w") for c in cfgs}
    rows_by_cfg: dict[str, list] = {c.name: [] for c in cfgs}
    ffmpeg = find_ffmpeg() if args.clip else None
    slow = [float(v) for v in args.slow_fps.split(",") if v.strip()]
    clip_writer = None
    clip_cam = None
    sheet = []
    n = 0
    seen = 0
    if args.every > 1 and any(c.mode == "track" for c in cfgs):
        raise SystemExit("--every needs consecutive frames for track mode; drop --every or the track config")
    for cam, rec, mono in frames:
        seen += 1
        if args.every > 1 and (seen - 1) % args.every:
            continue
        if args.max_frames and n >= args.max_frames:
            break
        n += 1
        if not stages:
            for c in cfgs:
                stages[c.name] = Stage(c, logger, mono.shape[1], mono.shape[0], labels, args.pose_conf, args.kpt_conf)
        panels = []
        for c in cfgs:
            st = stages[c.name]
            det = st.run_detect(mono)
            row = {"camera": cam, "recording_frame_id": rec, "config": c.name, "mode": c.mode,
                   "det_score": det["score"] if det else 0.0, "det_box": det["box"] if det else None}
            origin = None
            source = "model"
            if c.mode == "track":
                key = (c.name, cam)
                ts = states.setdefault(key, TrackState())
                reason = None
                if not ts.valid:
                    reason = "init"
                elif ts.frames_since_acquire >= args.watchdog_every:
                    reason = "watchdog"
                elif det and ts.centre and math.hypot(det["centre"][0] - ts.centre[0], det["centre"][1] - ts.centre[1]) > args.reacquire_px:
                    reason = "detector_disagrees"
                if reason:
                    if det:
                        origin = st.crop_origin(det["centre"])
                        ts.valid = True
                        ts.frames_since_acquire = 0
                        ts.reacquires += 1
                        ts.reasons[reason] = ts.reasons.get(reason, 0) + 1
                        ts.prev_centre = None
                    else:
                        ts.valid = False
                else:
                    pred = ts.centre
                    if args.velocity and ts.prev_centre:
                        pred = (2 * ts.centre[0] - ts.prev_centre[0], 2 * ts.centre[1] - ts.prev_centre[1])
                    origin = st.crop_origin(pred)
                    source = "tracked"
                row["reacquire_reason"] = reason
                row["tracked_vs_detected_px"] = (math.hypot(det["centre"][0] - ts.centre[0], det["centre"][1] - ts.centre[1])
                                                 if det and ts.centre else None)
            else:
                if det:
                    origin = st.crop_origin(det["centre"])
            pose, crop = (st.run_pose(mono, origin) if origin else (None, np.zeros((c.pose_crop, c.pose_crop), np.uint8)))
            row["crop_origin"] = origin
            row["crop_source"] = source if origin else None
            row["pose_conf"] = pose["conf"] if pose else None
            row["keypoints_full"] = ({k: [x, y, cf] for k, (x, y, cf) in kp_full(pose["keypoints"], origin).items()} if pose else None)
            if c.mode == "track":
                ts = states[(c.name, cam)]
                if pose:
                    xs = [k["x"] for k in pose["keypoints"]]
                    ys = [k["y"] for k in pose["keypoints"]]
                    near_edge = (min(xs) < args.edge_margin or min(ys) < args.edge_margin or
                                 max(xs) > c.pose_crop - args.edge_margin or max(ys) > c.pose_crop - args.edge_margin)
                    weak = pose["conf"] < args.min_pose_conf
                    row["near_edge"], row["weak"] = near_edge, weak
                    if near_edge or weak:
                        ts.valid = False  # next frame re-acquires from the detector
                        ts.reasons["edge" if near_edge else "weak"] = ts.reasons.get("edge" if near_edge else "weak", 0) + 1
                    else:
                        centroid = (origin[0] + sum(xs) / len(xs), origin[1] + sum(ys) / len(ys))
                        ts.prev_centre, ts.centre = ts.centre, centroid
                        ts.frames_since_acquire += 1
                else:
                    ts.valid = False
                    ts.reasons["no_pose"] = ts.reasons.get("no_pose", 0) + 1
            writers[c.name].write(json.dumps(row) + "\n")
            rows_by_cfg[c.name].append(row)
            if args.clip:
                txt = f"rec {rec} det {row['det_score']:.2f} pose {(pose['conf'] if pose else 0):.2f} {row['crop_source'] or '-'}"
                panels.append(draw_panel(crop, pose, c.name, txt, max(1, args.panel_px // c.pose_crop)))
        if args.clip and panels:
            h = max(p.shape[0] for p in panels)
            panels = [np.pad(p, ((0, h - p.shape[0]), (0, 0), (0, 0))) for p in panels]
            frame = np.hstack(panels)
            if clip_writer is None or clip_cam != cam:
                if clip_writer:
                    clip_writer.close()
                    for sf in slow:
                        write_slow_copy(ffmpeg, out_dir / f"replay_Cam{clip_cam}.mp4", out_dir / f"replay_Cam{clip_cam}_slow{sf:g}fps.mp4", args.fps, sf)
                    contact_sheet(sheet, out_dir / f"replay_Cam{clip_cam}_sheet.png")
                clip_cam, sheet = cam, []
                clip_writer = ClipWriter(out_dir / f"replay_Cam{cam}.mp4", args.fps, (frame.shape[1], frame.shape[0]), ffmpeg)
            clip_writer.write(frame)
            if len(sheet) < 20 and n % 5 == 0:
                sheet.append(frame)
    if clip_writer:
        clip_writer.close()
        for sf in slow:
            write_slow_copy(ffmpeg, out_dir / f"replay_Cam{clip_cam}.mp4", out_dir / f"replay_Cam{clip_cam}_slow{sf:g}fps.mp4", args.fps, sf)
        contact_sheet(sheet, out_dir / f"replay_Cam{clip_cam}_sheet.png")
    for w in writers.values():
        w.close()

    # Pairwise comparison against the first configuration.
    ref = cfgs[0].name
    summary = {"frames": n, "reference": ref, "configs": {}, "pairs": {}}
    for c in cfgs:
        rows = rows_by_cfg[c.name]
        s = {"frames": len(rows), "with_detection": sum(1 for r in rows if r["det_box"]), "with_pose": sum(1 for r in rows if r["pose_conf"] is not None),
             "pose_conf_mean": float(np.mean([r["pose_conf"] for r in rows if r["pose_conf"] is not None])) if any(r["pose_conf"] is not None for r in rows) else None}
        if c.mode == "track":
            st = [v for (name, _cam), v in states.items() if name == c.name]
            s["reacquires"] = sum(v.reacquires for v in st)
            s["reacquire_reasons"] = {k: sum(v.reasons.get(k, 0) for v in st) for k in set().union(*(v.reasons for v in st))} if st else {}
            s["tracked_frames"] = sum(1 for r in rows if r.get("crop_source") == "tracked")
            offs = [r["tracked_vs_detected_px"] for r in rows if r.get("tracked_vs_detected_px") is not None]
            s["tracked_vs_detected_px_p50"] = float(np.median(offs)) if offs else None
            s["tracked_vs_detected_px_p99"] = float(np.quantile(offs, 0.99)) if offs else None
            s["near_edge_frames"] = sum(1 for r in rows if r.get("near_edge"))
        summary["configs"][c.name] = s
        if c.name == ref:
            continue
        a = {(r["camera"], r["recording_frame_id"]): r for r in rows_by_cfg[ref]}
        per_label: dict[str, list] = {}
        conf_d = []
        missing = 0
        for r in rows:
            ra = a.get((r["camera"], r["recording_frame_id"]))
            if not ra or not ra["keypoints_full"] or not r["keypoints_full"]:
                if ra and ra["keypoints_full"] and not r["keypoints_full"]:
                    missing += 1
                continue
            conf_d.append(r["pose_conf"] - ra["pose_conf"])
            for label, (x, y, _c) in r["keypoints_full"].items():
                if label in ra["keypoints_full"]:
                    xa, ya, _ = ra["keypoints_full"][label]
                    per_label.setdefault(label, []).append(math.hypot(x - xa, y - ya))
        pair = {"frames_compared": len(conf_d), "pose_missing_vs_reference": missing,
                "pose_conf_delta_mean": float(np.mean(conf_d)) if conf_d else None,
                "keypoint_px": {label: {"median": float(np.median(v)), "p95": float(np.quantile(v, 0.95)), "max": float(np.max(v))}
                                for label, v in per_label.items()}}
        summary["pairs"][f"{c.name} vs {ref}"] = pair
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
