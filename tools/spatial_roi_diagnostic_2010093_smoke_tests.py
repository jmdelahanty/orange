#!/usr/bin/env python3
"""Non-hardware checks for the separately versioned 2010093 ROI diagnostic."""

from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SPEC_PATH = REPO_ROOT / (
    "experiment_specs/2010093_spatial_roi_diagnostic_plumbing_100fps_gpu3_v1.json"
)
RUNNER_PATH = REPO_ROOT / "scripts/run_spatial_roi_diagnostic_2010093.sh"
EXISTING_2010096_SPEC_PATH = REPO_ROOT / (
    "experiment_specs/2010096_spatial_roi_diagnostic_plumbing_100fps_gpu5_v1.json"
)

EXPECTED_AUTHORITIES = {
    "layout": {
        "id": "2010093_diagnostic_layout_v1",
        "path": "experiment_specs/spatial_roi_diagnostic_authority/2010093_diagnostic_layout_v1.json",
        "size_bytes": 1113,
        "sha256": "sha256:3c426ab2d4be238dca72b2d96f8dbbcc0de585acdcb5ebef4b1f182f71339656",
        "schema_id": "orange.spatial_roi_diagnostic_authority.layout",
    },
    "materialization": {
        "id": "2010093_diagnostic_materialization_v1",
        "path": "experiment_specs/spatial_roi_diagnostic_authority/2010093_diagnostic_materialization_v1.json",
        "size_bytes": 1651,
        "sha256": "sha256:5fa379643059c60edc111ea0b89b00cb7ac10dff4f04cee8c17a2763e2ce1b83",
        "schema_id": "orange.spatial_roi_diagnostic_authority.materialization",
    },
    "registration": {
        "id": "2010093_diagnostic_registration_v1",
        "path": "experiment_specs/spatial_roi_diagnostic_authority/2010093_diagnostic_registration_v1.json",
        "size_bytes": 811,
        "sha256": "sha256:cef85a0d4f51a4ef6b0a64fbf5e849f5947475bf11aa586a10f8553b6228e227",
        "schema_id": "orange.spatial_roi_diagnostic_authority.registration",
    },
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_spec_contract() -> None:
    spec = json.loads(SPEC_PATH.read_text(encoding="utf-8"))
    fixed = spec["fixed"]
    require(spec["experiment_id"] ==
            "2010093_spatial_roi_diagnostic_plumbing_100fps_gpu3_v1",
            "experiment identity drifted")
    require(spec["selection"] == {"camera_serials": ["2010093"], "gpu_ids": [3]},
            "selection drifted from the one-camera/source-GPU-3 contract")
    require(fixed["config_folder"] ==
            "config/validated_split_gop_hevc_100fps_gop25_fourcam_a16",
            "2010093 must use the validated four-camera config folder")
    require(fixed["stream_only"] is False and
            fixed["recording_sink_mode"] == "external_ipc",
            "combined full-frame plus ROI recording must use external_ipc")
    require(fixed["display"] is False and fixed["yolo"] is False and
            fixed["yolo_worker"] is False and fixed["pose_worker"] is False,
            "diagnostic display/inference workers must remain disabled")

    external_contract = fixed["external_recorder_contract"]
    require(external_contract["mode"] == "diagnostic_ipc_v1" and
            external_contract["supervise_processes"] is True,
            "combined recording must use a supervised external full-frame contract")
    require(external_contract["recorder_tool_path"] ==
            "targets/release/external_recorder_ipc_probe",
            "diagnostic must bind the repo-relative external recorder binary")
    recorder_tool = REPO_ROOT / external_contract["recorder_tool_path"]
    require(recorder_tool.is_file() and not recorder_tool.is_symlink(),
            "external recorder release binary is missing or symlinked")
    require(external_contract["artifact_root"] ==
            "/tmp/orange_external_recorder_spatial_roi_2010093" and
            external_contract["session_id"] == spec["experiment_id"],
            "external recorder root/session is not unique to 2010093")
    require(list(external_contract["streams"]) == ["2010093"],
            "external contract must have exactly one 2010093 full-frame stream")
    external_stream = external_contract["streams"]["2010093"]
    require(external_stream["analytics_gpu_id"] == 3 and
            external_stream["recorder_gpu_id"] == 3 and
            external_stream["routing_policy"] == "gop_modulo" and
            external_stream["expected_shard_gpu_ids"] == [3, 4],
            "full-frame recording must use the 2010093 split-GOP pair [3,4]")

    roi_config = fixed["spatial_roi_recording"]
    require(roi_config["schema_id"] == "orange.spatial_roi_recording.config" and
            roi_config["schema_version"] == 3 and roi_config["strict"] is True and
            roi_config["backend"] == "independent_hevc_external_ipc",
            "ROI config must remain the strict independent-HEVC v3 product")
    require(roi_config["encode_profile"] == {
                "name": "hevc_p1_low_latency_vbr_q20_gop25_v1",
                "codec": "hevc",
                "preset": "p1",
                "tuning": "ll",
                "lossless": False,
                "rate_control_mode": "vbr",
                "quality_value": 20,
                "gop_length": 25,
                "aq": False,
                "temporal_aq": False,
                "lookahead": False,
                "lookahead_depth": 0,
            }, "ROI P1/VBR-Q20/GOP-25 profile drifted")
    require(list(roi_config["cameras"]) == ["2010093"],
            "ROI config must have exactly one 2010093 camera")
    roi_camera = roi_config["cameras"]["2010093"]
    require(roi_camera["camera_id"] == 0 and
            roi_camera["camera_serial"] == "2010093",
            "2010093 must bind to runtime/Shaman camera_id 0")
    require(roi_camera["native_raster"] == {"width": 4512, "height": 4512} and
            roi_camera["source_frame_rate"] == 100,
            "2010093 native raster/cadence drifted")
    expected = [
        ("quadrant_top_left", "diagnostic_top_left", 0, 0),
        ("quadrant_top_right", "diagnostic_top_right", 2256, 0),
        ("quadrant_bottom_left", "diagnostic_bottom_left", 0, 2256),
        ("quadrant_bottom_right", "diagnostic_bottom_right", 2256, 2256),
    ]
    require(len(roi_camera["rois"]) == 4, "diagnostic spec must have four ROIs")
    for roi, (roi_id, region_id, x, y) in zip(roi_camera["rois"], expected):
        require(roi == {
                    "roi_id": roi_id,
                    "region_id": region_id,
                    "required": True,
                    "content_rect": {"x": x, "y": y, "width": 2256, "height": 2256},
                    "logical_stream_id": f"2010093_spatial_roi_{roi_id}",
                    "artifact_stem": f"Cam2010093_spatial_roi_{roi_id}",
                }, f"{roi_id} contract changed")
    require(fixed["spatial_roi_recorder_runtime"] == {
                "schema_id": "orange.spatial_roi_recording.recorder_runtime",
                "schema_version": 1,
                "mode": "explicit_per_stream",
                "recorder_gpu_by_logical_stream_id": {
                    "2010093_spatial_roi_quadrant_top_left": 1,
                    "2010093_spatial_roi_quadrant_top_right": 2,
                    "2010093_spatial_roi_quadrant_bottom_left": 1,
                    "2010093_spatial_roi_quadrant_bottom_right": 2,
                },
            }, "ROI recorder GPU placement drifted")


def test_camera_config_contract() -> None:
    path = REPO_ROOT / (
        "config/validated_split_gop_hevc_100fps_gop25_fourcam_a16/2010093.json"
    )
    config = json.loads(path.read_text(encoding="utf-8"))
    require(config["schema_id"] == "orange.camera.config" and
            config["schema_version"] == 3 and
            config["device_serial_number"] == "2010093",
            "camera config identity drifted")
    require(config["width"] == 4512 and config["height"] == 4512 and
            config["frame_rate"] == 100 and config["pixel_format"] == "Mono8" and
            config["source_gpu_id"] == 3 and config["gpu_direct"] is True,
            "camera raster/cadence/source GPU contract drifted")
    recording = config["recording"]
    require(recording["mode"] == "split_gop" and
            recording["split_gop"]["encoder_gpu_ids"] == [3, 4] and
            recording["split_gop"]["strict"] is True,
            "camera config no longer provides strict split-GOP [3,4]")


def test_authority_artifact_contract() -> None:
    spec = json.loads(SPEC_PATH.read_text(encoding="utf-8"))
    camera = spec["fixed"]["spatial_roi_recording"]["cameras"]["2010093"]
    authority_index = spec["diagnostic_authority"]
    require(authority_index["closed"] is True and
            authority_index["status"] == "diagnostic_not_physical_acceptance" and
            authority_index["camera_serial"] == "2010093" and
            authority_index["runtime_camera_id"] == 0 and
            authority_index["coordinate_space"] == "camera_native_0_indexed" and
            authority_index["native_raster"] == {"width": 4512, "height": 4512},
            "diagnostic authority index identity drifted")
    require(set(authority_index["artifacts"]) == set(EXPECTED_AUTHORITIES),
            "authority role set drifted")
    for role, expected in EXPECTED_AUTHORITIES.items():
        descriptor = authority_index["artifacts"][role]
        require(descriptor == {
                    key: expected[key] for key in ("id", "path", "size_bytes", "sha256")
                }, f"{role} authority descriptor drifted")
        require(camera[role] == {
                    "id": expected["id"], "sha256": expected["sha256"]
                }, f"{role} authority reference mismatch")
        relative_path = Path(expected["path"])
        require(not relative_path.is_absolute(),
                f"{role} authority path must be repository-relative")
        artifact_path = REPO_ROOT / relative_path
        require(artifact_path.is_file() and not artifact_path.is_symlink(),
                f"{role} authority must be a checked-in regular file")
        resolved = artifact_path.resolve()
        try:
            resolved.relative_to(REPO_ROOT.resolve())
        except ValueError as exc:
            raise AssertionError(f"{role} authority path escapes repository") from exc
        artifact_bytes = resolved.read_bytes()
        require(len(artifact_bytes) == expected["size_bytes"],
                f"{role} authority exact size drifted")
        require("sha256:" + hashlib.sha256(artifact_bytes).hexdigest() ==
                expected["sha256"], f"{role} authority exact digest drifted")
        artifact = json.loads(artifact_bytes.decode("utf-8"))
        require(artifact["schema_id"] == expected["schema_id"] and
                artifact["authority_id"] == expected["id"] and
                artifact["schema_version"] == 1 and artifact["closed"] is True and
                artifact["status"] == "diagnostic_not_physical_acceptance" and
                artifact["camera_serial"] == "2010093" and
                artifact["coordinate_space"] == "camera_native_0_indexed" and
                artifact["native_raster"] == {"width": 4512, "height": 4512},
                f"{role} authority metadata drifted")


def test_runner_shell_and_help() -> None:
    syntax = subprocess.run(
        ["bash", "-n", str(RUNNER_PATH)], cwd=REPO_ROOT,
        text=True, capture_output=True, check=False,
    )
    require(syntax.returncode == 0, f"runner shell syntax failed: {syntax.stderr}")
    help_result = subprocess.run(
        [str(RUNNER_PATH), "--help"], cwd=REPO_ROOT,
        text=True, capture_output=True, check=False,
    )
    require(help_result.returncode == 0, "runner --help failed")
    require("--execute" in help_result.stdout and
            "Default: validate/print only" in help_result.stdout and
            "GPUs 3 and 4" in help_result.stdout,
            "runner help does not preserve its execution/split-GOP guard")


def test_default_run_is_validation_only() -> None:
    result = subprocess.run(
        [str(RUNNER_PATH)], cwd=Path("/tmp"),
        text=True, capture_output=True, check=False,
    )
    require(result.returncode == 0, f"default runner failed: {result.stderr}")
    require("dry-run: validation/print only" in result.stdout and
            "no hardware or media execution" in result.stdout,
            "default runner did not identify the hardware/media guard")
    require("[EXPERIMENT VALIDATION] status=pass" in result.stdout,
            "orange_client did not validate the 2010093 diagnostic spec")
    require("full_frame_split_gop=[3,4] roi_products=4xsingle_shard roi_gpus=[1,2,1,2]" in result.stdout,
            "runner did not print the intended full/ROI topology")


def test_cross_camera_spec_is_rejected() -> None:
    result = subprocess.run(
        [str(RUNNER_PATH), "--spec", str(EXISTING_2010096_SPEC_PATH)],
        cwd=Path("/tmp"), text=True, capture_output=True, check=False,
    )
    require(result.returncode != 0,
            "2010093 runner accepted the independently versioned 2010096 spec")
    require("unexpected experiment_id" in result.stderr,
            "cross-camera rejection did not fail at the hardcoded profile identity")


def main() -> int:
    tests = [
        test_spec_contract,
        test_camera_config_contract,
        test_authority_artifact_contract,
        test_runner_shell_and_help,
        test_default_run_is_validation_only,
        test_cross_camera_spec_is_rejected,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("spatial_roi_diagnostic_2010093_smoke_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
