#!/usr/bin/env python3
"""Bootstrap-centering evidence must not masquerade as edge/homography proof."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "scripts/validate_gui_arena_centering_result.py"
STAGES = (
    "baseline", "probe_plus_x", "probe_minus_x", "probe_plus_y",
    "probe_minus_y", "candidate",
)
CAMERA = "2010093"


def make_bootstrap_result(root: Path) -> dict:
    records = []
    for stage in STAGES:
        stage_dir = root / stage
        stage_dir.mkdir()
        detection = stage_dir / "detections.json"
        overlay = stage_dir / f"Cam{CAMERA}_center_overlay.png"
        detection.write_text("{}\n", encoding="utf-8")
        overlay.write_bytes(b"test overlay")
        records.append({
            "stage_id": stage,
            "capture_group_id": f"confirm_{stage}",
            "analysis_gate": {"status": "passed"},
            "concurrent_analysis": {
                "task_count": 1,
                "detections": [{"camera_serial": CAMERA, "ok": True}],
                "projection_stability": {
                    "status": "passed",
                    "reference_capture_group_id": f"reference_{stage}",
                    "cameras": [{"camera_serial": CAMERA, "status": "stable"}],
                    "reference": {
                        "ptp_alignment": {"camera_count": 1, "span_ns": 0},
                        "center_detections": {
                            "task_count": 1,
                            "detections": [{"camera_serial": CAMERA, "ok": True}],
                        },
                    },
                },
            },
            "ptp_alignment": {"camera_count": 1, "span_ns": 0},
            "persistence": {"session_dir": str(root)},
            "analysis_artifacts": {
                "detection_json": str(detection),
                "overlays": [{"camera_serial": CAMERA, "path": str(overlay)}],
                "rectangle_overlays": [],
            },
        })
    return {
        "schema_id": "orange.gui_arena_centering_commissioning_result",
        "schema_version": 4,
        "workflow_mode": "bootstrap_center_fiducial_v1",
        "status": "pass",
        "targets": [{"camera_serial": CAMERA}],
        "config": {
            "bootstrap_centers_only": True,
            "foreground_gray_qualification": "provisional_for_center_fiducial_only",
            "save_captures": True,
            "require_projection_stability_capture": True,
            "maximum_ptp_capture_span_ns": 1000,
            "resize_arenas": False,
            "fit_homographies_after_centering": False,
            "accept_homographies_armed": False,
            "homography_quality_thresholds": {
                "projector_intensity_report_path": "",
                "projector_intensity_report_sha256": "",
            },
        },
        "ownership": {
            "mutated_geometry": "canonical_arena_center_canvas_px",
            "experimental_area_shape_or_size_changed": False,
            "experimental_area_offset_from_arena_center_preserved": True,
        },
        "stage_records": records,
        "verification": {"status": "passed"},
        "terminal_status": {"receipt": {
            "outcome": "committed",
            "geometry_mutation_scope": {
                "experimental_area_shape_or_size_changed": False,
                "experimental_area_offset_from_arena_center_preserved": True,
            },
        }},
        "homography": {
            "fit_requested": False,
            "promotion_requested": False,
            "committed": False,
        },
    }


def validate(root: Path, payload: dict, *options: str) -> subprocess.CompletedProcess[str]:
    result = root / "result.json"
    result.write_text(json.dumps(payload), encoding="utf-8")
    return subprocess.run(
        [sys.executable, str(VALIDATOR), str(result),
         "--expected-cameras", CAMERA, "--require-bootstrap-centers", *options],
        capture_output=True, text=True, check=False,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="orange-bootstrap-validator-") as temp:
        root = Path(temp)
        baseline = make_bootstrap_result(root)
        assert validate(root, baseline, "--require-committed").returncode == 0

        wrong_mode = copy.deepcopy(baseline)
        wrong_mode["workflow_mode"] = "center_rectangle_v1"
        assert validate(root, wrong_mode).returncode != 0

        no_stability = copy.deepcopy(baseline)
        no_stability["config"]["require_projection_stability_capture"] = False
        assert validate(root, no_stability).returncode != 0

        rectangle_claim = copy.deepcopy(baseline)
        rectangle_claim["stage_records"][0]["concurrent_analysis"][
            "rectangle_boundaries"] = {"task_count": 1}
        assert validate(root, rectangle_claim).returncode != 0

        fit_claim = copy.deepcopy(baseline)
        fit_claim["homography"]["fit_requested"] = True
        assert validate(root, fit_claim).returncode != 0
    print("validate_gui_arena_centering_result_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
