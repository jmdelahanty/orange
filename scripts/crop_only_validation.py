"""Runner adapter to Orange's shared read-only crop evidence verifier."""
from __future__ import annotations
import json
from pathlib import Path
import subprocess

PRODUCT = "registered_context_and_moving_crops"


def verify_collection(folder, verifier, worker_cpu, cameras, *, run=subprocess.run):
    if worker_cpu is None or worker_cpu < 0:
        raise ValueError("crop-only verification requires an explicit housekeeping --crop-validator-cpu")
    result = run([str(verifier), str(folder), "--worker-cpu", str(worker_cpu)],
                 capture_output=True, text=True, timeout=300, check=False)
    if result.returncode:
        raise ValueError(f"crop-only evidence verifier failed: {result.stdout[-2000:]} {result.stderr[-2000:]}")
    report = json.loads(result.stdout)
    if (report.get("schema_id") != "orange.recording.crop_only_validation" or
            type(report.get("schema_version")) is not int or report["schema_version"] != 1 or
            report.get("status") != "pass" or
            report.get("recording_folder") != str(Path(folder).resolve())):
        raise ValueError("crop-only verifier returned an invalid or wrong-recording report")
    parent = report["recording_session"]
    if parent.get("media_product_mode") != PRODUCT or parent.get("status") != "completed":
        raise ValueError("crop-only parent did not complete")
    if sorted(parent.get("cameras", [])) != sorted(cameras):
        raise ValueError("crop-only logical camera membership differs from expected cameras")
    return report
