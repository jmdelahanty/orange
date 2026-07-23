#!/usr/bin/env python3

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from validate_gui_guided_capture_result import validate_result  # noqa: E402


def valid_payload() -> dict:
    restore = {"state": "restored", "presented": True, "active": False}
    return {
        "schema_id": "orange.gui_guided_capture_smoke_result",
        "schema_version": 1,
        "status": "pass",
        "config": {"recipe": "black_reference"},
        "workflow": {
            "state": "complete",
            "terminal_outcome": "complete",
            "capture_group_id": "calgrp_test",
            "expected_camera_serials": ["2010093", "2010094"],
            "capture_group_membership": {"status": "complete"},
            "citrus_scene_consistency": {"status": "same_scene"},
            "citrus_scene_restore_status": restore,
        },
        "captures": [
            {
                "camera_serial": serial,
                "valid": True,
                "width": 4512,
                "height": 4512,
                "last_local_frame_id": 10,
                "last_camera_frame_id": 20,
            }
            for serial in ("2010093", "2010094")
        ],
        "persistence": {"requested": False},
        "shutdown": {"stream_stopped": True},
    }


def main() -> int:
    payload = valid_payload()
    assert validate_result(payload, ["2010093", "2010094"], "black_reference", False) == []

    payload = valid_payload()
    payload["workflow"]["citrus_scene_consistency"]["status"] = "changed"
    errors = validate_result(payload, ["2010093", "2010094"], "black_reference", False)
    assert any("citrus_scene_consistency" in error for error in errors)

    payload = valid_payload()
    payload["captures"].pop()
    errors = validate_result(payload, ["2010093", "2010094"], "black_reference", False)
    assert any("captured cameras" in error for error in errors)
    assert any("capture count" in error for error in errors)

    payload = valid_payload()
    payload["shutdown"]["stream_stopped"] = False
    errors = validate_result(payload, ["2010093", "2010094"], "black_reference", False)
    assert any("stream was not stopped" in error for error in errors)

    payload = valid_payload()
    payload["schema_version"] = 3
    payload["config"].update({
        "workflow_profile_id": "unobstructed_canvas_commissioning",
        "foreground_gray_u8": 255,
        "sweep_foreground_grays_u8": [80, 96],
        "sweep_repeats": 1,
        "include_arena_outline_reference": True,
    })
    payload["calibration_preflight"] = {
        "applied": True,
        "restore_attempted": True,
        "restore_ok": True,
        "capture_camera_settings": [
            {"camera_serial": serial, "frame_rate_hz": 5,
             "exposure_us": 100000, "sync_mode": "ptp_gate"}
            for serial in ("2010093", "2010094")
        ],
    }
    payload["ptp_capture_alignment"] = {"camera_count": 2, "span_ns": 20}
    sample_base = {
        "workflow": {"state": "complete", "terminal_outcome": "complete"},
        "captures": [
            {"camera_serial": serial, "valid": True}
            for serial in ("2010093", "2010094")
        ],
        "ptp_capture_alignment": {"camera_count": 2, "span_ns": 30},
    }
    payload["samples"] = [
        dict(sample_base, recipe="arena_outline", purpose="arena_projection",
             foreground_gray_u8=None, repeat_index=1),
        dict(sample_base, recipe="homography_grid", purpose="homography_grid",
             foreground_gray_u8=80, repeat_index=1),
        dict(sample_base, recipe="homography_grid", purpose="homography_grid",
             foreground_gray_u8=96, repeat_index=1),
    ]
    payload["session_policy"] = "one_session_per_sweep"
    errors = validate_result(
        payload,
        ["2010093", "2010094"],
        "black_reference",
        False,
        expected_profile="unobstructed_canvas_commissioning",
        expected_foreground_gray_u8=255,
        require_calibration_preflight=True,
        expected_frame_rate_hz=5,
        expected_exposure_us=100000,
        expected_sweep_foreground_grays_u8=[80, 96],
        expected_sweep_repeats=1,
        expect_arena_outline_reference=True,
        require_ptp_gate=True,
        max_ptp_capture_span_ns=100000,
    )
    assert errors == [], errors

    payload["samples"][1]["ptp_capture_alignment"]["span_ns"] = 20_000_000
    errors = validate_result(
        payload,
        ["2010093", "2010094"],
        "black_reference",
        False,
        expected_sweep_foreground_grays_u8=[80, 96],
        expected_sweep_repeats=1,
        expect_arena_outline_reference=True,
        max_ptp_capture_span_ns=100000,
    )
    assert any("PTP capture span" in error for error in errors)

    payload = valid_payload()
    payload["schema_version"] = 4
    sequence = [
        "black_reference",
        "uniform_gray",
        "arena_outline",
        "homography_rings",
        "verification_dots",
    ]
    payload["config"].update({
        "recipe_sequence": sequence,
        "fixture_aperture_shape": "circle",
    })
    sequence_purposes = [
        "validation_pattern",
        "validation_pattern",
        "arena_projection",
        "homography_grid",
        "verification_dots",
    ]
    payload["samples"] = [
        {
            "recipe": recipe,
            "purpose": purpose,
            "workflow": {"state": "complete", "terminal_outcome": "complete"},
            "captures": [
                {"camera_serial": serial, "valid": True}
                for serial in ("2010093", "2010094")
            ],
        }
        for recipe, purpose in zip(sequence, sequence_purposes)
    ]
    payload["session_policy"] = "one_session_per_recipe_sequence"
    errors = validate_result(
        payload,
        ["2010093", "2010094"],
        "black_reference",
        False,
        expected_recipe_sequence=sequence,
        expected_fixture_aperture_shape="circle",
    )
    assert errors == [], errors

    with tempfile.TemporaryDirectory() as temporary_text:
        candidate_dir = Path(temporary_text) / "homography_set_test"
        candidate_dir.mkdir()
        (candidate_dir / "candidate_set.json").write_text(
            "{}\n", encoding="utf-8"
        )
        payload["config"]["fit_homographies_after_capture"] = True
        payload["homography"] = {
            "fit_requested": True,
            "candidate_released_for_external_review": True,
            "promotion_requested": False,
            "runtime_authority_changed": False,
            "citrus_candidate_status": {
                "state": "rejected",
                "active": False,
                "candidate_set_id": "homography_set_test",
                "candidate_set_dir": str(candidate_dir),
                "candidates": [
                    {"homography_role": "operational_candidate"},
                    {"homography_role": "operational_candidate"},
                ],
            },
        }
        errors = validate_result(
            payload,
            ["2010093", "2010094"],
            "black_reference",
            False,
            expected_recipe_sequence=sequence,
            expected_fixture_aperture_shape="circle",
            expect_persisted_homography_candidate=True,
        )
        assert errors == [], errors

    print("validate_gui_guided_capture_result_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
