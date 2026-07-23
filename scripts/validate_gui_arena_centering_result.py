#!/usr/bin/env python3
"""Validate a completed Orange/Citrus arena-centering commissioning run."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys


REQUIRED_PROBES = {
    "baseline",
    "probe_plus_x",
    "probe_minus_x",
    "probe_plus_y",
    "probe_minus_y",
    "candidate",
}


def fail(message: str) -> None:
    raise ValueError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("result", type=Path)
    parser.add_argument("--expected-cameras", required=True)
    parser.add_argument("--require-committed", action="store_true")
    parser.add_argument("--require-homography-fit", action="store_true")
    parser.add_argument("--require-homography-committed", action="store_true")
    args = parser.parse_args()

    payload = json.loads(args.result.read_text(encoding="utf-8"))
    expected = [item for item in args.expected_cameras.split(",") if item]
    if payload.get("schema_id") != "orange.gui_arena_centering_commissioning_result":
        fail("unexpected result schema")
    schema_version = int(payload.get("schema_version", 1))
    if payload.get("status") != "pass":
        fail(f"run did not pass: {payload.get('error')}")
    targets = payload.get("targets", [])
    actual = [str(item.get("camera_serial", "")) for item in targets]
    if actual != expected:
        fail(f"camera target order mismatch: {actual} != {expected}")
    config = payload.get("config", {})
    resize_enabled = bool(config.get("resize_arenas", False))
    ownership = payload.get("ownership", {})
    expected_mutation = (
        "canonical_arena_square_placement_canvas_px"
        if resize_enabled
        else "canonical_arena_center_canvas_px"
    )
    if ownership.get("mutated_geometry") != expected_mutation:
        fail("canonical arena placement was not the declared mutation target")
    if schema_version >= 2:
        if ownership.get("experimental_area_shape_or_size_changed") is not False:
            fail("experimental-area shape/size was not invariant")
        if ownership.get("experimental_area_offset_from_arena_center_preserved") is not True:
            fail("experimental-area offset from arena center was not preserved")
    elif ownership.get("experimental_area_local_geometry_changed") is not False:
        fail("legacy experimental-area local geometry was not invariant")

    records = payload.get("stage_records", [])
    required_stages = set(REQUIRED_PROBES)
    if resize_enabled:
        required_stages.add("resized_candidate")
    stage_ids = {str(item.get("stage_id", "")) for item in records}
    if not required_stages.issubset(stage_ids):
        fail(f"missing required capture stages: {sorted(required_stages - stage_ids)}")
    session_dirs = set()
    for record in records:
        stage_id = record.get("stage_id", "unknown")
        analysis_gate = record.get("analysis_gate", {})
        if analysis_gate and analysis_gate.get("status") != "passed":
            fail(f"{stage_id}: analysis gate did not pass")
        analysis = record.get("concurrent_analysis", {})
        if analysis.get("task_count") != len(expected):
            fail(f"{stage_id}: analysis task count mismatch")
        detections = analysis.get("detections", [])
        if len(detections) != len(expected) or not all(row.get("ok") for row in detections):
            fail(f"{stage_id}: incomplete or failed detections")
        if config.get("require_projection_stability_capture", False):
            stability = analysis.get("projection_stability", {})
            if stability.get("status") != "passed":
                fail(f"{stage_id}: projection stability gate did not pass")
            if stability.get("reference_capture_group_id") == record.get(
                "capture_group_id"
            ):
                fail(f"{stage_id}: stability captures reused one capture group")
            if len(stability.get("cameras", [])) != len(expected) or not all(
                row.get("status") == "stable"
                for row in stability.get("cameras", [])
            ):
                fail(f"{stage_id}: per-camera projection stability is incomplete")
            reference = stability.get("reference", {})
            reference_ptp = reference.get("ptp_alignment", {})
            if reference_ptp.get("camera_count") != len(expected):
                fail(f"{stage_id}: stability-reference PTP camera count mismatch")
            maximum_span = int(config.get("maximum_ptp_capture_span_ns", 0))
            if int(reference_ptp.get("span_ns", maximum_span + 1)) > maximum_span:
                fail(f"{stage_id}: stability-reference PTP span exceeded")
            reference_centers = reference.get("center_detections", {})
            if reference_centers.get("task_count") != len(expected) or not all(
                row.get("ok")
                for row in reference_centers.get("detections", [])
            ):
                fail(f"{stage_id}: stability-reference center detections failed")
        if schema_version >= 2 and stage_id in {
            "baseline", "candidate", "refined_candidate", "resized_candidate"
        }:
            rectangles = analysis.get("rectangle_boundaries", {})
            rectangle_rows = rectangles.get("detections", [])
            if rectangles.get("task_count") != len(expected):
                fail(f"{stage_id}: rectangle analysis task count mismatch")
            if len(rectangle_rows) != len(expected) or not all(
                row.get("ok") for row in rectangle_rows
            ):
                fail(f"{stage_id}: incomplete or failed rectangle detections")
            if config.get("require_projection_stability_capture", False):
                reference_rectangles = (
                    analysis.get("projection_stability", {})
                    .get("reference", {})
                    .get("rectangle_detections", {})
                )
                if reference_rectangles.get("task_count") != len(expected) or not all(
                    row.get("ok")
                    for row in reference_rectangles.get("detections", [])
                ):
                    fail(f"{stage_id}: stability-reference rectangle detections failed")
        ptp = record.get("ptp_alignment", {})
        if ptp.get("camera_count") != len(expected):
            fail(f"{stage_id}: PTP camera count mismatch")
        maximum_span = int(payload.get("config", {}).get("maximum_ptp_capture_span_ns", 0))
        if int(ptp.get("span_ns", maximum_span + 1)) > maximum_span:
            fail(f"{stage_id}: PTP span exceeded")
        persistence = record.get("persistence", {})
        session_dir = persistence.get("session_dir", "")
        if not session_dir:
            fail(f"{stage_id}: session directory missing")
        session_dirs.add(session_dir)
        artifacts = record.get("analysis_artifacts", {})
        detection_json = Path(artifacts.get("detection_json", ""))
        if not detection_json.is_file():
            fail(f"{stage_id}: detection artifact missing")
        overlays = artifacts.get("overlays", [])
        if len(overlays) != len(expected):
            fail(f"{stage_id}: overlay count mismatch")
        for overlay in overlays:
            if not Path(overlay.get("path", "")).is_file():
                fail(f"{stage_id}: overlay file missing")
        rectangle_overlays = artifacts.get("rectangle_overlays", [])
        expected_rectangle_overlays = (
            len(expected)
            if schema_version >= 2 and stage_id in {
                "baseline", "candidate", "refined_candidate", "resized_candidate"
            }
            else 0
        )
        if len(rectangle_overlays) != expected_rectangle_overlays:
            fail(f"{stage_id}: rectangle overlay count mismatch")
        for overlay in rectangle_overlays:
            if not Path(overlay.get("path", "")).is_file():
                fail(f"{stage_id}: rectangle overlay file missing")
    if len(session_dirs) != 1:
        fail(f"captures did not remain in one calibration session: {session_dirs}")
    if payload.get("verification", {}).get("status") != "passed":
        fail("all-camera final verification did not pass")
    if resize_enabled:
        for camera in payload.get("verification", {}).get("cameras", []):
            if camera.get("rectangle_verification", {}).get("status") != "passed":
                fail(f"{camera.get('camera_serial')}: rectangle verification failed")

    terminal = payload.get("terminal_status", {})
    receipt = terminal.get("receipt", {})
    outcome = receipt.get("outcome")
    if args.require_committed:
        if outcome != "committed":
            fail(f"expected committed receipt, got {outcome!r}")
        scope = receipt.get("geometry_mutation_scope", {})
        if schema_version >= 2:
            if scope.get("experimental_area_shape_or_size_changed") is not False:
                fail("commit receipt did not preserve experimental-area shape/size")
            if scope.get("experimental_area_offset_from_arena_center_preserved") is not True:
                fail("commit receipt did not preserve experimental-area center offset")
        elif scope.get("experimental_area_local_geometry_changed") is not False:
            fail("legacy commit receipt did not prove experimental-area invariance")
    elif outcome not in {"committed", "aborted"}:
        fail(f"terminal receipt is missing or invalid: {outcome!r}")

    homography = payload.get("homography", {})
    if args.require_homography_fit or args.require_homography_committed:
        if schema_version < 3:
            fail("homography validation requires result schema version 3")
        if not homography.get("fit_requested"):
            fail("homography fit was not requested")
        if not homography.get("transaction_id") or not homography.get("capture_group_id"):
            fail("homography transaction or capture-group identity is missing")
        quality_thresholds = config.get("homography_quality_thresholds", {})
        report_path = Path(
            quality_thresholds.get("projector_intensity_report_path", "")
        )
        expected_report_sha256 = quality_thresholds.get(
            "projector_intensity_report_sha256", ""
        )
        if not report_path.is_file() or not expected_report_sha256:
            fail("projector-intensity commissioning provenance is missing")
        report_bytes = report_path.read_bytes()
        if hashlib.sha256(report_bytes).hexdigest() != expected_report_sha256:
            fail("projector-intensity commissioning report checksum changed")
        report = json.loads(report_bytes)
        if (
            report.get("schema_id")
            != "orange.projector_intensity_commissioning.report"
            or report.get("schema_version") != 1
            or report.get("status") != "pass"
        ):
            fail("projector-intensity commissioning report is not passing")
        selected_gray = int(config.get("foreground_gray_u8", -1))
        if selected_gray != int(report.get("recommended_foreground_gray_u8", -2)):
            fail("homography gray is not the report's commissioned recommendation")
        if report.get("level_passes_all_cameras", {}).get(str(selected_gray)) is not True:
            fail("commissioned homography gray did not pass every camera")
        report_method = report.get("method", {})
        report_gates = report_method.get("quality_gates", {})
        expected_photometry_thresholds = {
            "saturation_pixel_threshold_u8": int(
                report_method.get("saturation_pixel_threshold_u8", -1)
            ),
            "maximum_dot_core_saturation_fraction": float(
                report_gates.get("max_core_saturation_fraction", -1)
            ),
            "minimum_dot_background_contrast_u8": float(
                report_gates.get("min_dot_background_contrast_u8", -1)
            ),
        }
        for name, expected_value in expected_photometry_thresholds.items():
            if float(quality_thresholds.get(name, -2)) != float(expected_value):
                fail(f"homography photometry threshold diverged from report: {name}")
        if homography.get("committed_canvas_checksum") != receipt.get("new_canvas_checksum"):
            fail("homography fit did not bind to the committed centered canvas checksum")
        candidate = homography.get("candidate_status", {})
        rows = candidate.get("candidates", [])
        if len(rows) != len(expected):
            fail("homography candidate count does not match the expected camera set")
        candidate_set_dir = Path(candidate.get("candidate_set_dir", ""))
        if not candidate_set_dir.is_dir():
            fail("homography candidate-set directory is missing")
        layout_evidence = candidate.get("coordinate_frame_evidence", {})
        if (
            layout_evidence.get("schema_id")
            != "citrus.homography.logical_canvas_layout_evidence"
            or layout_evidence.get("status") != "complete"
        ):
            fail("logical-canvas coordinate evidence is missing")
        if layout_evidence.get("rendering_behavior_changed") is not False:
            fail("coordinate evidence unexpectedly reports a rendering change")
        if layout_evidence.get("presentation_mapping", {}) != {
            "status": "separate_from_logical_canvas_contract",
            "reflection_allowed": True,
            "authority": "render_path_plus_optical_calibration",
        }:
            fail("logical-canvas evidence conflates presentation and composition axes")
        for name in (
            "logical_canvas_layout_evidence.png",
            "logical_canvas_layout_evidence.json",
        ):
            if not (candidate_set_dir / name).is_file():
                fail(f"logical-canvas review artifact is missing: {candidate_set_dir / name}")
        for row in rows:
            if row.get("quality", {}).get("status") != "passed":
                fail(f"homography quality gate failed for {row.get('camera_id')}")
            photometry = row.get("source_photometry", {})
            if (
                photometry.get("schema_id")
                != "citrus.homography.source_photometry"
                or photometry.get("status") != "passed"
            ):
                fail(f"source photometry failed for {row.get('camera_id')}")
            gates = photometry.get("gates", {})
            if gates.get("saturation", {}).get("passed") is not True:
                fail(f"source saturation failed for {row.get('camera_id')}")
            if gates.get("contrast", {}).get("passed") is not True:
                fail(f"source contrast failed for {row.get('camera_id')}")
            metrics = photometry.get("metrics", {})
            if not isinstance(metrics.get("dot_core_saturation_fraction_ge_threshold"), (int, float)):
                fail(f"source saturation metric is missing for {row.get('camera_id')}")
            if not isinstance(metrics.get("dot_background_contrast_u8"), (int, float)):
                fail(f"source contrast metric is missing for {row.get('camera_id')}")
            provenance = photometry.get("commissioning_provenance", {})
            if (
                provenance.get("report_path") != str(report_path)
                or provenance.get("report_sha256") != expected_report_sha256
                or int(provenance.get("recommended_foreground_gray_u8", -1))
                != selected_gray
            ):
                fail(f"source photometry provenance mismatch for {row.get('camera_id')}")
            orientation = row.get("orientation_validation", {})
            if orientation.get("status") != "passed":
                fail(
                    "homography orientation validation failed for "
                    f"{row.get('camera_id')}: {orientation.get('error')}"
                )
            coordinate = row.get("coordinate_contract", {})
            source = coordinate.get("source", {})
            destination = coordinate.get("destination", {})
            expected_axes = {
                "origin": "top_left",
                "positive_x": "right",
                "positive_y": "down",
                "units": "px",
            }
            if source != {
                "space": "camera_native_px",
                "raster_role": "camera_sensor_raster",
                **expected_axes,
            }:
                fail(f"camera coordinate contract is invalid for {row.get('camera_id')}")
            if destination != {
                "space": "final_display_canvas_px",
                "raster_role": "logical_citrus_composition_canvas",
                **expected_axes,
            }:
                fail(f"canvas coordinate contract is invalid for {row.get('camera_id')}")
            presentation = coordinate.get("presentation_mapping", {})
            if presentation != {
                "status": "separate_from_logical_canvas_contract",
                "reflection_allowed": True,
                "authority": "render_path_plus_optical_calibration",
            }:
                fail(f"display presentation contract is invalid for {row.get('camera_id')}")
            if coordinate.get("homography_direction") != (
                "camera_native_px_to_final_display_canvas_px"
            ):
                fail(f"homography direction is invalid for {row.get('camera_id')}")
            frame_evidence = row.get("coordinate_frame_evidence", {})
            if (
                frame_evidence.get("schema_id")
                != "citrus.homography.coordinate_frame_evidence"
                or frame_evidence.get("status") != "passed"
            ):
                fail(f"coordinate-frame evidence is missing for {row.get('camera_id')}")
            if len(frame_evidence.get("basis_points", [])) != 3:
                fail(f"coordinate basis is incomplete for {row.get('camera_id')}")
            observed = frame_evidence.get("logical_basis_observed_in_camera", {})
            if observed.get("handedness") not in {
                "reflected_in_camera_raster",
                "orientation_preserving_in_camera_raster",
            } or not isinstance(observed.get("reflection_observed"), bool):
                fail(f"observed camera/canvas handedness is missing for {row.get('camera_id')}")
            pattern = row.get("projected_pattern", {})
            if pattern.get("mode") == "rectangular_grid":
                if frame_evidence.get("basis_source") != (
                    "detected_rectangular_grid_correspondences"
                ):
                    fail(f"rectangular coordinate basis is not observed for {row.get('camera_id')}")
                params = pattern.get("params", {})
                if int(params.get("pattern_revision", 0)) < 2:
                    fail(f"rectangular pattern is not orientation-safe for {row.get('camera_id')}")
                if params.get("rectangular_show_orientation_marker") is not True:
                    fail(f"rectangular marker was not rendered for {row.get('camera_id')}")
                if (
                    int(params.get("rectangular_orientation_marker_row", -1)) != 0
                    or int(params.get("rectangular_orientation_marker_col", -1)) != 1
                ):
                    fail(f"rectangular marker basis is invalid for {row.get('camera_id')}")
            artifact_dir = candidate_set_dir / (
                f"{row.get('arena_id', '')}_{row.get('camera_id', '')}"
            )
            for name in (
                "candidate.json",
                "homography.yml",
                "detection_overlay.png",
                "reprojection_overlay.png",
                "coordinate_frame_evidence.png",
            ):
                if not (artifact_dir / name).is_file():
                    fail(f"homography review artifact is missing: {artifact_dir / name}")
        if len(session_dirs) != 1 or payload.get("persistence", {}).get(
            "session_dir"
        ) != next(iter(session_dirs)):
            fail("homography capture did not remain in the centering calibration session")

    if args.require_homography_committed:
        if not homography.get("committed"):
            fail("homography candidates were not committed")
        candidate = homography.get("candidate_status", {})
        if candidate.get("state") != "committed":
            fail("homography candidate lifecycle did not reach committed")
        homography_receipt = candidate.get("receipt", {})
        if homography_receipt.get("outcome") != "committed":
            fail("homography acceptance receipt is missing")
        accepted = homography_receipt.get("accepted", [])
        if len(accepted) != len(expected):
            fail("homography acceptance receipt target count mismatch")
        for row in accepted:
            if not Path(row.get("active_pointer_path", "")).is_file():
                fail("accepted homography active pointer is missing")
            if not Path(row.get("compatibility_sidecar_path", "")).is_file():
                fail("accepted homography compatibility sidecar is missing")

    print(
        f"arena-centering validation passed: cameras={len(expected)} "
        f"stages={len(records)} outcome={outcome} session={next(iter(session_dirs))}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"arena-centering validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
