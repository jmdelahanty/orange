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


def orange_status(started: bool, stopped: bool) -> dict[str, Any]:
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
        require(payload["citrus"]["terminal_state"] == "completed", "summary should report Citrus terminal")
        require(payload["citrus"]["perf_jsonl_path"] == "/tmp/citrus_perf.jsonl", "summary should carry perf path")
        require(len(payload["validations"]) == 1, "summary should carry validation results")
        require(payload["validations"][0]["returncode"] == 0, "validation command should pass")
        require("validation-ok" in payload["validations"][0]["stdout"], "validation stdout should be captured")
        require(summary_path.exists(), "summary JSON should be written")
        require(json.loads(summary_path.read_text())["result"] == "pass", "summary file should match")


def test_persist_artifacts_copies_logs_into_recording_folder() -> None:
    module = load_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording_folder = root / "recording"
        recording_folder.mkdir()
        orange_log = root / "orange.log"
        citrus_log = root / "citrus.log"
        validation_json = root / "validation.json"
        orange_log.write_text("orange-log\n", encoding="utf-8")
        citrus_log.write_text("citrus-log\n", encoding="utf-8")
        validation_json.write_text("{\"ok\": true}\n", encoding="utf-8")
        args = module.parse_args(
            [
                "--execute",
                "--operation-id",
                "op-artifacts",
                "--orange-log",
                str(orange_log),
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
            payload["artifacts"]["validations"]["orange_validation_1"][0]["copied"],
            "artifact summary should report copied validation artifact",
        )


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
        test_persist_artifacts_copies_logs_into_recording_folder,
        test_failure_summary_uses_last_known_status_for_artifacts,
        test_wait_reports_launched_process_exit,
        test_post_terminal_citrus_exit_is_not_an_orange_wait_failure,
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
