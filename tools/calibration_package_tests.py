#!/usr/bin/env python3
"""Deterministic tests for non-active calibration package primitives."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from calibration_package_lib import (  # noqa: E402
    CalibrationPackageError,
    asset,
    canonical_json_bytes,
    canonical_sha256,
    compare_projection_geometry,
    package_id,
    projection_geometry_fingerprint,
    seal_candidate_package,
    sha256_bytes,
    validate_candidate_package,
)
from calibration_package import (  # noqa: E402
    build_canvas_binding_candidate,
    build_capsule_candidate,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def expect_error(function, text: str) -> None:
    try:
        function()
    except CalibrationPackageError as exc:
        require(text in str(exc), f"expected {text!r} in {exc!r}")
        return
    raise RuntimeError(f"expected CalibrationPackageError containing {text!r}")


def identity() -> dict:
    return {
        "authority": "citrus",
        "rig_id": "rig1",
        "rig_geometry_revision": 1,
        "canvas_id": "canvas1",
        "source_release_id": "release1",
        "source_release_sha256": "sha256:" + "a" * 64,
        "requirements_profile_id": "fixture-v1",
        "target_plane": "projected_surface",
        "canvas_raster": {"width_px": 1920, "height_px": 1080},
        "coordinate_conventions": {
            "camera_native_origin": "top_left",
            "camera_native_positive_x": "right",
            "camera_native_positive_y": "down",
            "canvas_origin": "top_left",
            "canvas_positive_x": "right",
            "canvas_positive_y": "down",
            "pixel_center_convention": "integer_coordinates_are_pixel_centers",
            "boundary_inclusion": "inclusive",
        },
        "projection_geometry_identity": {
            "schema_id": "citrus.calibration.canvas_projection_geometry_identity",
            "schema_version": 1,
            "fingerprint": "sha256:" + "b" * 64,
        },
        "camera_arena_map": {
            "cam1": {
                "arena_id": "arena_1",
                "target_plane": "projected_surface",
                "native_width_px": 100,
                "native_height_px": 80,
                "tank_design_id": "tank1",
            }
        },
    }


def canvas() -> dict:
    return {
        "canvas_name": "canvas1",
        "canvas_width_px": 1920,
        "canvas_height_px": 1080,
        "arenas": {
            "arena_1": {
                "config_name": "arena_1",
                "arena_region_width_mm": 80.0,
                "arena_region_height_mm": 80.0,
                "experimental_area_radius_mm": 40.0,
                "experimental_area_radius_px": 160.0,
                "calibration_pattern_mode": "circular_rings",
                "calibration_ring_count": 4,
                "camera_calibrations": [
                    {
                        "camera_id": "cam1",
                        "native_width_px": 100,
                        "native_height_px": 80,
                        "arena_center_x_px": 50,
                        "arena_center_y_px": 40,
                        "arena_width_px": 80,
                        "arena_height_px": 80,
                        "pixels_per_mm_camera": 2.0,
                        "pixels_per_mm_projector": 4.0,
                        "real_world_ref_mm": 10.0,
                        "scale_image_path": "old.png",
                        "scale_models": [
                            {
                                "target_plane": "projected_surface",
                                "pixels_per_mm_camera": 2.0,
                            },
                            {"target_plane": "fish_observation", "id": "keep"},
                        ],
                    }
                ],
            }
        },
    }


def test_canonical_order_and_vector() -> None:
    left = {"z": [3, True, None], "a": {"é": "snow ☃", "b": -2}}
    right = {"a": {"b": -2, "é": "snow ☃"}, "z": [3, True, None]}
    expected = b'{"a":{"b":-2,"\xc3\xa9":"snow \xe2\x98\x83"},"z":[3,true,null]}'
    require(canonical_json_bytes(left) == expected, "canonical vector changed")
    require(canonical_json_bytes(left) == canonical_json_bytes(right), "key order leaked")
    require(canonical_sha256(left) == canonical_sha256(right), "digest is not stable")
    expect_error(lambda: canonical_sha256({"float": 1.25}), "floating-point")
    expect_error(lambda: canonical_sha256({"integer": 2**63}), "signed 64-bit")


def test_projection_geometry_identity_rule() -> None:
    accepted = canvas()
    require(
        projection_geometry_fingerprint(accepted)
        == "sha256:01932ac4c8edd3b539c5b9a46b2a341761dc2149f83fa49129b0fa2af9da0857",
        "cross-language projection-geometry fingerprint vector changed",
    )
    current = json.loads(json.dumps(accepted))
    arena = current["arenas"]["arena_1"]
    arena["calibration_pattern_mode"] = "rectangular_grid"
    arena["calibration_ring_count"] = 8
    arena["experimental_area_radius_px"] = 167.25
    camera = arena["camera_calibrations"][0]
    camera["pixels_per_mm_camera"] = 9.0
    camera["pixels_per_mm_projector"] = 8.0
    camera["scale_models"][0]["pixels_per_mm_camera"] = 9.0
    result = compare_projection_geometry(current, accepted)
    require(result["compatible"], f"cache-only change rejected: {result}")
    require(
        result["warning"] == "canvas_non_geometry_calibration_state_only_change",
        "cache-only change lacks warning",
    )
    current["arenas"]["arena_1"]["camera_calibrations"][0][
        "arena_center_x_px"
    ] += 1
    result = compare_projection_geometry(current, accepted)
    require(not result["compatible"], "real arena-center change was accepted")
    require(result["error"] == "canvas_projection_geometry_changed", "wrong failure")


def build_package(root: Path) -> tuple[Path, Path]:
    source = root / "source.bin"
    source.write_bytes(b"exact calibration bytes\x00\xff")
    canvas_path = root / "canvas.json"
    canvas_path.write_text(json.dumps(canvas()), encoding="utf-8")
    package_identity = identity()
    package_identity["projection_geometry_identity"]["fingerprint"] = (
        projection_geometry_fingerprint(canvas())
    )
    package = seal_candidate_package(
        root / "candidates",
        "commissioned_rig_setup",
        package_identity,
        [
            asset(
                canvas_path,
                "assets/config/canvas.json",
                "authority_canvas_configuration_snapshot",
                target_plane="projected_surface",
                expected_sha256=sha256_bytes(canvas_path.read_bytes()),
            ),
            asset(
                source,
                "assets/cameras/Camcam1/data.bin",
                "homography_fixture",
                target_plane="projected_surface",
                camera_id="cam1",
                arena_id="arena_1",
                expected_sha256=sha256_bytes(source.read_bytes()),
            ),
        ],
        provenance={"writer": "test", "activation_performed": False},
    )
    return package, source


def test_exact_bytes_validate_without_sources() -> None:
    with tempfile.TemporaryDirectory(prefix="orange_package_test_") as temporary:
        root = Path(temporary)
        package, source = build_package(root)
        result = validate_candidate_package(package, verify_sources=True)
        require(result["status"] == "valid", "valid fixture rejected")
        source.unlink()
        result = validate_candidate_package(package)
        require(result["status"] == "valid", "package depends on source path")
        expect_error(
            lambda: validate_candidate_package(package, verify_sources=True),
            "source is unavailable",
        )


def test_tamper_and_scope_fail_closed() -> None:
    with tempfile.TemporaryDirectory(prefix="orange_package_tamper_") as temporary:
        root = Path(temporary)
        package, _ = build_package(root)
        member = package / "assets/cameras/Camcam1/data.bin"
        member.write_bytes(b"tampered")
        expect_error(
            lambda: validate_candidate_package(package),
            "size mismatch",
        )

    with tempfile.TemporaryDirectory(prefix="orange_package_scope_") as temporary:
        root = Path(temporary)
        package, _ = build_package(root)
        inventory_path = package / "inventory.json"
        inventory = json.loads(inventory_path.read_text())
        scoped = next(row for row in inventory["files"] if row["scope"]["camera_id"])
        scoped["scope"]["arena_id"] = "arena_other"
        inventory_path.write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
        manifest_path = package / "package.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["inventory"]["sha256"] = sha256_bytes(inventory_path.read_bytes())
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        expect_error(
            lambda: validate_candidate_package(package),
            "does not match identity",
        )


def test_path_escape_and_required_gap_fail_closed() -> None:
    with tempfile.TemporaryDirectory(prefix="orange_package_escape_") as temporary:
        root = Path(temporary)
        source = root / "source"
        source.write_bytes(b"x")
        expect_error(
            lambda: seal_candidate_package(
                root / "out",
                "commissioned_rig_setup",
                identity(),
                [asset(source, "../escape", "escape", target_plane="projected_surface")],
            ),
            "escapes",
        )
    with tempfile.TemporaryDirectory(prefix="orange_package_gap_") as temporary:
        root = Path(temporary)
        source = root / "source"
        source.write_bytes(b"x")
        package_identity = identity()
        canvas_path = root / "canvas.json"
        canvas_path.write_text(json.dumps(canvas()), encoding="utf-8")
        package_identity["projection_geometry_identity"]["fingerprint"] = (
            projection_geometry_fingerprint(canvas())
        )
        package = seal_candidate_package(
            root / "out",
            "commissioned_rig_setup",
            package_identity,
            [
                asset(
                    canvas_path,
                    "assets/canvas.json",
                    "authority_canvas_configuration_snapshot",
                    target_plane="projected_surface",
                )
            ],
            declared_gaps=[{"role": "fixture_contract", "required": True}],
        )
        canvas_path.unlink()
        expect_error(
            lambda: validate_candidate_package(package, verify_sources=True),
            "source is unavailable",
        )
        expect_error(
            lambda: validate_candidate_package(package),
            "missing required",
        )


def test_manifest_policy_and_symlink_fail_closed() -> None:
    with tempfile.TemporaryDirectory(prefix="orange_package_manifest_") as temporary:
        root = Path(temporary)
        package, _ = build_package(root)
        manifest_path = package / "package.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["completeness"]["gap_count"] = 1
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        expect_error(
            lambda: validate_candidate_package(package),
            "completeness disagrees",
        )

    with tempfile.TemporaryDirectory(prefix="orange_package_symlink_") as temporary:
        root = Path(temporary)
        package, source = build_package(root)
        member = package / "assets/cameras/Camcam1/data.bin"
        member.unlink()
        member.symlink_to(source)
        expect_error(
            lambda: validate_candidate_package(package),
            "contains a symlink",
        )


def test_package_id_is_identity_only() -> None:
    first_id, first_sha = package_id("commissioned_rig_setup", identity())
    reordered = dict(reversed(list(identity().items())))
    second_id, second_sha = package_id("commissioned_rig_setup", reordered)
    require((first_id, first_sha) == (second_id, second_sha), "package ID order leak")


def test_binding_and_base_capsule_bind_complete_parent_bytes() -> None:
    with tempfile.TemporaryDirectory(prefix="orange_package_composition_") as temporary:
        root = Path(temporary)
        commission, _ = build_package(root)
        binding = build_canvas_binding_candidate(commission, root / "bindings")
        binding_result = validate_candidate_package(binding)
        require(binding_result["status"] == "valid", "valid binding rejected")
        capsule = build_capsule_candidate(
            commission, binding, root / "capsules", daily_path=None
        )
        capsule_result = validate_candidate_package(capsule)
        require(capsule_result["status"] == "valid", "valid base capsule rejected")
        capsule_identity = json.loads((capsule / "package.json").read_text())["identity"]
        require(capsule_identity["composition_mode"] == "base_only", "wrong mode")
        require(
            "daily_registration" not in capsule_identity["composition"],
            "base-only capsule silently selected a daily registration",
        )


def test_v2_schema_documents_are_valid_json() -> None:
    names = [
        "orange_calibration_package_common.schema.json",
        "orange_commissioned_rig_setup.schema.json",
        "orange_experiment_canvas_binding.schema.json",
        "orange_daily_registration_set.schema.json",
        "orange_recording_calibration_capsule.schema.json",
        "orange_calibration_package_inventory.schema.json",
        "orange_calibration_registry_pointer.schema.json",
    ]
    for name in names:
        value = json.loads((REPO_ROOT / "docs" / "schemas" / name).read_text())
        require(value.get("$schema") is not None, f"{name} lacks JSON Schema dialect")
        require(value.get("$id") is not None, f"{name} lacks schema ID")


def main() -> int:
    tests = [
        test_canonical_order_and_vector,
        test_projection_geometry_identity_rule,
        test_exact_bytes_validate_without_sources,
        test_tamper_and_scope_fail_closed,
        test_path_escape_and_required_gap_fail_closed,
        test_manifest_policy_and_symlink_fail_closed,
        test_package_id_is_identity_only,
        test_binding_and_base_capsule_bind_complete_parent_bytes,
        test_v2_schema_documents_are_valid_json,
    ]
    for test in tests:
        try:
            test()
        except Exception as exc:  # noqa: BLE001 - standalone test runner
            print(f"[FAIL] {test.__name__}: {exc}", file=sys.stderr)
            return 1
        print(f"[PASS] {test.__name__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
