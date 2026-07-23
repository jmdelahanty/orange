#!/usr/bin/env python3
"""Validate a machine-readable Orange guided grouped-capture GUI smoke result."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def parse_csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def validate_result(
    payload: dict[str, Any],
    expected_cameras: list[str],
    expected_recipe: str,
    require_saved: bool,
    expected_profile: str = "",
    expected_foreground_gray_u8: int | None = None,
    require_calibration_preflight: bool = False,
    expected_frame_rate_hz: int | None = None,
    expected_exposure_us: int | None = None,
    expected_sweep_foreground_grays_u8: list[int] | None = None,
    expected_sweep_repeats: int | None = None,
    expect_arena_outline_reference: bool = False,
    require_ptp_gate: bool = False,
    max_ptp_capture_span_ns: int | None = None,
    expected_recipe_sequence: list[str] | None = None,
    expected_fixture_aperture_shape: str = "",
    expect_persisted_homography_candidate: bool = False,
) -> list[str]:
    errors: list[str] = []

    def require(condition: bool, message: str) -> None:
        if not condition:
            errors.append(message)

    require(
        payload.get("schema_id") == "orange.gui_guided_capture_smoke_result",
        f"unexpected schema_id={payload.get('schema_id')!r}",
    )
    require(payload.get("schema_version") in (1, 2, 3, 4),
            "schema_version must be 1, 2, 3, or 4")
    require(payload.get("status") == "pass", f"status={payload.get('status')!r}")

    config = payload.get("config") if isinstance(payload.get("config"), dict) else {}
    require(config.get("recipe") == expected_recipe,
            f"recipe={config.get('recipe')!r}, expected {expected_recipe!r}")
    if expected_recipe_sequence is not None:
        require(
            config.get("recipe_sequence") == expected_recipe_sequence,
            f"recipe_sequence={config.get('recipe_sequence')!r}, "
            f"expected {expected_recipe_sequence!r}",
        )
    if expected_fixture_aperture_shape:
        require(
            config.get("fixture_aperture_shape") == expected_fixture_aperture_shape,
            f"fixture_aperture_shape={config.get('fixture_aperture_shape')!r}, "
            f"expected {expected_fixture_aperture_shape!r}",
        )
    if expected_profile:
        require(
            config.get("workflow_profile_id") == expected_profile,
            f"workflow_profile_id={config.get('workflow_profile_id')!r}, "
            f"expected {expected_profile!r}",
        )
    if expected_foreground_gray_u8 is not None:
        require(
            config.get("foreground_gray_u8") == expected_foreground_gray_u8,
            f"foreground_gray_u8={config.get('foreground_gray_u8')!r}, "
            f"expected {expected_foreground_gray_u8}",
        )
    if expected_sweep_foreground_grays_u8 is not None:
        require(
            config.get("sweep_foreground_grays_u8") == expected_sweep_foreground_grays_u8,
            f"sweep_foreground_grays_u8={config.get('sweep_foreground_grays_u8')!r}, "
            f"expected {expected_sweep_foreground_grays_u8!r}",
        )
        require(
            config.get("sweep_repeats") == expected_sweep_repeats,
            f"sweep_repeats={config.get('sweep_repeats')!r}, "
            f"expected {expected_sweep_repeats}",
        )
        require(
            config.get("include_arena_outline_reference") is
            expect_arena_outline_reference,
            "include_arena_outline_reference does not match expectation",
        )

    preflight = (
        payload.get("calibration_preflight")
        if isinstance(payload.get("calibration_preflight"), dict)
        else {}
    )
    if require_calibration_preflight:
        require(preflight.get("applied") is True, "calibration preflight was not applied")
        require(preflight.get("restore_attempted") is True,
                "calibration preflight restore was not attempted")
        require(preflight.get("restore_ok") is True,
                f"calibration preflight restore failed: {preflight.get('restore_status')!r}")
        settings = preflight.get("capture_camera_settings")
        require(isinstance(settings, list), "preflight capture camera settings are missing")
        if isinstance(settings, list):
            require(len(settings) == len(expected_cameras),
                    "preflight camera setting count does not match camera scope")
            for setting in settings:
                if not isinstance(setting, dict):
                    errors.append("preflight camera setting is not an object")
                    continue
                if expected_frame_rate_hz is not None:
                    require(setting.get("frame_rate_hz") == expected_frame_rate_hz,
                            f"camera {setting.get('camera_serial')} frame rate "
                            f"{setting.get('frame_rate_hz')!r} != {expected_frame_rate_hz}")
                if expected_exposure_us is not None:
                    require(setting.get("exposure_us") == expected_exposure_us,
                            f"camera {setting.get('camera_serial')} exposure "
                            f"{setting.get('exposure_us')!r} != {expected_exposure_us}")
                if require_ptp_gate:
                    require(setting.get("sync_mode") == "ptp_gate",
                            f"camera {setting.get('camera_serial')} sync_mode "
                            f"{setting.get('sync_mode')!r} != 'ptp_gate'")

    workflow = payload.get("workflow") if isinstance(payload.get("workflow"), dict) else {}
    require(workflow.get("state") == "complete",
            f"workflow.state={workflow.get('state')!r}")
    require(workflow.get("terminal_outcome") == "complete",
            f"workflow.terminal_outcome={workflow.get('terminal_outcome')!r}")
    membership = (
        workflow.get("capture_group_membership")
        if isinstance(workflow.get("capture_group_membership"), dict)
        else {}
    )
    require(membership.get("status") == "complete",
            f"capture_group_membership.status={membership.get('status')!r}")
    consistency = (
        workflow.get("citrus_scene_consistency")
        if isinstance(workflow.get("citrus_scene_consistency"), dict)
        else {}
    )
    require(consistency.get("status") == "same_scene",
            f"citrus_scene_consistency.status={consistency.get('status')!r}")
    restore = (
        workflow.get("citrus_scene_restore_status")
        if isinstance(workflow.get("citrus_scene_restore_status"), dict)
        else {}
    )
    require(restore.get("state") == "restored",
            f"citrus_scene_restore_status.state={restore.get('state')!r}")
    require(restore.get("presented") is True,
            "citrus_scene_restore_status.presented must be true")
    require(restore.get("active") is False,
            "citrus_scene_restore_status.active must be false")

    expected_set = set(expected_cameras)
    workflow_expected = workflow.get("expected_camera_serials")
    require(isinstance(workflow_expected, list),
            "workflow.expected_camera_serials must be an array")
    if isinstance(workflow_expected, list):
        require(set(map(str, workflow_expected)) == expected_set,
                f"workflow expected cameras={workflow_expected!r}, expected={expected_cameras!r}")

    captures = payload.get("captures") if isinstance(payload.get("captures"), list) else []
    captured_serials: list[str] = []
    for index, capture in enumerate(captures):
        if not isinstance(capture, dict):
            errors.append(f"captures[{index}] is not an object")
            continue
        serial = str(capture.get("camera_serial", ""))
        captured_serials.append(serial)
        require(capture.get("valid") is True, f"capture {serial or index} is not valid")
        require(isinstance(capture.get("width"), int) and capture["width"] > 0,
                f"capture {serial or index} width is invalid")
        require(isinstance(capture.get("height"), int) and capture["height"] > 0,
                f"capture {serial or index} height is invalid")
        require(isinstance(capture.get("last_local_frame_id"), int) and
                capture["last_local_frame_id"] > 0,
                f"capture {serial or index} local frame id is invalid")
        require(isinstance(capture.get("last_camera_frame_id"), int) and
                capture["last_camera_frame_id"] > 0,
                f"capture {serial or index} camera frame id is invalid")
    require(set(captured_serials) == expected_set,
            f"captured cameras={captured_serials!r}, expected={expected_cameras!r}")
    require(len(captures) == len(expected_cameras),
            f"capture count={len(captures)}, expected={len(expected_cameras)}")
    if max_ptp_capture_span_ns is not None:
        alignment = (
            payload.get("ptp_capture_alignment")
            if isinstance(payload.get("ptp_capture_alignment"), dict)
            else {}
        )
        require(alignment.get("camera_count") == len(expected_cameras),
                f"top-level PTP alignment has incomplete timestamps: {alignment!r}")
        span_ns = alignment.get("span_ns")
        require(isinstance(span_ns, int) and span_ns <= max_ptp_capture_span_ns,
                f"top-level PTP capture span {span_ns!r} ns exceeds "
                f"{max_ptp_capture_span_ns} ns")

    shutdown = payload.get("shutdown") if isinstance(payload.get("shutdown"), dict) else {}
    require(shutdown.get("stream_stopped") is True, "Orange stream was not stopped")

    persistence = (
        payload.get("persistence")
        if isinstance(payload.get("persistence"), dict)
        else {}
    )
    if require_saved:
        require(persistence.get("requested") is True, "capture save was not requested")
        require(bool(persistence.get("session_id")), "saved session_id is missing")
        session_dir = persistence.get("session_dir")
        require(isinstance(session_dir, str) and Path(session_dir).is_dir(),
                f"saved session_dir is missing: {session_dir!r}")

    if expected_sweep_foreground_grays_u8 is not None and expected_sweep_repeats is not None:
        samples = payload.get("samples") if isinstance(payload.get("samples"), list) else []
        expected_pairs = [
            (gray, repeat)
            for gray in expected_sweep_foreground_grays_u8
            for repeat in range(1, expected_sweep_repeats + 1)
        ]
        grid_samples = [
            sample for sample in samples
            if isinstance(sample, dict) and sample.get("recipe") != "arena_outline"
        ]
        actual_pairs = [
            (sample.get("foreground_gray_u8"), sample.get("repeat_index"))
            for sample in grid_samples
        ]
        require(actual_pairs == expected_pairs,
                f"sweep sample sequence={actual_pairs!r}, expected={expected_pairs!r}")
        require(payload.get("session_policy") == "one_session_per_sweep",
                "sweep result must declare one_session_per_sweep")
        outline_samples = [
            sample for sample in samples
            if isinstance(sample, dict) and sample.get("recipe") == "arena_outline"
        ]
        require(
            len(outline_samples) == (1 if expect_arena_outline_reference else 0),
            f"arena outline reference count={len(outline_samples)}, "
            f"expected {1 if expect_arena_outline_reference else 0}",
        )
        if outline_samples:
            require(samples[0] is outline_samples[0],
                    "arena outline reference must be the first sweep sample")
            require(outline_samples[0].get("purpose") == "arena_projection",
                    "arena outline reference purpose must be arena_projection")
        session_ids: set[str] = set()
        for index, sample in enumerate(samples):
            if not isinstance(sample, dict):
                errors.append(f"samples[{index}] is not an object")
                continue
            sample_workflow = sample.get("workflow", {})
            require(sample_workflow.get("state") == "complete",
                    f"samples[{index}].workflow.state is not complete")
            require(sample_workflow.get("terminal_outcome") == "complete",
                    f"samples[{index}].workflow.terminal_outcome is not complete")
            sample_captures = sample.get("captures", [])
            sample_serials = {
                str(capture.get("camera_serial", ""))
                for capture in sample_captures
                if isinstance(capture, dict) and capture.get("valid") is True
            }
            require(sample_serials == expected_set,
                    f"samples[{index}] cameras={sorted(sample_serials)!r}, "
                    f"expected={expected_cameras!r}")
            if require_saved:
                sample_persistence = sample.get("persistence", {})
                sample_session_id = str(sample_persistence.get("session_id", ""))
                require(bool(sample_session_id),
                        f"samples[{index}] saved session_id is missing")
                if sample_session_id:
                    session_ids.add(sample_session_id)
            if max_ptp_capture_span_ns is not None:
                alignment = sample.get("ptp_capture_alignment", {})
                require(alignment.get("camera_count") == len(expected_cameras),
                        f"samples[{index}] has incomplete PTP timestamps: {alignment!r}")
                span_ns = alignment.get("span_ns")
                require(isinstance(span_ns, int) and span_ns <= max_ptp_capture_span_ns,
                        f"samples[{index}] PTP capture span {span_ns!r} ns exceeds "
                        f"{max_ptp_capture_span_ns} ns")
        if require_saved:
            require(len(session_ids) == 1,
                    f"sweep must use one calibration session, got {sorted(session_ids)!r}")

    if expected_recipe_sequence is not None:
        samples = payload.get("samples") if isinstance(payload.get("samples"), list) else []
        actual_recipes = [
            sample.get("recipe") for sample in samples if isinstance(sample, dict)
        ]
        require(actual_recipes == expected_recipe_sequence,
                f"recipe sample sequence={actual_recipes!r}, "
                f"expected={expected_recipe_sequence!r}")
        require(payload.get("session_policy") == "one_session_per_recipe_sequence",
                "recipe sequence must declare one_session_per_recipe_sequence")
        expected_purposes = {
            "black_reference": "validation_pattern",
            "uniform_gray": "validation_pattern",
            "arena_outline": "arena_projection",
            "experimental_area_center_and_outline": "crosshair_alignment",
            "homography_grid": "homography_grid",
            "homography_rings": "homography_grid",
            "verification_dots": "verification_dots",
        }
        session_ids: set[str] = set()
        for index, sample in enumerate(samples):
            if not isinstance(sample, dict):
                errors.append(f"samples[{index}] is not an object")
                continue
            recipe = str(sample.get("recipe", ""))
            require(sample.get("purpose") == expected_purposes.get(recipe),
                    f"samples[{index}] purpose={sample.get('purpose')!r} "
                    f"does not match recipe={recipe!r}")
            sample_workflow = sample.get("workflow", {})
            require(sample_workflow.get("state") == "complete",
                    f"samples[{index}].workflow.state is not complete")
            require(sample_workflow.get("terminal_outcome") == "complete",
                    f"samples[{index}].workflow.terminal_outcome is not complete")
            sample_captures = sample.get("captures", [])
            sample_serials = {
                str(capture.get("camera_serial", ""))
                for capture in sample_captures
                if isinstance(capture, dict) and capture.get("valid") is True
            }
            require(sample_serials == expected_set,
                    f"samples[{index}] cameras={sorted(sample_serials)!r}, "
                    f"expected={expected_cameras!r}")
            if require_saved:
                sample_persistence = sample.get("persistence", {})
                sample_session_id = str(sample_persistence.get("session_id", ""))
                require(bool(sample_session_id),
                        f"samples[{index}] saved session_id is missing")
                if sample_session_id:
                    session_ids.add(sample_session_id)
            if max_ptp_capture_span_ns is not None:
                alignment = sample.get("ptp_capture_alignment", {})
                require(alignment.get("camera_count") == len(expected_cameras),
                        f"samples[{index}] has incomplete PTP timestamps: {alignment!r}")
                span_ns = alignment.get("span_ns")
                require(isinstance(span_ns, int) and span_ns <= max_ptp_capture_span_ns,
                        f"samples[{index}] PTP capture span {span_ns!r} ns exceeds "
                        f"{max_ptp_capture_span_ns} ns")
        if require_saved:
            require(len(session_ids) == 1,
                    f"recipe sequence must use one calibration session, got "
                    f"{sorted(session_ids)!r}")

    if expect_persisted_homography_candidate:
        homography = (
            payload.get("homography")
            if isinstance(payload.get("homography"), dict)
            else {}
        )
        require(config.get("fit_homographies_after_capture") is True,
                "result does not declare requested post-capture homography fitting")
        require(homography.get("fit_requested") is True,
                "Citrus homography fit was not requested successfully")
        require(homography.get("candidate_released_for_external_review") is True,
                "persisted homography candidate was not safely released for review")
        require(homography.get("promotion_requested") is False,
                "holder capture must not automatically promote homographies")
        require(homography.get("runtime_authority_changed") is False,
                "holder capture unexpectedly changed runtime homography authority")
        candidate = (
            homography.get("citrus_candidate_status")
            if isinstance(homography.get("citrus_candidate_status"), dict)
            else {}
        )
        require(candidate.get("state") == "rejected",
                f"persisted candidate terminal state={candidate.get('state')!r}")
        require(candidate.get("active") is False,
                "persisted candidate transaction remains active")
        require(bool(candidate.get("candidate_set_id")),
                "candidate_set_id is missing")
        candidate_set_dir = candidate.get("candidate_set_dir")
        require(isinstance(candidate_set_dir, str) and
                (Path(candidate_set_dir) / "candidate_set.json").is_file(),
                f"persisted candidate set is missing: {candidate_set_dir!r}")
        rows = candidate.get("candidates")
        require(isinstance(rows, list) and len(rows) == len(expected_cameras),
                "candidate count does not match camera scope")
        if isinstance(rows, list):
            for row in rows:
                require(row.get("homography_role") == "operational_candidate",
                        "holder candidate has the wrong homography_role")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("result_json", type=Path)
    parser.add_argument("--expected-cameras", required=True)
    parser.add_argument("--expected-recipe", required=True)
    parser.add_argument("--expected-profile", default="")
    parser.add_argument("--require-saved", action="store_true")
    parser.add_argument("--expected-foreground-gray-u8", type=int)
    parser.add_argument("--require-calibration-preflight", action="store_true")
    parser.add_argument("--expected-frame-rate-hz", type=int)
    parser.add_argument("--expected-exposure-us", type=int)
    parser.add_argument("--expected-sweep-foreground-grays-u8")
    parser.add_argument("--expected-sweep-repeats", type=int)
    parser.add_argument("--expect-arena-outline-reference", action="store_true")
    parser.add_argument("--require-ptp-gate", action="store_true")
    parser.add_argument("--max-ptp-capture-span-ns", type=int)
    parser.add_argument("--expected-recipe-sequence")
    parser.add_argument("--expected-fixture-aperture-shape", default="")
    parser.add_argument("--expect-persisted-homography-candidate", action="store_true")
    args = parser.parse_args()

    try:
        payload = json.loads(args.result_json.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"FAIL: could not read result JSON: {error}", file=sys.stderr)
        return 1
    if not isinstance(payload, dict):
        print("FAIL: result JSON root is not an object", file=sys.stderr)
        return 1

    errors = validate_result(
        payload,
        parse_csv(args.expected_cameras),
        args.expected_recipe,
        args.require_saved,
        args.expected_profile,
        args.expected_foreground_gray_u8,
        args.require_calibration_preflight,
        args.expected_frame_rate_hz,
        args.expected_exposure_us,
        ([int(value) for value in parse_csv(args.expected_sweep_foreground_grays_u8)]
         if args.expected_sweep_foreground_grays_u8 else None),
        args.expected_sweep_repeats,
        args.expect_arena_outline_reference,
        args.require_ptp_gate,
        args.max_ptp_capture_span_ns,
        (parse_csv(args.expected_recipe_sequence)
         if args.expected_recipe_sequence is not None else None),
        args.expected_fixture_aperture_shape,
        args.expect_persisted_homography_candidate,
    )
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print(
        "PASS: guided GUI capture"
        f" recipe={args.expected_recipe}"
        f" cameras={args.expected_cameras}"
        f" group={payload.get('workflow', {}).get('capture_group_id', '')}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
