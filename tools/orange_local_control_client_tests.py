#!/usr/bin/env python3
"""Focused tests for orange_local_control_client.py."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "orange_local_control_client.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_client(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def parse_stdout_json(result: subprocess.CompletedProcess[str]) -> dict:
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"stdout was not JSON: {result.stdout!r}") from exc
    require(isinstance(payload, dict), "stdout JSON should be an object")
    return payload


def test_status_dry_run_builds_schema_request() -> None:
    result = run_client(["--dry-run", "status", "--request-id", "status-req"])

    require(result.returncode == 0, f"dry-run failed: {result.stderr}")
    payload = parse_stdout_json(result)
    require(payload["schema_id"] == "orange.local_control.request", "schema id")
    require(payload["schema_version"] == 1, "schema version")
    require(payload["method"] == "status", "status method")
    require(payload["request_id"] == "status-req", "request id")
    require(payload["source"] == "orange_local_control_client", "status default source")
    require("operation_id" not in payload, "status request should not need operation_id")


def test_citrus_completion_dry_run_defaults_operation_id() -> None:
    result = run_client(
        [
            "--dry-run",
            "citrus-completion",
            "--request-id",
            "completion-req",
            "--experiment-id",
            "citrus-exp-1",
            "--terminal-state",
            "completed",
            "--reason",
            "protocol_finished",
            "--grace-seconds",
            "7.5",
        ]
    )

    require(result.returncode == 0, f"dry-run failed: {result.stderr}")
    payload = parse_stdout_json(result)
    require(payload["method"] == "citrus_completion", "completion method")
    require(payload["source"] == "citrus", "completion should default to Citrus source")
    require(payload["operation_id"] == "citrus-exp-1", "operation defaults to experiment id")
    params = payload["params"]
    require(params["experiment_id"] == "citrus-exp-1", "experiment id")
    require(params["terminal_state"] == "completed", "terminal state")
    require(params["reason"] == "protocol_finished", "reason")
    require(params["grace_seconds"] == 7.5, "grace seconds")


def test_citrus_completion_dry_run_defaults_grace_seconds() -> None:
    result = run_client(
        [
            "--dry-run",
            "citrus-completion",
            "--request-id",
            "completion-default-grace",
            "--experiment-id",
            "citrus-exp-default-grace",
            "--terminal-state",
            "completed",
            "--reason",
            "protocol_finished",
        ]
    )

    require(result.returncode == 0, f"dry-run failed: {result.stderr}")
    payload = parse_stdout_json(result)
    require(payload["method"] == "citrus_completion", "completion method")
    require(
        payload["params"]["grace_seconds"] == 10.0,
        "completion client should default to 10s Orange stop grace",
    )


def test_citrus_completion_source_can_be_overridden() -> None:
    result = run_client(
        [
            "--dry-run",
            "--source",
            "diagnostic_tool",
            "citrus-completion",
            "--request-id",
            "completion-source",
            "--experiment-id",
            "citrus-exp-source",
        ]
    )

    require(result.returncode == 0, f"dry-run failed: {result.stderr}")
    payload = parse_stdout_json(result)
    require(payload["method"] == "citrus_completion", "completion method")
    require(payload["source"] == "diagnostic_tool", "explicit source should win")


def test_start_stop_dry_run_include_operation_ids() -> None:
    start = run_client(
        [
            "--dry-run",
            "start-recording",
            "--request-id",
            "start-req",
            "--operation-id",
            "run-42",
        ]
    )
    stop = run_client(
        [
            "--dry-run",
            "stop-recording",
            "--request-id",
            "stop-req",
            "--operation-id",
            "run-42",
            "--grace-seconds",
            "3",
        ]
    )

    require(start.returncode == 0, f"start dry-run failed: {start.stderr}")
    require(stop.returncode == 0, f"stop dry-run failed: {stop.stderr}")
    start_payload = parse_stdout_json(start)
    stop_payload = parse_stdout_json(stop)
    require(start_payload["method"] == "start_recording", "start method")
    require(stop_payload["method"] == "stop_recording", "stop method")
    require(start_payload["source"] == "orange_local_control_client", "start default source")
    require(stop_payload["source"] == "orange_local_control_client", "stop default source")
    require(start_payload["operation_id"] == "run-42", "start operation id")
    require(stop_payload["operation_id"] == "run-42", "stop operation id")
    require(stop_payload["params"]["grace_seconds"] == 3.0, "stop grace seconds")


def test_citrus_completion_requires_experiment_id() -> None:
    result = run_client(["--dry-run", "citrus-completion"])

    require(result.returncode != 0, "missing experiment id should fail")
    require("--experiment-id" in result.stderr, "argparse should explain missing experiment id")


def test_missing_socket_fails_before_connect() -> None:
    result = run_client(
        [
            "--socket",
            "/tmp/orange_local_control_missing_for_test.sock",
            "status",
            "--request-id",
            "missing-socket",
        ]
    )

    require(result.returncode == 2, "missing socket should return preflight failure")
    require("does not exist" in result.stderr, "missing socket should be explained")


def main() -> int:
    tests = [
        test_status_dry_run_builds_schema_request,
        test_citrus_completion_dry_run_defaults_operation_id,
        test_citrus_completion_dry_run_defaults_grace_seconds,
        test_citrus_completion_source_can_be_overridden,
        test_start_stop_dry_run_include_operation_ids,
        test_citrus_completion_requires_experiment_id,
        test_missing_socket_fails_before_connect,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("orange_local_control_client_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
