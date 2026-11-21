#!/usr/bin/env python3
"""Tests for the launch-configuration check and the image-levels freshness warning in
validate_gui_ptp_recording.py (session.launch_configuration v1, session.image_levels_check v3)."""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import validate_gui_ptp_recording as validator  # noqa: E402

TOKEN_A = "sha256:" + "a" * 64
TOKEN_B = "sha256:" + "b" * 64


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def launch_block(changed: list[str]) -> dict:
    preset_on_disk = TOKEN_B if "camera_preset" in changed else TOKEN_A
    app_on_disk = TOKEN_B if "app_config" in changed else TOKEN_A
    return {
        "schema_id": "orange.recording.launch_configuration",
        "schema_version": 1,
        "captured_at_utc": "2026-11-21T09:00:00Z",
        "camera_preset": {
            "name": "100_cam4_ptp_fourcam",
            "path": "/home/x/orange_data/config/local/100_cam4_ptp_fourcam",
            "hash_rule": "orange_camera_config_dir_sha256_v1",
            "loaded_at_utc": "2026-11-21T08:00:00Z",
            "at_launch": {"sha256": TOKEN_A, "files": [{"name": "Cam1.json", "sha256": TOKEN_A}], "error": None},
            "on_disk_at_record_start": {"sha256": preset_on_disk,
                                        "files": [{"name": "Cam1.json", "sha256": preset_on_disk}], "error": None},
            "changed": "camera_preset" in changed,
            "changed_files": ["Cam1.json"] if "camera_preset" in changed else [],
        },
        "app_config": {
            "path": "/home/x/orange_data/config/app/default.json",
            "loaded_at_utc": "2026-11-21T07:59:00Z",
            "loaded_status": "loaded",
            "loaded_sha256": TOKEN_A,
            "on_disk_status": "loaded",
            "on_disk_sha256": app_on_disk,
            "changed": "app_config" in changed,
        },
        "configuration_changed_since_launch": bool(changed),
        "changed_since_launch": changed,
    }


def run_launch(block: dict | None) -> validator.Reporter:
    snapshot = {"session": {} if block is None else {"launch_configuration": block}}
    reporter = validator.Reporter(verbose=False)
    validator.check_launch_configuration(reporter, snapshot)
    return reporter


def test_launch_configuration() -> None:
    r = run_launch(launch_block([]))
    require(not r.failures and not r.warnings and any("nothing changed" in p for p in r.passes),
            "unchanged launch configuration passes")
    r = run_launch(launch_block(["camera_preset", "app_config"]))
    require(not r.failures and len(r.warnings) == 2, "both changes are warnings, never failures")
    require(any("Cam1.json" in w for w in r.warnings), "the changed camera JSON is named")
    require(any("effective_configuration" in w for w in r.warnings), "the app config warning explains the seal")
    r = run_launch(None)
    require(not r.failures and len(r.warnings) == 1, "an older build without the block is a warning")
    bad = launch_block([])
    bad["schema_version"] = 2
    require(run_launch(bad).failures, "an unknown version fails")


def image_levels_snapshot(camera: dict) -> dict:
    return {"session": {"image_levels_check": {
        "schema_id": "orange.recording.image_levels_check", "schema_version": 3, "enabled": True,
        "cameras": {"2010093": camera}}}}


def run_levels(camera: dict) -> validator.Reporter:
    reporter = validator.Reporter(verbose=False)
    validator.check_lens_feedback(reporter, image_levels_snapshot(camera), ["2010093"])
    return reporter


def test_image_levels_freshness() -> None:
    camera = {
        "status": "ok", "warnings": [], "checked_region": "whole_frame",
        "measurement": {"ok": True, "statistics": {"mean": 120.0, "saturated_fraction": 0.0, "p99": 200, "p999": 210}},
        "measurement_trigger": "after_recording", "measurement_age_s": 4.2, "freshness": "fresh", "stale_reasons": [],
    }
    r = run_levels(camera)
    require(not any("stale" in w for w in r.warnings), "a fresh measurement raises no staleness warning")
    camera = {**camera, "freshness": "stale", "measurement_age_s": 900.5,
              "stale_reasons": ["measured_before_previous_recording", "older_than_max_age"]}
    r = run_levels(camera)
    require(not r.failures, "a stale measurement never fails a recording")
    require(any("stale" in w and "900.5" in w for w in r.warnings), "a stale measurement is a warning with its age")


def main() -> int:
    for test in (test_launch_configuration, test_image_levels_freshness):
        test()
        print(f"ok   {test.__name__}")
    print("validate_launch_configuration_tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
