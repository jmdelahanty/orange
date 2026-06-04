#!/usr/bin/env python3
"""Focused tests for GUI local-control recording-session validation."""

from __future__ import annotations

import json
import sys
import tempfile
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


def write_local_control_event_log(
    path: Path,
    *,
    generic_stop_enabled: bool = False,
    citrus_completion_enabled: bool = True,
) -> None:
    request_id = "citrus_completion:manual-stop-all:stopped:stopped_by_local_control"
    operation_id = "manual-stop-all"
    rows = [
        {
            "received_at_utc": "2026-05-29T15:00:00Z",
            "request": {
                "method": "citrus_completion",
                "request_id": request_id,
                "operation_id": operation_id,
                "source": "citrus",
                "params": {
                    "experiment_id": "manual-stop-all",
                    "terminal_state": "stopped",
                    "reason": "stopped_by_local_control",
                    "grace_seconds": 10,
                },
            },
            "response": {
                "method": "citrus_completion",
                "request_id": request_id,
                "operation_id": operation_id,
                "ok": True,
                "accepted": True,
                "queued_for_gui_thread": True,
                "responded_at_utc": "2026-05-29T15:00:00Z",
            },
        },
        {
            "schema_id": "orange.local_control.gui_event",
            "schema_version": 1,
            "event": "gui_command_accepted",
            "event_at_utc": "2026-05-29T15:00:00Z",
            "request_id": request_id,
            "operation_id": operation_id,
            "method": "citrus_completion",
            "command_source": "citrus",
            "received_at_utc": "2026-05-29T15:00:00Z",
            "start_enabled": False,
            "stop_enabled": generic_stop_enabled,
            "stop_recording_enabled": generic_stop_enabled,
            "citrus_completion_enabled": citrus_completion_enabled,
        },
        {
            "schema_id": "orange.local_control.gui_event",
            "schema_version": 1,
            "event": "recording_stop_scheduled",
            "event_at_utc": "2026-05-29T15:00:00Z",
            "request_id": request_id,
            "operation_id": operation_id,
            "method": "citrus_completion",
            "command_source": "citrus",
            "experiment_id": "manual-stop-all",
            "terminal_state": "stopped",
            "reason": "stopped_by_local_control",
            "grace_seconds": 10,
        },
        {
            "schema_id": "orange.local_control.gui_event",
            "schema_version": 1,
            "event": "recording_stop_triggered",
            "event_at_utc": "2026-05-29T15:00:10Z",
            "request_id": request_id,
            "operation_id": operation_id,
            "method": "citrus_completion",
            "command_source": "citrus",
            "experiment_id": "manual-stop-all",
            "terminal_state": "stopped",
            "reason": "stopped_by_local_control",
        },
        {
            "schema_id": "orange.local_control.gui_event",
            "schema_version": 1,
            "event": "recording_drain_finalized",
            "event_at_utc": "2026-05-29T15:00:12Z",
            "request_id": request_id,
            "operation_id": operation_id,
            "method": "citrus_completion",
            "command_source": "citrus",
            "experiment_id": "manual-stop-all",
            "terminal_state": "stopped",
            "reason": "stopped_by_local_control",
            "drain_timed_out": False,
            "health": "ok",
            "error_code": "",
        },
    ]
    path.write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows),
        encoding="utf-8",
    )


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


def test_local_control_event_log_passes_for_citrus_only_stop_gate() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "orange_local_control.events.jsonl"
        write_local_control_event_log(path)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest_with_control(),
            str(path),
            required=True,
        )
        require(check["ok"], f"expected event-log check to pass: {check}")
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(
            any(
                item.get("request_id")
                == "citrus_completion:manual-stop-all:stopped:stopped_by_local_control"
                for item in check["request_chains"]
            ),
            "event-log check should summarize the Citrus completion request chain",
        )


def test_local_control_event_log_defaults_to_manifest_copy() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / "orange_local_control.events.jsonl"
        write_local_control_event_log(path)
        copied_bytes = path.stat().st_size
        manifest = manifest_with_control(
            event_log={
                "source_path": "/tmp/orange_local_control.sock.events.jsonl",
                "copied_path": str(path),
                "relative_path": path.name,
                "copied": True,
                "copied_at_utc": "2026-05-29T15:00:12Z",
                "bytes": copied_bytes,
            }
        )
        manifest["recording_folder"] = str(root)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest,
            "",
            required=True,
        )
        require(check["ok"], f"expected manifest event-log check to pass: {check}")
        require(check["path"] == str(path), f"expected copied path, got: {check['path']}")
        require(
            check["manifest_event_log"].get("copied") is True,
            "event-log check should report manifest copy metadata",
        )


