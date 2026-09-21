#!/usr/bin/env python3
"""Write the runtime manifest for a pose TensorRT engine built by direct trtexec.

Follows the field vocabulary of scripts/write_tensorrt_engine_manifest.py but
takes the pose identity contract from pose_model_input_contract.json instead of
hardcoding detect letterbox preprocessing.
"""
import hashlib, json, re, subprocess, sys, datetime
from pathlib import Path

def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()

def first(text, pat, cast=str):
    m = re.search(pat, text)
    return cast(m.group(1)) if m else None

def pct_block(text, label):
    # e.g. "GPU Compute Time: min = 0.5 ms, max = 0.9 ms, mean = 0.6 ms, median = 0.6 ms, percentile(90%) = ..."
    m = re.search(rf"{re.escape(label)}: (.+)", text)
    if not m:
        return None
    out = {}
    for k, v in re.findall(r"([a-z0-9()%.]+) = ([0-9.eE+-]+) ms", m.group(1)):
        out[k] = float(v)
    return out

bundle = Path(sys.argv[1]); run_id = sys.argv[2]; build_id = sys.argv[3]; repo = Path(sys.argv[4])
eng_dir = bundle / "engines"
onnx = bundle / f"{run_id}.onnx"
canon = bundle / f"{run_id}.canonical.manifest.json"
contract = bundle / "pose_model_input_contract.json"
engine = eng_dir / f"{run_id}_{build_id}.engine"
build_log = eng_dir / "build_a16_gpu5_trt100_fp16.log"
bench_log = eng_dir / "benchmark_a16_gpu5_trt100_fp16.log"
bench_graph_log = eng_dir / "benchmark_a16_gpu5_trt100_fp16_cudagraph.log"

bt = build_log.read_text(); nt = bench_log.read_text()
gt = bench_graph_log.read_text() if bench_graph_log.exists() else ""
c = json.load(open(contract))["payload"]
prof = c["runtime_profiles"][0]
cmd = first(bt, r"&&&& RUNNING TensorRT\.trtexec \[TensorRT v[0-9]+\] # (.+)")

bindings = []
for m in re.finditer(r"\[I\] (Input|Output) binding for (\S+) with dimensions ([0-9x]+) is created", bt):
    bindings.append({"role": m.group(1).lower(), "name": m.group(2), "dtype": "FP32", "shape": [int(x) for x in m.group(3).split("x")]})

def git(*a):
    return subprocess.run(["git", "-C", str(repo), *a], capture_output=True, text=True).stdout.strip()

nvsmi = subprocess.run(["nvidia-smi", "--query-gpu=index,uuid,driver_version,compute_cap", "--format=csv,noheader"], capture_output=True, text=True).stdout
gpu5 = [l for l in nvsmi.splitlines() if l.startswith("5,")]
now = datetime.datetime.now(datetime.timezone.utc).isoformat()

