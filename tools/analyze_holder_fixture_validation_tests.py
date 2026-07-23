#!/usr/bin/env python3
"""Synthetic checks for holder-aperture segmentation and active-H residual QC."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import analyze_holder_fixture_validation as analysis  # noqa: E402


def main() -> int:
    shape = (800, 800)
    black = np.zeros(shape, dtype=np.uint8)
    uniform = np.zeros(shape, dtype=np.uint8)
    cv2.circle(uniform, (400, 400), 350, 100, cv2.FILLED)
    contour, _, segmentation = analysis.segment_aperture(uniform, black)
    assert 0.55 < segmentation["area_fraction_of_sensor"] < 0.65
    boundary = analysis.resample_contour(contour)
    fitted = analysis.fit_circle(boundary)
    assert np.linalg.norm(
        np.asarray([fitted["center"]["x"], fitted["center"]["y"]]) - [400, 400]
    ) < 1.0
    assert abs(fitted["radius"] - 350.0) < 2.0

    # A holder larger than the configured arena produces an intersection:
    # flat projected-arena sides plus physically observed holder arcs at the
    # corners. Preserve that distinction instead of treating the whole outer
    # contour as a measured holder circle.
    clipped_uniform = np.zeros(shape, dtype=np.uint8)
    vertical_gradient = np.linspace(75, 145, 600, dtype=np.uint8)[:, None]
    projected_arena = np.repeat(vertical_gradient, 600, axis=1)
    clipped_uniform[100:700, 100:700] = projected_arena
    holder_mask = np.zeros(shape, dtype=np.uint8)
    cv2.circle(holder_mask, (400, 400), 350, 255, cv2.FILLED)
    clipped_uniform[holder_mask == 0] = 0
    clipped_contour, _, clipped_segmentation = analysis.segment_aperture(
        clipped_uniform, black
    )
    assert clipped_segmentation["threshold_method"] == \
        "global_otsu_on_uniform_minus_black"
    clipped_boundary = analysis.resample_contour(
        clipped_contour, maximum_points=512
    )
    clipped_target = {
        "arena": {
            "origin_final_display_canvas_px": {"x": 100, "y": 100},
            "size_px": {"width": 600, "height": 600},
        }
    }
    classified = analysis.classify_visibility_boundary(
        clipped_boundary,
        np.eye(3, dtype=np.float64),
        clipped_target,
        shape,
        arena_edge_margin_canvas_px=3.0,
        sensor_edge_margin_camera_px=12.0,
    )
    assert np.count_nonzero(classified["arena_clipped_mask"]) > 20
    assert np.count_nonzero(classified["aperture_observed_mask"]) > 20
    assert np.count_nonzero(classified["sensor_clipped_mask"]) == 0

    target = {
        "arena": {"origin_final_display_canvas_px": {"x": 0, "y": 0}},
        "projected_pattern": {
            "center_x_px": 400,
            "center_y_px": 400,
            "ring_count": 3,
            "ring_dots_inner": 4,
            "ring_dots_outer": 8,
            "ring_inner_radius_px": 80,
            "ring_outer_radius_px": 240,
            "ring_include_center_dot": True,
        },
    }
    expected = analysis.expected_ring_points(target, {})
    assert len(expected) == 19
    pattern = np.zeros(shape, dtype=np.uint8)
    for x, y in expected:
        cv2.circle(pattern, (int(round(x)), int(round(y))), 9, 100, cv2.FILLED)
    metrics = analysis.detect_expected_dots(
        pattern,
        black,
        expected,
        np.eye(3, dtype=np.float64),
        contour,
        12.0,
    )
    assert metrics["expected_visible_count"] == len(expected)
    assert metrics["detected_visible_count"] == len(expected)
    assert metrics["active_homography_rms_canvas_px"] < 0.75
    assert metrics["active_homography_max_canvas_px"] < 1.0
    refit = analysis.diagnostic_refit(metrics, metrics)
    assert refit["status"] == "available"
    assert refit["authority_role"] == "diagnostic_only_not_a_candidate"
    assert refit["candidate_created"] is False
    assert refit["promotion_allowed"] is False
    assert refit["held_out_verification"]["rms_canvas_px"] < 0.75
    assessment = analysis.assess_operational_candidate(refit, 0.75, 1.5)
    assert assessment["status"] == "passed"
    assert assessment["candidate_created"] is False
    assert assessment["runtime_authority_changed"] is False

    verification_target = {
        "arena": {"origin_final_display_canvas_px": {"x": 0, "y": 0}},
        "projected_pattern": {
            "center_x_px": 400,
            "center_y_px": 400,
            "verification_area_radius_px": 250,
            "verification_inner_radius_fraction": 0.38,
            "verification_outer_radius_fraction": 0.78,
            "verification_inner_point_count": 4,
            "verification_outer_point_count": 8,
            "verification_include_center_dot": True,
        },
    }
    verification = analysis.expected_verification_points(verification_target, {})
    assert len(verification) == 13

    # Holder overlays are derived evidence inside the source calibration
    # session. Persist them without rewriting the immutable image_set or its
    # manifest/fingerprint.
    with tempfile.TemporaryDirectory() as temporary_text:
        temporary = Path(temporary_text)
        session = temporary / "calsess_test_shadow"
        artifact = session / "artifacts" / "Cam2010093_arena_1"
        captures = artifact / "captures"
        captures.mkdir(parents=True)
        image_set = artifact / "image_set.json"
        source_manifest = artifact / "manifest.json"
        image_set.write_text('{"fingerprint":"unchanged"}\n', encoding="utf-8")
        source_manifest.write_text('{"summary":"unchanged"}\n', encoding="utf-8")
        original_image_set = image_set.read_bytes()
        original_manifest = source_manifest.read_bytes()
        source_images = {}
        for index, recipe in enumerate(
            ("black_reference", "uniform_gray", "homography_rings", "verification_dots")
        ):
            source = captures / f"{recipe}.png"
            cv2.imwrite(str(source), np.full((24, 24), index * 20, dtype=np.uint8))
            source_images[recipe] = {
                "path": str(source),
                "checksum": f"fnv1a64:{index:016x}",
                "capture_group_id": f"group_{index}",
                "camera_frame_id": index + 1,
                "local_frame_id": index + 1,
                "camera_timestamp_ns": 1000 + index,
                "camera_timestamp_clock_domain": "camera_ptp",
            }
        run_dir = temporary / "holder_fixture_test_run"
        overlay = run_dir / "overlays" / "Cam2010093.png"
        primary_qc = run_dir / "homography_qc" / "primary.png"
        heldout_qc = run_dir / "homography_qc" / "heldout.png"
        for path in (overlay, primary_qc, heldout_qc):
            path.parent.mkdir(parents=True, exist_ok=True)
            cv2.imwrite(str(path), np.full((24, 24, 3), 127, dtype=np.uint8))
        guided_result = run_dir / "guided_capture_result.json"
        guided_result.write_text("{}\n", encoding="utf-8")
        report = {
            "created_utc": "2026-07-20T00:00:00Z",
            "status": "pass",
            "guided_capture_result_path": str(guided_result),
            "authority_contract": {"role": "validation_only"},
            "coordinate_contract": {"camera_native_px": {"origin": "top_left"}},
            "operational_candidate_assessment": {"status": "passed"},
            "commissioning_reference_comparison": {
                "status": "mismatch_detected",
                "within_operational_tolerance": False,
                "findings": ["dry reference differs from holder plane"],
                "authority_effect": "diagnostic_only",
            },
            "camera_results": [{
                "camera_serial": "2010093",
                "arena_id": "arena_1",
                "status": "pass",
                "errors": [],
                "overlay_path": str(overlay),
                "homography_qc_images": {
                    "active_primary_reprojection": str(primary_qc),
                    "active_heldout_reprojection": str(heldout_qc),
                },
                "source_images": source_images,
                "fixture_aperture": {"semantic_role": "fixture_visibility_aperture"},
                "active_homography": {"candidate_id": "dry_reference"},
                "primary_support": {"active_homography_rms_canvas_px": 0.1},
                "verification": {"active_homography_rms_canvas_px": 0.2},
                "diagnostic_refit": {"authority_role": "diagnostic_only_not_a_candidate"},
                "operational_candidate_assessment": {"status": "passed"},
                "commissioning_reference_comparison": {
                    "status": "mismatch_detected",
                    "within_operational_tolerance": False,
                    "findings": ["dry reference differs from holder plane"],
                    "authority_effect": "diagnostic_only",
                },
            }],
        }
        manifest = {
            "calibration_session": {
                "session_id": session.name,
                "session_dir": str(session),
            },
            "fixture_state": {"state_id": "holder_installed_dish_absent"},
        }
        result = {
            "persistence": {
                "session_id": session.name,
                "session_dir": str(session),
            }
        }
        persisted = analysis.persist_holder_fixture_evidence(
            report,
            manifest,
            result,
            run_dir / "validation_report.json",
            run_dir / "validation_report.md",
            "# Holder Fixture Validation\n",
        )
        package_path = Path(persisted["session_manifest_path"])
        package = json.loads(package_path.read_text(encoding="utf-8"))
        assert package["schema_id"] == \
            "orange.calibration.holder_fixture_evidence_package"
        assert package["source_image_sets_modified"] is False
        assert len(package["camera_observations"]) == 1
        observation_path = session / package["camera_observations"][0][
            "observation_path"
        ]
        observation = json.loads(observation_path.read_text(encoding="utf-8"))
        assert observation["schema_id"] == \
            "orange.calibration.holder_fixture_observation"
        assert observation["homography_evaluation"]["runtime_authority_changed"] is False
        assert observation["status"] == "pass"
        assert observation["commissioning_reference_comparison"]["status"] == \
            "mismatch_detected"
        assert package["commissioning_reference_comparison"]["status"] == \
            "mismatch_detected"
        assert image_set.read_bytes() == original_image_set
        assert source_manifest.read_bytes() == original_manifest
    print("analyze_holder_fixture_validation_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
