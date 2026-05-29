#!/usr/bin/env python3
"""Focused tests for GUI local-control recording-session validation."""

from __future__ import annotations

import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

import validate_gui_ptp_recording as validator  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def manifest_with_control(**overrides: object) -> dict:
    control = {
        "source": "orange_gui_local_control",
        "method": "citrus_completion",
        "request_id": "citrus_completion:manual-stop-all:stopped:stopped_by_local_control",
        "operation_id": "manual-stop-all",
        "command_source": "citrus",
        "terminal_state": "stopped",
        "reason": "stopped_by_local_control",
        "received_at_utc": "2026-05-29T15:00:00Z",
        "stop_triggered_at_utc": "2026-05-29T15:00:10Z",
        "drain_completed": True,
        "drain_completed_at_utc": "2026-05-29T15:00:12Z",
        "drain_timed_out": False,
        "last_event": "finalized",
        "ack_state": "executed",
    }
    control.update(overrides)
    return {"recording": {"control": control}}


def check(
    manifest: dict,
    *,
    expected_method: str | None = "citrus_completion",
    expected_operation_id: str | None = None,
    expected_command_source: str | None = "citrus",
    expected_terminal_state: str | None = "stopped",
    expected_reason: str | None = "stopped_by_local_control",
    expected_ack_state: str | None = "executed",
) -> validator.Reporter:
    reporter = validator.Reporter(verbose=False)
    validator.check_local_control_stop_expectations(
        reporter,
        manifest,
        expected_method,
        expected_operation_id,
        expected_command_source,
        expected_terminal_state,
        expected_reason,
        expected_ack_state,
    )
    return reporter


def test_stop_all_expectations_pass_for_stopped_citrus_completion() -> None:
    reporter = check(manifest_with_control())
    require(not reporter.failures, f"unexpected failures: {reporter.failures}")
    for expected_pass in [
        "recording_session recording.control method=citrus_completion",
        "recording_session recording.control command_source=citrus",
        "recording_session recording.control terminal_state=stopped",
        "recording_session recording.control reason=stopped_by_local_control",
        "recording_session recording.control ack_state=executed",
    ]:
        require(expected_pass in reporter.passes, f"missing pass: {expected_pass}")


def test_terminal_state_mismatch_fails() -> None:
    reporter = check(manifest_with_control(terminal_state="completed"))
    require(
        any("terminal_state='completed'" in failure for failure in reporter.failures),
        f"expected terminal-state failure, got: {reporter.failures}",
    )


def test_reason_mismatch_fails() -> None:
    reporter = check(manifest_with_control(reason="protocol_finished"))
    require(
        any("reason='protocol_finished'" in failure for failure in reporter.failures),
        f"expected reason failure, got: {reporter.failures}",
    )


def test_ack_state_mismatch_fails() -> None:
    reporter = check(manifest_with_control(ack_state="executing"))
    require(
        any("ack_state='executing'" in failure for failure in reporter.failures),
        f"expected ACK-state failure, got: {reporter.failures}",
    )


def main() -> int:
    tests = [
        test_stop_all_expectations_pass_for_stopped_citrus_completion,
        test_terminal_state_mismatch_fails,
        test_reason_mismatch_fails,
        test_ack_state_mismatch_fails,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("validate_gui_local_control_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