man = {
    "schema_id": "orange.tensorrt_engine_manifest",
    "schema_version": 1,
    "task": "pose",
    "created_at_utc": now,
    "status": "candidate",
    "selector_activation": False,
    "deployment_runtime": "orange",
    "target_hardware_class": "A16",
    "palette_target": {"runtime": "orange", "hardware_class": "A16", "note": "distinct from the portable ONNX registration"},
    "palette_run_id": run_id,
    "set_id": c["model"]["set_id"],
    "build_id": build_id,
    "source": {
        "onnx": {"path": str(onnx), "sha256": sha256(onnx)},
        "canonical_manifest": {"path": str(canon), "sha256": sha256(canon)},
        "input_contract": {"path": str(contract), "sha256": sha256(contract), "payload_digest": json.load(open(contract)).get("payload_digest")},
        "source_export_manifest": {"present": False, "expected_sha256": json.load(open(canon))["payload"]["source_export_manifest"]["sha256"], "note": "gap 7: not delivered in the bundle"},
        "weights": c["model"]["weights"],
    },
    "engine": {"path": str(engine), "sha256": sha256(engine), "bytes": engine.stat().st_size, "format": "TensorRT serialized engine", "portable": False},
    "precision": "fp16",
    "bindings": bindings,
    "input_contract": {
        "input_name": "images", "dtype": "FP32", "layout": "NCHW",
        "shape": [1, 3, *prof["network_shape_hw"]],
        "profile_id": prof["profile_id"],
        "native_shape_hw": prof["native_shape_hw"],
        "submitted_to_network": prof["submitted_to_network"],
        "native_to_submitted": prof["native_to_submitted"],
        "preprocessing": [
            "mono/luma crop, native 192x192, identity (no resize, no letterbox, padding value 0)",
            "uint8 divide by 255.0 to float32",
            "replicate luma into three channels",
            "planar NCHW FP32 tensor",
        ],
        "preprocessing_probe_digest": prof["submitted_to_network"].get("preprocessing_probe", {}),
        "orange_reproduced_probe": None,
    },
    "keypoints": c.get("keypoints") or c.get("keypoint_schema") or prof.get("keypoints"),
    "build": {
        "tool": "trtexec", "trtexec_path": "/usr/local/TensorRT-10.0.1.6/bin/trtexec",
        "tensorrt_version": first(bt, r"TensorRT version:\s+(.+)"),
        "cuda_toolkit": "12.2", "driver_version": gpu5[0].split(", ")[2] if gpu5 else None,
        "selected_device": first(bt, r"Selected Device:\s+(.+)"),
        "selected_device_id": first(bt, r"Selected Device ID:\s+(\d+)", int),
        "selected_device_uuid": first(bt, r"Selected Device UUID:\s+(\S+)"),
        "compute_capability": first(bt, r"Compute Capability:\s+(\S+)") or (gpu5[0].split(", ")[3] if gpu5 else None),
        "builder_optimization_level": 5, "avg_timing": 32, "profiling_verbosity": "detailed",
        "timing_cache": str(eng_dir / "timing_a16_trt100.cache"),
        "build_duration_s": first(bt, r"Engine built in\s+([0-9.eE+-]+)\s+sec", float),
        "created_engine_size_mib": first(bt, r"Created engine with size:\s+([0-9.eE+-]+)\s+MiB", float),
        "command": cmd, "log_path": str(build_log),
        "warnings": sorted(set(w.strip() for w in re.findall(r"\[W\] (.+)", bt) if w.strip()))[:40],
    },
    "standalone_benchmark": {
        "plain": {"log_path": str(bench_log), "command": first(nt, r"# (.+)"), "throughput_qps": first(nt, r"Throughput:\s+([0-9.eE+-]+)", float),
                  "latency_ms": pct_block(nt, "Latency"), "gpu_compute_ms": pct_block(nt, "GPU Compute Time"), "enqueue_ms": pct_block(nt, "Enqueue Time")},
        "cuda_graph": ({"log_path": str(bench_graph_log), "command": first(gt, r"# (.+)"), "throughput_qps": first(gt, r"Throughput:\s+([0-9.eE+-]+)", float),
                        "latency_ms": pct_block(gt, "Latency"), "gpu_compute_ms": pct_block(gt, "GPU Compute Time"), "enqueue_ms": pct_block(gt, "Enqueue Time")} if gt else None),
    },
    "parity": {"status": "pending", "tolerance": "unowned", "note": "probe-digest harness not yet run (inspection report claim 3)"},
    "orange_repo": {"path": str(repo), "branch": git("rev-parse", "--abbrev-ref", "HEAD"), "commit": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain"))},
    "artifacts": sorted(p.name for p in eng_dir.iterdir()),
}
out = eng_dir / f"{run_id}_{build_id}.manifest.json"
out.write_text(json.dumps(man, indent=2) + "\n")
print(out)
