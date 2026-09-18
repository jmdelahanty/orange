#!/usr/bin/env python3
"""Parity check between two detect engines (for example FP16 vs INT8) on held-out frames.

Runs both engines on the same preprocessed inputs (the production
preprocessing from calibrate_tensorrt_int8.py) and compares the
EfficientNMS outputs frame by frame: detection present or not, box IoU,
box centre offset in full-frame pixels, and confidence delta. Prints a
summary and writes a JSON report with per-frame rows.

Gates (defaults, override with flags): no missed or extra detections at the
score threshold, median IoU >= 0.90, |confidence delta| p95 <= 0.05.

Usage:
  compare_tensorrt_engines.py --reference <fp16.engine> --candidate <int8.engine>
      --frames <dir with *.pgm> [--device 5] [--report out.json]
"""

from __future__ import annotations

import argparse
import ctypes
import json
import sys
import time
from pathlib import Path

import numpy as np

import tensorrt as trt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from calibrate_tensorrt_int8 import INPUT_SIZE, LetterboxSampler, read_pgm, _cudart, cuda_check  # noqa: E402

CUDA_MEMCPY_H2D = 1
CUDA_MEMCPY_D2H = 2
_cudart.cudaDeviceSynchronize.argtypes = []


class Engine:
    def __init__(self, path: Path, logger: trt.Logger):
        runtime = trt.Runtime(logger)
        self.engine = runtime.deserialize_cuda_engine(path.read_bytes())
        if self.engine is None:
            raise SystemExit(f"cannot load {path}")
        self.context = self.engine.create_execution_context()
        self.buffers: dict[str, tuple[ctypes.c_void_p, np.ndarray]] = {}
        for i in range(self.engine.num_io_tensors):
            name = self.engine.get_tensor_name(i)
            shape = tuple(self.engine.get_tensor_shape(name))
            dtype = trt.nptype(self.engine.get_tensor_dtype(name))
            host = np.zeros(shape, dtype=dtype)
            dev = ctypes.c_void_p()
            cuda_check(_cudart.cudaMalloc(ctypes.byref(dev), host.nbytes), f"cudaMalloc({name})")
            self.buffers[name] = (dev, host)
            self.context.set_tensor_address(name, int(dev.value))
        self.input_name = next(self.engine.get_tensor_name(i) for i in range(self.engine.num_io_tensors)
                               if self.engine.get_tensor_mode(self.engine.get_tensor_name(i)) == trt.TensorIOMode.INPUT)

    def run(self, tensor: np.ndarray) -> dict[str, np.ndarray]:
        dev, host = self.buffers[self.input_name]
        cuda_check(_cudart.cudaMemcpy(dev, tensor.ctypes.data, tensor.nbytes, CUDA_MEMCPY_H2D), "cudaMemcpy(input)")
        if not self.context.execute_async_v3(0):
            raise RuntimeError("execute_async_v3 failed")
        _cudart.cudaDeviceSynchronize()
        out = {}
        for name, (d, h) in self.buffers.items():
            if name == self.input_name:
                continue
            cuda_check(_cudart.cudaMemcpy(h.ctypes.data, d, h.nbytes, CUDA_MEMCPY_D2H), f"cudaMemcpy({name})")
            out[name] = h.copy()
        return out


