#!/usr/bin/env python3
"""Focused integrity tests for recording-local geometry asset validation."""

from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from validate_recording_artifacts import (  # noqa: E402
    Reporter,
    validate_daily_registered_masks,
    validate_recording_geometry_artifacts,
)


def sha256(data: bytes) -> str:
    return f"sha256:{hashlib.sha256(data).hexdigest()}"


def json_bytes(value: dict) -> bytes:
    return (json.dumps(value, indent=2) + "\n").encode("utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def build_fixture(root: Path) -> tuple[dict, Path]:
    bundle = root / "recording_geometry_assets"
    asset_path = bundle / "cameras" / "Camcam1" / "spatial" / "dish_mask_runtime.json"
    asset_path.parent.mkdir(parents=True)
    asset_bytes = b'{"dish_mask":"camera-local"}\n'
    asset_path.write_bytes(asset_bytes)
    request_sha = "sha256:" + "a" * 64
    manifest = {
        "schema_id": "orange.recording.geometry_assets",
        "schema_version": 1,
        "status": "complete",
        "request_sha256": request_sha,
        "scope": {
            "camera_serials": ["cam1"],
            "arena_by_camera": {"cam1": "arena_1"},
        },
        "required_requested_file_count": 1,
        "required_failure_count": 0,
        "optional_requested_file_count": 0,
        "optional_failure_count": 0,
        "materialized_file_count": 1,
        "total_bytes": len(asset_bytes),
        "files": [
            {
                "role": "orange_spatial_dish_mask_runtime.json",
                "relative_path": "cameras/Camcam1/spatial/dish_mask_runtime.json",
                "sha256": sha256(asset_bytes),
                "size_bytes": len(asset_bytes),
                "context": {
                    "camera_serial": "cam1",
                    "arena_id": "arena_1",
                },
            }
        ],
        "failures": [],
        "warnings": [],
    }
    manifest_bytes = json_bytes(manifest)
    (bundle / "manifest.json").write_bytes(manifest_bytes)
    assets_reference = {
        "schema_id": "orange.recording.geometry_assets",
        "schema_version": 1,
        "status": "complete",
        "relative_path": "recording_geometry_assets/manifest.json",
        "sha256": sha256(manifest_bytes),
        "request_sha256": request_sha,
        "file_count": 1,
        "total_bytes": len(asset_bytes),
        "required_failure_count": 0,
    }
    contract = {
        "schema_id": "orange.recording.geometry_contract",
        "schema_version": 1,
        "status": "resolved",
        "materialized_assets": assets_reference,
    }
    contract_bytes = json_bytes(contract)
    (root / "recording_geometry_contract.json").write_bytes(contract_bytes)
    snapshot = {
        "recording_geometry_contract": {
            "schema_id": "orange.recording.geometry_contract",
            "schema_version": 1,
            "status": "resolved",
            "relative_path": "recording_geometry_contract.json",
            "sha256": sha256(contract_bytes),
        }
    }
    return snapshot, asset_path


def test_valid_bundle() -> None:
    with tempfile.TemporaryDirectory(prefix="orange_geometry_validator_") as temporary:
        root = Path(temporary)
        snapshot, _ = build_fixture(root)
        reporter = Reporter()
        summary = validate_recording_geometry_artifacts(root, snapshot, reporter)
        require(not reporter.failures, f"valid bundle failed: {reporter.failures}")
        require(summary["files"] == 1, "valid bundle should report one file")
        require(summary["asset_status"] == "complete", "valid bundle status should be complete")


def test_tampered_asset_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="orange_geometry_validator_tamper_") as temporary:
        root = Path(temporary)
        snapshot, asset_path = build_fixture(root)
        asset_path.write_bytes(b"tampered\n")
        reporter = Reporter()
        with redirect_stdout(io.StringIO()):
            validate_recording_geometry_artifacts(root, snapshot, reporter)
        require(
            any("checksum mismatch" in failure for failure in reporter.failures),
            f"tampered asset should fail checksum validation: {reporter.failures}",
        )


def test_daily_mask_contract_and_snapshot_agree() -> None:
    inner = {
        "coordinate_space": "camera_native_pixels",
        "target_plane": "dish_top_rim",
        "geometry": {
            "type": "circle",
            "center_px": {"x": 100.5, "y": 101.5},
            "radius_px": 90.0,
        },
    }
    valid = {
        "coordinate_space": "camera_native_pixels",
        "purpose": "bounding_box_centroid_detection_gating",
        "offset_direction": "outward",
        "geometry": {
            "type": "circle",
            "center_px": {"x": 100.5, "y": 101.5},
            "radius_px": 92.0,
        },
    }
    entry = {
        "artifact_id": "dishrim_cam1",
        "accepted_inner_rim_boundary": inner,
        "valid_detection_region": valid,
    }
    contract = {
        "daily_registration_geometry": {
            "status": "selected_resolved",
            "mode": "selected_daily_registration",
            "cameras": {
                "cam1": {
                    "status": "resolved",
                    "recording_snapshot_entry": entry,
                }
            },
        }
    }
    snapshot = {
        "calibrations": {"cam1": {"dish_top_rim_observation": entry}}
    }
    files = [
        {
            "role": role,
            "context": {"camera_serial": "cam1", "arena_id": "arena_1"},
        }
        for role in (
            "daily_rim_observation",
            "daily_rim_manifest",
            "daily_rim_image_set",
            "daily_rim_spatial_mask_export",
            "daily_rim_palette_mask_export",
        )
    ]
    reporter = Reporter()
    with redirect_stdout(io.StringIO()):
        summary = validate_daily_registered_masks(
            contract, snapshot, {"cam1"}, files, reporter
        )
    require(not reporter.failures, f"coherent daily mask failed: {reporter.failures}")
    require(summary["resolved_mask_count"] == 1, "daily mask should resolve")

    contract["daily_registration_geometry"]["cameras"]["cam1"][
        "recording_snapshot_entry"
    ]["valid_detection_region"]["geometry"]["radius_px"] = 80.0
    reporter = Reporter()
    with redirect_stdout(io.StringIO()):
        validate_daily_registered_masks(contract, snapshot, {"cam1"}, files, reporter)
    require(
        any("invalid or contradictory" in failure for failure in reporter.failures),
        "an inward or contradictory centroid gate must fail validation",
    )


def main() -> int:
    tests = [
        ("valid_bundle", test_valid_bundle),
        ("tampered_asset_fails", test_tampered_asset_fails),
        ("daily_mask_contract_and_snapshot_agree",
         test_daily_mask_contract_and_snapshot_agree),
    ]
    for name, test in tests:
        try:
            test()
        except Exception as exc:  # noqa: BLE001 - tiny standalone test runner
            print(f"[FAIL] {name}: {exc}", file=sys.stderr)
            return 1
        print(f"[PASS] {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
