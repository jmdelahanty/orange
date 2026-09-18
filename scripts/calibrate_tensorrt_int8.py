#!/usr/bin/env python3
"""Produce a TensorRT INT8 calibration cache for the detect model.

Runs the model's exact production preprocessing on a set of full-frame
Mono8 PGMs and feeds them to TensorRT's entropy calibrator (v2) while
building a throwaway engine at the lowest optimisation level. The result is
a calibration cache that ``scripts/build_tensorrt_detect_engine.sh
--precision int8 --calib-cache <file>`` hands to trtexec for the production
build, plus a JSON record of exactly which frames calibrated it.

Preprocessing mirrors ``src/optimized_yolo_preprocess.cu`` (mono path, no
circle mask): letterbox to 640x640 with pad value 114, bilinear sampling at
``src = dst * (src_size / scaled_size)`` with no half-pixel offset, divide by
255, luma replicated into three planes, NCHW FP32.

Environment: the juicebox env (numpy, cv2) with the TensorRT 10.0.1 wheel
installed and ``LD_LIBRARY_PATH`` including the TensorRT and CUDA libs;
cudart is used through ctypes so no pycuda is needed.

Usage:
  calibrate_tensorrt_int8.py --onnx <source.onnx> --frames <dir with *.pgm>
      --cache <out.cache> [--device 5] [--max-frames 1000] [--record <out.json>]
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

try:
    import tensorrt as trt
except ImportError as exc:  # pragma: no cover
    raise SystemExit("tensorrt python bindings missing; install the wheel from /usr/local/TensorRT-10.0.1.6/python") from exc

INPUT_SIZE = 640
PAD_VALUE = 114.0

_cudart = ctypes.CDLL("libcudart.so.12")
_cudart.cudaMalloc.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_size_t]
_cudart.cudaMemcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
_cudart.cudaFree.argtypes = [ctypes.c_void_p]
_cudart.cudaSetDevice.argtypes = [ctypes.c_int]
CUDA_MEMCPY_H2D = 1


def cuda_check(err: int, what: str) -> None:
    if err != 0:
        raise RuntimeError(f"{what} failed with cudaError {err}")


def read_pgm(path: Path) -> np.ndarray:
    """Binary PGM (P5), 8-bit, as written by cv2.imwrite."""
    with path.open("rb") as fh:
        magic = fh.readline().strip()
        if magic != b"P5":
            raise ValueError(f"{path}: not a binary PGM")
        dims = []
        while len(dims) < 3:
            line = fh.readline()
            if line.startswith(b"#"):
                continue
            dims += [int(v) for v in line.split()]
        width, height, maxval = dims[:3]
        if maxval != 255:
            raise ValueError(f"{path}: expected 8-bit, got maxval {maxval}")
        data = np.frombuffer(fh.read(width * height), dtype=np.uint8)
    return data.reshape(height, width)


class LetterboxSampler:
    """Precomputed bilinear gather matching the CUDA preprocess kernel."""

    def __init__(self, src_w: int, src_h: int, dst: int = INPUT_SIZE):
        scale = min(dst / src_w, dst / src_h)
        scaled_w = int(src_w * scale)
        scaled_h = int(src_h * scale)
        self.pad_left = (dst - scaled_w) // 2
        self.pad_top = (dst - scaled_h) // 2
        scale_x = src_w / scaled_w
        scale_y = src_h / scaled_h
        self.dst = dst
        self.scaled_w, self.scaled_h = scaled_w, scaled_h
        xs = np.arange(scaled_w, dtype=np.float32) * np.float32(scale_x)
        ys = np.arange(scaled_h, dtype=np.float32) * np.float32(scale_y)
        x0 = np.floor(xs).astype(np.int64)
        y0 = np.floor(ys).astype(np.int64)
        self.wx = (xs - x0).astype(np.float32)
        self.wy = (ys - y0).astype(np.float32)
        self.x1 = np.minimum(x0 + 1, src_w - 1)
        self.y1 = np.minimum(y0 + 1, src_h - 1)
        self.x0 = np.clip(x0, 0, src_w - 1)
        self.y0 = np.clip(y0, 0, src_h - 1)

    def __call__(self, mono: np.ndarray) -> np.ndarray:
        """Returns the 1x3x640x640 FP32 tensor."""
        src = mono.astype(np.float32)
        p00 = src[np.ix_(self.y0, self.x0)]
        p01 = src[np.ix_(self.y0, self.x1)]
        p10 = src[np.ix_(self.y1, self.x0)]
        p11 = src[np.ix_(self.y1, self.x1)]
        wx = self.wx[None, :]
        wy = self.wy[:, None]
        top = p00 * (1 - wx) + p01 * wx
        bot = p10 * (1 - wx) + p11 * wx
        sampled = top * (1 - wy) + bot * wy
        out = np.full((self.dst, self.dst), PAD_VALUE, dtype=np.float32)
        out[self.pad_top:self.pad_top + self.scaled_h, self.pad_left:self.pad_left + self.scaled_w] = sampled
        out /= 255.0
        return np.ascontiguousarray(np.broadcast_to(out[None, None], (1, 3, self.dst, self.dst)))


class PgmEntropyCalibrator(trt.IInt8EntropyCalibrator2):
    def __init__(self, frames: list[Path], cache_path: Path, input_name: str):
        super().__init__()
        self.frames = frames
        self.cache_path = cache_path
        self.input_name = input_name
        self.index = 0
        self.sampler: LetterboxSampler | None = None
        self.nbytes = 3 * INPUT_SIZE * INPUT_SIZE * 4
        self.device_ptr = ctypes.c_void_p()
        cuda_check(_cudart.cudaMalloc(ctypes.byref(self.device_ptr), self.nbytes), "cudaMalloc(calibration batch)")
        self.used: list[str] = []

    def get_batch_size(self) -> int:
        return 1

    def get_batch(self, names):
        if self.index >= len(self.frames):
            return None
        path = self.frames[self.index]
        self.index += 1
        mono = read_pgm(path)
        if self.sampler is None:
            self.sampler = LetterboxSampler(mono.shape[1], mono.shape[0])
        tensor = self.sampler(mono)
        cuda_check(_cudart.cudaMemcpy(self.device_ptr, tensor.ctypes.data, self.nbytes, CUDA_MEMCPY_H2D),
                   "cudaMemcpy(calibration batch)")
        self.used.append(path.name)
        if self.index % 50 == 0:
            print(f"[calib] {self.index}/{len(self.frames)} frames", file=sys.stderr, flush=True)
        return [int(self.device_ptr.value)]

    def read_calibration_cache(self):
        return None  # always calibrate from frames; never reuse a stale cache silently

    def write_calibration_cache(self, cache):
        self.cache_path.write_bytes(cache)

    def free(self):
        if self.device_ptr.value:
            _cudart.cudaFree(self.device_ptr)
            self.device_ptr = ctypes.c_void_p()


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--frames", required=True, help="directory of Mono8 PGM frames")
    ap.add_argument("--cache", required=True, help="output calibration cache")
    ap.add_argument("--record", help="output JSON record (default <cache>.json)")
    ap.add_argument("--device", type=int, default=5)
    ap.add_argument("--max-frames", type=int, default=1000)
    ap.add_argument("--workspace-gb", type=float, default=4.0)
    args = ap.parse_args()

    frames = sorted(Path(args.frames).glob("*.pgm"))[: args.max_frames]
    if not frames:
        raise SystemExit(f"no *.pgm under {args.frames}")
    cuda_check(_cudart.cudaSetDevice(args.device), "cudaSetDevice")

    logger = trt.Logger(trt.Logger.INFO)
    trt.init_libnvinfer_plugins(logger, "")
    builder = trt.Builder(logger)
    network = builder.create_network(1 << int(trt.NetworkDefinitionCreationFlag.EXPLICIT_BATCH))
    parser = trt.OnnxParser(network, logger)
    onnx_path = Path(args.onnx)
    if not parser.parse(onnx_path.read_bytes()):
        for i in range(parser.num_errors):
            print(parser.get_error(i), file=sys.stderr)
        raise SystemExit("ONNX parse failed")
    input_name = network.get_input(0).name
    shape = tuple(network.get_input(0).shape)
    if shape[-2:] != (INPUT_SIZE, INPUT_SIZE):
        raise SystemExit(f"unexpected input shape {shape}")

    config = builder.create_builder_config()
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, int(args.workspace_gb * (1 << 30)))
    config.set_flag(trt.BuilderFlag.FP16)
    config.set_flag(trt.BuilderFlag.INT8)
    config.builder_optimization_level = 0  # the engine is discarded; only the cache matters
    cache_path = Path(args.cache)
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    calibrator = PgmEntropyCalibrator(frames, cache_path, input_name)
    config.int8_calibrator = calibrator

    start = time.time()
    print(f"[calib] calibrating on {len(frames)} frames from {args.frames} (device {args.device})", file=sys.stderr)
    serialized = builder.build_serialized_network(network, config)
    calibrator.free()
    if serialized is None or not cache_path.exists():
        raise SystemExit("calibration build failed; no cache written")
    elapsed = time.time() - start

    record = {
        "schema_id": "orange.tensorrt_int8_calibration",
        "schema_version": 1,
        "created_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "onnx": {"path": str(onnx_path.resolve()), "sha256": sha256(onnx_path)},
        "tensorrt_version": trt.__version__,
        "device": args.device,
        "calibrator": "IInt8EntropyCalibrator2",
        "batch_size": 1,
        "input_name": input_name,
        "preprocessing": ["Mono/luma source frame", "letterbox resize to 640x640 (bilinear, src = dst * scale, no half-pixel offset)",
                          "padding value 114", "divide by 255.0", "replicate luma into B, G, R planes", "planar NCHW FP32 tensor"],
        "frames_dir": str(Path(args.frames).resolve()),
        "frames_used": calibrator.used,
        "frame_count": len(calibrator.used),
        "cache": {"path": str(cache_path.resolve()), "sha256": sha256(cache_path), "bytes": cache_path.stat().st_size},
        "calibration_seconds": round(elapsed, 1),
    }
    record_path = Path(args.record) if args.record else cache_path.with_suffix(cache_path.suffix + ".json")
    record_path.write_text(json.dumps(record, indent=2))
    print(f"[calib] cache {cache_path} ({record['cache']['bytes']} bytes) from {len(calibrator.used)} frames in {elapsed:.0f} s; record {record_path}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
