#!/usr/bin/env python3
"""Focused tests for scripts/orange_citrus_orchestrator.py."""

from __future__ import annotations

import importlib.util
import json
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "orange_citrus_orchestrator.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_module() -> Any:
    spec = importlib.util.spec_from_file_location("orange_citrus_orchestrator", SCRIPT)
    require(spec is not None and spec.loader is not None, "failed to load orchestrator spec")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def orange_status(
    started: bool,
    stopped: bool,
    *,
    drain_timed_out: bool = False,
    stop_method: str = "stop_recording",
    stop_source: str = "orange_citrus_orchestrator",
    operation_id: str = "op-test",
    terminal_state: str = "",
    reason: str = "orchestrator_stop",
) -> dict[str, Any]:
    return {
        "phase": "streaming" if not started else ("streaming" if stopped else "recording"),
        "readiness": {
            "ready_for_recording_request": not started and not stopped,
            "ready_for_citrus_experiment": started and not stopped,
            "recording_finalized": stopped,
            "recording_active": started and not stopped,
            "recording_finalizing": False,
        },
        "recording": {
            "folder": "/tmp/orange_citrus_fake_recording",
            "sink_mode": "external_ipc",
        },
        "local_control": {
            "recording_stop": {
                "enabled": True,
                "state": (
                    "finalized_after_drain_timeout"
                    if drain_timed_out
                    else ("finalized" if stopped else "idle")
                ),
                "health": "warning" if drain_timed_out else "ok",
                "ack_state": (
                    "failed_timeout"
                    if drain_timed_out
                    else ("executed" if stopped else "idle")
                ),
                "error_code": "drain_timeout" if drain_timed_out else "",
                "stop_triggered": stopped,
                "drain_active": False,
                "drain_timed_out": drain_timed_out,
                "forced_finalize_requested": drain_timed_out,
                "forced_finalize_stream_stop_requested": drain_timed_out,
                "drain_timeout_seconds": 60.0,
                "drain_elapsed_seconds": 61.25 if drain_timed_out else 0.0,
                "request_id": "stop-req",
                "operation_id": operation_id,
                "method": stop_method if stopped or drain_timed_out else "",
                "source": stop_source if stopped or drain_timed_out else "",
                "terminal_state": terminal_state if stopped or drain_timed_out else "",
                "reason": reason if stopped or drain_timed_out else "",
                "last_event": "finalized_after_drain_timeout"
                if drain_timed_out
                else "finalized",
            }
        },
    }


def citrus_status(started: bool, terminal: bool) -> dict[str, Any]:
    return {
        "readiness": {
            "ready_to_start": not started,
            "reasons": [],
        },
        "experiment": {
            "active": started and not terminal,
            "armed": False,
            "terminal_state": "completed" if terminal else ("start_requested" if started else ""),
            "terminal_reason": "protocol_finished" if terminal else "",
            "last_operation_id": "op-test" if started else "",
        },
        "output": {
            "perf_jsonl_enabled": True,
            "perf_jsonl_path": "/tmp/citrus_perf.jsonl" if terminal else "",
            "perf_jsonl_path_known": terminal,
        },
    }


def response_for(request: dict[str, Any], status: dict[str, Any], ok: bool = True) -> dict[str, Any]:
    return {
        "schema_id": request["schema_id"].replace(".request", ".response"),
        "schema_version": 1,
        "ok": ok,
        "accepted": ok,
        "duplicate": False,
        "request_id": request.get("request_id", ""),
        "operation_id": request.get("operation_id", ""),
        "method": request.get("method", ""),
        "status": status,
        "effect": {},
    }