def test_local_control_event_log_prefers_relative_manifest_copy() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / "orange_local_control.events.jsonl"
        write_local_control_event_log(path)
        copied_bytes = path.stat().st_size
        stale_absolute_path = root / "old_location" / "orange_local_control.events.jsonl"
        manifest = manifest_with_control(
            event_log={
                "source_path": "/tmp/orange_local_control.sock.events.jsonl",
                "copied_path": str(stale_absolute_path),
                "relative_path": path.name,
                "copied": True,
                "copied_at_utc": "2026-05-29T15:00:12Z",
                "bytes": copied_bytes,
            }
        )
        manifest["recording_folder"] = str(root)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest,
            "",
            required=True,
        )
        require(check["ok"], f"expected relative event-log check to pass: {check}")
        require(
            check["path"] == str(path),
            f"expected relative path to win over stale absolute path, got: {check['path']}",
        )


def test_local_control_event_log_manifest_copy_validates_when_not_required() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / "orange_local_control.events.jsonl"
        write_local_control_event_log(path)
        copied_bytes = path.stat().st_size
        manifest = manifest_with_control(
            event_log={
                "source_path": "/tmp/orange_local_control.sock.events.jsonl",
                "relative_path": path.name,
                "copied": True,
                "copied_at_utc": "2026-05-29T15:00:12Z",
                "bytes": copied_bytes,
            }
        )
        manifest["recording_folder"] = str(root)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest,
            "",
            required=False,
        )
        require(check["ok"], f"expected manifest event-log check to pass: {check}")
        require(check["required"] is False, "explicit required flag should remain false")
        require(
            check["required_by_manifest"] is True,
            "manifest event-log metadata should make validation active",
        )
        require(check["path"] == str(path), f"expected relative copied path, got: {check['path']}")


def test_local_control_event_log_manifest_copy_missing_fails_even_when_not_required() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        missing_path = root / "orange_local_control.events.jsonl"
        manifest = manifest_with_control(
            event_log={
                "source_path": "/tmp/orange_local_control.sock.events.jsonl",
                "relative_path": missing_path.name,
                "copied": True,
                "copied_at_utc": "2026-05-29T15:00:12Z",
                "bytes": 1024,
            }
        )
        manifest["recording_folder"] = str(root)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest,
            "",
            required=False,
        )
        require(not check["ok"], "expected missing manifest event-log check to fail")
        require(
            any("Orange local-control event log missing" in failure for failure in reporter.failures),
            f"expected missing event-log failure, got: {reporter.failures}",
        )


def test_local_control_event_log_manifest_copy_byte_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / "orange_local_control.events.jsonl"
        write_local_control_event_log(path)
        manifest = manifest_with_control(
            event_log={
                "source_path": "/tmp/orange_local_control.sock.events.jsonl",
                "relative_path": path.name,
                "copied": True,
                "copied_at_utc": "2026-05-29T15:00:12Z",
                "bytes": path.stat().st_size + 1,
            }
        )
        manifest["recording_folder"] = str(root)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest,
            "",
            required=False,
        )
        require(not check["ok"], "expected event-log byte mismatch to fail")
        require(
            any("event_log bytes mismatch" in failure for failure in reporter.failures),
            f"expected byte mismatch failure, got: {reporter.failures}",
        )


def test_local_control_event_log_fails_when_citrus_gate_disabled() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "orange_local_control.events.jsonl"
        write_local_control_event_log(path, citrus_completion_enabled=False)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest_with_control(),
            str(path),
            required=True,
        )
        require(not check["ok"], "expected event-log check to fail")
        require(
            any("citrus_completion_enabled" in failure for failure in reporter.failures),
            f"expected Citrus gate failure, got: {reporter.failures}",
        )


def test_local_control_event_log_passes_manual_citrus_only_gate_expectations() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "orange_local_control.events.jsonl"
        write_local_control_event_log(path)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest_with_control(),
            str(path),
            required=False,
            expected_generic_stop_enabled=False,
            expected_citrus_stop_enabled=True,
        )
        require(check["ok"], f"expected manual Citrus-only gate check to pass: {check}")
        require(
            check["required_by_gate_expectation"] is True,
            "gate expectation should require event-log validation",
        )
        require(
            check["expected_generic_stop_enabled"] is False,
            "gate expectation summary should record expected generic stop=false",
        )
        require(
            check["expected_citrus_stop_enabled"] is True,
            "gate expectation summary should record expected Citrus stop=true",
        )


