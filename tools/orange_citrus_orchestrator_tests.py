#!/usr/bin/env python3
"""Focused tests for scripts/orange_citrus_orchestrator.py."""

from __future__ import annotations

import copy
import hashlib
import shutil
import threading
import importlib.util
import json
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Callable


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


def test_citrus_bound_start_checks_and_delivery() -> None:
    module = load_module()
    ready = {
        "readiness": {"ready_to_start": True, "start_blocked_reason": None,
                      "selected_arena_count": 4, "selected_protocol_loaded_count": 4},
        "runtime_rig_canvas_commissioning_compatibility": {"compatible": True, "all_selected_runtime_safe": True},
        "runtime_homography_compatibility": {"all_selected_compatible": True},
        "runtime_projected_surface_scale_compatibility": {
            "all_selected_compatible": True, "all_selected_runtime_safe": True,
            "selected_invalid_pointer_count": 0, "selected_missing_pointer_count": 0},
        "daily_registration": {"runtime": {"all_selected_runtime_safe": True}},
        "experiment": {"terminal_state": "not_started"},
    }
    require(module.citrus_start_blockers(ready, 4) == [], "a fully ready Citrus has no blockers")
    three = copy.deepcopy(ready)
    three["readiness"]["selected_arena_count"] = 3
    require(any("selected_arena_count" in b for b in module.citrus_start_blockers(three, 4)),
            "the arena count is checked when expected")
    require(module.citrus_start_blockers(three, None) == [], "no expected count, no count check")
    stale = copy.deepcopy(ready)
    stale["runtime_homography_compatibility"]["all_selected_compatible"] = False
    stale["experiment"]["terminal_state"] = "completed"
    blockers = module.citrus_start_blockers(stale, 4)
    require(len(blockers) == 2 and "homography" in blockers[0], f"each failing item is named: {blockers}")
    require(len(module.citrus_start_blockers({}, None)) > 5, "a missing status block blocks")

    delivered = {"recording_observation_finalization": {"state": "delivered"}}
    idle = {"recording_observation_finalization": {"state": "idle"}}
    require(module.citrus_finalization_settled(delivered), "delivered settles")
    require(not module.citrus_finalization_settled(idle), "idle keeps waiting")
    require(module.citrus_finalization_settled({"recording_observation_finalization": {"state": "refused"}}),
            "a refusal settles (and is then reported as a failure)")
    require(module.citrus_finalization_settled({"recording_observation_finalization": {"state": "unconfirmed"}}),
            "unconfirmed settles as a failure")
    require(not module.citrus_finalization_settled({"recording_observation_finalization": {"state": "verifying"}}),
            "verifying keeps waiting")
    require(module.citrus_finalization_never_started(
        {"recording_observation_finalization": {"state": "idle", "attempts": 0}}),
        "idle with no attempts: Citrus built no receipts")
    require(not module.citrus_finalization_never_started(
        {"recording_observation_finalization": {"state": "idle", "attempts": 1}}),
        "idle after an attempt is not 'never started'")
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder / "a.bin").write_bytes(b"x")
        require(module.citrus_receipt_wait_limit_seconds(str(folder)) == 330 + 120 + 3 + 60,
                "a tiny folder gets Citrus's fixed bound plus one 100 MB step")
        with open(folder / "b.bin", "wb") as handle:
            handle.truncate(1000 * 1000 * 1000)
        require(module.citrus_receipt_wait_limit_seconds(str(folder)) == 330 + 120 + 3 * 11 + 60,
                "3 s per started 100 MB, as Orange's GUI exit computes it")
    require(module.is_citrus_binary_cmdline(["./targets/citrus"]), "relative Citrus binary")
    require(module.is_citrus_binary_cmdline(["/home/jeremy/citrus/targets/citrus", "--x"]), "absolute Citrus binary")
    require(not module.is_citrus_binary_cmdline(
        ["/bin/bash", "-c", "python3 orchestrator.py --citrus-command 'env -C ~/citrus ./targets/citrus'"]),
        "a shell whose command line mentions targets/citrus is not Citrus (2026-11-15 false positive)")
    require(not module.is_citrus_binary_cmdline(["env", "-C", "/home/jeremy/citrus", "./targets/citrus"]),
            "the env launcher is not Citrus itself (it execs it)")
    require(isinstance(module.other_citrus_processes({0}), list), "the /proc scan runs")

    args = module.parse_args(["--declared-by", "delahantyj", "--intent", "stimulus_experiment", "--blank-subjects"])
    require(module.Orchestrator(args).bound_run() if hasattr(module, "Orchestrator") else True, "stimulus run is bound")


def test_citrus_completion_under_its_own_experiment_id() -> None:
    module = load_module()
    citexp = "citexp_1b1b12ec1ca0ad2bef272e7465f8b55055cd7a759746cffb3fc425ebef32181e"
    orange = {"local_control": {"recording_stop": {
        "method": "citrus_completion", "command_source": "citrus", "operation_id": citexp,
        "terminal_state": "completed", "reason": "protocol_finished"}}}
    citrus = {"experiment": {"terminal_state": "completed", "terminal_reason": "protocol_finished",
                             "last_operation_id": citexp}}
    failures = module.check_orange_citrus_notify_stop_status(
        orange, citrus, operation_id="orange_citrus_20261115T035850Z_2d7c5865")
    require(failures == [], f"an autorun Citrus completes under its citexp id (2026-11-15 false failure): {failures}")
    citrus_other = copy.deepcopy(citrus)
    citrus_other["experiment"]["last_operation_id"] = "citexp_other"
    failures = module.check_orange_citrus_notify_stop_status(orange, citrus_other, operation_id="orch-op")
    require(any("operation id mismatch" in f for f in failures), "a foreign completion id still fails")


def test_clean_orange_exit_during_receipt_wait_is_expected() -> None:
    module = load_module()
    args = module.parse_args(["--declared-by", "delahantyj", "--intent", "stimulus_experiment", "--blank-subjects"])
    orchestrator = module.Orchestrator(args)
    orchestrator.refresh_started_processes = lambda: None
    orchestrator.started_processes = [{"label": "orange", "pid": 1, "returncode": 0, "log_path": "x"}]
    raised = False
    try:
        orchestrator.raise_if_started_process_exited("waiting")
    except module.OrchestratorError:
        raised = True
    require(raised, "before the receipt wait an Orange exit is a failure")
    orchestrator.orange_exit_expected = True
    orchestrator.raise_if_started_process_exited("waiting for citrus receipts_delivered")
    orchestrator.started_processes = [{"label": "orange", "pid": 1, "returncode": 3, "log_path": "x"}]
    raised = False
    try:
        orchestrator.raise_if_started_process_exited("waiting")
    except module.OrchestratorError:
        raised = True
    require(raised, "a non-zero Orange exit still fails")


def test_long_waits_poll_slowly() -> None:
    module = load_module()
    args = module.parse_args([])
    require(module.poll_interval_for(args, "ready_to_start") == 0.25, "readiness keeps the short interval")
    require(module.poll_interval_for(args, "terminal_state") == 1.5, "a protocol-long wait polls every 1.5 s")
    require(module.poll_interval_for(args, "receipts_delivered") == 1.5, "the receipt wait polls slowly too")
    fast = module.parse_args(["--long-wait-poll-interval-seconds", "0.5"])
    require(module.poll_interval_for(fast, "terminal_state") == 0.5, "the long interval is configurable")


def test_orange_start_carries_declaration() -> None:
    module = load_module()
    plain = module.build_orange_start_request("op-1", "req", "test")
    require("declaration" not in plain["params"], "no declaration: the operator confirms in the GUI")
    args = module.parse_args(
        ["--declared-by", "delahantyj", "--intent", "stimulus_experiment", "--blank-subjects"]
    )
    require(
        args.declaration
        == {
            "schema_id": "orange.recording_start_declaration",
            "schema_version": 1,
            "declared_by": "delahantyj",
            "recording_intent": "stimulus_experiment",
            "subject_references": {"schema_version": 1},
        },
        f"declaration from the flags: {args.declaration}",
    )
    declared = module.build_orange_start_request("op-1", "req", "test", args.declaration)
    require(declared["params"]["declaration"] == args.declaration, "the start carries the declaration")
    require(module.parse_args([]).declaration is None, "no flags, no declaration")
    for bad in (
        ["--intent", "stimulus_experiment"],
        ["--orange-mode", "headless", "--declared-by", "delahantyj", "--intent", "recording_only",
         "--blank-subjects"],
    ):
        result = subprocess.run([sys.executable, str(SCRIPT), *bad], capture_output=True, text=True, check=False)
        require(result.returncode == 2, f"refused: {bad}: {result.stderr}")