def run_script(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_request_builders_and_readiness_helpers() -> None:
    module = load_module()
    orange_start = module.build_orange_start_request("op-1", "req-start", "test")
    citrus_start = module.build_citrus_start_request("op-1", "req-citrus", "test")

    require(orange_start["schema_id"] == "orange.local_control.request", "orange schema")
    require(orange_start["method"] == "start_recording", "orange start method")
    require(orange_start["operation_id"] == "op-1", "orange operation id")
    require(citrus_start["schema_id"] == "citrus.local_control.request", "citrus schema")
    require(citrus_start["method"] == "start_experiment", "citrus start method")
    require(module.orange_ready_for_recording(orange_status(False, False)), "orange ready")
    require(module.orange_ready_for_citrus(orange_status(True, False)), "orange recording")
    require(module.orange_recording_finalized(orange_status(True, True)), "orange finalized")
    require(
        not module.orange_recording_stop_drain_timed_out(orange_status(True, True)),
        "orange drain timeout should default false",
    )
    require(
        module.orange_recording_stop_drain_timed_out(
            orange_status(True, True, drain_timed_out=True)
        ),
        "orange drain timeout helper should read local-control status",
    )
    require(
        module.orange_recording_stop_ack_state(orange_status(True, True)) == "executed",
        "orange stop ACK-state helper should read local-control status",
    )
    require(
        module.orange_recording_stop_ack_state(
            orange_status(True, True, drain_timed_out=True)
        )
        == "failed_timeout",
        "orange stop ACK-state helper should report drain timeout",
    )
    stop_args = module.parse_args(["--stop-policy", "stop_recording"])
    require(
        module.effective_orange_stop_grace_seconds(stop_args) == 0.0,
        "stop_recording should default to immediate stop",
    )
    citrus_args = module.parse_args(["--stop-policy", "citrus_completion"])
    require(
        module.effective_orange_stop_grace_seconds(citrus_args) == 10.0,
        "orchestrator-sent citrus_completion should default to 10 seconds",
    )
    notify_args = module.parse_args(["--stop-policy", "citrus_completion_notify"])
    require(
        module.effective_orange_stop_grace_seconds(notify_args) == 10.0,
        "Citrus-notified completion should default to 10 seconds",
    )
    explicit_args = module.parse_args(
        ["--stop-policy", "citrus_completion", "--orange-stop-grace-seconds", "2.5"]
    )
    require(
        module.effective_orange_stop_grace_seconds(explicit_args) == 2.5,
        "explicit Orange stop grace should override policy defaults",
    )
    require(module.citrus_ready_to_start(citrus_status(False, False)), "citrus ready")
    require(module.citrus_is_terminal(citrus_status(True, True)), "citrus terminal")
    require(module.citrus_perf_jsonl_path_known(citrus_status(True, True)), "perf path known")

    rendered = module.render_validation_command(
        "validator {orange_recording_folder} {citrus_perf_jsonl_path} {operation_id}",
        operation_id="op with spaces",
        orange_status={"recording": {"folder": "/tmp/orange folder"}},
        citrus_status={"output": {"perf_jsonl_path": "/tmp/citrus perf.jsonl"}},
    )
    require(
        shlex.split(rendered)
        == [
            "validator",
            "/tmp/orange folder",
            "/tmp/citrus perf.jsonl",
            "op with spaces",
        ],
        "validation placeholders should render as safely quoted argv tokens",
    )
    try:
        module.render_validation_command(
            "validator {orange_recording_folder}",
            operation_id="op-1",
            orange_status={"recording": {"folder": ""}},
            citrus_status={},
        )
    except module.OrchestratorError as exc:
        require(
            "{orange_recording_folder}" in str(exc),
            "missing placeholder error should identify Orange folder placeholder",
        )
    else:
        raise AssertionError("expected missing Orange recording folder to fail")


def test_dry_run_default_does_not_open_sockets() -> None:
    result = run_script(
        [
            "--operation-id",
            "op-dry",
            "--orange-socket",
            "/tmp/missing_orange_dry.sock",
            "--orange-local-control-log",
            "/tmp/missing_orange_dry.sock.events.jsonl",
            "--citrus-socket",
            "/tmp/missing_citrus_dry.sock",
            "--orange-env",
            "ORANGE_GUI_SHOW_SPEED_GRAPHS=0",
            "--require-citrus-perf-jsonl",
            "--validation-command",
            f"quick={sys.executable} -c \"print('dry-run-validation')\"",
            "--validation-artifact",
            "quick=/tmp/quick_validation.json",
        ]
    )
    require(result.returncode == 0, f"dry-run failed: {result.stderr}")
    payload = json.loads(result.stdout)
    require(payload["result"] == "dry_run", "default mode should be dry-run")
    require(payload["execute_required"], "dry-run should say execute is required")
    require(
        payload["orange"]["start_request"]["method"] == "start_recording",
        "dry-run should show Orange start request",
    )
    require(
        payload["citrus"]["start_request"]["method"] == "start_experiment",
        "dry-run should show Citrus start request",
    )
    require(
        payload["citrus"]["env_overlay"]["CITRUS_PERF_JSONL"] == "1",
        "dry-run should show perf JSONL env overlay when required",
    )
    require(
        payload["orange"]["env_overlay"]["ORANGE_GUI_SHOW_SPEED_GRAPHS"] == "0",
        "dry-run should include extra Orange env overrides",
    )
    require(
        payload["orange"]["env_overlay"]["ORANGE_GUI_AUTORUN_START_RECORDING"] == "0",
        "orchestrator should leave Orange autorun in stream-only mode",
    )
    require(
        payload["orange"]["log_path"] == "/tmp/orange_citrus_orchestrator_orange.log",
        "dry-run should expose the Orange process log path",
    )
    require(
        payload["orange"]["local_control_event_log_path"]
        == "/tmp/missing_orange_dry.sock.events.jsonl",
        "dry-run should expose the Orange local-control event log path",
    )
    require(
        payload["citrus"]["log_path"] == "/tmp/orange_citrus_orchestrator_citrus.log",
        "dry-run should expose the Citrus process log path",
    )
    require(
        payload["orange"]["env_overlay"]["ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE"] == "0",
        "orchestrator should not use autorun finalize as its exit trigger",
    )
    require(
        payload["orange"]["env_overlay"]["ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE"] == "1",
        "orchestrator should close launched Orange after local-control finalization",
    )
    require(
        not payload["orange"]["preflight_existing_socket"],
        "attach-mode dry-run should not preflight an Orange launch socket",
    )
    require(
        not payload["orange"]["allow_drain_timeout"],
        "orchestrator should fail on Orange drain timeout telemetry by default",
    )
    require(
        not payload["orange"]["require_local_control_event_log"],
        "base orchestrator should make event-log evidence opt-in",
    )
    require(payload["validations"][0]["label"] == "quick", "dry-run should show validation labels")
    require(
        payload["validations"][0]["command"] == f"{sys.executable} -c \"print('dry-run-validation')\"",
        "dry-run should show validation command text",
    )
    require(
        payload["validations"][0]["artifact_paths"] == ["/tmp/quick_validation.json"],
        "dry-run should show validation artifact paths",
    )


def test_dry_run_launch_socket_preflight_flags() -> None:
    result = run_script(
        [
            "--operation-id",
            "op-dry-launch",
            "--orange-command",
            "/bin/true",
            "--citrus-command",
            "/bin/true",
        ]
    )
    require(result.returncode == 0, f"launch dry-run failed: {result.stderr}")
    payload = json.loads(result.stdout)
    require(
        payload["orange"]["preflight_existing_socket"],
        "launch dry-run should preflight Orange socket by default",
    )
    require(
        payload["citrus"]["preflight_existing_socket"],
        "launch dry-run should preflight Citrus socket by default",
    )

    allowed = run_script(
        [
            "--operation-id",
            "op-dry-launch-allowed",
            "--orange-command",
            "/bin/true",
            "--citrus-command",
            "/bin/true",
            "--allow-preexisting-orange-socket",
            "--allow-preexisting-citrus-socket",
        ]
    )
    require(allowed.returncode == 0, f"allowed launch dry-run failed: {allowed.stderr}")
    allowed_payload = json.loads(allowed.stdout)
    require(
        not allowed_payload["orange"]["preflight_existing_socket"],
        "Orange preflight should be disabled by override",
    )
    require(
        not allowed_payload["citrus"]["preflight_existing_socket"],
        "Citrus preflight should be disabled by override",
    )


def test_execute_against_fake_local_control_servers() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path = root / "summary.json"
        orange_state = {"started": False, "stopped": False}
        citrus_state = {"started": False, "status_polls_after_start": 0}

        def handle_orange(request: dict[str, Any]) -> dict[str, Any]:
            method = request.get("method")
            if method == "start_recording":
                orange_state["started"] = True
            elif method == "stop_recording":
                orange_state["stopped"] = True
            return response_for(
                request,
                orange_status(orange_state["started"], orange_state["stopped"]),
            )

        def handle_citrus(request: dict[str, Any]) -> dict[str, Any]:
            method = request.get("method")
            if method == "start_experiment":
                citrus_state["started"] = True
            elif method == "status" and citrus_state["started"]:
                citrus_state["status_polls_after_start"] += 1
            terminal = citrus_state["status_polls_after_start"] >= 1
            return response_for(
                request,
                citrus_status(citrus_state["started"], terminal),
            )

        def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
            if socket_path == "orange-test.sock":
                return handle_orange(request)
            if socket_path == "citrus-test.sock":
                return handle_citrus(request)
            raise AssertionError(f"unexpected socket path: {socket_path}")

        original_send = module.send_unix_json
        module.send_unix_json = fake_send
        try:
            args = module.parse_args(
                [
                    "--execute",
                    "--operation-id",
                    "op-live",
                    "--orange-socket",
                    "orange-test.sock",
                    "--citrus-socket",
                    "citrus-test.sock",
                    "--poll-interval-seconds",
                    "0.01",
                    "--timeout-seconds",
                    "2",
                    "--citrus-terminal-timeout-seconds",
                    "2",
                    "--orange-finalize-timeout-seconds",
                    "2",
                    "--require-citrus-perf-jsonl",
                    "--validation-command",
                    f"quick={sys.executable} -c \"print('validation-ok')\"",
                    "--summary-json",
                    str(summary_path),
                ]
            )
            payload = module.Orchestrator(args).run()
            module.write_summary(args.summary_json, payload)
        finally:
            module.send_unix_json = original_send

        require(payload["result"] == "pass", "orchestrator should pass fake run")
        require(payload["orange"]["recording_finalized"], "summary should report Orange finalized")
        require(
            not payload["orange"]["local_control_stop_drain_timed_out"],
            "summary should report no Orange drain timeout",
        )
        require(
            payload["orange"]["local_control_recording_stop"]["last_event"] == "finalized",
            "summary should include Orange local-control stop status",
        )
        require(
            payload["orange"]["local_control_stop_state"] == "finalized",
            "summary should include Orange stop state",
        )
        require(
            payload["orange"]["local_control_stop_ack_state"] == "executed",
            "summary should include Orange stop ACK state",
        )
        require(
            payload["orange"]["local_control_stop_ack_status_check"]["ok"],
            "summary should include passing Orange stop ACK-state check",
        )
        require(
            payload["orange"]["local_control_stop_health"] == "ok",
            "summary should include Orange stop health",
        )
        require(
            payload["orange"]["local_control_stop_timeout_status_check"]["ok"],
            "summary should include passing Orange timeout-status check",
        )
        require(
            not payload["orange"]["local_control_event_log_check"]["required"],
            "event-log evidence check should be opt-in for the base orchestrator",
        )
        require(payload["citrus"]["terminal_state"] == "completed", "summary should report Citrus terminal")
        require(payload["citrus"]["perf_jsonl_path"] == "/tmp/citrus_perf.jsonl", "summary should carry perf path")
        require(len(payload["validations"]) == 1, "summary should carry validation results")
        require(payload["validations"][0]["returncode"] == 0, "validation command should pass")
        require("validation-ok" in payload["validations"][0]["stdout"], "validation stdout should be captured")
        require(summary_path.exists(), "summary JSON should be written")
        require(json.loads(summary_path.read_text())["result"] == "pass", "summary file should match")


def test_execute_waits_for_citrus_completion_notify_stop() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path = root / "summary.json"
        orange_state = {
            "started": False,
            "stopped": False,
            "orchestrator_stop_requests": 0,
        }
        citrus_state = {"started": False, "status_polls_after_start": 0}

        def handle_orange(request: dict[str, Any]) -> dict[str, Any]:
            method = request.get("method")
            if method == "start_recording":
                orange_state["started"] = True
            elif method in {"stop_recording", "citrus_completion"}:
                orange_state["orchestrator_stop_requests"] += 1
                orange_state["stopped"] = True
            status = orange_status(
                orange_state["started"],
                orange_state["stopped"],
                stop_method="citrus_completion",
                stop_source="citrus",
                operation_id="op-notify",
                terminal_state="completed",
                reason="protocol_finished",
            )
            return response_for(
                request,
                status,
            )

        def handle_citrus(request: dict[str, Any]) -> dict[str, Any]:
            method = request.get("method")
            if method == "start_experiment":
                citrus_state["started"] = True
            elif method == "status" and citrus_state["started"]:
                citrus_state["status_polls_after_start"] += 1
            terminal = citrus_state["status_polls_after_start"] >= 1
            if terminal:
                # Simulate Citrus's own Orange completion notifier. The
                # orchestrator should wait for Orange finalization, not send its
                # own Orange stop request for this policy.
                orange_state["stopped"] = True
            return response_for(
                request,
                citrus_status(citrus_state["started"], terminal),
            )

        def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
            if socket_path == "orange-test.sock":
                return handle_orange(request)
            if socket_path == "citrus-test.sock":
                return handle_citrus(request)
            raise AssertionError(f"unexpected socket path: {socket_path}")

        original_send = module.send_unix_json
        module.send_unix_json = fake_send
        try:
            args = module.parse_args(
                [
                    "--execute",
                    "--operation-id",
                    "op-notify",
                    "--orange-socket",
                    "orange-test.sock",
                    "--citrus-socket",
                    "citrus-test.sock",
                    "--poll-interval-seconds",
                    "0.01",
                    "--timeout-seconds",
                    "2",
                    "--citrus-terminal-timeout-seconds",
                    "2",
                    "--orange-finalize-timeout-seconds",
                    "2",
                    "--stop-policy",
                    "citrus_completion_notify",
                    "--validation-command",
                    f"quick={sys.executable} -c \"print('validation-ok')\"",
                    "--summary-json",
                    str(summary_path),
                ]
            )
            payload = module.Orchestrator(args).run()
            module.write_summary(args.summary_json, payload)
        finally:
            module.send_unix_json = original_send

        require(payload["result"] == "pass", "notify-stop orchestrator should pass fake run")
        require(
            orange_state["orchestrator_stop_requests"] == 0,
            "notify-stop policy must not send an Orange stop request itself",
        )
        require(payload["orange"]["recording_finalized"], "Orange should finalize after Citrus notification")
        require(
            payload["orange"]["local_control_stop_ack_state"] == "executed",
            "summary should include executed ACK state for Citrus-notified stop",
        )
        require(
            payload["orange"]["local_control_stop_ack_status_check"]["stop_policy"]
            == "citrus_completion_notify",
            "ACK-state check should preserve notify stop policy",
        )
        require(
            payload["orange"]["local_control_citrus_notify_stop_status_check"]["ok"],
            "summary should include a clean Citrus-notify status consistency check",
        )
        require(summary_path.exists(), "summary JSON should be written")


def test_execute_stops_citrus_after_run_seconds() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        orange_state = {"started": False, "stopped": False}
        citrus_state = {"started": False, "stopped": False, "stop_requests": 0}

        def handle_orange(request: dict[str, Any]) -> dict[str, Any]:
            method = request.get("method")
            if method == "start_recording":
                orange_state["started"] = True
            elif method == "stop_recording":
                orange_state["stopped"] = True
            return response_for(
                request,
                orange_status(orange_state["started"], orange_state["stopped"]),
            )

        def handle_citrus(request: dict[str, Any]) -> dict[str, Any]:
            method = request.get("method")
            if method == "start_experiment":
                citrus_state["started"] = True
            elif method == "stop_experiment":
                citrus_state["stopped"] = True
                citrus_state["stop_requests"] += 1
            terminal = citrus_state["stopped"]
            status = {
                "readiness": {
                    "ready_to_start": not citrus_state["started"],
                    "reasons": [],
                },
                "experiment": {
                    "active": citrus_state["started"] and not terminal,
                    "armed": False,
                    "terminal_state": "stopped" if terminal else ("active" if citrus_state["started"] else ""),
                    "terminal_reason": "stopped_by_local_control" if terminal else "",
                    "last_operation_id": "op-run-seconds" if citrus_state["started"] else "",
                },
                "output": {
                    "perf_jsonl_enabled": True,
                    "perf_jsonl_path": str(root / "citrus_perf.jsonl") if terminal else "",
                    "perf_jsonl_path_known": terminal,
                },
            }
            return response_for(request, status)

        def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
            if socket_path == "orange-run-seconds.sock":
                return handle_orange(request)
            if socket_path == "citrus-run-seconds.sock":
                return handle_citrus(request)
            raise AssertionError(f"unexpected socket path: {socket_path}")

        original_send = module.send_unix_json
        module.send_unix_json = fake_send
        try:
            args = module.parse_args(
                [
                    "--execute",
                    "--operation-id",
                    "op-run-seconds",
                    "--orange-socket",
                    "orange-run-seconds.sock",
                    "--citrus-socket",
                    "citrus-run-seconds.sock",
                    "--poll-interval-seconds",
                    "0.01",
                    "--timeout-seconds",
                    "2",
                    "--citrus-terminal-timeout-seconds",
                    "2",
                    "--citrus-run-seconds",
                    "0.02",
                    "--orange-finalize-timeout-seconds",
                    "2",
                    "--allow-preexisting-orange-socket",
                    "--allow-preexisting-citrus-socket",
                    "--require-citrus-perf-jsonl",
                    "--summary-json",
                    str(root / "summary.json"),
                ]
            )
            payload = module.Orchestrator(args).run()
        finally:
            module.send_unix_json = original_send

        require(payload["result"] == "pass", "orchestrator should pass run-seconds fake run")
        require(citrus_state["stop_requests"] == 1, "orchestrator should send one Citrus stop request")
        require(
            payload["citrus"]["terminal_state"] == "stopped",
            "summary should report Citrus stopped by run duration",
        )
        step_names = [step["name"] for step in payload["steps"]]
        require(
            "wait_citrus_terminal_or_run_duration" in step_names,
            "run should include the run-duration wait step",
        )
        require(
            "citrus_stop_experiment_run_duration" in step_names,
            "run should include the Citrus stop request step",
        )


def test_launch_preflights_all_sockets_before_starting_processes() -> None:
    module = load_module()
    args = module.parse_args(
        [
            "--execute",
            "--operation-id",
            "op-preflight-before-start",
            "--orange-command",
            "/bin/true",
            "--citrus-command",
            "/bin/true",
        ]
    )
    orchestrator = module.Orchestrator(args)
    calls: list[tuple[str, str]] = []

    def fake_preflight(
        label: str,
        schema_id: str,
        socket_path: str,
        allow_preexisting: bool,
    ) -> None:
        calls.append(("preflight", label))
        if label == "citrus":
            raise module.OrchestratorError("synthetic stale Citrus socket")

    def fake_start(
        label: str,
        command: str,
        env_overlay: dict[str, str],
        log_path: str,
    ) -> None:
        calls.append(("start", label))
        raise AssertionError("processes must not start after a failed launch preflight")

    orchestrator.preflight_launch_socket = fake_preflight
    orchestrator.start_process = fake_start

    try:
        orchestrator.run()
    except module.OrchestratorError as exc:
        require("synthetic stale Citrus socket" in str(exc), "run should report the preflight failure")
    else:
        raise AssertionError("expected stale Citrus preflight to fail")

    require(
        calls == [("preflight", "orange"), ("preflight", "citrus")],
        "orchestrator should preflight both launch sockets before starting any process",
    )


def test_orchestrator_fails_on_orange_drain_timeout_by_default() -> None:
    module = load_module()
    orange_state = {"stopped": False}

    def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
        require(socket_path == "orange-timeout.sock", "unexpected socket path")
        if request.get("method") == "stop_recording":
            orange_state["stopped"] = True
        return response_for(
            request,
            orange_status(
                True,
                orange_state["stopped"],
                drain_timed_out=orange_state["stopped"],
            ),
        )

    original_send = module.send_unix_json
    module.send_unix_json = fake_send
    try:
        args = module.parse_args(
            [
                "--execute",
                "--operation-id",
                "op-timeout",
                "--orange-socket",
                "orange-timeout.sock",
                "--poll-interval-seconds",
                "0.01",
                "--orange-finalize-timeout-seconds",
                "1",
            ]
        )
        orchestrator = module.Orchestrator(args)
        try:
            orchestrator.request_orange_stop(citrus_status(True, True))
        except module.OrchestratorError as exc:
            require(
                "Orange recording drain timed out" in str(exc),
                "drain timeout should fail",
            )
        else:
            raise AssertionError("expected Orange drain timeout to fail")
        orchestrator.orange_recording_started = True
        require(
            orchestrator.best_effort_stop_after_failure() is None,
            "failure cleanup should not resend stop after finalized timeout status",
        )

        args_allow = module.parse_args(
            [
                "--execute",
                "--operation-id",
                "op-timeout-allow",
                "--orange-socket",
                "orange-timeout.sock",
                "--poll-interval-seconds",
                "0.01",
                "--orange-finalize-timeout-seconds",
                "1",
                "--allow-orange-drain-timeout",
            ]
        )
        orange_state["stopped"] = False
        allowed_status = module.Orchestrator(args_allow).request_orange_stop(
            citrus_status(True, True)
        )
        require(
            module.orange_recording_stop_drain_timed_out(allowed_status),
            "allowed path should still return timeout status",
        )
        require(
            module.orange_recording_stop_state(allowed_status)
            == "finalized_after_drain_timeout",
            "allowed path should expose finalized-after-timeout state",
        )
        require(
            module.orange_recording_stop_health(allowed_status) == "warning",
            "allowed path should expose warning health after timeout finalizes",
        )
        require(
            module.orange_recording_stop_ack_state(allowed_status) == "failed_timeout",
            "allowed path should expose failed-timeout ACK state",
        )
        allowed_check = module.summarize_orange_drain_timeout_status(
            allowed_status,
            allow_drain_timeout=True,
        )
        require(allowed_check["ok"], f"allowed timeout status should pass: {allowed_check}")
        require(
            allowed_check["ack_state"] == "failed_timeout",
            "timeout status summary should carry failed-timeout ACK state",
        )
        require(
            allowed_check["drain_timed_out"],
            "allowed timeout status summary should report timeout",
        )
        require(
            allowed_check["policy_ok"],
            "allowed timeout status summary should report policy ok",
        )
        broken_status = orange_status(True, True, drain_timed_out=True)
        broken_status["local_control"]["recording_stop"]["forced_finalize_requested"] = False
        broken_status["local_control"]["recording_stop"][
            "forced_finalize_stream_stop_requested"
        ] = False
        broken_failures = module.check_orange_drain_timeout_status(broken_status)
        require(
            any("forced_finalize_requested" in failure for failure in broken_failures),
            "timeout status check should require forced finalize request",
        )
        try:
            module.Orchestrator(args_allow).require_orange_drain_not_timed_out(
                broken_status
            )
        except module.OrchestratorError as exc:
            require(
                "timeout status is inconsistent" in str(exc),
                "allow-orange-drain-timeout must not allow inconsistent timeout status",
            )
        else:
            raise AssertionError("expected inconsistent timeout status to fail")

        partial_status = orange_status(True, True, drain_timed_out=True)
        partial_status["local_control"]["recording_stop"][
            "forced_finalize_stream_stop_requested"
        ] = False
        partial_failures = module.check_orange_drain_timeout_status(partial_status)
        require(
            any(
                "forced_finalize_stream_stop_requested" in failure
                for failure in partial_failures
            ),
            "finalized timeout status check should require forced stream-stop request",
        )
    finally:
        module.send_unix_json = original_send


def test_orchestrator_checks_orange_stop_ack_state() -> None:
    module = load_module()

    clean_status = orange_status(True, True)
    clean_check = module.summarize_orange_stop_ack_status(
        clean_status,
        stop_policy="stop_recording",
        finalized_wait_skipped=False,
    )
    require(clean_check["ok"], f"clean finalized ACK status should pass: {clean_check}")
    require(clean_check["ack_state"] == "executed", "clean ACK summary should report executed")

    timeout_status = orange_status(True, True, drain_timed_out=True)
    timeout_check = module.summarize_orange_stop_ack_status(
        timeout_status,
        stop_policy="stop_recording",
        finalized_wait_skipped=False,
    )
    require(timeout_check["ok"], f"timeout ACK status should pass: {timeout_check}")
    require(
        timeout_check["ack_state"] == "failed_timeout",
        "timeout ACK summary should report failed_timeout",
    )

    skipped_status = orange_status(True, False)
    skipped_status["local_control"]["recording_stop"]["ack_state"] = "executing"
    skipped_check = module.summarize_orange_stop_ack_status(
        skipped_status,
        stop_policy="stop_recording",
        finalized_wait_skipped=True,
    )
    require(
        skipped_check["ok"],
        f"skip-wait ACK status should allow in-progress state: {skipped_check}",
    )

    missing_status = orange_status(True, True)
    del missing_status["local_control"]["recording_stop"]["ack_state"]
    missing_check = module.summarize_orange_stop_ack_status(
        missing_status,
        stop_policy="stop_recording",
        finalized_wait_skipped=False,
    )
    require(not missing_check["ok"], "missing ACK state should fail")
    require(
        "missing" in missing_check["failures"][0],
        "missing ACK failure should explain missing field",
    )

    inconsistent_status = orange_status(True, True)
    inconsistent_status["local_control"]["recording_stop"]["ack_state"] = "executing"
    inconsistent_check = module.summarize_orange_stop_ack_status(
        inconsistent_status,
        stop_policy="stop_recording",
        finalized_wait_skipped=False,
    )
    require(not inconsistent_check["ok"], "finalized but executing ACK state should fail")
    require(
        "finalized cleanly" in inconsistent_check["failures"][0],
        "inconsistent ACK failure should explain finalized-state mismatch",
    )

    ignored_status = orange_status(True, False)
    ignored_status["local_control"]["recording_stop"]["ack_state"] = "ignored"
    ignored_check = module.summarize_orange_stop_ack_status(
        ignored_status,
        stop_policy="stop_recording",
        finalized_wait_skipped=True,
    )
    require(not ignored_check["ok"], "ignored stop ACK state should fail for a stop policy")


def test_orchestrator_checks_citrus_notify_stop_status() -> None:
    module = load_module()

    clean_status = orange_status(
        True,
        True,
        stop_method="citrus_completion",
        stop_source="citrus",
        operation_id="op-test",
        terminal_state="completed",
        reason="protocol_finished",
    )
    clean_check = module.summarize_orange_citrus_notify_stop_status(
        clean_status,
        citrus_status(True, True),
        stop_policy="citrus_completion_notify",
        operation_id="op-test",
    )
    require(clean_check["ok"], f"clean Citrus notify stop should pass: {clean_check}")
    require(
        clean_check["method"] == "citrus_completion",
        "notify check should report the Orange stop method",
    )

    skipped_check = module.summarize_orange_citrus_notify_stop_status(
        clean_status,
        citrus_status(True, True),
        stop_policy="stop_recording",
        operation_id="op-test",
    )
    require(skipped_check["ok"] and skipped_check["skipped"], "non-notify policies should skip the notify check")

    wrong_method_status = orange_status(
        True,
        True,
        stop_method="stop_recording",
        stop_source="orange_citrus_orchestrator",
        operation_id="op-test",
        terminal_state="completed",
        reason="protocol_finished",
    )
    wrong_method_check = module.summarize_orange_citrus_notify_stop_status(
        wrong_method_status,
        citrus_status(True, True),
        stop_policy="citrus_completion_notify",
        operation_id="op-test",
    )
    require(not wrong_method_check["ok"], "notify policy should fail when Orange stop was not Citrus completion")
    require(
        any("method" in failure for failure in wrong_method_check["failures"]),
        "notify method failure should identify the bad method",
    )

    wrong_terminal_status = orange_status(
        True,
        True,
        stop_method="citrus_completion",
        stop_source="citrus",
        operation_id="op-test",
        terminal_state="completed",
        reason="protocol_finished",
    )
    wrong_terminal_check = module.summarize_orange_citrus_notify_stop_status(
        wrong_terminal_status,
        {
            "experiment": {
                "terminal_state": "stopped",
                "terminal_reason": "stopped_by_local_control",
            }
        },
        stop_policy="citrus_completion_notify",
        operation_id="op-test",
    )
    require(
        not wrong_terminal_check["ok"],
        "notify policy should fail when Orange stop terminal metadata does not match Citrus",
    )
    require(
        any("terminal state mismatch" in failure for failure in wrong_terminal_check["failures"]),
        "notify terminal failure should identify the bad terminal state",
    )


def test_persist_artifacts_copies_logs_into_recording_folder() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording_folder = root / "recording"
        recording_folder.mkdir()
        orange_log = root / "orange.log"
        citrus_log = root / "citrus.log"
        orange_control_log = root / "orange_local_control.events.jsonl"
        validation_json = root / "validation.json"
        orange_log.write_text("orange-log\n", encoding="utf-8")
        citrus_log.write_text("citrus-log\n", encoding="utf-8")
        orange_control_log.write_text(
            json.dumps(
                {
                    "schema_id": "orange.local_control.gui_event",
                    "schema_version": 1,
                    "event": "recording_stop_triggered",
                    "event_at_utc": "2026-05-29T00:00:01Z",
                    "request_id": "stop-req",
                    "operation_id": "op-artifacts",
                }
            )
            + "\n",
            encoding="utf-8",
        )
        validation_json.write_text("{\"ok\": true}\n", encoding="utf-8")
        args = module.parse_args(
            [
                "--execute",
                "--operation-id",
                "op-artifacts",
                "--orange-log",
                str(orange_log),
                "--orange-local-control-log",
                str(orange_control_log),
                "--citrus-log",
                str(citrus_log),
                "--validation-artifact",
                f"orange_validation_1={validation_json}",
            ]
        )
        orchestrator = module.Orchestrator(args)
        summary = {
            "schema_id": module.SUMMARY_SCHEMA_ID,
            "schema_version": module.SUMMARY_SCHEMA_VERSION,
            "result": "pass",
            "orange": {"recording_folder": str(recording_folder)},
            "citrus": {},
            "validations": [
                {
                    "label": "orange_validation_1",
                    "artifact_paths": [str(validation_json)],
                }
            ],
        }

        orchestrator.persist_artifacts(summary)

        artifact_dir = recording_folder / "orchestrator"
        require((artifact_dir / "orange.log").read_text(encoding="utf-8") == "orange-log\n", "Orange log should be copied")
        require((artifact_dir / "citrus.log").read_text(encoding="utf-8") == "citrus-log\n", "Citrus log should be copied")
        require(
            (artifact_dir / "orange_local_control.events.jsonl").read_text(encoding="utf-8")
            == orange_control_log.read_text(encoding="utf-8"),
            "Orange local-control event log should be copied",
        )
        require(
            (artifact_dir / "orange_validation_1_validation.json").read_text(encoding="utf-8")
            == "{\"ok\": true}\n",
            "validation JSON should be copied",
        )
        artifact_summary = artifact_dir / "orchestrator_summary.json"
        require(artifact_summary.exists(), "artifact summary should be written")
        payload = json.loads(artifact_summary.read_text(encoding="utf-8"))
        require(
            payload["artifacts"]["artifact_dir"] == str(artifact_dir),
            "artifact summary should record artifact directory",
        )
        require(
            payload["artifacts"]["logs"]["orange"]["copied"],
            "artifact summary should report copied Orange log",
        )
        require(
            payload["artifacts"]["logs"]["orange_local_control"]["copied"],
            "artifact summary should report copied Orange local-control event log",
        )
        require(
            payload["artifacts"]["validations"]["orange_validation_1"][0]["copied"],
            "artifact summary should report copied validation artifact",
        )


def test_orange_local_control_event_log_summary() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        log_path = root / "orange.events.jsonl"
        rows = [
            {
                "received_at_utc": "2026-05-29T00:00:00Z",
                "request": {"method": "stop_recording"},
                "response": {
                    "request_id": "stop-req",
                    "operation_id": "op-log",
                },
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_start_triggered",
                "event_at_utc": "2026-05-29T00:00:00Z",
                "request_id": "start-req",
                "operation_id": "op-log",
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_stop_triggered",
                "event_at_utc": "2026-05-29T00:00:01Z",
                "request_id": "stop-req",
                "operation_id": "op-log",
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_drain_finalized",
                "event_at_utc": "2026-05-29T00:00:02Z",
                "request_id": "stop-req",
                "operation_id": "op-log",
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_drain_forced_finalize_requested",
                "event_at_utc": "2026-05-29T00:00:03Z",
                "request_id": "stop-req",
                "operation_id": "op-log",
            },
            "not json",
        ]
        with log_path.open("w", encoding="utf-8") as handle:
            for row in rows:
                if isinstance(row, str):
                    handle.write(row + "\n")
                else:
                    handle.write(json.dumps(row) + "\n")

        summary = module.summarize_orange_local_control_event_log(str(log_path))
        require(summary["exists"], "event log should exist")
        require(summary["row_count"] == 6, "event log row count should parse")
        require(summary["socket_event_count"] == 1, "socket event count should parse")
        require(summary["gui_event_count"] == 4, "GUI event count should parse")
        require(summary["invalid_row_count"] == 1, "invalid row count should parse")
        require(summary["events"]["recording_start_triggered"] == 1, "start trigger event should count")
        require(summary["events"]["recording_stop_triggered"] == 1, "stop trigger event should count")
        require(
            summary["events"]["recording_drain_forced_finalize_requested"] == 1,
            "forced-finalize event should count",
        )
        require(summary["has_start_triggered"], "start-trigger flag should be true")
        require(summary["has_stop_triggered"], "stop-trigger flag should be true")
        require(
            summary["has_forced_finalize_requested"],
            "forced-finalize flag should be true",
        )
        require(summary["has_drain_finalized"], "drain-finalized flag should be true")
        require(summary["request_ids"] == ["start-req", "stop-req"], "request ids should summarize")
        require(summary["operation_ids"] == ["op-log"], "operation ids should summarize")


def test_orange_local_control_event_log_required_check() -> None:
    module = load_module()
    status = orange_status(True, True)
    status["local_control"]["recording_stop"]["request_id"] = "op-log:orange:stop_recording"
    status["local_control"]["recording_stop"]["operation_id"] = "op-log"
    event_log = {
        "path": "/tmp/orange.events.jsonl",
        "exists": True,
        "row_count": 5,
        "socket_event_count": 2,
        "gui_event_count": 3,
        "invalid_row_count": 0,
        "request_ids": [
            "op-log:orange:start_recording",
            "op-log:orange:stop_recording",
        ],
        "operation_ids": ["op-log"],
        "has_start_triggered": True,
        "has_stop_triggered": True,
        "has_drain_timeout": False,
        "has_forced_finalize_requested": False,
        "has_drain_finalized": True,
    }
    ok_check = module.check_orange_local_control_event_log(
        event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(ok_check["ok"], f"valid event log should pass: {ok_check}")

    missing_check = module.check_orange_local_control_event_log(
        {"path": "/tmp/missing.events.jsonl", "exists": False},
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(not missing_check["ok"], "missing required event log should fail")
    require(
        any("missing" in failure for failure in missing_check["failures"]),
        "missing check should identify missing event log",
    )

    timeout_status = orange_status(True, True, drain_timed_out=True)
    timeout_status["local_control"]["recording_stop"]["request_id"] = "op-log:orange:stop_recording"
    timeout_status["local_control"]["recording_stop"]["operation_id"] = "op-log"
    timeout_check = module.check_orange_local_control_event_log(
        event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=timeout_status,
    )
    require(not timeout_check["ok"], "drain-timeout status should require timeout event evidence")
    require(
        any("recording_drain_timeout" in failure for failure in timeout_check["failures"]),
        "timeout failure should name the missing drain-timeout event",
    )

    timeout_event_log = dict(event_log)
    timeout_event_log["has_drain_timeout"] = True
    timeout_forced_check = module.check_orange_local_control_event_log(
        timeout_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=timeout_status,
    )
    require(
        not timeout_forced_check["ok"],
        "drain-timeout status should require forced-finalize event evidence",
    )
    require(
        any(
            "recording_drain_forced_finalize_requested" in failure
            for failure in timeout_forced_check["failures"]
        ),
        "timeout failure should name the missing forced-finalize event",
    )

    timeout_event_log["has_forced_finalize_requested"] = True
    timeout_ok_check = module.check_orange_local_control_event_log(
        timeout_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=timeout_status,
    )
    require(
        timeout_ok_check["ok"],
        f"timeout event log with forced-finalize evidence should pass: {timeout_ok_check}",
    )


def test_execute_requires_orange_local_control_event_log_when_enabled() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        orange_state = {"started": False, "stopped": False}
        citrus_state = {"started": False, "status_polls_after_start": 0}

        def handle_orange(request: dict[str, Any]) -> dict[str, Any]:
            method = request.get("method")
            if method == "start_recording":
                orange_state["started"] = True
            elif method == "stop_recording":
                orange_state["stopped"] = True
            return response_for(
                request,
                orange_status(orange_state["started"], orange_state["stopped"]),
            )

        def handle_citrus(request: dict[str, Any]) -> dict[str, Any]:
            method = request.get("method")
            if method == "start_experiment":
                citrus_state["started"] = True
            elif method == "status" and citrus_state["started"]:
                citrus_state["status_polls_after_start"] += 1
            terminal = citrus_state["status_polls_after_start"] >= 1
            return response_for(
                request,
                citrus_status(citrus_state["started"], terminal),
            )

        def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
            if socket_path == "orange-require-events.sock":
                return handle_orange(request)
            if socket_path == "citrus-require-events.sock":
                return handle_citrus(request)
            raise AssertionError(f"unexpected socket path: {socket_path}")

        original_send = module.send_unix_json
        module.send_unix_json = fake_send
        try:
            args = module.parse_args(
                [
                    "--execute",
                    "--operation-id",
                    "op-require-events",
                    "--orange-socket",
                    "orange-require-events.sock",
                    "--orange-local-control-log",
                    str(root / "missing_orange.events.jsonl"),
                    "--citrus-socket",
                    "citrus-require-events.sock",
                    "--poll-interval-seconds",
                    "0.01",
                    "--timeout-seconds",
                    "2",
                    "--citrus-terminal-timeout-seconds",
                    "2",
                    "--orange-finalize-timeout-seconds",
                    "2",
                    "--require-orange-local-control-event-log",
                ]
            )
            try:
                module.Orchestrator(args).run()
            except module.OrchestratorError as exc:
                require(
                    "event-log check failed" in str(exc),
                    "required missing event log should fail the run",
                )
            else:
                raise AssertionError("expected missing required event log to fail")
        finally:
            module.send_unix_json = original_send


def test_failure_summary_uses_last_known_status_for_artifacts() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording_folder = root / "recording"
        recording_folder.mkdir()
        orange_log = root / "orange.log"
        citrus_log = root / "citrus.log"
        orange_log.write_text("orange-log\n", encoding="utf-8")
        citrus_log.write_text("citrus-log\n", encoding="utf-8")
        args = module.parse_args(
            [
                "--execute",
                "--operation-id",
                "op-failure-artifacts",
                "--orange-log",
                str(orange_log),
                "--citrus-log",
                str(citrus_log),
            ]
        )
        orchestrator = module.Orchestrator(args)
        orchestrator.last_orange_status = {
            "phase": "recording",
            "readiness": {"recording_finalized": False},
            "recording": {"folder": str(recording_folder)},
        }
        orchestrator.last_citrus_status = citrus_status(True, True)

        summary = orchestrator.summary("fail", error="synthetic failure")
        orchestrator.persist_artifacts(summary)

        require(
            summary["orange"]["recording_folder"] == str(recording_folder),
            "failure summary should retain the last known Orange recording folder",
        )
        require(
            summary["citrus"]["perf_jsonl_path"] == "/tmp/citrus_perf.jsonl",
            "failure summary should retain the last known Citrus perf path",
        )
        require(
            (recording_folder / "orchestrator" / "orange.log").exists(),
            "failure artifact persistence should copy logs when a recording folder was observed",
        )


def test_wait_reports_launched_process_exit() -> None:
    module = load_module()
    args = module.parse_args(
        [
            "--execute",
            "--operation-id",
            "op-process-exit",
            "--poll-interval-seconds",
            "0.01",
            "--socket-timeout-seconds",
            "0.01",
            "--timeout-seconds",
            "1",
        ]
    )
    orchestrator = module.Orchestrator(args)
    command = f"{shlex.quote(sys.executable)} -c {shlex.quote('import sys; sys.exit(7)')}"
    orchestrator.start_process("orange", command, {}, "")
    try:
        orchestrator.wait_for_status(
            "orange",
            module.ORANGE_REQUEST_SCHEMA_ID,
            "/tmp/orange_citrus_missing_process_exit.sock",
            lambda status: False,
            1,
            "ready_for_recording_request",
        )
    except module.OrchestratorError as exc:
        message = str(exc)
        require("orange process exited" in message, "error should name exited process")
        require("returncode=7" in message, "error should include return code")
    else:
        raise AssertionError("expected wait_for_status to fail on launched process exit")
    require(
        orchestrator.started_processes[0]["returncode"] == 7,
        "started process metadata should capture return code",
    )


def test_post_terminal_citrus_exit_is_not_an_orange_wait_failure() -> None:
    module = load_module()
    args = module.parse_args(
        [
            "--execute",
            "--operation-id",
            "op-citrus-exit-after-terminal",
            "--poll-interval-seconds",
            "0.01",
        ]
    )
    orchestrator = module.Orchestrator(args)
    command = f"{shlex.quote(sys.executable)} -c {shlex.quote('import sys; sys.exit(0)')}"
    orchestrator.start_process("citrus", command, {}, "")
    while orchestrator.processes["citrus"].poll() is None:
        pass
    orchestrator.citrus_control_complete = True
    orchestrator.raise_if_started_process_exited("waiting for orange recording_finalized")
    require(
        orchestrator.started_processes[0]["returncode"] == 0,
        "post-terminal Citrus exit should still be recorded",
    )


def test_clean_orange_exit_after_manifest_finalization_is_accepted() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording_folder = root / "recording"
        recording_folder.mkdir()
        operation_id = "op-clean-orange-exit-finalized"
        (recording_folder / "recording_session.json").write_text(
            json.dumps(
                {
                    "recording": {
                        "control": {
                            "ack_state": "failed_timeout",
                            "drain_completed": True,
                            "drain_timed_out": True,
                            "error_code": "drain_timeout",
                            "forced_finalize_requested": True,
                            "forced_finalize_stream_stop_requested": True,
                            "last_event": "finalized_after_drain_timeout",
                            "method": "stop_recording",
                            "operation_id": operation_id,
                            "request_id": f"{operation_id}:orange:stop_recording",
                        }
                    }
                }
            ),
            encoding="utf-8",
        )
        args = module.parse_args(
            [
                "--execute",
                "--operation-id",
                operation_id,
                "--poll-interval-seconds",
                "0.01",
                "--socket-timeout-seconds",
                "0.01",
                "--timeout-seconds",
                "1",
            ]
        )
        orchestrator = module.Orchestrator(args)
        orchestrator.last_orange_status = {
            "recording": {"folder": str(recording_folder)},
            "readiness": {
                "recording_active": False,
                "recording_finalized": False,
                "recording_finalizing": True,
            },
            "local_control": {
                "recording_stop": {
                    "ack_state": "executing",
                    "operation_id": operation_id,
                }
            },
        }
        command = f"{shlex.quote(sys.executable)} -c {shlex.quote('import sys; sys.exit(0)')}"
        orchestrator.start_process("orange", command, {}, "")
        status = orchestrator.wait_for_status(
            "orange",
            module.ORANGE_REQUEST_SCHEMA_ID,
            str(root / "missing-orange.sock"),
            module.orange_recording_finalized,
            1,
            "recording_finalized",
        )
        require(
            module.orange_recording_finalized(status),
            "manifest-backed clean Orange exit should satisfy finalized predicate",
        )
        require(
            module.orange_recording_stop_ack_state(status) == "failed_timeout",
            "manifest-backed status should preserve timeout ack state",
        )
        require(
            orchestrator.steps[-1].detail.get("accepted_clean_process_exit"),
            "wait step should record that a clean Orange exit was accepted",
        )


def test_cleanup_started_processes_terminates_launched_children() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        citrus_socket = root / "citrus.sock"
        citrus_socket.touch()
        args = module.parse_args(
            [
                "--execute",
                "--operation-id",
                "op-cleanup-child",
                "--citrus-socket",
                str(citrus_socket),
            ]
        )
        orchestrator = module.Orchestrator(args)
        command = f"{shlex.quote(sys.executable)} -c {shlex.quote('import time; time.sleep(60)')}"
        orchestrator.start_process("citrus", command, {}, "")
        try:
            orchestrator.cleanup_started_processes(labels={"citrus"}, terminate_timeout_seconds=1.0)
            orchestrator.cleanup_launched_socket_files()
            process = orchestrator.processes["citrus"]
            require(process.poll() is not None, "cleanup should terminate the launched process")
            require(
                orchestrator.started_processes[0]["returncode"] is not None,
                "cleanup should record the launched process return code",
            )
            require(
                "cleanup_action" in orchestrator.started_processes[0],
                "cleanup should record its action",
            )
            require(not citrus_socket.exists(), "cleanup should remove launched stale socket")
        finally:
            process = orchestrator.processes.get("citrus")
            if process is not None and process.poll() is None:
                process.kill()


def test_launch_socket_preflight_refuses_live_socket() -> None:
    module = load_module()
    args = module.parse_args(
        [
            "--execute",
            "--operation-id",
            "op-live-socket",
            "--launch-socket-preflight-timeout-seconds",
            "0.01",
        ]
    )
    orchestrator = module.Orchestrator(args)

    def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
        return response_for(request, orange_status(False, False))

    original_send = module.send_unix_json
    module.send_unix_json = fake_send
    try:
        try:
            orchestrator.preflight_launch_socket(
                "orange",
                module.ORANGE_REQUEST_SCHEMA_ID,
                "/tmp/already-live.sock",
                False,
            )
        except module.OrchestratorError as exc:
            require("already answering before launch" in str(exc), "preflight should reject live socket")
        else:
            raise AssertionError("expected live socket preflight to fail")
    finally:
        module.send_unix_json = original_send


def test_launch_socket_preflight_allows_absent_socket() -> None:
    module = load_module()
    args = module.parse_args(["--execute", "--operation-id", "op-missing-socket"])
    orchestrator = module.Orchestrator(args)

    def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
        raise FileNotFoundError("missing")

    original_send = module.send_unix_json
    module.send_unix_json = fake_send
    try:
        orchestrator.preflight_launch_socket(
            "orange",
            module.ORANGE_REQUEST_SCHEMA_ID,
            "/tmp/missing.sock",
            False,
        )
    finally:
        module.send_unix_json = original_send
    require(orchestrator.steps[-1].ok, "missing socket preflight should pass")


def main() -> int:
    tests = [
        test_request_builders_and_readiness_helpers,
        test_dry_run_default_does_not_open_sockets,
        test_dry_run_launch_socket_preflight_flags,
        test_execute_against_fake_local_control_servers,
        test_execute_waits_for_citrus_completion_notify_stop,
        test_execute_stops_citrus_after_run_seconds,
        test_launch_preflights_all_sockets_before_starting_processes,
        test_orchestrator_fails_on_orange_drain_timeout_by_default,
        test_orchestrator_checks_orange_stop_ack_state,
        test_orchestrator_checks_citrus_notify_stop_status,
        test_persist_artifacts_copies_logs_into_recording_folder,
        test_orange_local_control_event_log_summary,
        test_orange_local_control_event_log_required_check,
        test_execute_requires_orange_local_control_event_log_when_enabled,
        test_failure_summary_uses_last_known_status_for_artifacts,
        test_wait_reports_launched_process_exit,
        test_post_terminal_citrus_exit_is_not_an_orange_wait_failure,
        test_clean_orange_exit_after_manifest_finalization_is_accepted,
        test_cleanup_started_processes_terminates_launched_children,
        test_launch_socket_preflight_refuses_live_socket,
        test_launch_socket_preflight_allows_absent_socket,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("orange_citrus_orchestrator_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
