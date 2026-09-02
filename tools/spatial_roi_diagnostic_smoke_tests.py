#!/usr/bin/env python3
"""Non-hardware checks for the 2010096 spatial-ROI diagnostic smoke."""

from __future__ import annotations

import json
import hashlib
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SPEC_PATH = REPO_ROOT / (
    "experiment_specs/2010096_spatial_roi_diagnostic_plumbing_100fps_gpu5_v1.json"
)
RUNNER_PATH = REPO_ROOT / "scripts/run_spatial_roi_diagnostic_2010096.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_spec_contract() -> None:
    spec = json.loads(SPEC_PATH.read_text(encoding="utf-8"))
    fixed = spec["fixed"]
    require(spec["selection"] == {"camera_serials": ["2010096"], "gpu_ids": [5]},
            "selection drifted from the one-camera/GPU-5 diagnostic contract")
    require(fixed["stream_only"] is False and
            fixed["recording_sink_mode"] == "external_ipc",
            "the first combined full-frame plus ROI product must use external_ipc")
    external_contract = fixed["external_recorder_contract"]
    require(external_contract["mode"] == "diagnostic_ipc_v1" and
            external_contract["supervise_processes"] is True,
            "combined ROI recording must use a supervised external full-frame contract")
    require(external_contract["recorder_tool_path"] ==
            "targets/release/external_recorder_ipc_probe",
            "diagnostic must bind the repo-relative external recorder binary")
    recorder_tool = REPO_ROOT / external_contract["recorder_tool_path"]
    require(recorder_tool.is_file() and not recorder_tool.is_symlink(),
            "external recorder release binary is missing or symlinked")
    external_stream = external_contract["streams"]["2010096"]
    require(external_stream["routing_policy"] == "gop_modulo" and
            external_stream["expected_shard_gpu_ids"] == [5, 6],
            "full-frame recording must use the 2010096 split-GOP pair")
    require(fixed["display"] is False and fixed["yolo"] is False,
            "display and YOLO must remain disabled")
    require(fixed["yolo_worker"] is False and fixed["pose_worker"] is False,
            "YOLO/pose workers must remain disabled")
    roi_camera = fixed["spatial_roi_recording"]["cameras"]["2010096"]
    require(roi_camera["camera_id"] == 3,
            "current four-camera rig mapping requires runtime camera_id 3")
    expected = [
        ("quadrant_top_left", 0, 0),
        ("quadrant_top_right", 2256, 0),
        ("quadrant_bottom_left", 0, 2256),
        ("quadrant_bottom_right", 2256, 2256),
    ]
    require(len(roi_camera["rois"]) == 4, "diagnostic spec must have four ROIs")
    for roi, (roi_id, x, y) in zip(roi_camera["rois"], expected):
        require(roi["roi_id"] == roi_id, "quadrant order changed")
        require(roi["content_rect"] ==
                {"x": x, "y": y, "width": 2256, "height": 2256},
                f"{roi_id} geometry changed")


def test_authority_artifact_contract() -> None:
    spec = json.loads(SPEC_PATH.read_text(encoding="utf-8"))
    camera = spec["fixed"]["spatial_roi_recording"]["cameras"]["2010096"]
    authority_index = spec["diagnostic_authority"]
    require(authority_index["closed"] is True,
            "diagnostic authority index must be closed")
    require(authority_index["status"] == "diagnostic_not_physical_acceptance",
            "diagnostic authority index must not claim physical acceptance")
    require(authority_index["camera_serial"] == "2010096" and
            authority_index["runtime_camera_id"] == 3 and
            authority_index["runtime_mapping"] ==
            "normal_four_camera_rig_inventory_2010093_to_2010096_maps_0_to_3" and
            authority_index["coordinate_space"] == "camera_native_0_indexed" and
            authority_index["native_raster"] == {"width": 4512, "height": 4512},
            "diagnostic authority index camera-native identity mismatch")
    for role in ("layout", "materialization", "registration"):
        descriptor = authority_index["artifacts"][role]
        require(descriptor["id"] == camera[role]["id"] and
                descriptor["sha256"] == camera[role]["sha256"],
                f"{role} authority reference mismatch")
        relative_path = Path(descriptor["path"])
        require(not relative_path.is_absolute(),
                f"{role} authority path must be repo-relative")
        artifact_path = REPO_ROOT / relative_path
        require(artifact_path.is_file() and not artifact_path.is_symlink(),
                f"{role} authority path must be a checked-in regular file")
        artifact_path = artifact_path.resolve()
        try:
            artifact_path.relative_to(REPO_ROOT.resolve())
        except ValueError as exc:
            raise AssertionError(f"{role} authority path escapes repository") from exc
        artifact_bytes = artifact_path.read_bytes()
        require(len(artifact_bytes) == descriptor["size_bytes"],
                f"{role} authority size does not match exact bytes")
        require("sha256:" + hashlib.sha256(artifact_bytes).hexdigest() ==
                descriptor["sha256"],
                f"{role} authority sha256 does not match exact bytes")
        artifact = json.loads(artifact_bytes.decode("utf-8"))
        require(artifact["authority_id"] == descriptor["id"] and
                artifact["schema_version"] == 1 and artifact["closed"] is True and
                artifact["status"] == "diagnostic_not_physical_acceptance" and
                artifact["camera_serial"] == "2010096" and
                artifact["coordinate_space"] == "camera_native_0_indexed" and
                artifact["native_raster"] == {"width": 4512, "height": 4512},
                f"{role} authority metadata is not closed camera-native diagnostic metadata")


def test_runner_shell_and_help() -> None:
    syntax = subprocess.run(
        ["bash", "-n", str(RUNNER_PATH)],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    require(syntax.returncode == 0, f"runner shell syntax failed: {syntax.stderr}")
    help_result = subprocess.run(
        [str(RUNNER_PATH), "--help"],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    require(help_result.returncode == 0, "runner --help failed")
    require("--execute" in help_result.stdout, "runner must expose explicit execute opt-in")
    require("Default: validate/print only" in help_result.stdout,
            "runner help must document dry-run default")


def test_default_run_is_validation_only() -> None:
    result = subprocess.run(
        [str(RUNNER_PATH)],
        cwd=Path("/tmp"),
        text=True,
        capture_output=True,
        check=False,
    )
    require(result.returncode == 0, f"default runner failed: {result.stderr}")
    require("dry-run: validation/print only" in result.stdout,
            "default runner did not identify itself as dry-run")
    require("[EXPERIMENT VALIDATION] status=pass" in result.stdout,
            "orange_client did not validate the diagnostic spec")
    require("no hardware or media execution" in result.stdout,
            "default runner did not state the hardware/media guard")


def main() -> int:
    tests = [
        test_spec_contract,
        test_authority_artifact_contract,
        test_runner_shell_and_help,
        test_default_run_is_validation_only,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("spatial_roi_diagnostic_smoke_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