def test_request_builders_and_readiness_helpers() -> None:
    module = load_module()
    orange_start = module.build_orange_start_request("op-1", "req-start", "test")
    citrus_start = module.build_citrus_start_request("op-1", "req-citrus", "test")
    citrus_base_only = module.build_citrus_daily_registration_mode_request(
        "op-1", "req-base-only", "test", "base_only"
    )

    require(orange_start["schema_id"] == "orange.local_control.request", "orange schema")
    require(orange_start["method"] == "start_recording", "orange start method")
    require(orange_start["operation_id"] == "op-1", "orange operation id")
    require(citrus_start["schema_id"] == "citrus.local_control.request", "citrus schema")
    require(citrus_start["method"] == "start_experiment", "citrus start method")
    require(
        citrus_base_only["method"] == "select_daily_registration_runtime_mode",
        "Citrus runtime-mode method",
    )
    require(
        citrus_base_only["params"]
        == {"mode": "base_only", "select_runtime_mode_armed": True},
        "Citrus base-only request must be explicitly armed",
    )
    require(module.orange_ready_for_recording(orange_status(False, False)), "orange ready")
    warming_status = orange_status(False, False)
    warming_status["autorun_stage"] = "stream_warmup"
    require(
        not module.orange_ready_for_recording(warming_status),
        "orchestrator must not start recording during Orange stream warmup",
    )
    warming_status["autorun_stage"] = "done"
    require(
        module.orange_ready_for_recording(warming_status),
        "orchestrator should accept completed Orange stream warmup",
    )
    require(module.orange_ready_for_citrus(orange_status(True, False)), "orange recording")
    require(module.orange_recording_finalized(orange_status(True, True)), "orange finalized")
    require(
        module.orange_wait_failure(
            {"autorun_stage": "failed"},
            description="ready_for_recording_request",
            operation_id="op-1",
        )
        == "Orange GUI autorun reported stage=failed",
        "Orange autorun failure must fail readiness immediately",
    )
    require(
        "recording start failed" in module.orange_wait_failure(
            {
                "local_control": {
                    "recording_start": {
                        "last_event": "start_failed",
                        "operation_id": "op-1",
                    }
                }
            },
            description="ready_for_citrus_experiment",
            operation_id="op-1",
        ),
        "matching asynchronous recording-start failure must fail readiness immediately",
    )
    require(
        module.orange_recording_start_failed_for_operation(
            {
                "local_control": {
                    "recording_start": {
                        "last_event": "start_failed",
                        "operation_id": "op-1",
                    }
                }
            },
            "op-1",
        ),
        "GUI start failure must distinguish queued ACK from active recording",
    )
    require(
        module.orange_wait_failure(
            {
                "local_control": {
                    "recording_start": {
                        "last_event": "start_failed",
                        "operation_id": "some-other-operation",
                    }
                }
            },
            description="ready_for_citrus_experiment",
            operation_id="op-1",
        )
        == "",
        "attach mode must ignore stale recording-start failure from another operation",
    )
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
    selectable_status = citrus_status(False, False)
    selectable_status["experiment"]["loaded"] = True
    selectable_status["readiness"]["selected_arena_count"] = 4
    require(
        module.citrus_ready_for_runtime_selection(selectable_status),
        "loaded Citrus canvas should be ready for explicit runtime selection",
    )
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
    require(
        module.orange_recording_covers_active_citrus(orange_status(True, False)),
        "active Orange recording should cover a live Citrus experiment",
    )
    require(
        not module.orange_recording_covers_active_citrus(orange_status(True, True)),
        "finalized Orange recording must not cover a live Citrus experiment",
    )
    require(
        not module.orange_recording_covers_active_citrus(
            {"readiness": {"recording_active": "true", "recording_finalized": False}}
        ),
        "coverage helper must reject truthy-string recording state",
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
        payload["citrus"]["daily_registration_mode"] == "preserve"
        and payload["citrus"]["daily_registration_mode_request"] is None,
        "generic orchestrator must preserve the operator's daily selection by default",
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


def test_wait_clears_provisional_recording_state_after_gui_start_failure() -> None:
    module = load_module()
    args = module.parse_args(
        [
            "--execute",
            "--operation-id",
            "op-start-failed",
            "--poll-interval-seconds",
            "0.01",
            "--timeout-seconds",
            "1",
        ]
    )
    orchestrator = module.Orchestrator(args)
    orchestrator.orange_recording_started = True
    failed_status = orange_status(False, False, operation_id="op-start-failed")
    failed_status["local_control"]["recording_start"] = {
        "last_event": "start_failed",
        "operation_id": "op-start-failed",
    }
    orchestrator.status = lambda label, schema_id, socket_path: ({}, failed_status)
    try:
        orchestrator.wait_for_status(
            "orange",
            module.ORANGE_REQUEST_SCHEMA_ID,
            "/tmp/not-used.sock",
            lambda status: False,
            1,
            "ready_for_citrus_experiment",
        )
    except module.OrchestratorError as exc:
        require("recording start failed" in str(exc), "wait should report GUI start failure")
    else:
        raise AssertionError("expected GUI recording-start failure")
    require(
        not orchestrator.orange_recording_started,
        "failed queued start must not preserve Orange as though recording were active",
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


def test_orange_coverage_check_rejects_premature_recording_stop() -> None:
    module = load_module()
    args = module.parse_args(
        [
            "--execute",
            "--operation-id",
            "op-premature-orange-stop",
            "--orange-socket",
            "orange-test.sock",
        ]
    )
    orchestrator = module.Orchestrator(args)

    def fake_status(
        label: str,
        schema_id: str,
        socket_path: str,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        require(label == "orange", "coverage check should query Orange")
        status = orange_status(True, True)
        orchestrator.last_orange_status = status
        return {}, status

    orchestrator.status = fake_status
    step = orchestrator.step("coverage_test")
    try:
        orchestrator.check_orange_recording_coverage(
            citrus_status(True, False),
            step,
        )
    except module.OrchestratorError as exc:
        require(
            "ended before Citrus" in str(exc),
            "coverage failure should explain the ordering violation",
        )
    else:
        raise AssertionError("premature Orange finalization must fail coverage")
    require(not step.ok, "coverage failure should be recorded in the step log")
    require(
        step.detail.get("outcome") == "orange_recording_ended_before_citrus",
        "coverage failure should carry a stable outcome code",
    )


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


def headless_citrus_status(
    *,
    started: bool,
    terminal: bool,
    autorun: dict[str, Any] | None = None,
    automation_error: str = "",
) -> dict[str, Any]:
    status = citrus_status(started, terminal)
    status["automation"] = {"error": automation_error}
    if autorun is not None:
        status["automation"]["autorun"] = autorun
    return status


def headless_autorun(started: bool) -> dict[str, Any]:
    return {
        "enabled": True,
        "start_mode": "wait_for_control",
        "waiting_for_start": not started,
        "start_source": "local_control" if started else None,
    }


def headless_orange_status(recording_folder: Path, *, recording: bool) -> dict[str, Any]:
    return {
        "process": "orange_client",
        "phase": "recording" if recording else "streaming",
        "readiness": {
            "ready_for_citrus_experiment": recording,
            "recording_active": recording,
            "recording_finalizing": False,
            "recording_finalized": False,
        },
        "recording": {"folder": str(recording_folder) if recording else "", "sink_mode": "real"},
        # The shape orange_client reports (src/orange_local_control.cpp).
        "local_control": {
            "recording_stop": {"enabled": True},
            "citrus_completion_stop": {"enabled": True},
        },
    }


def write_headless_run(
    root: Path,
    *,
    lifecycle: str = "accepted_pending_finalization",
    receipts_status: str = "finalized",
) -> Path:
    folder = root / "exp" / "exp__run_0001"
    (folder / "recording_observation_bindings").mkdir(parents=True)
    (folder / "recording_observation_bindings" / "pre_arm_decision.json").write_text(
        json.dumps({"lifecycle_status": lifecycle})
    )
    (root / "exp" / "runs.json").write_text(
        json.dumps(
            {
                "runs": [
                    {
                        "recording_folder": str(folder),
                        "status": "completed",
                        "pass_fail": "pass",
                        "reason": "",
                        "citrus_receipts": {
                            "binding_mode": "required",
                            "pre_arm_lifecycle_status": lifecycle,
                            "status": receipts_status,
                        },
                    }
                ]
            }
        )
    )
    return folder


def run_headless_fake(
    module: Any,
    root: Path,
    folder: Path,
    *,
    citrus_autorun: Callable[[bool], dict[str, Any] | None] = headless_autorun,
    automation_error: str = "",
) -> tuple[dict[str, Any] | None, str, dict[str, Any]]:
    """Fake sockets; orange_command is a process that records until Citrus is
    terminal, then exits as orange_client does after its receipts."""
    release = root / "orange_may_exit"
    state = {"orange_launched": False, "citrus_started": False, "polls_after_start": 0, "starts": 0}

    def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
        if ":prelaunch_status:" in request.get("request_id", ""):
            raise FileNotFoundError(socket_path)
        if socket_path == "orange-test.sock":
            state["orange_launched"] = True
            return response_for(request, headless_orange_status(folder, recording=True))
        method = request.get("method")
        if method == "start_experiment":
            state["citrus_started"] = True
            state["starts"] += 1
        elif state["citrus_started"]:
            state["polls_after_start"] += 1
        terminal = state["polls_after_start"] >= 2
        if terminal:
            release.write_text("go")
        return response_for(
            request,
            headless_citrus_status(
                started=state["citrus_started"],
                terminal=terminal,
                autorun=citrus_autorun(state["citrus_started"]),
                automation_error=automation_error,
            ),
        )

    orange_command = (
        f"{shlex.quote(sys.executable)} -c "
        + shlex.quote(
            "import pathlib,sys,time\n"
            f"p=pathlib.Path({str(release)!r})\n"
            "t=time.time()\n"
            "while not p.exists() and time.time()-t<10: time.sleep(0.01)\n"
            "sys.exit(0 if p.exists() else 3)\n"
        )
    )
    original_send = module.send_unix_json
    module.send_unix_json = fake_send
    try:
        args = module.parse_args(
            [
                "--execute",
                "--orange-mode", "headless",
                "--stop-policy", "citrus_completion_notify",
                "--operation-id", "op-headless",
                "--orange-socket", "orange-test.sock",
                "--citrus-socket", "citrus-test.sock",
                "--orange-command", orange_command,
                "--orange-log", str(root / "orange.log"),
                "--poll-interval-seconds", "0.01",
                "--timeout-seconds", "2",
                "--orange-ready-timeout-seconds", "2",
                "--citrus-terminal-timeout-seconds", "5",
                "--orange-exit-timeout-seconds", "10",
            ]
        )
        orchestrator = module.Orchestrator(args)
        try:
            payload = orchestrator.run()
            orchestrator.persist_artifacts(payload)
            return payload, "", state
        except module.OrchestratorError as exc:
            orchestrator.cleanup_started_processes(labels={"orange"})
            return None, str(exc), state
    finally:
        module.send_unix_json = original_send


def test_headless_mode_starts_citrus_only_after_orange_is_bound() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        folder = write_headless_run(root)
        payload, error, state = run_headless_fake(module, root, folder)
        require(payload is not None, f"headless run should pass: {error}")
        require(payload["result"] == "pass", f"headless result: {payload['result']} {payload['error']}")
        require(state["starts"] == 1, "exactly one start_experiment")
        headless = payload["headless"]
        require(headless["recording_folder"] == str(folder), "summary carries the recording folder")
        require(headless["citrus_receipts"]["status"] == "finalized", "summary carries the receipts")
        require(headless["citrus_start_source"] == "local_control", "Citrus started from the socket")
        require(headless["orange_returncode"] == 0, "orange_client exit code")
        require(
            payload["artifacts"]["reason"] == "headless_recording_sealed_by_orange_client",
            "headless never copies into the sealed recording",
        )
        require(sorted(p.name for p in folder.iterdir()) == ["recording_observation_bindings"],
                "nothing written into the recording folder")
        names = [step["name"] for step in payload["steps"]]
        require(
            names.index("wait_orange_headless_recording_bound")
            < names.index("check_orange_headless_pre_arm")
            < names.index("citrus_start_experiment")
            < names.index("wait_orange_process_exit"),
            f"headless step order: {names}",
        )


def test_headless_mode_fails_on_missing_receipts() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        folder = write_headless_run(root, receipts_status="not_received")
        payload, error, _ = run_headless_fake(module, root, folder)
        require(payload is not None and payload["result"] == "fail", f"missing receipts must fail: {error}")
        require("citrus_receipts status='not_received'" in payload["error"], payload["error"])


def test_headless_mode_refuses_start_without_accepted_pre_arm() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        folder = write_headless_run(root, lifecycle="unbound")
        payload, error, state = run_headless_fake(module, root, folder)
        require(payload is None, "an unaccepted pre-arm must stop the run")
        require("accepted_pending_finalization" in error, error)
        require(state["starts"] == 0, "no start_experiment without an accepted pre-arm")


def test_headless_mode_refuses_citrus_without_wait_for_control() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        folder = write_headless_run(root)
        payload, error, state = run_headless_fake(module, root, folder, citrus_autorun=lambda started: None)
        require(payload is None and "PR #109" in error, f"old Citrus build must be refused: {error}")
        require(not state["orange_launched"] and state["starts"] == 0, "nothing launched or started")

        payload, error, state = run_headless_fake(
            module, root, folder, automation_error="CITRUS_GUI_AUTORUN_START=wait_for_control needs CITRUS_GUI_AUTORUN=1"
        )
        require(payload is None and "Citrus automation error" in error, error)
        require(state["starts"] == 0, "no start on a Citrus automation error")

        payload, error, state = run_headless_fake(
            module,
            root,
            folder,
            citrus_autorun=lambda started: {
                "enabled": True, "start_mode": "auto", "waiting_for_start": False, "start_source": "autorun",
            },
        )
        require(payload is None and "already accepted a start" in error, error)
        require(state["starts"] == 0, "no second start for a Citrus that started itself")


def test_headless_completion_flag_read_where_orange_client_reports_it() -> None:
    module = load_module()
    status = headless_orange_status(Path("/tmp/x"), recording=True)
    require(module.orange_headless_citrus_completion_enabled(status), "citrus_completion_stop.enabled")
    status["local_control"]["citrus_completion_stop"]["enabled"] = False
    require(not module.orange_headless_citrus_completion_enabled(status), "completion off")
    # 2026-11-11 rig run: the old path (recording_stop.citrus_completion_enabled)
    # does not exist in orange_client's status and must not count.
    stale = {"local_control": {"recording_stop": {"enabled": True, "citrus_completion_enabled": True}}}
    require(not module.orange_headless_citrus_completion_enabled(stale), "stale path ignored")


def test_headless_mode_needs_orange_command_and_notify_policy() -> None:
    module = load_module()
    args = module.parse_args(["--execute", "--orange-mode", "headless", "--stop-policy", "citrus_completion_notify"])
    try:
        module.Orchestrator(args).run()
        raise AssertionError("headless without --orange-command must be refused")
    except module.OrchestratorError as exc:
        require("--orange-command" in str(exc), str(exc))
    args = module.parse_args(["--execute", "--orange-mode", "headless", "--orange-command", "true"])
    try:
        module.Orchestrator(args).run()
        raise AssertionError("headless with stop_recording policy must be refused")
    except module.OrchestratorError as exc:
        require("citrus_completion_notify" in str(exc), str(exc))


def test_event_log_follower() -> None:
    """The follower reads only new complete rows, by file identity and size."""
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "citrus.sock.events.jsonl"
        path.write_text(json.dumps({"event": "experiment_terminal", "old": True}) + "\n")
        follower = module.EventLogFollower("citrus", str(path))
        require(follower.poll() == [], "rows from an earlier run are skipped")
        with path.open("a") as handle:
            handle.write(json.dumps({"event": "local_control_request"}) + "\n")
            handle.write(json.dumps({"event": "binding_accepted"}) + "\n")
            handle.write('{"event": "start_requ')  # partial line
        rows = follower.poll()
        require([r["event"] for r in rows] == ["binding_accepted"], f"own status requests never wake: {rows}")
        with path.open("a") as handle:
            handle.write('ested"}\nnot json\n')
        require([r["event"] for r in follower.poll()] == ["start_requested"], "a partial line is completed")
        require(follower.unparsed_rows == 1, "a bad line is counted, not raised")
        # Citrus renames its log to .1 at start: a new file is read from its start.
        path.rename(path.with_suffix(".jsonl.1"))
        path.write_text(json.dumps({"event": "receipt_delivery", "state": "verifying"}) + "\n" +
                        json.dumps({"event": "receipt_delivery", "state": "delivered"}) + "\n")
        require(len(follower.poll()) == 2 and follower.resets == 1, "a renamed log is a new log")
        # Orange truncates its log at server start: a shrink is a new log.
        path.write_text(json.dumps({"event": "x"}) + "\n")
        require([r["event"] for r in follower.poll()] == ["x"] and follower.resets == 2, "a shrink is a new log")
        record = follower.record()
        require(record["receipt_delivery_states"] == ["verifying", "delivered"], record)
        require(record["event_counts"]["local_control_request"] == 1 and record["waking_rows"] == 5, record)
        socket_row = Path(tmp) / "orange.sock.events.jsonl"
        orange = module.EventLogFollower("orange", str(socket_row))
        socket_row.write_text(json.dumps({"request": {"method": "start_recording"}}) + "\n")
        require(len(orange.poll()) == 1 and orange.event_counts == {"socket_request": 1},
                "an Orange socket row (no event key) wakes")


def test_lifecycle_waker() -> None:
    """Wake on a lifecycle row, else at the heartbeat; plain polling without a log."""
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "orange.sock.events.jsonl"
        follower = module.EventLogFollower("orange", str(path))
        now = [0.0]
        sleeps: list[float] = []

        def sleep(seconds: float) -> None:
            sleeps.append(seconds)
            now[0] += seconds

        waker = module.LifecycleWaker([follower], 0.1, 5.0, clock=lambda: now[0], sleep=sleep)
        require(not waker.available and waker.interval(1.5) == 1.5, "no log: the poll interval")
        require(waker.wait(1.5) == "fallback" and sleeps == [1.5], "no log: a plain sleep")
        path.write_text("")
        require(waker.available and waker.interval(1.5) == 5.0, "with a log the heartbeat is longer")
        sleeps.clear()
        require(waker.wait(0.35) == "heartbeat" and abs(sum(sleeps) - 0.35) < 1e-9 and max(sleeps) <= 0.1,
                f"checked every 0.1 s, never past the deadline: {sleeps}")

        def append_on_third_sleep(seconds: float) -> None:
            sleep(seconds)
            if len(sleeps) == 3:
                with path.open("a") as handle:
                    handle.write(json.dumps({"event": "recording_drain_finalized"}) + "\n")

        sleeps.clear()
        waker.sleep = append_on_third_sleep
        require(waker.wait(5.0) == "event" and len(sleeps) == 3, "a new row ends the wait at once")
        require(waker.record()["wakes_by_event"] == 1 and waker.record()["fallback_waits"] == 1, waker.record())


def test_receipt_build_error_settles_and_is_reported() -> None:
    module = load_module()
    error = "H5 Cam2010093 sealed FAILED"
    failed = {"recording_observation_finalization": {"state": "idle", "attempts": 0,
                                                    "receipt_build_error": error},
              "experiment": {"orange_completion_sent": True}}
    require(module.citrus_receipt_build_error(failed) == error, "the build error is read")
    require(module.citrus_receipt_build_error(
        {"recording_observation_finalization": {"receipt_build_error": None}}) == "", "null means none")
    require(module.citrus_receipt_build_error({}) == "", "an older Citrus without the field")

    args = module.parse_args(["--declared-by", "delahantyj", "--intent", "stimulus_experiment",
                              "--blank-subjects", "--citrus-receipt-timeout-seconds", "60"])
    orchestrator = module.Orchestrator(args)
    seen: dict[str, Any] = {}

    def fake_wait(label, schema_id, socket_path, predicate, timeout_seconds, description):
        seen["settles_at_once"] = predicate(failed)
        return failed

    orchestrator.wait_for_status = fake_wait
    message = ""
    try:
        orchestrator.settle_bound_receipts({})
    except module.OrchestratorError as exc:
        message = str(exc)
    require(seen.get("settles_at_once") is True, "a build error ends the wait without the idle grace")
    require(error in message, f"the failure names Citrus's reason: {message!r}")
    receipts = orchestrator.bound_record["receipts"]
    require(receipts["receipt_build_error"] == error and receipts["orange_completion_sent"] is True,
            f"the summary records both fields: {receipts}")


def test_citrus_declare_subjects_and_executor_rule() -> None:
    module = load_module()
    fixture = next((REPO_ROOT / "tools" / "fixtures" / "declaration_v2" / "orchard_v2").glob("*.orange_declaration.json"))
    orange_declaration = json.loads(fixture.read_text())
    plan = orange_declaration["source"]

    def arena(arena_id: str, serial: str, **changes: Any) -> dict[str, Any]:
        declaration = {"schema_id": "citrus.subject_declaration", "schema_version": 2,
                       "source": "orchard_run_plan", "run_plan": copy.deepcopy(plan),
                       "subject_type": "dish_group", "subject_count": 5}
        declaration.update(changes)
        return {"arena_id": arena_id, "camera_serial": serial, "declaration": declaration}

    request = {"schema_id": "citrus.subject_declaration_request", "schema_version": 1, "rig_id": "omnifin0",
               "canvas_name": "c", "protocol": {"path": "p.json", "protocol_semantic_hash": "0" * 64,
                                                 "semantic_version": "4"},
               "arenas": [arena("arena_1", "2010093"), arena("arena_2", "2010094")]}

    # What Orange's pre-arm cross-check would refuse is refused before sending.
    require(module.citrus_declaration_plan_mismatches(request, orange_declaration) == [], "a matching plan")
    require(module.citrus_declaration_plan_mismatches(request, None) == [], "headless: the spec declares")
    edited = copy.deepcopy(request)
    edited["arenas"][1]["declaration"]["run_plan"]["plan_revision"] = 3
    edited["arenas"][0]["camera_serial"] = "9999999"
    problems = module.citrus_declaration_plan_mismatches(edited, orange_declaration)
    require(len(problems) == 2 and "9999999" in problems[0] and "arena_2: run_plan" in problems[1], f"{problems}")
    v1 = {k: v for k, v in orange_declaration.items() if k != "source"}
    v1["schema_version"] = 1
    require(any("not a v2 orchard_run_plan" in p for p in module.citrus_declaration_plan_mismatches(request, v1)),
            "a plan-sourced Citrus arena against an Orange v1 declaration")
    operator = copy.deepcopy(request)
    for item in operator["arenas"]:
        item["declaration"].update(source="citrus_operator_declaration", run_plan=None)
    require(module.citrus_declaration_plan_mismatches(operator, v1) == [], "operator declarations carry no plan")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        request_path = base / "request.json"
        request_path.write_text(json.dumps(request))
        for name, bad in {"schema": {**request, "schema_version": 2},
                          "duplicate": {**request, "arenas": [request["arenas"][0]] * 2},
                          "empty": {**request, "arenas": []}}.items():
            path = base / f"{name}.json"
            path.write_text(json.dumps(bad))
            refused = False
            try:
                module.load_citrus_subject_declaration_request(path)
            except ValueError:
                refused = True
            require(refused, f"{name} request refused at load")

        # The fixture plan pins a placeholder preset and capture_at_setup.
        preset_record = base / "camera_config.json"
        preset_record.write_text(json.dumps(placeholder_preset_record()))
        common = ["--declaration-file", str(fixture), "--camera-config-json", str(preset_record),
                  "--run-dir", str(base / "run")]
        args = module.parse_args(common + ["--citrus-subject-declaration-request", str(request_path)])
        require(args.citrus_subject_declaration_request == request, "the request is loaded whole")
        dry = module.dry_run_summary(args)["citrus"]
        require(dry["declare_subjects_request"]["method"] == "declare_subjects"
                and dry["declare_subjects_request"]["params"] == request and dry["declare_subjects_mismatches"] == [],
                "the dry run shows the request")
        recording_only = base / "recording_only.json"
        recording_only.write_text(json.dumps({**orange_declaration, "recording_intent": "recording_only"}))
        exited = False
        try:
            module.parse_args(["--declaration-file", str(recording_only),
                               "--declaration-files-root", str(fixture.parent),
                               "--camera-config-json", str(preset_record), "--run-dir", str(base / "run"),
                               "--citrus-subject-declaration-request", str(request_path)])
        except SystemExit:
            exited = True
        require(exited, "declare_subjects needs a bound run")

        sha = {"arena_1": "sha256:" + "1" * 64, "arena_2": "sha256:" + "2" * 64}
        accepted_reply = {"ok": True, "accepted": True, "effect": {"subject_declarations": {
            "status": "accepted", "plan": {"plan_id": plan["plan_id"]},
            "arenas": [{"arena_id": a, "subject_type": "dish_group", "subject_count": 5,
                        "declaration_sha256": h} for a, h in sha.items()]}}}
        require(module.accepted_citrus_subject_declarations(accepted_reply) == sha, "the reply's shas")

        def status_with(**per_arena: Any) -> dict[str, Any]:
            return {"arenas": [{"arena_id": a, "plan_subject_declaration": v} for a, v in per_arena.items()]}

        declared = status_with(arena_1={"state": "declared", "declaration_sha256": sha["arena_1"]},
                               arena_2={"state": "declared", "declaration_sha256": sha["arena_2"]})
        require(module.citrus_subject_declaration_problems(declared, sha) == [], "declared with the accepted sha")
        cases = {
            "null": (status_with(arena_1=declared["arenas"][0]["plan_subject_declaration"], arena_2=None), "is null"),
            "overridden": (status_with(arena_1=declared["arenas"][0]["plan_subject_declaration"],
                                       arena_2={"state": "overridden", "declaration_sha256": sha["arena_2"]}),
                           "'overridden'"),
            "other_sha": (status_with(arena_1=declared["arenas"][0]["plan_subject_declaration"],
                                      arena_2={"state": "declared", "declaration_sha256": "sha256:" + "3" * 64}),
                          "is not the accepted"),
            "missing": (status_with(arena_1=declared["arenas"][0]["plan_subject_declaration"]), "not in Citrus status"),
        }
        for name, (status, text) in cases.items():
            problems = module.citrus_subject_declaration_problems(status, sha)
            require(len(problems) == 1 and problems[0].startswith("arena_2") and text in problems[0],
                    f"{name}: {problems}")

        # The orchestrator: send, wait for the apply, check again before START.
        orchestrator = module.Orchestrator(args)
        sent: list[dict[str, Any]] = []
        statuses = [status_with(arena_1=None, arena_2=None), declared]
        orchestrator.send = lambda socket, req: (sent.append(req), accepted_reply)[1]

        def fake_wait(label, schema_id, socket_path, predicate, timeout_seconds, description):
            for status in statuses:
                if predicate(status):
                    return status
            raise module.OrchestratorError("timed out")

        current = {"status": declared}
        orchestrator.wait_for_status = fake_wait
        orchestrator.status = lambda label, schema_id, socket_path: (current["status"], {})
        orchestrator.declare_citrus_subjects()
        require(len(sent) == 1 and sent[0]["method"] == "declare_subjects" and sent[0]["params"] == request
                and sent[0]["operation_id"] == args.operation_id, f"one declare_subjects: {sent}")
        orchestrator.check_citrus_subject_declarations("before_start")
        current["status"] = cases["overridden"][0]
        message = ""
        try:
            orchestrator.check_citrus_subject_declarations("before_start")
        except module.OrchestratorError as exc:
            message = str(exc)
        require("before_start" in message and "arena_2" in message and "overridden" in message,
                f"a panel override after the declaration refuses START: {message!r}")
        checks = orchestrator.bound_record["citrus_subject_declaration_checks"]
        require([c["when"] for c in checks] == ["applied", "before_start", "before_start"]
                and checks[-1]["problems"], f"each check is recorded: {checks}")

        # An apply Citrus skipped (an experiment armed in between) never shows.
        orchestrator = module.Orchestrator(args)
        orchestrator.send = lambda socket, req: accepted_reply
        statuses = [status_with(arena_1=None, arena_2=None)]
        current["status"] = statuses[0]
        orchestrator.wait_for_status = fake_wait
        orchestrator.status = lambda label, schema_id, socket_path: (current["status"], {})
        message = ""
        try:
            orchestrator.declare_citrus_subjects()
        except module.OrchestratorError as exc:
            message = str(exc)
        require("(applied)" in message and "is null" in message, f"a skipped apply refuses: {message!r}")

        # A refusal names Citrus's code and detail; nothing is checked after it.
        orchestrator = module.Orchestrator(args)
        orchestrator.send = lambda socket, req: {
            "ok": True, "accepted": False, "error": "subject_declaration_protocol_mismatch",
            "effect": {"subject_declarations": {"status": "refused", "detail": "arena_2 protocol", "arena_id": "arena_2"}}}
        message = ""
        try:
            orchestrator.declare_citrus_subjects()
        except module.OrchestratorError as exc:
            message = str(exc)
        require("subject_declaration_protocol_mismatch" in message and "arena_2 protocol" in message, message)
        require(orchestrator.check_citrus_subject_declarations("before_start") == {},
                "no accepted declaration, no executor check")

        # A mismatch with Orange's declaration is refused before anything is sent.
        edited_path = base / "edited.json"
        edited_path.write_text(json.dumps(edited))
        orchestrator = module.Orchestrator(module.parse_args(common + ["--citrus-subject-declaration-request",
                                                                       str(edited_path)]))
        sent.clear()
        orchestrator.send = lambda socket, req: (sent.append(req), accepted_reply)[1]
        message = ""
        try:
            orchestrator.declare_citrus_subjects()
        except module.OrchestratorError as exc:
            message = str(exc)
        require(not sent and "does not match Orange's" in message, f"refused before sending: {message!r}")

    plain = module.Orchestrator(module.parse_args(["--declared-by", "delahantyj", "--intent", "stimulus_experiment",
                                                   "--blank-subjects"]))
    plain.send = lambda socket, req: (_ for _ in ()).throw(AssertionError("nothing sent"))
    plain.declare_citrus_subjects()
    require(plain.check_citrus_subject_declarations("before_start") == {}, "without the option nothing changes")


def test_long_wait_wakes_on_lifecycle_event() -> None:
    """wait_for_status on a long wait reads status again as soon as a row arrives,
    and confirms by status (a row alone never satisfies the predicate)."""
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        citrus_log = root / "citrus.sock.events.jsonl"
        citrus_log.write_text("")
        args = module.parse_args(["--execute", "--citrus-socket", str(root / "citrus.sock"),
                                  "--orange-socket", str(root / "orange.sock"),
                                  "--lifecycle-event-heartbeat-seconds", "30"])
        orchestrator = module.Orchestrator(args)
        statuses = iter([{"recording_observation_finalization": {"state": "delivering"}},
                         {"recording_observation_finalization": {"state": "delivered"}}])
        reads: list[float] = []

        def fake_status(label: str, schema_id: str, socket_path: str):
            reads.append(time.monotonic())
            status = next(statuses)
            if len(reads) == 1:
                with citrus_log.open("a") as handle:
                    handle.write(json.dumps({"event": "receipt_delivery", "state": "delivered"}) + "\n")
            return {}, status

        orchestrator.status = fake_status  # type: ignore[assignment]
        orchestrator.raise_if_started_process_exited = lambda context: None  # type: ignore[assignment]
        started = time.monotonic()
        status = orchestrator.wait_for_status(
            "citrus", module.CITRUS_REQUEST_SCHEMA_ID, args.citrus_socket,
            module.citrus_finalization_settled, 60.0, "receipts_delivered")
        require(module.citrus_finalization_state(status) == "delivered", status)
        require(time.monotonic() - started < 2.0, "woken by the row, not the 30 s heartbeat")
        record = orchestrator.lifecycle_waker.record()
        require(record["wakes_by_event"] == 1 and record["logs"]["citrus"]["receipt_delivery_states"] == ["delivered"],
                record)
        off = module.Orchestrator(module.parse_args(["--lifecycle-events", "off"]))
        require(off.lifecycle_waker is None, "--lifecycle-events off polls only")


def test_camera_config_dir_digest_rule() -> None:
    digest_tool = REPO_ROOT / "scripts" / "camera_config_dir_digest.py"
    import hashlib
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / "preset"
        folder.mkdir()
        (folder / "2010093.json").write_text('{"exposure": 50}\n')
        (folder / "2010094.json").write_text('{"exposure": 50, "x": "\u00e9"}\n')
        (folder / "README.md").write_text("not part of the digest\n")
        files = {p.name: "sha256:" + hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob("*.json")}
        expected = "sha256:" + hashlib.sha256(
            json.dumps({"files": files}, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
        ).hexdigest()

        def run(*extra: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run([sys.executable, str(digest_tool), str(folder), *extra],
                                  capture_output=True, text=True, check=False)

        result = run()
        require(result.returncode == 0 and result.stdout.strip() == expected,
                f"the rule: raw bytes per file, sort-keys compact JSON: {result.stdout} {result.stderr}")
        (folder / "README.md").write_text("changed\n")
        require(run().stdout.strip() == expected, "README and other non-JSON files are not in the digest")
        record_path = Path(tmp) / "out" / "camera_config.json"
        require(run("--expect", expected, "--write", str(record_path)).returncode == 0, "a matching pin passes")
        record = json.loads(record_path.read_text())
        require(record["sha256"] == expected and record["expected_sha256"] == expected
                and record["hash_rule"] == "orange_camera_config_dir_sha256_v1"
                and record["name"] == "preset" and record["files"] == files, f"the record: {record}")
        mismatch = run("--expect", "sha256:" + "0" * 64, "--write", str(record_path))
        require(mismatch.returncode == 3 and "refusing" in mismatch.stderr, f"a different pin refuses: {mismatch}")
        require(json.loads(record_path.read_text())["expected_sha256"] == "sha256:" + "0" * 64,
                "the refused record still names both digests")
        require(run("--expect", "abc").returncode == 2, "a malformed pin is refused")
        (folder / "2010095.json").symlink_to(folder / "2010093.json")
        linked = run()
        require(linked.returncode == 2 and "2010095.json" in linked.stderr and "symlink" in linked.stderr,
                f"a symlinked camera JSON is an error naming it: {linked}")
        (folder / "2010095.json").unlink()
        (folder / "2010096.json").mkdir()
        require(run().returncode == 2, "a *.json that is not a regular file is an error")
        (folder / "2010096.json").rmdir()
        (folder / "2010093.json").write_text('{"exposure": 100}\n')
        require(run().stdout.strip() != expected, "any camera JSON byte change changes the digest")
        empty = Path(tmp) / "empty"
        empty.mkdir()
        require(subprocess.run([sys.executable, str(digest_tool), str(empty)], capture_output=True,
                               check=False).returncode == 2, "a folder with no camera JSON has no digest")


def test_bound_launcher_preset_pin_and_clip_seconds() -> None:
    launcher = REPO_ROOT / "scripts" / "run_orange_citrus_bound.sh"
    digest_tool = REPO_ROOT / "scripts" / "camera_config_dir_digest.py"
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        citrus = root / "citrus"
        (citrus / "targets" / "rigs" / "omnifin0" / "shadow").mkdir(parents=True)
        (citrus / "targets" / "rigs" / "omnifin0" / "shadow" / "shadow.json").write_text("{}\n")
        (citrus / "targets" / "citrus").write_text("#!/bin/sh\nexit 1\n")
        (citrus / "targets" / "citrus").chmod(0o755)
        protocol = root / "protocol.json"
        protocol.write_text("{}\n")
        preset = root / "30_cam4_ptp_fourcam"
        preset.mkdir()
        (preset / "2010093.json").write_text('{"frame_rate": 30}\n')
        digest = subprocess.run([sys.executable, str(digest_tool), str(preset)], capture_output=True,
                                text=True, check=True).stdout.strip()
        env = {**__import__("os").environ, "CITRUS_ROOT": str(citrus)}
        env.pop("ORANGE_GUI_CONFIG_DIR", None)

        def launch(out: str, *extra: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                ["bash", str(launcher), "--protocol", str(protocol), "--declared-by", "tester",
                 "--camera-config-dir", str(preset), "--out-dir", str(root / out), *extra],
                capture_output=True, text=True, check=False, env=env,
            )

        ok = launch("ok", "--camera-config-sha256", digest, "--clip-seconds", "900")
        require(ok.returncode == 0, f"a matching pin dry-runs: {ok.stderr}")
        plan = json.loads(ok.stdout)
        require(plan["result"] == "dry_run", "the launcher without --execute is a dry run")
        require(plan["camera_config"]["sha256"] == digest and plan["camera_config"]["expected_sha256"] == digest
                and plan["camera_config"]["path"] == str(preset.resolve()), f"summary records the preset: {plan}")
        require(plan["orange"]["command"][-2:] == ["--clip-seconds", "900"], f"clip passthrough: {plan['orange']}")
        require(plan["orange"]["env_overlay"]["ORANGE_GUI_CONFIG_DIR"] == str(preset.resolve()),
                "the preset reaches Orange")
        require(f"camera_config_sha256={digest}" in ok.stderr and "clip_seconds=900" in ok.stderr,
                f"the [bound-run] line names digest and clip: {ok.stderr}")
        require(json.loads((root / "ok" / "camera_config.json").read_text())["sha256"] == digest,
                "camera_config.json is written in the out dir")

        unpinned = launch("unpinned")
        require(unpinned.returncode == 0, f"no pin still runs: {unpinned.stderr}")
        unpinned_plan = json.loads(unpinned.stdout)
        require(unpinned_plan["camera_config"]["expected_sha256"] is None
                and unpinned_plan["camera_config"]["sha256"] == digest, "the digest is always recorded")
        require("--clip-seconds" not in unpinned_plan["orange"]["command"]
                and "clip_seconds=app_config" in unpinned.stderr, "no clip flag: the app config decides")

        mismatch = launch("mismatch", "--camera-config-sha256", "sha256:" + "1" * 64)
        require(mismatch.returncode == 2 and "not launching" in mismatch.stderr and not mismatch.stdout.strip(),
                f"a different folder digest refuses before the orchestrator runs: {mismatch}")
        bad_clip = launch("bad_clip", "--clip-seconds", "0")
        require(bad_clip.returncode == 2 and "positive integer" in bad_clip.stderr, f"clip 0 refused: {bad_clip}")


def test_plan_declaration_recording_only_all_blank_is_accepted() -> None:
    module = load_module()
    fixture = next((REPO_ROOT / "tools" / "fixtures" / "declaration_v2" / "orchard_v2").glob("*.orange_declaration.json"))
    declaration = json.loads(fixture.read_text())
    declaration["recording_intent"] = "recording_only"
    declaration["subject_references"]["cameras"] = {
        serial: {"blank": True} for serial in declaration["subject_references"]["cameras"]
    }
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "fish_free.orange_declaration.json"
        path.write_text(json.dumps(declaration))
        shutil.copytree(fixture.parent / "run_plan", Path(tmp) / "run_plan")
        preset_record = Path(tmp) / "camera_config.json"
        preset_record.write_text(json.dumps(placeholder_preset_record()))
        args = module.parse_args(["--declaration-file", str(path), "--camera-config-json", str(preset_record),
                                  "--run-dir", str(Path(tmp) / "run")])
        require(args.declaration == declaration, f"accepted unchanged: {args.declaration}")
        require(args.declaration["source"]["kind"] == "orchard_run_plan", "still plan-sourced")
        start = module.build_orange_start_request("op", "req", "test", args.declaration)
        require(start["params"]["declaration"] == declaration, "the start carries it")

ORCHARD_FIXTURE = REPO_ROOT / "tools" / "fixtures" / "declaration_v2" / "orchard_v2"


def placeholder_preset_record() -> dict[str, Any]:
    """A preset record equal to the fixture plan's placeholder pin."""
    return {"name": "PLACEHOLDER_PRESET", "path": "/x/PLACEHOLDER_PRESET",
            "hash_rule": "orange_camera_config_dir_sha256_v1",
            "sha256": "sha256:" + "0" * 64, "expected_sha256": None, "files": {}}


def make_orchard_dir(root: Path, *, preset: dict[str, Any] | None = None,
                     empty_background: Any = "capture_at_setup",
                     intent: str = "stimulus_experiment", all_blank: bool = False) -> Path:
    """A declare.py --out folder built from the vendored fixture, with the plan
    session's orange.preset / empty_background replaced and every name and
    sha256 kept consistent (plan JSON copy renamed by its new sha256)."""
    root.mkdir(parents=True, exist_ok=True)
    declaration = json.loads(next(ORCHARD_FIXTURE.glob("*.orange_declaration.json")).read_text())
    shutil.copytree(ORCHARD_FIXTURE / "run_plan", root / "run_plan")
    source = declaration["source"]
    plan_path = root / source["plan_json_copy_relative_path"]
    plan = json.loads(plan_path.read_text())
    session = next(s for s in plan["sessions"] if s["session_id"] == source["plan_session_id"])
    if preset == {}:
        session["orange"].pop("preset")  # a plan that pins no preset
    elif preset is not None:
        session["orange"]["preset"] = preset
    if empty_background is None:
        session["orange"].pop("empty_background", None)
    else:
        session["orange"]["empty_background"] = empty_background
    data = json.dumps(plan, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    plan_sha = hashlib.sha256(data).hexdigest()
    plan_path.unlink()
    (root / "run_plan" / f"{plan_sha}.json").write_bytes(data)
    source["plan_sha256"] = plan_sha
    source["plan_json_copy_relative_path"] = f"run_plan/{plan_sha}.json"
    declaration["recording_intent"] = intent
    if all_blank:
        declaration["subject_references"]["cameras"] = {
            serial: {"blank": True} for serial in declaration["subject_references"]["cameras"]}
    (root / "test.r5.S1.orange_declaration.json").write_text(json.dumps(declaration, indent=2))
    citrus_request = {
        "schema_id": "citrus.subject_declaration_request", "schema_version": 1, "rig_id": "omnifin0",
        "canvas_name": "c", "protocol": {"path": "p.json", "protocol_semantic_hash": "0" * 64,
                                          "semantic_version": "citrus.protocol.semantic.v5"},
        "arenas": [{"arena_id": f"arena_{i + 1}", "camera_serial": serial,
                    "declaration": {"schema_id": "citrus.subject_declaration", "schema_version": 2,
                                    "source": "orchard_run_plan", "run_plan": copy.deepcopy(source)}}
                   for i, serial in enumerate(sorted(declaration["subject_references"]["cameras"]))],
    }
    (root / "test.r5.S1.citrus_declaration_request.json").write_text(json.dumps(citrus_request, indent=2))
    return root


def parse_error(module: Any, argv: list[str]) -> str:
    """The parser's refusal text (argparse exits 2 and prints to stderr)."""
    result = subprocess.run([sys.executable, str(SCRIPT), *argv], capture_output=True, text=True, check=False)
    require(result.returncode == 2, f"expected a refusal for {argv}: rc={result.returncode} {result.stderr}")
    return result.stderr


def test_orchard_declaration_files_preset_and_citrus_gate() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        preset = {"name": "30_cam4_ptp_fourcam", "sha256": "sha256:" + "a" * 64,
                  "hash_rule": "orange_camera_config_dir_sha256_v1"}
        record = {**preset, "path": "/x/30_cam4_ptp_fourcam", "files": {}, "expected_sha256": preset["sha256"]}
        record_path = base / "camera_config.json"
        record_path.write_text(json.dumps(record))
        folder = make_orchard_dir(base / "declared", preset=preset)
        declaration = next(folder.glob("*.orange_declaration.json"))
        request = next(folder.glob("*.citrus_declaration_request.json"))
        good = ["--declaration-file", str(declaration), "--citrus-subject-declaration-request", str(request),
                "--camera-config-json", str(record_path), "--run-dir", str(base / "run")]

        args = module.parse_args(good)
        require(args.declaration_files_root == folder.resolve(), "the files root defaults to the declaration's folder")
        require(args.plan_session["plan_session_id"] == "S1" and args.plan_preset_check["problems"] == [],
                f"the plan session and its preset check: {args.plan_session} {args.plan_preset_check}")
        require(args.empty_background == "capture_at_setup", "auto takes the plan's capture_at_setup")
        require(args.background_declared_by == "delahantyj", "the capture's claim defaults to the declaration's identity")
        dry = module.dry_run_summary(args)
        start = dry["orange"]["start_request"]["params"]
        require(start["declaration"] == json.loads(declaration.read_text())
                and start["declaration_files_root"] == str(folder.resolve()),
                f"the start carries the declaration unchanged and its files root: {start.keys()}")
        require(dry["plan"]["preset_check"]["preset"] == preset and dry["plan"]["plan_id"],
                f"the dry run shows the plan and its pin: {dry['plan']}")
        require(dry["citrus"]["declare_subjects_request"]["params"] == json.loads(request.read_text()),
                "Citrus gets the plan's request")

        # Preset pin (abort rule A8): no record, other digest, other name.
        text = parse_error(module, [a for a in good if a not in ("--camera-config-json", str(record_path))])
        require("no preset record" in text, text)
        for key, value in (("sha256", "sha256:" + "b" * 64), ("name", "100_cam4_ptp_fourcam")):
            other = base / f"camera_config_{key}.json"
            other.write_text(json.dumps({**record, key: value}))
            text = parse_error(module, [str(other) if a == str(record_path) else a for a in good])
            require(f"preset {key}" in text and "not the one the plan pins" in text, text)

        # A plan without a pin records none and does not refuse.
        unpinned = make_orchard_dir(base / "unpinned", preset={})
        args = module.parse_args(["--declaration-file", str(next(unpinned.glob("*.orange_declaration.json"))),
                                  "--citrus-subject-declaration-request", str(request), "--run-dir", str(base / "run")])
        require(args.plan_preset_check == {"preset": None, "problems": []}, f"no pin: {args.plan_preset_check}")

        # The bound run needs Citrus's plan declaration (pre-arm cross-check v2).
        text = parse_error(module, [a for a in good if a not in ("--citrus-subject-declaration-request", str(request))])
        require("--citrus-subject-declaration-request" in text and "pre-arm" in text, text)

        # The run_plan/ folder follows Orange's rule.
        extra = make_orchard_dir(base / "extra", preset=preset)
        (extra / "run_plan" / "notes.txt").write_text("x")
        text = parse_error(module, [str(next(extra.glob("*.orange_declaration.json"))) if a == str(declaration) else a
                                    for a in good])
        require("not named by the declaration" in text, text)
        tampered = make_orchard_dir(base / "tampered", preset=preset)
        with open(next((tampered / "run_plan" / "sign_off").glob("*.json")), "a") as handle:
            handle.write(" ")
        text = parse_error(module, [str(next(tampered.glob("*.orange_declaration.json"))) if a == str(declaration)
                                    else a for a in good])
        require("does not hash" in text, text)
        text = parse_error(module, good + ["--declaration-files-root", str(base / "nowhere")])
        require("missing declaration file" in text, text)
        text = parse_error(module, ["--declared-by", "delahantyj", "--intent", "recording_only", "--blank-subjects",
                                    "--declaration-files-root", str(folder)])
        require("only for an orchard_run_plan" in text, text)

        # The plan's background rule binds; the runner never drops it.
        text = parse_error(module, good + ["--empty-background", "none"])
        require("capture_at_setup" in text, text)
        text = parse_error(module, [a for a in good if a not in ("--run-dir", str(base / "run"))])
        require("--run-dir" in text, text)
        no_rule = make_orchard_dir(base / "no_rule", preset=preset, empty_background=None)
        args = module.parse_args([str(next(no_rule.glob("*.orange_declaration.json"))) if a == str(declaration)
                                  else a for a in good])
        require(args.empty_background == "none" and module.dry_run_summary(args)["empty_background"] == {"mode": "none"},
                "a plan without the rule records no background")
        odd = make_orchard_dir(base / "odd", preset=preset, empty_background="capture_between_sessions")
        text = parse_error(module, [str(next(odd.glob("*.orange_declaration.json"))) if a == str(declaration)
                                    else a for a in good])
        require("not one this runner knows" in text, text)

        # Fish-free test mode never confirms 'no fish' for a declaration with fish.
        text = parse_error(module, good + ["--fish-free-test"])
        require("names subjects" in text, text)
        blank = make_orchard_dir(base / "blank", preset=preset, intent="recording_only", all_blank=True)
        args = module.parse_args(["--declaration-file", str(next(blank.glob("*.orange_declaration.json"))),
                                  "--camera-config-json", str(record_path), "--run-dir", str(base / "run"),
                                  "--fish-free-test"])
        require(args.fish_free_test and args.empty_background == "capture_at_setup",
                "an all-blank plan may run the fish-free test")
        text = parse_error(module, ["--declared-by", "delahantyj", "--intent", "recording_only", "--blank-subjects",
                                    "--fish-free-test"])
        require("only applies to an empty background" in text, text)
        text = parse_error(module, ["--empty-background", "capture_at_setup", "--run-dir", str(base / "run")])
        require("declared start" in text, text)
        text = parse_error(module, ["--declared-by", "delahantyj", "--intent", "recording_only", "--blank-subjects",
                                    "--empty-background", "capture_at_setup", "--run-dir", str(base / "run"),
                                    "--background-stimulus-display", '{"state": "bright"}'])
        require("stimulus-display" in text, text)


def background_orange_status(base: dict[str, Any], capture: dict[str, Any] | None,
                             staging: dict[str, Any]) -> dict[str, Any]:
    status = copy.deepcopy(base)
    status["local_control"]["empty_background_capture"] = {**(capture or {"state": "idle"}), "staging": staging}
    return status


def run_background_fake(
    module: Any,
    root: Path,
    argv: list[str],
    *,
    confirm: Callable[[Path], None] | None = None,
    camera_results: dict[str, str] | None = None,
    staging: dict[str, Any] | None = None,
    allowed_uids: set[int] | None = None,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """A GUI run against fake sockets whose Orange answers capture_empty_background.
    `confirm` runs once the runner listens on its confirmation socket."""
    cameras = camera_results or {"2010093": "captured", "2010094": "captured"}
    staging = staging or {serial: {"reserved": True, "bytes": 1, "error": None} for serial in cameras}
    state: dict[str, Any] = {"started": False, "stopped": False, "capture": None, "citrus_started": False,
                             "citrus_polls": 0}
    sent: list[dict[str, Any]] = []

    def handle_orange(request: dict[str, Any]) -> dict[str, Any]:
        method = request.get("method")
        if method != "status":
            sent.append(request)
        if method == "capture_empty_background":
            state["capture"] = {
                "request_id": request["request_id"], "operation_id": request["operation_id"],
                "state": "captured", "capture_id": "eb_20261116T000000Z_test",
                "descriptor_path": "/x/backgrounds/eb_20261116T000000Z_test/background.json",
                "descriptor_sha256": "sha256:" + "c" * 64, "error": None,
                "started_at_utc": "2026-11-16T00:00:00Z", "finished_at_utc": "2026-11-16T00:00:01Z",
                "cameras": {serial: {"capture_status": result, "failure_reason": None if result == "captured"
                                     else "burst did not complete within 5 s"}
                            for serial, result in cameras.items()},
            }
        elif method == "start_recording":
            state["started"] = True
        elif method == "stop_recording":
            state["stopped"] = True
        status = background_orange_status(orange_status(state["started"], state["stopped"]), state["capture"], staging)
        return response_for(request, status)

    def handle_citrus(request: dict[str, Any]) -> dict[str, Any]:
        if request.get("method") == "start_experiment":
            state["citrus_started"] = True
        elif request.get("method") == "status" and state["citrus_started"]:
            state["citrus_polls"] += 1
        return response_for(request, citrus_status(state["citrus_started"], state["citrus_polls"] >= 1))

    def fake_send(socket_path: str, request: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
        return handle_orange(request) if socket_path == "orange-test.sock" else handle_citrus(request)

    original = module.send_unix_json
    module.send_unix_json = fake_send
    watcher = None
    if confirm is not None:
        def watch() -> None:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not (root / module.SCENE_CONFIRMATION_SOCKET_NAME).exists():
                time.sleep(0.01)
            confirm(root)
        watcher = threading.Thread(target=watch)
        watcher.start()
    try:
        args = module.parse_args([
            "--execute", "--operation-id", "op-bg", "--orange-socket", "orange-test.sock",
            "--citrus-socket", "citrus-test.sock", "--poll-interval-seconds", "0.01", "--timeout-seconds", "2",
            "--citrus-terminal-timeout-seconds", "2", "--orange-finalize-timeout-seconds", "2",
            "--lifecycle-events", "off", "--run-dir", str(root), *argv])
        orchestrator = module.Orchestrator(args)
        if allowed_uids is not None:
            orchestrator.scene_allowed_uids = allowed_uids  # a foreign peer, without a second user
        try:
            summary = orchestrator.run()
        except module.OrchestratorError as exc:
            summary = orchestrator.summary("fail", error=str(exc))
    finally:
        module.send_unix_json = original
        if watcher is not None:
            watcher.join()
    return summary, sent


def test_empty_background_capture_confirmation_and_start() -> None:
    module = load_module()
    blank = ["--declared-by", "delahantyj", "--intent", "recording_only", "--blank-subjects",
             "--empty-background", "capture_at_setup"]
    confirm_script = REPO_ROOT / "scripts" / "confirm_bound_run_scene.py"
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)

        # Fish-free test: the runner confirms itself, recorded as such.
        summary, sent = run_background_fake(module, base / "fish_free", blank + ["--fish-free-test"])
        require(summary["result"] == "pass", f"fish-free run passes: {summary.get('error')}")
        methods = [r["method"] for r in sent]
        require(methods[:2] == ["capture_empty_background", "start_recording"], f"capture before start: {methods}")
        capture_params = sent[0]["params"]
        require(capture_params["declared_by"] == "delahantyj" and capture_params["dishes_empty"] is True
                and capture_params["stimulus_display"] == {"state": "unknown"} and sent[0]["operation_id"] == "op-bg",
                f"the capture request: {sent[0]}")
        start = sent[1]["params"]
        require(start["empty_background"] == {"capture_id": "eb_20261116T000000Z_test",
                                              "descriptor_sha256": "sha256:" + "c" * 64,
                                              "scene_unchanged_confirmed": True}
                and start["declaration"]["subject_references"] == {"schema_version": 1},
                f"the start names the capture: {start}")
        record = summary["empty_background"]
        confirmation = json.loads((base / "fish_free" / module.SCENE_CONFIRMATION_NAME).read_text())
        require(record["state"] == "confirmed" and record["scene_confirmation"]["content"] == confirmation
                and confirmation["decision"] == module.SCENE_DECISION_FISH_FREE
                and confirmation["requester"]["transport"] == "runner_fish_free_test"
                and not (base / "fish_free" / module.SCENE_CONFIRMATION_SOCKET_NAME).exists(),
                f"the summary records the self-confirmation as fish-free, no socket opened: {record}")
        capture_record = json.loads((base / "fish_free" / module.CAPTURE_RECORD_NAME).read_text())
        require(capture_record["capture_id"] == "eb_20261116T000000Z_test"
                and capture_record["descriptor_sha256"] == "sha256:" + "c" * 64
                and set(capture_record["cameras"]) == {"2010093", "2010094"} and capture_record["recorded_at_utc"],
                f"the capture record: {capture_record}")

        # An operator confirmation, with the CLI over the run's socket.
        seen: dict[str, Any] = {}

        def operator(run_dir: Path) -> None:
            seen["mode"] = (run_dir / module.SCENE_CONFIRMATION_SOCKET_NAME).stat().st_mode & 0o777
            seen["owner"] = (run_dir / module.SCENE_CONFIRMATION_SOCKET_NAME).stat().st_uid
            result = subprocess.run([sys.executable, str(confirm_script), "--run-dir", str(run_dir),
                                     "--confirmed-by", "delahantyj"], capture_output=True, text=True)
            seen["result"] = result

        summary, sent = run_background_fake(module, base / "operator", blank, confirm=operator)
        require(summary["result"] == "pass", f"operator-confirmed run passes: {summary.get('error')}")
        require(seen["mode"] == 0o600 and seen["owner"] == __import__("os").getuid(),
                f"the socket is 0600 and the invoking user's: {oct(seen['mode'])}")
        require(seen["result"].returncode == 0 and "recorded by the runner" in seen["result"].stdout,
                f"the CLI reports the runner's acceptance: {seen['result']}")
        confirmation = summary["empty_background"]["scene_confirmation"]["content"]
        peer = confirmation["requester"]["peer"]
        require(confirmation["decision"] == module.SCENE_DECISION_FISH_LOADED
                and confirmation["confirmed_by"] == "delahantyj"
                and confirmation["requester"]["transport"] == "scene_confirmation_socket"
                and peer["uid"] == __import__("os").getuid() and peer["pid"] != __import__("os").getpid()
                and peer["user"] and peer["exe"] and any("confirm_bound_run_scene.py" in a for a in peer["cmdline"]),
                f"the caller is named by SO_PEERCRED and /proc, not by itself: {confirmation['requester']}")
        written = json.loads((base / "operator" / module.SCENE_CONFIRMATION_NAME).read_text())
        require(written == confirmation and not (base / "operator" / module.SCENE_CONFIRMATION_SOCKET_NAME).exists(),
                "the runner wrote the file it reports, and removed its socket")
        methods = [r["method"] for r in sent]
        require(methods.index("capture_empty_background") < methods.index("start_recording"),
                f"the start follows the capture and its confirmation: {methods}")
        again = subprocess.run([sys.executable, str(confirm_script), "--run-dir", str(base / "operator"),
                                "--confirmed-by", "delahantyj", "--abort"], capture_output=True, text=True)
        require(again.returncode == 2 and "not waiting" in again.stderr, f"no second confirmation: {again}")
        none_yet = subprocess.run([sys.executable, str(confirm_script), "--run-dir", str(base / "empty"),
                                   "--confirmed-by", "delahantyj"], capture_output=True, text=True)
        require(none_yet.returncode == 2 and "no capture record" in none_yet.stderr, "nothing to confirm yet")

        # Abort: the operator refuses; nothing starts.
        def abort(run_dir: Path) -> None:
            subprocess.run([sys.executable, str(confirm_script), "--run-dir", str(run_dir),
                            "--confirmed-by", "delahantyj", "--abort"], check=True, capture_output=True)

        summary, sent = run_background_fake(module, base / "abort", blank, confirm=abort)
        require(summary["result"] == "fail" and "aborted" in summary["error"]
                and "start_recording" not in [r["method"] for r in sent]
                and summary["empty_background"]["state"] == "aborted_by_operator",
                f"an abort refuses the start: {summary['error']}")

        # Timeout: no confirmation, no start.
        summary, sent = run_background_fake(module, base / "timeout", blank +
                                            ["--scene-confirmation-timeout-seconds", "0.2"])
        require(summary["result"] == "fail" and "no scene confirmation" in summary["error"]
                and "start_recording" not in [r["method"] for r in sent]
                and summary["empty_background"]["state"] == "confirmation_timed_out",
                f"a missing confirmation refuses: {summary['error']}")

        # Bad requests are answered and recorded; the wait goes on until a good one.
        replies: list[dict[str, Any]] = []

        def bad_then_good(run_dir: Path) -> None:
            record = json.loads((run_dir / module.CAPTURE_RECORD_NAME).read_text())
            sock = run_dir / module.SCENE_CONFIRMATION_SOCKET_NAME
            for request in (
                module.build_scene_confirmation_request({**record, "capture_id": "eb_other"}, "delahantyj",
                                                        module.SCENE_DECISION_FISH_LOADED),
                module.build_scene_confirmation_request(record, "delahantyj", module.SCENE_DECISION_FISH_FREE),
                {**module.build_scene_confirmation_request(record, "delahantyj", module.SCENE_DECISION_FISH_LOADED),
                 "requester": {"uid": 0}},
                module.build_scene_confirmation_request(record, "delahantyj", module.SCENE_DECISION_FISH_LOADED),
            ):
                replies.append(module.send_scene_confirmation_request(sock, request))

        summary, sent = run_background_fake(module, base / "bad_then_good", blank, confirm=bad_then_good)
        refused = summary["empty_background"]["scene_confirmation"]["refused_requests"]
        require(summary["result"] == "pass" and [r["accepted"] for r in replies] == [False, False, False, True],
                f"three refusals, then the good one: {replies}")
        require(len(refused) == 3 and "capture_id" in refused[0]["problems"][0]
                and "decision" in refused[1]["problems"][0] and "unknown field requester" in refused[2]["problems"]
                and refused[0]["peer"]["pid"] == __import__("os").getpid(),
                f"each refusal is recorded with its peer: {refused}")

        # A peer whose uid is neither the invoking user's nor root's is refused.
        foreign: dict[str, Any] = {}

        def foreign_peer(run_dir: Path) -> None:
            foreign["result"] = subprocess.run([sys.executable, str(confirm_script), "--run-dir", str(run_dir),
                                                "--confirmed-by", "delahantyj"], capture_output=True, text=True)

        summary, sent = run_background_fake(module, base / "foreign", blank +
                                            ["--scene-confirmation-timeout-seconds", "1"],
                                            confirm=foreign_peer, allowed_uids={987654})
        refused = summary["empty_background"]["scene_confirmation"]["refused_requests"]
        require(foreign["result"].returncode == 1 and "neither the invoking user" in foreign["result"].stderr
                and summary["result"] == "fail" and "start_recording" not in [r["method"] for r in sent]
                and refused and refused[0]["request"] is None, f"foreign uid refused unread: {summary['error']}")

        # A camera that failed its burst, or had no staging, refuses before the fish go in.
        summary, sent = run_background_fake(module, base / "failed_cam", blank + ["--fish-free-test"],
                                            camera_results={"2010093": "captured", "2010094": "failed"})
        require(summary["result"] == "fail" and "2010094" in summary["error"] and "before fish loading" in summary["error"]
                and "start_recording" not in [r["method"] for r in sent], f"failed camera: {summary['error']}")
        summary, sent = run_background_fake(
            module, base / "no_staging", blank + ["--fish-free-test"],
            staging={"2010093": {"reserved": True}, "2010094": {"reserved": False, "error": "out of memory"}})
        require(summary["result"] == "fail" and "out of memory" in summary["error"] and not sent,
                f"unreserved staging refuses before the capture: {summary['error']}")

        # An orchard plan (recording_only, so no Citrus binding): declaration,
        # files root and capture all reach the start.
        preset = placeholder_preset_record()
        record_path = base / "camera_config.json"
        record_path.write_text(json.dumps(preset))
        folder = make_orchard_dir(base / "plan", intent="recording_only")
        summary, sent = run_background_fake(
            module, base / "plan_run",
            ["--declaration-file", str(next(folder.glob("*.orange_declaration.json"))),
             "--camera-config-json", str(record_path)], confirm=operator)
        require(summary["result"] == "pass", f"plan run passes: {summary.get('error')}")
        start = next(r for r in sent if r["method"] == "start_recording")["params"]
        require(start["declaration"]["source"]["kind"] == "orchard_run_plan"
                and start["declaration_files_root"] == str(folder.resolve())
                and start["empty_background"]["scene_unchanged_confirmed"] is True,
                f"the plan start: {start.keys()}")
        require(summary["plan"]["plan_session_id"] == "S1" and summary["plan"]["preset_check"]["problems"] == []
                and summary["empty_background"]["capture_record"]["plan"]["plan_sha256"]
                == start["declaration"]["source"]["plan_sha256"],
                f"the summary and the capture record name the plan: {summary['plan']}")


def test_bound_launcher_declaration_dir() -> None:
    launcher = REPO_ROOT / "scripts" / "run_orange_citrus_bound.sh"
    digest_tool = REPO_ROOT / "scripts" / "camera_config_dir_digest.py"
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        citrus = root / "citrus"
        (citrus / "targets" / "rigs" / "omnifin0" / "shadow").mkdir(parents=True)
        (citrus / "targets" / "rigs" / "omnifin0" / "shadow" / "shadow.json").write_text("{}\n")
        (citrus / "targets" / "citrus").write_text("#!/bin/sh\nexit 1\n")
        (citrus / "targets" / "citrus").chmod(0o755)
        protocol = root / "protocol.json"
        protocol.write_text("{}\n")
        preset_dir = root / "30_cam4_ptp_fourcam"
        preset_dir.mkdir()
        (preset_dir / "2010093.json").write_text('{"frame_rate": 30}\n')
        digest = subprocess.run([sys.executable, str(digest_tool), str(preset_dir)], capture_output=True,
                                text=True, check=True).stdout.strip()
        pinned = make_orchard_dir(root / "declared", preset={
            "name": "30_cam4_ptp_fourcam", "sha256": digest, "hash_rule": "orange_camera_config_dir_sha256_v1"})
        env = {**__import__("os").environ, "CITRUS_ROOT": str(citrus)}
        env.pop("ORANGE_GUI_CONFIG_DIR", None)

        def launch(out: str, *extra: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                ["bash", str(launcher), "--protocol", str(protocol), "--declared-by", "operator1",
                 "--camera-config-dir", str(preset_dir), "--out-dir", str(root / out), *extra],
                capture_output=True, text=True, check=False, env=env)

        def citrus_acceptance(version: int) -> None:
            (citrus / "system_config.yml").write_text(
                "orange_completion:\n    subject_identity_version: 4\n"
                f"    recording_observation_acceptance_schema_version: {version}\n")

        citrus_acceptance(3)
        refused = launch("v3", "--declaration-dir", str(pinned))
        require(refused.returncode == 2 and "acceptance v3" in refused.stderr and "Not launching" in refused.stderr
                and not refused.stdout.strip(), f"Citrus acceptance v3 refuses a plan-declared run: {refused}")
        (citrus / "system_config.yml").unlink()
        missing = launch("noconf", "--declaration-dir", str(pinned))
        require(missing.returncode == 2 and "vunknown" in missing.stderr, f"no Citrus config refuses: {missing}")

        citrus_acceptance(4)
        ok = launch("ok", "--declaration-dir", str(pinned))
        require(ok.returncode == 0, f"a v4 Citrus and the plan's preset dry-run: {ok.stderr}")
        plan = json.loads(ok.stdout)
        start = plan["orange"]["start_request"]["params"]
        require(start["declaration"]["source"]["kind"] == "orchard_run_plan"
                and start["declaration_files_root"] == str(pinned.resolve()), f"plan start: {start.keys()}")
        require(plan["citrus"]["declare_subjects_request"]["method"] == "declare_subjects"
                and plan["citrus"]["declare_subjects_mismatches"] == [], "Citrus gets the plan")
        require(plan["empty_background"]["mode"] == "capture_at_setup"
                and plan["empty_background"]["capture_request"]["params"]["declared_by"] == "operator1"
                and plan["empty_background"]["scene_confirmation_path"] == str(root / "ok" / "scene_confirmation.json"),
                f"the plan's background rule, claimed by the operator: {plan['empty_background']}")
        require("declaration=" + str(pinned.resolve()) in ok.stderr, ok.stderr)

        unpinned_dir = root / "100_cam4_ptp_fourcam"
        unpinned_dir.mkdir()
        (unpinned_dir / "2010093.json").write_text('{"frame_rate": 100}\n')
        wrong = subprocess.run(
            ["bash", str(launcher), "--protocol", str(protocol), "--declared-by", "operator1",
             "--camera-config-dir", str(unpinned_dir), "--out-dir", str(root / "wrong"),
             "--declaration-dir", str(pinned)], capture_output=True, text=True, check=False, env=env)
        require(wrong.returncode == 2 and "not the one the plan pins" in wrong.stderr,
                f"another preset folder refuses: {wrong.stderr}")
        fish_free = launch("ff", "--declaration-dir", str(pinned), "--fish-free-test")
        require(fish_free.returncode == 2 and "names subjects" in fish_free.stderr, fish_free.stderr)

        blank = launch("blank", "--empty-background", "capture_at_setup", "--fish-free-test")
        require(blank.returncode == 0, f"a blank fish-free capture test dry-runs: {blank.stderr}")
        blank_plan = json.loads(blank.stdout)
        require("fish_free_test" in blank_plan["empty_background"]["confirmation"]
                and blank_plan["orange"]["start_request"]["params"]["declaration"]["subject_references"]
                == {"schema_version": 1}, f"blank: {blank_plan['empty_background']}")
        default = launch("default")
        require(default.returncode == 0 and json.loads(default.stdout)["empty_background"] == {"mode": "none"},
                f"without options nothing changes: {default.stderr}")
        two = root / "two"
        shutil.copytree(pinned, two)
        shutil.copy(next(pinned.glob("*.orange_declaration.json")), two / "second.orange_declaration.json")
        ambiguous = launch("two", "--declaration-dir", str(two))
        require(ambiguous.returncode == 2 and "exactly one" in ambiguous.stderr, ambiguous.stderr)


def main() -> int:
    tests = [
        test_event_log_follower,
        test_lifecycle_waker,
        test_long_wait_wakes_on_lifecycle_event,
        test_receipt_build_error_settles_and_is_reported,
        test_citrus_declare_subjects_and_executor_rule,
        test_camera_config_dir_digest_rule,
        test_bound_launcher_preset_pin_and_clip_seconds,
        test_plan_declaration_recording_only_all_blank_is_accepted,
        test_orchard_declaration_files_preset_and_citrus_gate,
        test_empty_background_capture_confirmation_and_start,
        test_bound_launcher_declaration_dir,
        test_request_builders_and_readiness_helpers,
        test_orange_start_carries_declaration,
        test_long_waits_poll_slowly,
        test_clean_orange_exit_during_receipt_wait_is_expected,
        test_citrus_completion_under_its_own_experiment_id,
        test_citrus_bound_start_checks_and_delivery,
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
        test_wait_clears_provisional_recording_state_after_gui_start_failure,
        test_post_terminal_citrus_exit_is_not_an_orange_wait_failure,
        test_clean_orange_exit_after_manifest_finalization_is_accepted,
        test_manifest_inferred_status_prefers_command_source,
        test_cleanup_started_processes_terminates_launched_children,
        test_orange_coverage_check_rejects_premature_recording_stop,
        test_launch_socket_preflight_refuses_live_socket,
        test_launch_socket_preflight_allows_absent_socket,
        test_headless_mode_starts_citrus_only_after_orange_is_bound,
        test_headless_mode_fails_on_missing_receipts,
        test_headless_mode_refuses_start_without_accepted_pre_arm,
        test_headless_mode_refuses_citrus_without_wait_for_control,
        test_headless_mode_needs_orange_command_and_notify_policy,
        test_headless_completion_flag_read_where_orange_client_reports_it,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("orange_citrus_orchestrator_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
