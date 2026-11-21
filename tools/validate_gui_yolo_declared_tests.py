#!/usr/bin/env python3
"""Tests for validate_gui_ptp_recording.py checking YOLO only where the recording
declares detections (realtime_products), 2026-11-21."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import validate_gui_ptp_recording as validator  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_session(folder: Path, statuses: dict[str, str]) -> None:
    cameras = {serial: {"detections": {"status": status}, "pose": {"status": status}}
               for serial, status in statuses.items()}
    (folder / "recording_session.json").write_text(json.dumps({"realtime_products": {"cameras": cameras}}))


def run_check(summary: dict, cameras: list[str], absent: set[str]) -> validator.Reporter:
    reporter = validator.Reporter(verbose=False)
    validator.check_yolo(reporter, summary, cameras, 10.0, None, None, None, None, None, None, absent)
    return reporter


def test_declared_absent_cameras() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        write_session(folder, {"2010093": "absent", "2010094": "present"})
        require(validator.cameras_declaring_detections_absent(folder) == {"2010093"}, "only the absent camera")
        (folder / "recording_session.json").write_text("{}")
        require(validator.cameras_declaring_detections_absent(folder) == set(), "no block, nothing skipped")
    with tempfile.TemporaryDirectory() as tmp:
        require(validator.cameras_declaring_detections_absent(Path(tmp)) == set(), "no manifest, nothing skipped")


def test_absent_detections_skip_yolo_checks() -> None:
    reporter = run_check({}, ["2010093", "2010094"], {"2010093", "2010094"})
    require(not reporter.failures, f"analytics off by declaration is not a failure: {reporter.failures}")
    require(all("YOLO checks skipped" in p for p in reporter.passes), reporter.passes)


def test_undeclared_missing_yolo_still_fails() -> None:
    reporter = run_check({}, ["2010093", "2010094"], {"2010093"})
    require(reporter.failures == ["Cam2010094 missing YOLO perf CSV"], reporter.failures)


def test_yolo_present_but_declared_absent_fails() -> None:
    summary = {"yolo": {"2010093": {"rows": 10, "ok_rows": 10}}}
    reporter = run_check(summary, ["2010093"], {"2010093"})
    require(any("declares detections absent" in f for f in reporter.failures), reporter.failures)


def main() -> int:
    for test in (test_declared_absent_cameras, test_absent_detections_skip_yolo_checks,
                 test_undeclared_missing_yolo_still_fails, test_yolo_present_but_declared_absent_fails):
        test()
        print(f"[PASS] {test.__name__}")
    print("validate_gui_yolo_declared_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
