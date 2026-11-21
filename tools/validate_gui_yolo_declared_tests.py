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


def write_binding(folder: Path, intent: str, requests: int, receipts: int, status: str | None) -> None:
    contexts = {str(2010093 + i): {"recording_intent": intent} for i in range(4)}
    (folder / "recording_snapshot_start.json").write_text(json.dumps({"session": {"recording_contexts": contexts}}))
    bindings = folder / "recording_observation_bindings"
    for sub, count in (("requests", requests), ("receipts", receipts)):
        (bindings / sub).mkdir(parents=True, exist_ok=True)
        for i in range(count):
            (bindings / sub / f"obsctx_{i}.json").write_text("{}")
    if status is not None:
        (bindings / "finalized_collection.json").write_text(json.dumps({"binding_status": status}))


def test_stimulus_recording_needs_a_finalized_binding() -> None:
    cases = [("stimulus_experiment", 4, 4, "bound", True), ("stimulus_experiment", 4, 0, None, False),
             ("stimulus_experiment", 4, 3, "bound", False), ("stimulus_experiment", 4, 4, "not_bound", False)]
    for intent, requests, receipts, status, ok in cases:
        with tempfile.TemporaryDirectory() as tmp:
            write_binding(Path(tmp), intent, requests, receipts, status)
            reporter = validator.Reporter(verbose=False)
            validator.check_stimulus_binding_finalized(reporter, Path(tmp))
            require((not reporter.failures) == ok, f"{intent} {requests}/{receipts} {status}: {reporter.failures}")
    with tempfile.TemporaryDirectory() as tmp:
        write_binding(Path(tmp), "recording_only", 0, 0, None)
        reporter = validator.Reporter(verbose=False)
        validator.check_stimulus_binding_finalized(reporter, Path(tmp))
        require(not reporter.failures and not reporter.passes, "recording_only needs no binding")


def main() -> int:
    for test in (test_declared_absent_cameras, test_absent_detections_skip_yolo_checks,
                 test_undeclared_missing_yolo_still_fails, test_yolo_present_but_declared_absent_fails,
                 test_stimulus_recording_needs_a_finalized_binding):
        test()
        print(f"[PASS] {test.__name__}")
    print("validate_gui_yolo_declared_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