def iou(a, b) -> float:
    ax0, ay0, ax1, ay1 = a
    bx0, by0, bx1, by1 = b
    iw = max(0.0, min(ax1, bx1) - max(ax0, bx0))
    ih = max(0.0, min(ay1, by1) - max(ay0, by0))
    inter = iw * ih
    union = (ax1 - ax0) * (ay1 - ay0) + (bx1 - bx0) * (by1 - by0) - inter
    return inter / union if union > 0 else 0.0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--frames", required=True)
    ap.add_argument("--device", type=int, default=5)
    ap.add_argument("--report")
    ap.add_argument("--score-threshold", type=float, default=0.3)
    ap.add_argument("--min-median-iou", type=float, default=0.90)
    ap.add_argument("--max-conf-delta-p95", type=float, default=0.05)
    ap.add_argument("--max-frames", type=int, default=100000)
    args = ap.parse_args()

    frames = sorted(Path(args.frames).glob("*.pgm"))[: args.max_frames]
    if not frames:
        raise SystemExit(f"no *.pgm under {args.frames}")
    cuda_check(_cudart.cudaSetDevice(args.device), "cudaSetDevice")
    logger = trt.Logger(trt.Logger.WARNING)
    trt.init_libnvinfer_plugins(logger, "")
    ref = Engine(Path(args.reference), logger)
    cand = Engine(Path(args.candidate), logger)

    sampler = None
    rows = []
    t0 = time.time()
    for path in frames:
        mono = read_pgm(path)
        if sampler is None:
            sampler = LetterboxSampler(mono.shape[1], mono.shape[0])
            scale_back = mono.shape[1] / sampler.scaled_w  # 640-space -> full-frame pixels
        tensor = sampler(mono)
        a = ref.run(tensor)
        b = cand.run(tensor)
        na = int(a["num_dets"].reshape(-1)[0])
        nb = int(b["num_dets"].reshape(-1)[0])
        sa = float(a["scores"].reshape(-1)[0]) if na else 0.0
        sb = float(b["scores"].reshape(-1)[0]) if nb else 0.0
        da = na > 0 and sa >= args.score_threshold
        db = nb > 0 and sb >= args.score_threshold
        row = {"frame": path.name, "ref_det": da, "cand_det": db, "ref_score": sa, "cand_score": sb}
        if da and db:
            ba = a["bboxes"].reshape(-1, 4)[0].astype(float)
            bb = b["bboxes"].reshape(-1, 4)[0].astype(float)
            row["iou"] = iou(ba, bb)
            ca = ((ba[0] + ba[2]) / 2, (ba[1] + ba[3]) / 2)
            cb = ((bb[0] + bb[2]) / 2, (bb[1] + bb[3]) / 2)
            row["centre_offset_px"] = float(np.hypot(ca[0] - cb[0], ca[1] - cb[1]) * scale_back)
            row["conf_delta"] = sb - sa
        rows.append(row)
    elapsed = time.time() - t0

    both = [r for r in rows if r["ref_det"] and r["cand_det"]]
    missed = [r["frame"] for r in rows if r["ref_det"] and not r["cand_det"]]
    extra = [r["frame"] for r in rows if r["cand_det"] and not r["ref_det"]]
    ious = np.array([r["iou"] for r in both]) if both else np.array([])
    offs = np.array([r["centre_offset_px"] for r in both]) if both else np.array([])
    deltas = np.array([abs(r["conf_delta"]) for r in both]) if both else np.array([])
    summary = {
        "frames": len(rows),
        "ref_detections": sum(r["ref_det"] for r in rows),
        "cand_detections": sum(r["cand_det"] for r in rows),
        "missed_by_candidate": missed,
        "extra_in_candidate": extra,
        "iou_median": float(np.median(ious)) if len(ious) else None,
        "iou_p05": float(np.quantile(ious, 0.05)) if len(ious) else None,
        "iou_min": float(ious.min()) if len(ious) else None,
        "centre_offset_px_median": float(np.median(offs)) if len(offs) else None,
        "centre_offset_px_p95": float(np.quantile(offs, 0.95)) if len(offs) else None,
        "conf_delta_abs_p95": float(np.quantile(deltas, 0.95)) if len(deltas) else None,
        "conf_delta_mean_signed": float(np.mean([r["conf_delta"] for r in both])) if both else None,
        "seconds": round(elapsed, 1),
    }
    passed = (not missed and not extra and summary["iou_median"] is not None
              and summary["iou_median"] >= args.min_median_iou
              and summary["conf_delta_abs_p95"] <= args.max_conf_delta_p95)
    summary["pass"] = bool(passed)
    summary["gates"] = {"score_threshold": args.score_threshold, "min_median_iou": args.min_median_iou,
                        "max_conf_delta_p95": args.max_conf_delta_p95}
    print(json.dumps(summary, indent=2))
    if args.report:
        Path(args.report).write_text(json.dumps({"reference": args.reference, "candidate": args.candidate,
                                                 "summary": summary, "rows": rows}, indent=1))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
