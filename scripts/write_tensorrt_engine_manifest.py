#!/usr/bin/env python3
"""Write Orange TensorRT engine runtime/build manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def file_artifact(path: Path, kind: str) -> dict[str, Any]:
    out: dict[str, Any] = {
        "kind": kind,
        "path": str(path),
        "exists": path.exists(),
    }
    if path.exists():
        out["bytes"] = path.stat().st_size
        out["sha256"] = sha256(path)
    return out


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"expected JSON object in {path}")
    return data


def read_text_if_exists(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def first_match(text: str, pattern: str) -> str | None:
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        return None
    return match.group(1).strip()


def first_float(text: str, pattern: str) -> float | None:
    value = first_match(text, pattern)
    if value is None:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def first_int(text: str, pattern: str) -> int | None:
    value = first_match(text, pattern)
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def parse_stats_line(text: str, label: str) -> dict[str, float] | None:
    pattern = (
        rf"{re.escape(label)}:\s+min = (?P<min>[0-9.eE+-]+) ms,"
        rf"\s+max = (?P<max>[0-9.eE+-]+) ms,"
        rf"\s+mean = (?P<mean>[0-9.eE+-]+) ms,"
        rf"\s+median = (?P<median>[0-9.eE+-]+) ms,"
        rf"\s+percentile\(90%\) = (?P<p90>[0-9.eE+-]+) ms,"
        rf"\s+percentile\(95%\) = (?P<p95>[0-9.eE+-]+) ms,"
        rf"\s+percentile\(99%\) = (?P<p99>[0-9.eE+-]+) ms"
    )
    match = re.search(pattern, text)
    if not match:
        return None
    return {key: float(value) for key, value in match.groupdict().items()}


def collect_warnings(text: str) -> list[str]:
    warnings: list[str] = []
    for line in text.splitlines():
        if " [W] " in line or " WARNING" in line or "[W]" in line:
            cleaned = re.sub(r"^\[[^\]]+\]\s*", "", line).strip()
            warnings.append(cleaned)
    return warnings[:50]


def build_command(args: argparse.Namespace, staged_engine: Path) -> list[str]:
    command = [
        str(args.trtexec),
        f"--device={args.device}",
        f"--onnx={args.source_onnx}",
        f"--saveEngine={staged_engine}",
    ]
    if args.precision == "fp16":
        command.append("--fp16")
    elif args.precision == "int8":
        command.append("--int8")
    command.extend(
        [
            f"--builderOptimizationLevel={args.builder_optimization_level}",
            f"--avgTiming={args.avg_timing}",
            f"--profilingVerbosity={args.profiling_verbosity}",
            f"--exportTimes={args.build_times_json}",
            f"--exportProfile={args.profile_json}",
            f"--exportLayerInfo={args.layer_info_json}",
        ]
    )
    return command


def benchmark_command(args: argparse.Namespace, staged_engine: Path) -> list[str]:
    return [
        str(args.trtexec),
        f"--device={args.device}",
        f"--loadEngine={staged_engine}",
        f"--duration={args.benchmark_duration}",
        f"--warmUp={args.benchmark_warmup_ms}",
        f"--exportTimes={args.benchmark_times_json}",
    ]


def write_readme(args: argparse.Namespace, engine_stem: str, runtime_engine: Path) -> None:
    readme = f"""# TensorRT Build Staging: {args.run_id}

This directory stores non-runtime build inputs, TensorRT logs, profile exports,
and benchmark outputs for an Orange TensorRT detect engine build.

Runtime artifacts intentionally live in `{args.output_engine_dir}`:

- `{runtime_engine.name}`
- `{args.runtime_manifest.name}`

The runtime manifest points back to this staging directory for source ONNX and
build provenance paths.