def test_local_control_event_log_fails_when_generic_stop_gate_unexpectedly_enabled() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "orange_local_control.events.jsonl"
        write_local_control_event_log(path, generic_stop_enabled=True)
        reporter = validator.Reporter(verbose=False)
        check = validator.check_local_control_event_log_expectations(
            reporter,
            manifest_with_control(),
            str(path),
            required=False,
            expected_generic_stop_enabled=False,
            expected_citrus_stop_enabled=True,
        )
        require(not check["ok"], "expected generic-stop gate mismatch to fail")
        require(
            any("stop_recording_enabled=True; expected False" in failure for failure in reporter.failures),
            f"expected generic-stop gate failure, got: {reporter.failures}",
        )


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


def test_failed_timeout_ack_requires_timeout_evidence() -> None:
    manifest = manifest_with_control(
        ack_state="failed_timeout",
        forced_finalize_requested=True,
        forced_finalize_stream_stop_requested=True,
        forced_finalize_requested_at_utc="2026-05-29T15:00:11Z",
        error_code="drain_timeout",
        last_event="finalized_after_drain_timeout",
    )
    del manifest["recording"]["control"]["drain_timed_out"]
    reporter = check(manifest, expected_ack_state="failed_timeout")
    require(
        any("ack_state='failed_timeout' but drain_timed_out=None" in failure for failure in reporter.failures),
        f"expected failed-timeout drain_timed_out failure, got: {reporter.failures}",
    )


def test_forced_finalize_without_timeout_fails() -> None:
    reporter = check(
        manifest_with_control(
            drain_timed_out=False,
            forced_finalize_requested=True,
            forced_finalize_requested_at_utc="2026-05-29T15:00:11Z",
        )
    )
    require(
        any("forced_finalize_requested=true but drain_timed_out=False" in failure for failure in reporter.failures),
        f"expected forced-finalize timeout failure, got: {reporter.failures}",
    )


def test_completed_timeout_requires_forced_stream_stop() -> None:
    reporter = check(
        manifest_with_control(
            drain_timed_out=True,
            forced_finalize_requested=True,
            forced_finalize_stream_stop_requested=False,
            forced_finalize_requested_at_utc="2026-05-29T15:00:11Z",
            error_code="drain_timeout",
            ack_state="failed_timeout",
        ),
        expected_ack_state="failed_timeout",
    )
    require(
        any("forced_finalize_stream_stop_requested=False" in failure for failure in reporter.failures),
        f"expected forced stream-stop failure, got: {reporter.failures}",
    )


def test_completed_timeout_requires_finalized_after_timeout_event() -> None:
    reporter = check(
        manifest_with_control(
            drain_timed_out=True,
            forced_finalize_requested=True,
            forced_finalize_stream_stop_requested=True,
            forced_finalize_requested_at_utc="2026-05-29T15:00:11Z",
            error_code="drain_timeout",
            ack_state="failed_timeout",
            last_event="finalized",
        ),
        expected_ack_state="failed_timeout",
    )
    require(
        any("last_event='finalized'" in failure for failure in reporter.failures),
        f"expected finalized-after-timeout event failure, got: {reporter.failures}",
    )


def main() -> int:
    tests = [
        test_stop_all_expectations_pass_for_stopped_citrus_completion,
        test_local_control_event_log_passes_for_citrus_only_stop_gate,
        test_local_control_event_log_defaults_to_manifest_copy,
        test_local_control_event_log_prefers_relative_manifest_copy,
        test_local_control_event_log_manifest_copy_validates_when_not_required,
        test_local_control_event_log_manifest_copy_missing_fails_even_when_not_required,
        test_local_control_event_log_manifest_copy_byte_mismatch_fails,
        test_local_control_event_log_fails_when_citrus_gate_disabled,
        test_local_control_event_log_passes_manual_citrus_only_gate_expectations,
        test_local_control_event_log_fails_when_generic_stop_gate_unexpectedly_enabled,
        test_terminal_state_mismatch_fails,
        test_reason_mismatch_fails,
        test_ack_state_mismatch_fails,
        test_failed_timeout_ack_requires_timeout_evidence,
        test_forced_finalize_without_timeout_fails,
        test_completed_timeout_requires_forced_stream_stop,
        test_completed_timeout_requires_finalized_after_timeout_event,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("validate_gui_local_control_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
