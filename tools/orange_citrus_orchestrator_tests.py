#!/usr/bin/env python3
"""Focused tests for scripts/orange_citrus_orchestrator.py."""

from __future__ import annotations

import copy
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
        not module.orange_ready_for_recording(
            {"readiness": {"ready_for_recording_request": "true"}}
        ),
        "orange readiness helper must reject truthy-string booleans",
    )
    require(
        not module.orange_recording_finalized(
            {"readiness": {"recording_finalized": "true"}}
        ),
        "orange finalized helper must reject truthy-string booleans",
    )
    require(
        not module.orange_recording_stop_drain_timed_out(orange_status(True, True)),
        "orange drain timeout should default false",
    )
    string_timeout_status = orange_status(True, True)
    string_timeout_status["local_control"]["recording_stop"]["drain_timed_out"] = "true"
    require(
        not module.orange_recording_stop_drain_timed_out(string_timeout_status),
        "orange drain timeout helper must reject truthy-string booleans",
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
    require(
        module.response_accepted({"ok": True, "accepted": True}) is True,
        "accepted response with JSON booleans should pass",
    )
    require(
        module.response_accepted({"ok": True, "duplicate": True}) is True,
        "duplicate response with JSON booleans should pass",
    )
    require(
        module.response_accepted({"ok": "true", "accepted": True}) is False,
        "response acceptance must reject truthy-string ok",
    )
    require(
        module.response_accepted({"ok": True, "accepted": "true"}) is False,
        "response acceptance must reject truthy-string accepted",
    )
    require(
        module.response_accepted({"ok": True, "duplicate": "true"}) is False,
        "response acceptance must reject truthy-string duplicate",
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
    require(
        not module.citrus_ready_to_start({"readiness": {"ready_to_start": "true"}}),
        "citrus readiness helper must reject truthy-string booleans",
    )
    require(module.citrus_is_terminal(citrus_status(True, True)), "citrus terminal")
    require(module.citrus_perf_jsonl_path_known(citrus_status(True, True)), "perf path known")
    require(
        not module.citrus_perf_jsonl_path_known(
            {"output": {"perf_jsonl_path_known": "true"}}
        ),
        "citrus perf path-known helper must reject truthy-string booleans",
    )

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
        bad_error_status = orange_status(True, True, drain_timed_out=True)
        bad_error_status["local_control"]["recording_stop"]["error_code"] = ""
        bad_error_failures = module.check_orange_drain_timeout_status(bad_error_status)
        require(
            any("error_code=''" in failure for failure in bad_error_failures),
            "timeout status check should require drain_timeout error code when present",
        )
        impossible_failed_ack_status = orange_status(True, True)
        impossible_failed_ack_status["local_control"]["recording_stop"][
            "ack_state"
        ] = "failed_timeout"
        impossible_failed_ack_check = module.summarize_orange_drain_timeout_status(
            impossible_failed_ack_status,
            allow_drain_timeout=True,
        )
        require(
            not impossible_failed_ack_check["ok"],
            "failed_timeout ACK without drain_timed_out should fail consistency",
        )
        require(
            any(
                "failed-timeout ACK" in failure
                for failure in impossible_failed_ack_check["consistency_failures"]
            ),
            "failed_timeout ACK consistency failure should name failed-timeout ACK",
        )
        impossible_forced_status = orange_status(True, True)
        impossible_forced_status["local_control"]["recording_stop"][
            "forced_finalize_requested"
        ] = True
        impossible_forced_failures = module.check_orange_drain_timeout_status(
            impossible_forced_status
        )
        require(
            any("forced finalize" in failure for failure in impossible_forced_failures),
            "forced-finalize status without timeout should fail consistency",
        )
        impossible_state_status = orange_status(True, True)
        impossible_state_status["local_control"]["recording_stop"][
            "state"
        ] = "finalized_after_drain_timeout"
        impossible_state_failures = module.check_orange_drain_timeout_status(
            impossible_state_status
        )
        require(
            any("finalized-after-drain-timeout state" in failure for failure in impossible_state_failures),
            "finalized-after-timeout state without timeout should fail consistency",
        )
        string_bool_status = orange_status(True, True)
        string_bool_status["local_control"]["recording_stop"][
            "drain_timed_out"
        ] = "true"
        string_bool_status["local_control"]["recording_stop"][
            "forced_finalize_requested"
        ] = "true"
        string_bool_status["local_control"]["recording_stop"][
            "forced_finalize_stream_stop_requested"
        ] = "false"
        string_bool_failures = module.check_orange_drain_timeout_status(
            string_bool_status
        )
        require(
            sum("expected JSON boolean" in failure for failure in string_bool_failures) == 3,
            f"status check should reject non-boolean stop flags: {string_bool_failures}",
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
                "request": {
                    "method": "stop_recording",
                    "request_id": "stop-req",
                    "source": "orange_citrus_orchestrator",
                },
                "response": {
                    "method": "stop_recording",
                    "request_id": "stop-req",
                    "operation_id": "op-log",
                    "ok": True,
                    "accepted": True,
                    "responded_at_utc": "2026-05-29T00:00:00Z",
                    "duplicate": False,
                    "queued_for_gui_thread": True,
                },
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "gui_command_accepted",
                "event_at_utc": "2026-05-29T00:00:00Z",
                "request_id": "start-req",
                "operation_id": "op-log",
                "method": "start_recording",
                "command_source": "orange_citrus_orchestrator",
                "received_at_utc": "2026-05-29T00:00:00Z",
                "start_enabled": True,
                "stop_enabled": True,
                "stop_recording_enabled": True,
                "citrus_completion_enabled": True,
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_start_queued",
                "event_at_utc": "2026-05-29T00:00:00Z",
                "request_id": "start-req",
                "operation_id": "op-log",
                "method": "start_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_start",
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_start_triggered",
                "event_at_utc": "2026-05-29T00:00:00Z",
                "request_id": "start-req",
                "operation_id": "op-log",
                "method": "start_recording",
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_stop_scheduled",
                "event_at_utc": "2026-05-29T00:00:01Z",
                "request_id": "stop-req",
                "operation_id": "op-log",
                "method": "stop_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_stop",
                "grace_seconds": 0.0,
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_stop_triggered",
                "event_at_utc": "2026-05-29T00:00:01Z",
                "request_id": "stop-req",
                "operation_id": "op-log",
                "method": "stop_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_stop",
                "grace_seconds": 0.0,
            },
            {
                "schema_id": "orange.local_control.gui_event",
                "schema_version": 1,
                "event": "recording_drain_finalized",
                "event_at_utc": "2026-05-29T00:00:02Z",
                "request_id": "stop-req",
                "operation_id": "op-log",
                "method": "stop_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_stop",
                "drain_timed_out": False,
                "health": "ok",
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
        require(summary["row_count"] == 9, "event log row count should parse")
        require(summary["socket_event_count"] == 1, "socket event count should parse")
        require(summary["gui_event_count"] == 7, "GUI event count should parse")
        require(summary["invalid_row_count"] == 1, "invalid row count should parse")
        require(
            summary["first_socket_received_at_utc"] == "2026-05-29T00:00:00Z",
            "first socket timestamp should summarize",
        )
        require(
            summary["last_socket_responded_at_utc"] == "2026-05-29T00:00:00Z",
            "last socket response timestamp should summarize",
        )
        require(
            summary["first_gui_event_at_utc"] == "2026-05-29T00:00:00Z",
            "first GUI lifecycle timestamp should summarize",
        )
        require(
            summary["last_gui_event_at_utc"] == "2026-05-29T00:00:03Z",
            "last GUI lifecycle timestamp should summarize",
        )
        require(summary["events"]["recording_start_triggered"] == 1, "start trigger event should count")
        require(summary["events"]["recording_start_queued"] == 1, "start queued event should count")
        require(summary["events"]["recording_stop_scheduled"] == 1, "stop scheduled event should count")
        require(summary["events"]["recording_stop_triggered"] == 1, "stop trigger event should count")
        require(summary["events"]["gui_command_accepted"] == 1, "accepted GUI event should count")
        require(len(summary["socket_request_events"]) == 1, "socket request event should summarize")
        require(
            summary["socket_request_events"][0]["row_index"] == 1,
            "socket request event should preserve row index",
        )
        require(
            summary["socket_request_events"][0]["source"] == "orange_citrus_orchestrator",
            "socket request event should preserve source",
        )
        require(
            summary["socket_request_events"][0]["queued_for_gui_thread"] is True,
            "socket request event should preserve queued_for_gui_thread",
        )
        accepted_events = module.event_log_lifecycle_events_for_request(
            summary,
            "start-req",
            event_name="gui_command_accepted",
        )
        require(len(accepted_events) == 1, "accepted GUI event should summarize")
        require(
            accepted_events[0]["row_index"] == 2,
            "accepted GUI event should preserve row index",
        )
        require(
            accepted_events[0]["start_enabled"] is True,
            "accepted GUI event should preserve start_enabled",
        )
        require(
            summary["events"]["recording_drain_forced_finalize_requested"] == 1,
            "forced-finalize event should count",
        )
        require(summary["has_start_triggered"], "start-trigger flag should be true")
        require(summary["has_stop_scheduled"], "stop-scheduled flag should be true")
        require(summary["has_stop_triggered"], "stop-trigger flag should be true")
        require(
            summary["has_forced_finalize_requested"],
            "forced-finalize flag should be true",
        )
        require(summary["has_drain_finalized"], "drain-finalized flag should be true")
        require(summary["request_ids"] == ["start-req", "stop-req"], "request ids should summarize")
        require(summary["operation_ids"] == ["op-log"], "operation ids should summarize")
        stop_events = module.event_log_lifecycle_events_for_request(
            summary,
            "stop-req",
            event_name="recording_stop_triggered",
        )
        require(len(stop_events) == 1, "stop trigger lifecycle event should summarize")
        require(
            stop_events[0]["row_index"] == 6,
            "stop trigger lifecycle event should preserve row index",
        )
        require(
            stop_events[0]["method"] == "stop_recording",
            "lifecycle event should preserve stop method",
        )
        require(
            stop_events[0]["grace_seconds"] == 0.0,
            "lifecycle event should preserve stop grace",
        )
        finalize_events = module.event_log_lifecycle_events_for_request(
            summary,
            "stop-req",
            event_name="recording_drain_finalized",
        )
        require(
            finalize_events[0]["drain_timed_out"] is False,
            "finalize event should preserve drain timeout status",
        )
        require(
            finalize_events[0]["health"] == "ok",
            "finalize event should preserve health",
        )


def test_orange_local_control_event_log_required_check() -> None:
    module = load_module()
    status = orange_status(True, True)
    status["local_control"]["recording_stop"]["request_id"] = "op-log:orange:stop_recording"
    status["local_control"]["recording_stop"]["operation_id"] = "op-log"
    event_log = {
        "path": "/tmp/orange.events.jsonl",
        "exists": True,
        "row_count": 9,
        "socket_event_count": 2,
        "gui_event_count": 7,
        "invalid_row_count": 0,
        "request_ids": [
            "op-log:orange:start_recording",
            "op-log:orange:stop_recording",
        ],
        "operation_ids": ["op-log"],
        "socket_request_events": [
            {
                "row_index": 1,
                "request_id": "op-log:orange:start_recording",
                "operation_id": "op-log",
                "method": "start_recording",
                "source": "orange_citrus_orchestrator",
                "ok": True,
                "accepted": True,
                "queued_for_gui_thread": True,
            },
            {
                "row_index": 5,
                "request_id": "op-log:orange:stop_recording",
                "operation_id": "op-log",
                "method": "stop_recording",
                "source": "orange_citrus_orchestrator",
                "ok": True,
                "accepted": True,
                "queued_for_gui_thread": True,
            },
        ],
        "has_start_triggered": True,
        "has_stop_triggered": True,
        "has_drain_timeout": False,
        "has_forced_finalize_requested": False,
        "has_drain_finalized": True,
        "gui_lifecycle_events": [
            {
                "row_index": 2,
                "event": "gui_command_accepted",
                "request_id": "op-log:orange:start_recording",
                "operation_id": "op-log",
                "method": "start_recording",
                "command_source": "orange_citrus_orchestrator",
                "start_enabled": True,
                "stop_enabled": True,
                "stop_recording_enabled": True,
                "citrus_completion_enabled": True,
            },
            {
                "row_index": 3,
                "event": "recording_start_queued",
                "request_id": "op-log:orange:start_recording",
                "operation_id": "op-log",
                "method": "start_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_start",
            },
            {
                "row_index": 4,
                "event": "recording_start_triggered",
                "request_id": "op-log:orange:start_recording",
                "operation_id": "op-log",
                "method": "start_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_start",
            },
            {
                "row_index": 6,
                "event": "gui_command_accepted",
                "request_id": "op-log:orange:stop_recording",
                "operation_id": "op-log",
                "method": "stop_recording",
                "command_source": "orange_citrus_orchestrator",
                "start_enabled": True,
                "stop_enabled": True,
                "stop_recording_enabled": True,
                "citrus_completion_enabled": True,
            },
            {
                "row_index": 7,
                "event": "recording_stop_scheduled",
                "request_id": "op-log:orange:stop_recording",
                "operation_id": "op-log",
                "method": "stop_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_stop",
                "grace_seconds": 0.0,
            },
            {
                "row_index": 8,
                "event": "recording_stop_triggered",
                "request_id": "op-log:orange:stop_recording",
                "operation_id": "op-log",
                "method": "stop_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_stop",
            },
            {
                "row_index": 9,
                "event": "recording_drain_finalized",
                "request_id": "op-log:orange:stop_recording",
                "operation_id": "op-log",
                "method": "stop_recording",
                "command_source": "orange_citrus_orchestrator",
                "reason": "orchestrator_stop",
                "drain_timed_out": False,
                "health": "ok",
                "error_code": "",
            },
        ],
    }

    def copy_event_log() -> dict[str, Any]:
        copied = dict(event_log)
        copied["socket_request_events"] = [
            dict(row) for row in event_log["socket_request_events"]
        ]
        copied["gui_lifecycle_events"] = [
            dict(row) for row in event_log["gui_lifecycle_events"]
        ]
        return copied

    def mutate_socket_event(
        copied: dict[str, Any],
        *,
        request_id: str,
        field: str,
        value: Any,
    ) -> None:
        for row in copied["socket_request_events"]:
            if row.get("request_id") == request_id:
                row[field] = value

    def mutate_gui_event(
        copied: dict[str, Any],
        *,
        request_id: str,
        event_name: str,
        field: str,
        value: Any,
    ) -> None:
        for row in copied["gui_lifecycle_events"]:
            if row.get("request_id") == request_id and row.get("event") == event_name:
                row[field] = value

    ok_check = module.check_orange_local_control_event_log(
        event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(ok_check["ok"], f"valid event log should pass: {ok_check}")
    request_chains = {
        item["request_id"]: item for item in ok_check["request_chains"]
    }
    start_chain = request_chains["op-log:orange:start_recording"]
    require(start_chain["socket_rows"] == 1, "start chain should summarize socket rows")
    require(start_chain["has_socket"], "start chain should report socket presence")
    require(start_chain["has_queued_socket"], "start chain should report queued socket presence")
    require(
        start_chain["has_gui_command_accepted"],
        "start chain should report GUI accepted presence",
    )
    require(
        start_chain["has_recording_start_queued"],
        "start chain should report start queued presence",
    )
    require(
        start_chain["has_recording_start_triggered"],
        "start chain should report start trigger presence",
    )
    require(
        start_chain["socket_methods"] == ["start_recording"],
        "start chain should summarize socket method",
    )
    require(
        start_chain["socket_sources"] == ["orange_citrus_orchestrator"],
        "start chain should summarize socket source",
    )
    require(
        start_chain["socket_ok_values"] == [True],
        "start chain should summarize socket ok value",
    )
    require(
        start_chain["socket_accepted_values"] == [True],
        "start chain should summarize socket accepted value",
    )
    require(
        start_chain["socket_queued_for_gui_thread_values"] == [True],
        "start chain should summarize socket queued value",
    )
    require(
        start_chain["gui_command_accepted_methods"] == ["start_recording"],
        "start chain should summarize GUI accepted method",
    )
    require(
        start_chain["gui_command_accepted_sources"] == ["orange_citrus_orchestrator"],
        "start chain should summarize GUI accepted source",
    )
    require(
        start_chain["gui_command_accepted_start_enabled_values"] == [True],
        "start chain should summarize start enabled value",
    )
    require(
        start_chain["recording_start_queued_methods"] == ["start_recording"],
        "start chain should summarize start queued method",
    )
    require(
        start_chain["recording_start_queued_sources"] == ["orange_citrus_orchestrator"],
        "start chain should summarize start queued source",
    )
    require(
        start_chain["recording_start_triggered_methods"] == ["start_recording"],
        "start chain should summarize start trigger method",
    )
    require(
        start_chain["recording_start_triggered_sources"] == ["orange_citrus_orchestrator"],
        "start chain should summarize start trigger source",
    )
    require(
        start_chain["queued_socket_row_indexes"] == [1],
        "start chain should summarize queued socket row index",
    )
    require(
        start_chain["gui_command_accepted_row_indexes"] == [2],
        "start chain should summarize GUI accepted row index",
    )
    require(
        start_chain["recording_start_queued_row_indexes"] == [3],
        "start chain should summarize start queued row index",
    )
    require(
        start_chain["recording_start_triggered_row_indexes"] == [4],
        "start chain should summarize start trigger row index",
    )
    stop_chain = request_chains["op-log:orange:stop_recording"]
    require(stop_chain["has_queued_socket"], "stop chain should report queued socket presence")
    require(
        stop_chain["has_recording_stop_scheduled"],
        "stop chain should report stop scheduled presence",
    )
    require(
        stop_chain["has_recording_stop_triggered"],
        "stop chain should report stop trigger presence",
    )
    require(
        stop_chain["has_recording_drain_finalized"],
        "stop chain should report drain finalized presence",
    )
    require(
        stop_chain["socket_methods"] == ["stop_recording"],
        "stop chain should summarize socket method",
    )
    require(
        stop_chain["recording_stop_scheduled_methods"] == ["stop_recording"],
        "stop chain should summarize stop scheduled method",
    )
    require(
        stop_chain["recording_stop_scheduled_sources"] == ["orange_citrus_orchestrator"],
        "stop chain should summarize stop scheduled source",
    )
    require(
        stop_chain["recording_stop_scheduled_reasons"] == ["orchestrator_stop"],
        "stop chain should summarize stop scheduled reason",
    )
    require(
        stop_chain["recording_stop_triggered_methods"] == ["stop_recording"],
        "stop chain should summarize stop trigger method",
    )
    require(
        stop_chain["recording_stop_triggered_sources"] == ["orange_citrus_orchestrator"],
        "stop chain should summarize stop trigger source",
    )
    require(
        stop_chain["recording_stop_triggered_reasons"] == ["orchestrator_stop"],
        "stop chain should summarize stop trigger reason",
    )
    require(
        stop_chain["recording_drain_finalized_methods"] == ["stop_recording"],
        "stop chain should summarize drain finalized method",
    )
    require(
        stop_chain["recording_drain_finalized_sources"] == ["orange_citrus_orchestrator"],
        "stop chain should summarize drain finalized source",
    )
    require(
        stop_chain["recording_drain_finalized_reasons"] == ["orchestrator_stop"],
        "stop chain should summarize drain finalized reason",
    )
    require(
        stop_chain["recording_drain_finalized_drain_timed_out_values"] == [False],
        "stop chain should summarize clean drain-finalized timeout flag",
    )
    require(
        stop_chain["recording_drain_finalized_healths"] == ["ok"],
        "stop chain should summarize drain-finalized health",
    )
    require(
        stop_chain["gui_command_accepted_stop_enabled_values"] == [True],
        "stop chain should summarize stop enabled value",
    )
    require(
        stop_chain["gui_command_accepted_stop_recording_enabled_values"] == [True],
        "stop chain should summarize stop_recording enabled value",
    )
    require(
        stop_chain["gui_command_accepted_citrus_completion_enabled_values"] == [True],
        "stop chain should summarize Citrus completion enabled value",
    )
    require(
        stop_chain["queued_socket_row_indexes"] == [5],
        "stop chain should summarize queued socket row index",
    )
    require(
        stop_chain["gui_command_accepted_row_indexes"] == [6],
        "stop chain should summarize GUI accepted row index",
    )
    require(
        stop_chain["recording_stop_scheduled_row_indexes"] == [7],
        "stop chain should summarize stop scheduled row index",
    )
    require(
        stop_chain["recording_stop_triggered_row_indexes"] == [8],
        "stop chain should summarize stop trigger row index",
    )
    require(
        stop_chain["recording_drain_finalized_row_indexes"] == [9],
        "stop chain should summarize drain finalized row index",
    )

    missing_start_socket_event_log = dict(event_log)
    missing_start_socket_event_log["socket_request_events"] = [
        dict(row)
        for row in event_log["socket_request_events"]
        if row.get("request_id") != "op-log:orange:start_recording"
    ]
    missing_start_socket_check = module.check_orange_local_control_event_log(
        missing_start_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not missing_start_socket_check["ok"],
        "event log should fail when start request has no socket row",
    )
    require(
        any("start socket request/response" in failure for failure in missing_start_socket_check["failures"]),
        "missing start socket row failure should name start socket request/response",
    )

    rejected_start_socket_event_log = copy_event_log()
    mutate_socket_event(
        rejected_start_socket_event_log,
        request_id="op-log:orange:start_recording",
        field="accepted",
        value=False,
    )
    rejected_start_socket_check = module.check_orange_local_control_event_log(
        rejected_start_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not rejected_start_socket_check["ok"],
        "event log should fail when start socket row was not accepted",
    )
    require(
        any("start socket request" in failure and "accepted" in failure for failure in rejected_start_socket_check["failures"]),
        "rejected start socket row failure should name accepted state",
    )

    unqueued_start_socket_event_log = copy_event_log()
    mutate_socket_event(
        unqueued_start_socket_event_log,
        request_id="op-log:orange:start_recording",
        field="queued_for_gui_thread",
        value=False,
    )
    unqueued_start_socket_check = module.check_orange_local_control_event_log(
        unqueued_start_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not unqueued_start_socket_check["ok"],
        "event log should fail when start socket row was not queued for GUI thread",
    )
    require(
        any("queued_for_gui_thread" in failure for failure in unqueued_start_socket_check["failures"]),
        "unqueued start socket row failure should name queued_for_gui_thread",
    )

    string_queued_start_socket_event_log = copy_event_log()
    mutate_socket_event(
        string_queued_start_socket_event_log,
        request_id="op-log:orange:start_recording",
        field="queued_for_gui_thread",
        value="true",
    )
    string_queued_start_socket_check = module.check_orange_local_control_event_log(
        string_queued_start_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not string_queued_start_socket_check["ok"],
        "event log should fail when start queued flag is a truthy string",
    )
    require(
        any("queued_for_gui_thread" in failure for failure in string_queued_start_socket_check["failures"]),
        "truthy-string queued flag failure should name queued_for_gui_thread",
    )

    missing_start_source_event_log = copy_event_log()
    mutate_socket_event(
        missing_start_source_event_log,
        request_id="op-log:orange:start_recording",
        field="source",
        value="",
    )
    missing_start_source_check = module.check_orange_local_control_event_log(
        missing_start_source_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not missing_start_source_check["ok"],
        "event log should fail when start socket source is empty",
    )
    require(
        any("start socket request" in failure and "source is empty" in failure for failure in missing_start_source_check["failures"]),
        "missing start source failure should name empty source",
    )

    bad_start_source_event_log = copy_event_log()
    mutate_gui_event(
        bad_start_source_event_log,
        request_id="op-log:orange:start_recording",
        event_name="recording_start_triggered",
        field="command_source",
        value="unexpected",
    )
    bad_start_source_check = module.check_orange_local_control_event_log(
        bad_start_source_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not bad_start_source_check["ok"],
        "event log should fail when start trigger source mismatches socket source",
    )
    require(
        any("recording_start_triggered" in failure and "command_source" in failure for failure in bad_start_source_check["failures"]),
        "bad start trigger source failure should name command_source",
    )

    bad_start_method_event_log = copy_event_log()
    mutate_gui_event(
        bad_start_method_event_log,
        request_id="op-log:orange:start_recording",
        event_name="recording_start_triggered",
        field="method",
        value="",
    )
    bad_start_method_check = module.check_orange_local_control_event_log(
        bad_start_method_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not bad_start_method_check["ok"],
        "event log should fail when start trigger method is missing",
    )
    require(
        any("recording_start_triggered" in failure and "method" in failure for failure in bad_start_method_check["failures"]),
        "bad start trigger method failure should name method",
    )

    missing_start_accepted_event_log = dict(event_log)
    missing_start_accepted_event_log["gui_lifecycle_events"] = [
        dict(row)
        for row in event_log["gui_lifecycle_events"]
        if not (
            row.get("event") == "gui_command_accepted"
            and row.get("request_id") == "op-log:orange:start_recording"
        )
    ]
    missing_start_accepted_check = module.check_orange_local_control_event_log(
        missing_start_accepted_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not missing_start_accepted_check["ok"],
        "event log should fail when start request has no GUI accepted row",
    )
    require(
        any("start GUI-thread gui_command_accepted" in failure for failure in missing_start_accepted_check["failures"]),
        "missing start accepted row failure should name gui_command_accepted",
    )

    missing_start_queued_event_log = dict(event_log)
    missing_start_queued_event_log["gui_lifecycle_events"] = [
        dict(row)
        for row in event_log["gui_lifecycle_events"]
        if not (
            row.get("event") == "recording_start_queued"
            and row.get("request_id") == "op-log:orange:start_recording"
        )
    ]
    missing_start_queued_check = module.check_orange_local_control_event_log(
        missing_start_queued_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not missing_start_queued_check["ok"],
        "event log should fail when start request has no start queued row",
    )
    require(
        any("recording_start_queued" in failure for failure in missing_start_queued_check["failures"]),
        "missing start queued row failure should name recording_start_queued",
    )

    bad_start_enabled_event_log = copy_event_log()
    mutate_gui_event(
        bad_start_enabled_event_log,
        request_id="op-log:orange:start_recording",
        event_name="gui_command_accepted",
        field="start_enabled",
        value=False,
    )
    bad_start_enabled_check = module.check_orange_local_control_event_log(
        bad_start_enabled_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not bad_start_enabled_check["ok"],
        "event log should fail when start accepted row reports disabled start control",
    )
    require(
        any("start_enabled" in failure for failure in bad_start_enabled_check["failures"]),
        "bad start accepted row failure should name start_enabled",
    )

    string_start_enabled_event_log = copy_event_log()
    mutate_gui_event(
        string_start_enabled_event_log,
        request_id="op-log:orange:start_recording",
        event_name="gui_command_accepted",
        field="start_enabled",
        value="true",
    )
    string_start_enabled_check = module.check_orange_local_control_event_log(
        string_start_enabled_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not string_start_enabled_check["ok"],
        "event log should fail when start_enabled is a truthy string",
    )
    require(
        any("start_enabled" in failure for failure in string_start_enabled_check["failures"]),
        "truthy-string start_enabled failure should name start_enabled",
    )

    bad_start_order_event_log = copy_event_log()
    mutate_gui_event(
        bad_start_order_event_log,
        request_id="op-log:orange:start_recording",
        event_name="gui_command_accepted",
        field="row_index",
        value=5,
    )
    bad_start_order_check = module.check_orange_local_control_event_log(
        bad_start_order_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not bad_start_order_check["ok"],
        "event log should fail when start accepted row follows start queued",
    )
    require(
        any("out-of-order" in failure and "recording_start_queued" in failure for failure in bad_start_order_check["failures"]),
        "bad start order failure should name the start queued order",
    )

    missing_socket_event_log = dict(event_log)
    missing_socket_event_log["socket_request_events"] = [
        dict(row)
        for row in event_log["socket_request_events"]
        if row.get("request_id") != "op-log:orange:stop_recording"
    ]
    missing_socket_check = module.check_orange_local_control_event_log(
        missing_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not missing_socket_check["ok"],
        "event log should fail when final stop request has no socket row",
    )
    require(
        any("socket request/response" in failure for failure in missing_socket_check["failures"]),
        "missing stop socket row failure should name socket request/response",
    )

    rejected_socket_event_log = copy_event_log()
    mutate_socket_event(
        rejected_socket_event_log,
        request_id="op-log:orange:stop_recording",
        field="accepted",
        value=False,
    )
    rejected_socket_check = module.check_orange_local_control_event_log(
        rejected_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not rejected_socket_check["ok"],
        "event log should fail when final stop socket row was not accepted",
    )
    require(
        any("accepted" in failure for failure in rejected_socket_check["failures"]),
        "rejected stop socket row failure should name accepted state",
    )

    unqueued_stop_socket_event_log = copy_event_log()
    mutate_socket_event(
        unqueued_stop_socket_event_log,
        request_id="op-log:orange:stop_recording",
        field="queued_for_gui_thread",
        value=False,
    )
    unqueued_stop_socket_check = module.check_orange_local_control_event_log(
        unqueued_stop_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not unqueued_stop_socket_check["ok"],
        "event log should fail when stop socket row was not queued for GUI thread",
    )
    require(
        any("queued_for_gui_thread" in failure for failure in unqueued_stop_socket_check["failures"]),
        "unqueued stop socket row failure should name queued_for_gui_thread",
    )

    string_accepted_stop_socket_event_log = copy_event_log()
    mutate_socket_event(
        string_accepted_stop_socket_event_log,
        request_id="op-log:orange:stop_recording",
        field="accepted",
        value="true",
    )
    string_accepted_stop_socket_check = module.check_orange_local_control_event_log(
        string_accepted_stop_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not string_accepted_stop_socket_check["ok"],
        "event log should fail when stop accepted flag is a truthy string",
    )
    require(
        any("accepted" in failure for failure in string_accepted_stop_socket_check["failures"]),
        "truthy-string stop accepted failure should name accepted",
    )

    failed_socket_event_log = copy_event_log()
    mutate_socket_event(
        failed_socket_event_log,
        request_id="op-log:orange:stop_recording",
        field="ok",
        value=False,
    )
    failed_socket_check = module.check_orange_local_control_event_log(
        failed_socket_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not failed_socket_check["ok"],
        "event log should fail when final stop socket row was not ok",
    )
    require(
        any("ok" in failure for failure in failed_socket_check["failures"]),
        "failed stop socket row failure should name ok state",
    )

    bad_stop_event_log = copy_event_log()
    mutate_gui_event(
        bad_stop_event_log,
        request_id="op-log:orange:stop_recording",
        event_name="recording_stop_triggered",
        field="method",
        value="citrus_completion",
    )
    bad_stop_check = module.check_orange_local_control_event_log(
        bad_stop_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not bad_stop_check["ok"],
        "stop_recording event log should fail when trigger method mismatches final status",
    )
    require(
        any("method" in failure for failure in bad_stop_check["failures"]),
        "stop_recording event-log failure should name method mismatch",
    )

    missing_stop_source_status = orange_status(True, True)
    missing_stop_source_status["local_control"]["recording_stop"][
        "request_id"
    ] = "op-log:orange:stop_recording"
    missing_stop_source_status["local_control"]["recording_stop"][
        "operation_id"
    ] = "op-log"
    missing_stop_source_status["local_control"]["recording_stop"]["source"] = ""
    missing_stop_source_check = module.check_orange_local_control_event_log(
        event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=missing_stop_source_status,
    )
    require(
        not missing_stop_source_check["ok"],
        "event log should fail when final stop status has no command source",
    )
    require(
        any("command_source/source" in failure for failure in missing_stop_source_check["failures"]),
        "missing stop source failure should name source metadata",
    )

    missing_stop_method_status = orange_status(True, True)
    missing_stop_method_status["local_control"]["recording_stop"][
        "request_id"
    ] = "op-log:orange:stop_recording"
    missing_stop_method_status["local_control"]["recording_stop"][
        "operation_id"
    ] = "op-log"
    missing_stop_method_status["local_control"]["recording_stop"]["method"] = ""
    missing_stop_method_check = module.check_orange_local_control_event_log(
        event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=missing_stop_method_status,
    )
    require(
        not missing_stop_method_check["ok"],
        "event log should fail when final stop status has no method",
    )
    require(
        any("recording_stop.method" in failure for failure in missing_stop_method_check["failures"]),
        "missing stop method failure should name method metadata",
    )

    missing_stop_accepted_event_log = dict(event_log)
    missing_stop_accepted_event_log["gui_lifecycle_events"] = [
        dict(row)
        for row in event_log["gui_lifecycle_events"]
        if not (
            row.get("event") == "gui_command_accepted"
            and row.get("request_id") == "op-log:orange:stop_recording"
        )
    ]
    missing_stop_accepted_check = module.check_orange_local_control_event_log(
        missing_stop_accepted_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not missing_stop_accepted_check["ok"],
        "event log should fail when stop request has no GUI accepted row",
    )
    require(
        any("GUI-thread gui_command_accepted" in failure for failure in missing_stop_accepted_check["failures"]),
        "missing stop accepted row failure should name gui_command_accepted",
    )

    missing_stop_scheduled_event_log = dict(event_log)
    missing_stop_scheduled_event_log["gui_lifecycle_events"] = [
        dict(row)
        for row in event_log["gui_lifecycle_events"]
        if not (
            row.get("event") == "recording_stop_scheduled"
            and row.get("request_id") == "op-log:orange:stop_recording"
        )
    ]
    missing_stop_scheduled_check = module.check_orange_local_control_event_log(
        missing_stop_scheduled_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not missing_stop_scheduled_check["ok"],
        "event log should fail when stop request has no stop scheduled row",
    )
    require(
        any("recording_stop_scheduled" in failure for failure in missing_stop_scheduled_check["failures"]),
        "missing stop scheduled row failure should name recording_stop_scheduled",
    )

    bad_stop_accepted_event_log = copy_event_log()
    mutate_gui_event(
        bad_stop_accepted_event_log,
        request_id="op-log:orange:stop_recording",
        event_name="gui_command_accepted",
        field="command_source",
        value="unexpected",
    )
    bad_stop_accepted_check = module.check_orange_local_control_event_log(
        bad_stop_accepted_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not bad_stop_accepted_check["ok"],
        "event log should fail when stop accepted source mismatches final status",
    )
    require(
        any("gui_command_accepted" in failure and "command_source" in failure for failure in bad_stop_accepted_check["failures"]),
        "bad stop accepted source failure should name gui_command_accepted",
    )

    citrus_request_id = "citrus_completion:op-log:completed:protocol_finished"
    citrus_status = orange_status(
        True,
        True,
        stop_method="citrus_completion",
        stop_source="citrus",
        operation_id="op-log",
        terminal_state="completed",
        reason="protocol_finished",
    )
    citrus_status["local_control"]["recording_stop"]["request_id"] = citrus_request_id
    citrus_event_log = copy_event_log()
    citrus_event_log["request_ids"] = [
        "op-log:orange:start_recording",
        citrus_request_id,
    ]
    for row in citrus_event_log["socket_request_events"]:
        if row.get("request_id") == "op-log:orange:stop_recording":
            row["request_id"] = citrus_request_id
            row["method"] = "citrus_completion"
            row["source"] = "citrus"
    for row in citrus_event_log["gui_lifecycle_events"]:
        if row.get("request_id") != "op-log:orange:stop_recording":
            continue
        row["request_id"] = citrus_request_id
        row["method"] = "citrus_completion"
        row["command_source"] = "citrus"
        row["reason"] = "protocol_finished"
        row["terminal_state"] = "completed"
        if row.get("event") == "gui_command_accepted":
            row["stop_enabled"] = False
            row["stop_recording_enabled"] = False
            row["citrus_completion_enabled"] = True
    citrus_only_check = module.check_orange_local_control_event_log(
        citrus_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="citrus_completion_notify",
        orange_status=citrus_status,
    )
    require(
        citrus_only_check["ok"],
        f"Citrus-only completion stop event log should pass: {citrus_only_check}",
    )
    citrus_request_chains = {
        item["request_id"]: item for item in citrus_only_check["request_chains"]
    }
    citrus_chain = citrus_request_chains[citrus_request_id]
    require(
        citrus_chain["gui_command_accepted_stop_enabled_values"] == [False],
        "Citrus-only chain should preserve generic stop disabled value",
    )
    require(
        citrus_chain["gui_command_accepted_citrus_completion_enabled_values"] == [True],
        "Citrus-only chain should preserve Citrus completion enabled value",
    )

    bad_citrus_gate_event_log = copy.deepcopy(citrus_event_log)
    mutate_gui_event(
        bad_citrus_gate_event_log,
        request_id=citrus_request_id,
        event_name="gui_command_accepted",
        field="citrus_completion_enabled",
        value=False,
    )
    bad_citrus_gate_check = module.check_orange_local_control_event_log(
        bad_citrus_gate_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="citrus_completion_notify",
        orange_status=citrus_status,
    )
    require(
        not bad_citrus_gate_check["ok"],
        "Citrus-only event log should fail when Citrus completion gate is disabled",
    )
    require(
        any("citrus_completion_enabled" in failure for failure in bad_citrus_gate_check["failures"]),
        "bad Citrus gate failure should name citrus_completion_enabled",
    )

    bad_stop_enabled_event_log = copy_event_log()
    mutate_gui_event(
        bad_stop_enabled_event_log,
        request_id="op-log:orange:stop_recording",
        event_name="gui_command_accepted",
        field="stop_enabled",
        value=False,
    )
    bad_stop_enabled_check = module.check_orange_local_control_event_log(
        bad_stop_enabled_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not bad_stop_enabled_check["ok"],
        "event log should fail when stop accepted row reports disabled stop control",
    )
    require(
        any("stop_enabled" in failure for failure in bad_stop_enabled_check["failures"]),
        "bad stop accepted row failure should name stop_enabled",
    )

    string_stop_enabled_event_log = copy_event_log()
    mutate_gui_event(
        string_stop_enabled_event_log,
        request_id="op-log:orange:stop_recording",
        event_name="gui_command_accepted",
        field="stop_enabled",
        value="true",
    )
    string_stop_enabled_check = module.check_orange_local_control_event_log(
        string_stop_enabled_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not string_stop_enabled_check["ok"],
        "event log should fail when stop_enabled is a truthy string",
    )
    require(
        any("stop_enabled" in failure for failure in string_stop_enabled_check["failures"]),
        "truthy-string stop_enabled failure should name stop_enabled",
    )

    bad_stop_order_event_log = copy_event_log()
    mutate_gui_event(
        bad_stop_order_event_log,
        request_id="op-log:orange:stop_recording",
        event_name="recording_drain_finalized",
        field="row_index",
        value=5,
    )
    bad_stop_order_check = module.check_orange_local_control_event_log(
        bad_stop_order_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not bad_stop_order_check["ok"],
        "event log should fail when stop finalize row precedes stop trigger",
    )
    require(
        any("out-of-order" in failure and "recording_drain_finalized" in failure for failure in bad_stop_order_check["failures"]),
        "bad stop order failure should name the drain-finalized order",
    )

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
    timeout_event_log["gui_lifecycle_events"] = [
        dict(row) for row in event_log["gui_lifecycle_events"]
    ] + [
        {
            "row_index": 10,
            "event": "recording_drain_timeout",
            "request_id": "op-log:orange:stop_recording",
            "operation_id": "op-log",
            "method": "stop_recording",
            "command_source": "orange_citrus_orchestrator",
            "reason": "orchestrator_stop",
            "forced_finalize_requested": True,
            "health": "critical",
            "error_code": "drain_timeout",
        },
    ]
    mutate_gui_event(
        timeout_event_log,
        request_id="op-log:orange:stop_recording",
        event_name="recording_drain_finalized",
        field="row_index",
        value=12,
    )
    for field, value in (
        ("drain_timed_out", True),
        ("health", "warning"),
        ("error_code", "drain_timeout"),
    ):
        mutate_gui_event(
            timeout_event_log,
            request_id="op-log:orange:stop_recording",
            event_name="recording_drain_finalized",
            field=field,
            value=value,
        )
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
    timeout_event_log["gui_lifecycle_events"] = [
        dict(row) for row in timeout_event_log["gui_lifecycle_events"]
    ] + [
        {
            "row_index": 11,
            "event": "recording_drain_forced_finalize_requested",
            "request_id": "op-log:orange:stop_recording",
            "operation_id": "op-log",
            "method": "stop_recording",
            "command_source": "orange_citrus_orchestrator",
            "reason": "orchestrator_stop",
            "action": "stream_shutdown",
            "health": "critical",
            "error_code": "drain_timeout",
        },
    ]
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
    timeout_chain = {
        item["request_id"]: item for item in timeout_ok_check["request_chains"]
    }["op-log:orange:stop_recording"]
    require(
        timeout_chain["has_recording_drain_timeout"],
        "timeout chain should report drain-timeout evidence",
    )
    require(
        timeout_chain["has_recording_drain_forced_finalize_requested"],
        "timeout chain should report forced-finalize evidence",
    )
    require(
        timeout_chain["recording_drain_timeout_row_indexes"] == [10],
        "timeout chain should summarize drain-timeout row index",
    )
    require(
        timeout_chain["recording_drain_forced_finalize_requested_row_indexes"] == [11],
        "timeout chain should summarize forced-finalize row index",
    )
    require(
        timeout_chain["recording_drain_timeout_methods"] == ["stop_recording"],
        "timeout chain should summarize drain-timeout method",
    )
    require(
        timeout_chain["recording_drain_timeout_forced_finalize_requested_values"] == [True],
        "timeout chain should summarize timeout forced-finalize flag",
    )
    require(
        timeout_chain["recording_drain_timeout_healths"] == ["critical"],
        "timeout chain should summarize timeout health",
    )
    require(
        timeout_chain["recording_drain_forced_finalize_requested_sources"]
        == ["orange_citrus_orchestrator"],
        "timeout chain should summarize forced-finalize source",
    )
    require(
        timeout_chain["recording_drain_forced_finalize_requested_actions"]
        == ["stream_shutdown"],
        "timeout chain should summarize forced-finalize action",
    )
    require(
        timeout_chain["recording_drain_finalized_drain_timed_out_values"] == [True],
        "timeout chain should summarize timeout drain-finalized flag",
    )
    require(
        timeout_chain["recording_drain_finalized_healths"] == ["warning"],
        "timeout chain should summarize timeout drain-finalized health",
    )

    clean_status_with_timeout_events_check = module.check_orange_local_control_event_log(
        timeout_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=status,
    )
    require(
        not clean_status_with_timeout_events_check["ok"],
        "event log should fail when request-specific timeout evidence conflicts with clean status",
    )
    require(
        any(
            "recording_drain_timeout" in failure
            and "drain_timed_out is not true" in failure
            for failure in clean_status_with_timeout_events_check["failures"]
        ),
        "stale timeout evidence failure should name drain_timed_out mismatch",
    )
    require(
        any(
            "recording_drain_forced_finalize_requested" in failure
            and "forced_finalize_requested is not true" in failure
            for failure in clean_status_with_timeout_events_check["failures"]
        ),
        "stale forced-finalize evidence failure should name forced finalize mismatch",
    )

    bad_timeout_order_event_log = copy_event_log()
    bad_timeout_order_event_log["has_drain_timeout"] = True
    bad_timeout_order_event_log["has_forced_finalize_requested"] = True
    bad_timeout_order_event_log["gui_lifecycle_events"] = [
        dict(row) for row in event_log["gui_lifecycle_events"]
    ] + [
        {
            "row_index": 5,
            "event": "recording_drain_timeout",
            "request_id": "op-log:orange:stop_recording",
            "operation_id": "op-log",
            "method": "stop_recording",
            "command_source": "orange_citrus_orchestrator",
            "reason": "orchestrator_stop",
            "forced_finalize_requested": True,
            "health": "critical",
            "error_code": "drain_timeout",
        },
        {
            "row_index": 11,
            "event": "recording_drain_forced_finalize_requested",
            "request_id": "op-log:orange:stop_recording",
            "operation_id": "op-log",
            "method": "stop_recording",
            "command_source": "orange_citrus_orchestrator",
            "reason": "orchestrator_stop",
            "action": "stream_shutdown",
            "health": "critical",
            "error_code": "drain_timeout",
        },
    ]
    mutate_gui_event(
        bad_timeout_order_event_log,
        request_id="op-log:orange:stop_recording",
        event_name="recording_drain_finalized",
        field="row_index",
        value=12,
    )
    for field, value in (
        ("drain_timed_out", True),
        ("health", "warning"),
        ("error_code", "drain_timeout"),
    ):
        mutate_gui_event(
            bad_timeout_order_event_log,
            request_id="op-log:orange:stop_recording",
            event_name="recording_drain_finalized",
            field=field,
            value=value,
        )
    bad_timeout_order_check = module.check_orange_local_control_event_log(
        bad_timeout_order_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=timeout_status,
    )
    require(
        not bad_timeout_order_check["ok"],
        "event log should fail when drain timeout precedes stop trigger",
    )
    require(
        any(
            "out-of-order" in failure and "recording_drain_timeout" in failure
            for failure in bad_timeout_order_check["failures"]
        ),
        "bad timeout order failure should name recording_drain_timeout",
    )

    bad_forced_order_event_log = copy_event_log()
    bad_forced_order_event_log["has_drain_timeout"] = True
    bad_forced_order_event_log["has_forced_finalize_requested"] = True
    bad_forced_order_event_log["gui_lifecycle_events"] = [
        dict(row) for row in event_log["gui_lifecycle_events"]
    ] + [
        {
            "row_index": 10,
            "event": "recording_drain_timeout",
            "request_id": "op-log:orange:stop_recording",
            "operation_id": "op-log",
            "method": "stop_recording",
            "command_source": "orange_citrus_orchestrator",
            "reason": "orchestrator_stop",
            "forced_finalize_requested": True,
            "health": "critical",
            "error_code": "drain_timeout",
        },
        {
            "row_index": 8,
            "event": "recording_drain_forced_finalize_requested",
            "request_id": "op-log:orange:stop_recording",
            "operation_id": "op-log",
            "method": "stop_recording",
            "command_source": "orange_citrus_orchestrator",
            "reason": "orchestrator_stop",
            "action": "stream_shutdown",
            "health": "critical",
            "error_code": "drain_timeout",
        },
    ]
    mutate_gui_event(
        bad_forced_order_event_log,
        request_id="op-log:orange:stop_recording",
        event_name="recording_drain_finalized",
        field="row_index",
        value=12,
    )
    for field, value in (
        ("drain_timed_out", True),
        ("health", "warning"),
        ("error_code", "drain_timeout"),
    ):
        mutate_gui_event(
            bad_forced_order_event_log,
            request_id="op-log:orange:stop_recording",
            event_name="recording_drain_finalized",
            field=field,
            value=value,
        )
    bad_forced_order_check = module.check_orange_local_control_event_log(
        bad_forced_order_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="stop_recording",
        orange_status=timeout_status,
    )
    require(
        not bad_forced_order_check["ok"],
        "event log should fail when forced finalize precedes drain timeout",
    )
    require(
        any(
            "out-of-order" in failure
            and "recording_drain_forced_finalize_requested" in failure
            for failure in bad_forced_order_check["failures"]
        ),
        "bad forced-finalize order failure should name forced-finalize event",
    )

    notify_status = orange_status(
        True,
        True,
        stop_method="citrus_completion",
        stop_source="citrus",
        operation_id="op-log",
        terminal_state="stopped",
        reason="stopped_by_local_control",
    )
    notify_status["local_control"]["recording_stop"][
        "request_id"
    ] = "citrus_completion:op-log:stopped:stopped_by_local_control"
    notify_event_log = dict(event_log)
    notify_event_log["request_ids"] = [
        "op-log:orange:start_recording",
        "citrus_completion:op-log:stopped:stopped_by_local_control",
    ]
    notify_event_log["socket_request_events"] = [
        {
            "row_index": 1,
            "request_id": "op-log:orange:start_recording",
            "operation_id": "op-log",
            "method": "start_recording",
            "source": "orange_citrus_orchestrator",
            "ok": True,
            "accepted": True,
            "queued_for_gui_thread": True,
        },
        {
            "row_index": 5,
            "request_id": "citrus_completion:op-log:stopped:stopped_by_local_control",
            "operation_id": "op-log",
            "method": "citrus_completion",
            "source": "citrus",
            "ok": True,
            "accepted": True,
            "queued_for_gui_thread": True,
        },
    ]
    notify_event_log["gui_lifecycle_events"] = [
        {
            "row_index": 2,
            "event": "gui_command_accepted",
            "request_id": "op-log:orange:start_recording",
            "operation_id": "op-log",
            "method": "start_recording",
            "command_source": "orange_citrus_orchestrator",
            "start_enabled": True,
            "stop_enabled": True,
        },
        {
            "row_index": 3,
            "event": "recording_start_queued",
            "request_id": "op-log:orange:start_recording",
            "operation_id": "op-log",
            "method": "start_recording",
            "command_source": "orange_citrus_orchestrator",
            "reason": "orchestrator_start",
        },
        {
            "row_index": 4,
            "event": "recording_start_triggered",
            "request_id": "op-log:orange:start_recording",
            "operation_id": "op-log",
            "method": "start_recording",
            "command_source": "orange_citrus_orchestrator",
            "reason": "orchestrator_start",
        },
        {
            "row_index": 6,
            "event": "gui_command_accepted",
            "request_id": "citrus_completion:op-log:stopped:stopped_by_local_control",
            "operation_id": "op-log",
            "method": "citrus_completion",
            "command_source": "citrus",
            "start_enabled": True,
            "stop_enabled": True,
        },
        {
            "row_index": 7,
            "event": "recording_stop_scheduled",
            "request_id": "citrus_completion:op-log:stopped:stopped_by_local_control",
            "operation_id": "op-log",
            "method": "citrus_completion",
            "command_source": "citrus",
            "terminal_state": "stopped",
            "reason": "stopped_by_local_control",
        },
        {
            "row_index": 8,
            "event": "recording_stop_triggered",
            "request_id": "citrus_completion:op-log:stopped:stopped_by_local_control",
            "operation_id": "op-log",
            "method": "citrus_completion",
            "command_source": "citrus",
            "terminal_state": "stopped",
            "reason": "stopped_by_local_control",
        },
        {
            "row_index": 9,
            "event": "recording_drain_finalized",
            "request_id": "citrus_completion:op-log:stopped:stopped_by_local_control",
            "operation_id": "op-log",
            "method": "citrus_completion",
            "command_source": "citrus",
            "terminal_state": "stopped",
            "reason": "stopped_by_local_control",
            "drain_timed_out": False,
            "health": "ok",
            "error_code": "",
        },
    ]
    notify_ok_check = module.check_orange_local_control_event_log(
        notify_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="citrus_completion_notify",
        orange_status=notify_status,
    )
    require(
        notify_ok_check["ok"],
        f"Citrus notify event log should prove method/source/terminal: {notify_ok_check}",
    )

    notify_bad_event_log = dict(notify_event_log)
    notify_bad_event_log["gui_lifecycle_events"] = [
        dict(row) for row in notify_event_log["gui_lifecycle_events"]
    ]
    mutate_gui_event(
        notify_bad_event_log,
        request_id="citrus_completion:op-log:stopped:stopped_by_local_control",
        event_name="recording_stop_triggered",
        field="command_source",
        value="orange_citrus_orchestrator",
    )
    notify_bad_check = module.check_orange_local_control_event_log(
        notify_bad_event_log,
        required=True,
        operation_id="op-log",
        stop_policy="citrus_completion_notify",
        orange_status=notify_status,
    )
    require(
        not notify_bad_check["ok"],
        "Citrus notify event log should fail when trigger event source is not Citrus",
    )
    require(
        any("command_source" in failure for failure in notify_bad_check["failures"]),
        "Citrus notify event-log failure should name command_source",
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
        orange_control_log = root / "orange.events.jsonl"
        orange_log.write_text("orange-log\n", encoding="utf-8")
        citrus_log.write_text("citrus-log\n", encoding="utf-8")
        orange_control_log.write_text(
            "\n".join(
                json.dumps(row)
                for row in [
                    {
                        "received_at_utc": "2026-05-29T00:00:00Z",
                        "request": {
                            "method": "start_recording",
                            "request_id": "op-failure-artifacts:orange:start_recording",
                            "source": "orange_citrus_orchestrator",
                        },
                        "response": {
                            "method": "start_recording",
                            "request_id": "op-failure-artifacts:orange:start_recording",
                            "operation_id": "op-failure-artifacts",
                            "ok": True,
                            "accepted": True,
                            "queued_for_gui_thread": True,
                        },
                    },
                    {
                        "schema_id": "orange.local_control.gui_event",
                        "schema_version": 1,
                        "event": "gui_command_accepted",
                        "request_id": "op-failure-artifacts:orange:start_recording",
                        "operation_id": "op-failure-artifacts",
                        "method": "start_recording",
                        "command_source": "orange_citrus_orchestrator",
                        "start_enabled": True,
                    },
                    {
                        "schema_id": "orange.local_control.gui_event",
                        "schema_version": 1,
                        "event": "recording_start_queued",
                        "request_id": "op-failure-artifacts:orange:start_recording",
                        "operation_id": "op-failure-artifacts",
                        "method": "start_recording",
                        "command_source": "orange_citrus_orchestrator",
                    },
                    {
                        "schema_id": "orange.local_control.gui_event",
                        "schema_version": 1,
                        "event": "recording_start_triggered",
                        "request_id": "op-failure-artifacts:orange:start_recording",
                        "operation_id": "op-failure-artifacts",
                        "method": "start_recording",
                        "command_source": "orange_citrus_orchestrator",
                    },
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        args = module.parse_args(
            [
                "--execute",
                "--operation-id",
                "op-failure-artifacts",
                "--orange-log",
                str(orange_log),
                "--orange-local-control-log",
                str(orange_control_log),
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
        request_chains = {
            item["request_id"]: item
            for item in summary["orange"]["local_control_event_log_check"]["request_chains"]
        }
        start_chain = request_chains["op-failure-artifacts:orange:start_recording"]
        require(
            start_chain["queued_socket_row_indexes"] == [1],
            "failure summary should retain queued start socket row evidence",
        )
        require(
            start_chain["gui_command_accepted_row_indexes"] == [2],
            "failure summary should retain GUI accepted row evidence",
        )
        require(
            start_chain["recording_start_queued_row_indexes"] == [3],
            "failure summary should retain start queued row evidence",
        )
        require(
            start_chain["recording_start_triggered_row_indexes"] == [4],
            "failure summary should retain start trigger row evidence",
        )
        require(
            (recording_folder / "orchestrator" / "orange.log").exists(),
            "failure artifact persistence should copy logs when a recording folder was observed",
        )
        require(
            (recording_folder / "orchestrator" / "orange_local_control.events.jsonl").exists(),
            "failure artifact persistence should copy Orange local-control event log",
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


def test_manifest_inferred_status_prefers_command_source() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording_folder = root / "recording"
        recording_folder.mkdir()
        operation_id = "op-manifest-source"
        (recording_folder / "recording_session.json").write_text(
            json.dumps(
                {
                    "recording": {
                        "control": {
                            "ack_state": "executed",
                            "command_source": "citrus",
                            "drain_completed": True,
                            "last_event": "finalized",
                            "method": "citrus_completion",
                            "operation_id": operation_id,
                            "reason": "protocol_finished",
                            "request_id": "citrus_completion:op-manifest-source:completed:protocol_finished",
                            "source": "orange_gui_local_control",
                            "terminal_state": "completed",
                        }
                    }
                }
            ),
            encoding="utf-8",
        )
        inferred = module.infer_orange_finalized_status_from_session(
            {
                "recording": {"folder": str(recording_folder)},
                "readiness": {"recording_finalized": False},
            },
            operation_id=operation_id,
        )
        require(inferred is not None, "manifest should infer finalized status")
        require(
            module.orange_recording_stop_source(inferred) == "citrus",
            "manifest-backed stop source should prefer recording.control.command_source",
        )
        check = module.summarize_orange_citrus_notify_stop_status(
            inferred,
            citrus_status(True, True),
            stop_policy="citrus_completion_notify",
            operation_id=operation_id,
        )
        require(
            check["ok"],
            f"manifest-backed Citrus-notify stop status should pass: {check}",
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
        test_manifest_inferred_status_prefers_command_source,
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