Optional note: TensorRT may not emit `{engine_stem}_trtexec_times.json` during
profiled builds. When present, it is listed in `SHA256SUMS`. The standalone
benchmark timing export is `{engine_stem}_benchmark_times.json`.
"""
    (args.staging_dir / "README.md").write_text(readme, encoding="utf-8")


def write_checksums(args: argparse.Namespace, paths: list[Path]) -> None:
    with (args.staging_dir / "SHA256SUMS").open("w", encoding="utf-8") as handle:
        for path in paths:
            if path.exists():
                handle.write(f"{sha256(path)}  {path.name}\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Write Orange TensorRT runtime/build manifests."
    )
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--build-stamp", required=True)
    parser.add_argument("--source-onnx", type=Path, required=True)
    parser.add_argument("--source-onnx-manifest", type=Path, required=True)
    parser.add_argument("--source-input-onnx", type=Path)
    parser.add_argument("--source-input-onnx-manifest", type=Path)
    parser.add_argument("--staged-engine", type=Path, required=True)
    parser.add_argument("--runtime-engine", type=Path, required=True)
    parser.add_argument("--runtime-manifest", type=Path, required=True)
    parser.add_argument("--build-manifest", type=Path, required=True)
    parser.add_argument("--staging-dir", type=Path, required=True)
    parser.add_argument("--output-engine-dir", type=Path, required=True)
    parser.add_argument("--trtexec", type=Path, required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--target-hardware-class", required=True)
    parser.add_argument("--deployment-runtime", default="orange")
    parser.add_argument("--precision", choices=["fp16", "int8"], required=True)
    parser.add_argument("--trt-tag", required=True)
    parser.add_argument("--builder-optimization-level", type=int, required=True)
    parser.add_argument("--avg-timing", type=int, required=True)
    parser.add_argument("--profiling-verbosity", required=True)
    parser.add_argument("--build-log", type=Path, required=True)
    parser.add_argument("--profile-json", type=Path, required=True)
    parser.add_argument("--layer-info-json", type=Path, required=True)
    parser.add_argument("--build-times-json", type=Path, required=True)
    parser.add_argument("--benchmark-log", type=Path, required=True)
    parser.add_argument("--benchmark-times-json", type=Path, required=True)
    parser.add_argument("--benchmark-duration", type=int, required=True)
    parser.add_argument("--benchmark-warmup-ms", type=int, required=True)
    parser.add_argument("--palette-registry", default="/nvme1/palette_registry.sqlite")
    parser.add_argument("--status", default="candidate")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    now = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    source_input_onnx = args.source_input_onnx or args.source_onnx
    source_input_onnx_manifest = args.source_input_onnx_manifest or args.source_onnx_manifest
    onnx_manifest = read_json(source_input_onnx_manifest)
    build_text = read_text_if_exists(args.build_log)
    benchmark_text = read_text_if_exists(args.benchmark_log)

    engine_stem = f"{args.run_id}_{args.build_id}"
    build_cmd = build_command(args, args.staged_engine)
    bench_cmd = benchmark_command(args, args.staged_engine)

    selected_gpu = {
        "device_id": first_int(build_text, r"Selected Device ID:\s+([0-9]+)") or int(args.device),
        "name": first_match(build_text, r"Selected Device:\s+(.+)"),
        "uuid": first_match(build_text, r"Selected Device UUID:\s+(.+)"),
        "compute_capability": first_match(build_text, r"Compute Capability:\s+(.+)"),
        "sm_count": first_int(build_text, r"SMs:\s+([0-9]+)"),
        "device_global_memory_mib_trtexec": first_int(
            build_text, r"Device Global Memory:\s+([0-9]+)\s+MiB"
        ),
    }
    selected_gpu = {key: value for key, value in selected_gpu.items() if value is not None}

    benchmark = {
        "status": "passed" if "&&&& PASSED" in benchmark_text else "unknown",
        "tool": "trtexec",
        "command": bench_cmd,
        "duration_s": args.benchmark_duration,
        "warmup_ms": args.benchmark_warmup_ms,
        "throughput_qps": first_float(benchmark_text, r"Throughput:\s+([0-9.eE+-]+)\s+qps"),
        "latency_ms": parse_stats_line(benchmark_text, "Latency"),
        "enqueue_ms": parse_stats_line(benchmark_text, "Enqueue Time"),
        "h2d_ms": parse_stats_line(benchmark_text, "H2D Latency"),
        "gpu_compute_ms": parse_stats_line(benchmark_text, "GPU Compute Time"),
        "d2h_ms": parse_stats_line(benchmark_text, "D2H Latency"),
        "total_host_walltime_s": first_float(
            benchmark_text, r"Total Host Walltime:\s+([0-9.eE+-]+)\s+s"
        ),
        "total_gpu_compute_time_s": first_float(
            benchmark_text, r"Total GPU Compute Time:\s+([0-9.eE+-]+)\s+s"
        ),
        "log_path": str(args.benchmark_log),
        "times_json_path": str(args.benchmark_times_json),
    }
    benchmark = {key: value for key, value in benchmark.items() if value is not None}

    artifacts = [
        file_artifact(args.build_log, "trtexec_build_log"),
        file_artifact(args.profile_json, "trtexec_profile_json"),
        file_artifact(args.layer_info_json, "trtexec_layer_info_json"),
        file_artifact(args.build_times_json, "trtexec_profile_times_json"),
        file_artifact(args.benchmark_log, "trtexec_benchmark_log"),
        file_artifact(args.benchmark_times_json, "trtexec_benchmark_times_json"),
    ]

    common = {
        "schema_version": 1,
        "created_at_utc": now,
        "updated_at_utc": now,
        "status": args.status,
        "deployment_runtime": args.deployment_runtime,
        "target_hardware_class": args.target_hardware_class,
        "palette_run_id": args.run_id,
        "build_id": args.build_id,
        "build_stamp": args.build_stamp,
        "source": {
            "onnx": {
                "path": str(source_input_onnx),
                "sha256": sha256(source_input_onnx),
                "palette_manifest_sha256": onnx_manifest.get("onnx", {}).get("sha256"),
            },
            "onnx_manifest": {
                "path": str(source_input_onnx_manifest),
                "sha256": sha256(source_input_onnx_manifest),
            },
            "weights": {
                "source_path": onnx_manifest.get("weights", {}).get("path"),
                "sha256": onnx_manifest.get("weights", {}).get("sha256"),
            },
            "training_manifest": onnx_manifest.get("source_manifest", {}),
        },
        "engine": {
            "path": str(args.runtime_engine),
            "sha256": sha256(args.runtime_engine),
            "bytes": args.runtime_engine.stat().st_size,
            "format": "TensorRT serialized engine",
            "portable": False,
        },
        "precision": args.precision,
        "input_contract": {
            "input_name": "images",
            "dtype": "FP32",
            "shape": onnx_manifest.get("export", {}).get("input_shape", [1, 3, 640, 640]),
            "layout": "NCHW",
            "preprocessing": [
                "Mono/luma source frame",
                "letterbox resize to model input size",
                "padding value 114",
                "divide by 255.0",
                "replicate luma into B, G, R planes",
                "planar NCHW FP32 tensor",
            ],
        },
        "outputs": onnx_manifest.get("onnx", {}).get("outputs", []),
        "nms": onnx_manifest.get("export", {}).get("nms", {}),
        "plugins": onnx_manifest.get("onnx", {}).get("plugin_contract", {}),
        "build": {
            "tool": "trtexec",
            "trtexec_path": str(args.trtexec),
            "tensorrt_version": first_match(build_text, r"TensorRT version:\s+(.+)"),
            "tensorrt_log_version": first_match(build_text, r"TensorRT\.trtexec \[TensorRT ([^\]]+)\]"),
            "selected_gpu": selected_gpu,
            "precision": args.precision,
            "builder_optimization_level": args.builder_optimization_level,
            "avg_timing": args.avg_timing,
            "profiling_verbosity": args.profiling_verbosity,
            "build_duration_s": first_float(build_text, r"Engine built in\s+([0-9.eE+-]+)\s+sec"),
            "created_engine_size_mib": first_float(
                build_text, r"Created engine with size:\s+([0-9.eE+-]+)\s+MiB"
            ),
            "command": build_cmd,
            "log_path": str(args.build_log),
            "warnings": collect_warnings(build_text),
        },
        "standalone_benchmark": benchmark,
        "artifacts": artifacts,
        "build_staging": {
            "root": str(args.staging_dir),
            "build_manifest_path": str(args.build_manifest),
            "staged_engine_path": str(args.staged_engine),
            "source_snapshot": {
                "onnx": {
                    "path": str(args.source_onnx),
                    "sha256": sha256(args.source_onnx),
                    "bytes": args.source_onnx.stat().st_size,
                },
                "onnx_manifest": {
                    "path": str(args.source_onnx_manifest),
                    "sha256": sha256(args.source_onnx_manifest),
                    "bytes": args.source_onnx_manifest.stat().st_size,
                },
            },
        },
        "orange_app_validation": {
            "status": "pending",
            "notes": "Standalone TensorRT build/benchmark complete. Run Orange camera validation before promoting to preferred.",
        },
        "palette_registration": {
            "candidate_command": [
                "scripts/py",
                "-m",
                "fisheye.utils.register_model_deployment_artifact",
                "--registry",
                args.palette_registry,
                "--run-id",
                args.run_id,
                "--manifest-path",
                str(args.runtime_manifest),
                "--deployment-runtime",
                args.deployment_runtime,
                "--target-hardware-class",
                args.target_hardware_class,
                "--status",
                args.status,
                "--apply",
            ],
            "promotion_policy": "Do not mark preferred until Orange validation passes and an explicit default-engine replacement decision is made.",
        },
    }

    build_manifest = dict(common)
    build_manifest["schema_id"] = "orange.tensorrt_engine_build_manifest"
    build_manifest["engine"] = dict(common["engine"])
    build_manifest["engine"]["staged_path"] = str(args.staged_engine)
    args.build_manifest.write_text(json.dumps(build_manifest, indent=2) + "\n", encoding="utf-8")

    runtime_manifest = dict(common)
    runtime_manifest["schema_id"] = "orange.tensorrt_engine_manifest"
    runtime_manifest["build_staging"]["build_manifest_sha256"] = sha256(args.build_manifest)
    args.runtime_manifest.write_text(json.dumps(runtime_manifest, indent=2) + "\n", encoding="utf-8")

    write_readme(args, engine_stem, args.runtime_engine)
    checksum_paths = [
        args.staging_dir / "README.md",
        args.source_onnx,
        args.source_onnx_manifest,
        args.staged_engine,
        args.build_log,
        args.profile_json,
        args.layer_info_json,
        args.build_times_json,
        args.benchmark_log,
        args.benchmark_times_json,
        args.build_manifest,
    ]
    write_checksums(args, checksum_paths)

    print(args.runtime_manifest)
    print(sha256(args.runtime_manifest))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
