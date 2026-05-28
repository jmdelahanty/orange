#!/usr/bin/env python3
"""Coordinate a local Orange/Citrus experiment through local-control sockets."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import socket
import subprocess
import sys
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


ORANGE_REQUEST_SCHEMA_ID = "orange.local_control.request"
CITRUS_REQUEST_SCHEMA_ID = "citrus.local_control.request"
SCHEMA_VERSION = 1
SUMMARY_SCHEMA_ID = "orange_citrus.orchestrator.summary"
SUMMARY_SCHEMA_VERSION = 1
TERMINAL_CITRUS_STATES = {"completed", "stopped", "failed", "start_rejected"}


class OrchestratorError(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def json_path(payload: dict[str, Any], keys: list[str], default: Any = None) -> Any:
    current: Any = payload
    for key in keys:
        if not isinstance(current, dict) or key not in current:
            return default
        current = current[key]
    return current


def nonnegative_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if parsed < 0.0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def positive_float(value: str) -> float:
    parsed = nonnegative_float(value)
    if parsed <= 0.0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def build_request(
    schema_id: str,
    method: str,
    request_id: str,
    *,
    operation_id: str | None = None,
    source: str = "orange_citrus_orchestrator",
    params: dict[str, Any] | None = None,
) -> dict[str, Any]:
    request: dict[str, Any] = {
        "schema_id": schema_id,
        "schema_version": SCHEMA_VERSION,
        "method": method,
        "request_id": request_id,
        "source": source,
        "sent_at_utc": utc_now(),
        "params": params or {},
    }
    if operation_id is not None:
        request["operation_id"] = operation_id
    return request


def build_status_request(schema_id: str, request_id: str, source: str) -> dict[str, Any]:
    return build_request(schema_id, "status", request_id, source=source)


def build_orange_start_request(operation_id: str, request_id: str, source: str) -> dict[str, Any]:
    return build_request(
        ORANGE_REQUEST_SCHEMA_ID,
        "start_recording",
        request_id,
        operation_id=operation_id,
        source=source,
        params={"reason": "orchestrator_start"},
    )


def build_orange_stop_request(
    operation_id: str,
    request_id: str,
    source: str,
    reason: str,
    grace_seconds: float,
) -> dict[str, Any]:
    return build_request(
        ORANGE_REQUEST_SCHEMA_ID,
        "stop_recording",
        request_id,
        operation_id=operation_id,
        source=source,
        params={"reason": reason, "grace_seconds": grace_seconds},
    )


def build_orange_citrus_completion_request(
    operation_id: str,
    request_id: str,
    source: str,
    experiment_id: str,
    terminal_state: str,
    reason: str,
    grace_seconds: float,
) -> dict[str, Any]:
    return build_request(
        ORANGE_REQUEST_SCHEMA_ID,
        "citrus_completion",
        request_id,
        operation_id=operation_id,
        source=source,
        params={
            "experiment_id": experiment_id,
            "terminal_state": terminal_state,
            "reason": reason,
            "grace_seconds": grace_seconds,
        },
    )


def build_citrus_start_request(operation_id: str, request_id: str, source: str) -> dict[str, Any]:
    return build_request(
        CITRUS_REQUEST_SCHEMA_ID,
        "start_experiment",
        request_id,
        operation_id=operation_id,
        source=source,
    )


def build_citrus_stop_request(operation_id: str, request_id: str, source: str) -> dict[str, Any]:
    return build_request(
        CITRUS_REQUEST_SCHEMA_ID,
        "stop_experiment",
        request_id,
        operation_id=operation_id,
        source=source,
    )


def send_unix_json(socket_path: str, payload: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
    rendered = json.dumps(payload, separators=(",", ":")) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout_seconds)
        client.connect(socket_path)
        client.sendall(rendered.encode("utf-8"))
        client.shutdown(socket.SHUT_WR)

        chunks: list[bytes] = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    return json.loads(b"".join(chunks).decode("utf-8"))


def response_accepted(response: dict[str, Any]) -> bool:
    return bool(response.get("ok")) and (
        bool(response.get("accepted")) or bool(response.get("duplicate"))
    )


def parse_env_items(items: list[str]) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise OrchestratorError(f"environment override must be KEY=VALUE: {item}")
        key, value = item.split("=", 1)
        if not key:
            raise OrchestratorError(f"environment override has empty key: {item}")
        parsed[key] = value
    return parsed


def orange_ready_for_recording(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["readiness", "ready_for_recording_request"], False))


def orange_ready_for_citrus(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["readiness", "ready_for_citrus_experiment"], False))


def orange_recording_finalized(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["readiness", "recording_finalized"], False))


def citrus_ready_to_start(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["readiness", "ready_to_start"], False))


def citrus_terminal_state(status: dict[str, Any]) -> str:
    value = json_path(status, ["experiment", "terminal_state"], "")
    return value if isinstance(value, str) else ""


def citrus_terminal_reason(status: dict[str, Any]) -> str:
    value = json_path(status, ["experiment", "terminal_reason"], "")
    return value if isinstance(value, str) else ""


def citrus_is_terminal(status: dict[str, Any]) -> bool:
    return citrus_terminal_state(status) in TERMINAL_CITRUS_STATES


def citrus_perf_jsonl_enabled(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["output", "perf_jsonl_enabled"], False))


def citrus_perf_jsonl_path_known(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["output", "perf_jsonl_path_known"], False))


@dataclass
class StepLog:
    name: str
    started_at_utc: str = field(default_factory=utc_now)
    finished_at_utc: str = ""
    ok: bool = False
    detail: dict[str, Any] = field(default_factory=dict)

    def finish(self, *, ok: bool = True, **detail: Any) -> None:
        self.finished_at_utc = utc_now()
        self.ok = ok
        self.detail.update(detail)


class Orchestrator:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.steps: list[StepLog] = []
        self.started_processes: list[dict[str, Any]] = []
        self.orange_recording_started = False

    def step(self, name: str) -> StepLog:
        item = StepLog(name=name)
        self.steps.append(item)
        return item

    def send(self, socket_path: str, request: dict[str, Any]) -> dict[str, Any]:
        return send_unix_json(socket_path, request, self.args.socket_timeout_seconds)

    def status(self, label: str, schema_id: str, socket_path: str) -> tuple[dict[str, Any], dict[str, Any]]:
        request_id = f"{self.args.operation_id}:{label}:status:{uuid.uuid4()}"
        response = self.send(
            socket_path,
            build_status_request(schema_id, request_id, self.args.source),
        )
        if not response.get("ok", False):
            raise OrchestratorError(f"{label} status request failed: {response}")
        status = response.get("status", {})
        if not isinstance(status, dict):
            raise OrchestratorError(f"{label} status response did not include object status")
        return response, status

    def wait_for_status(
        self,
        label: str,
        schema_id: str,
        socket_path: str,
        predicate: Callable[[dict[str, Any]], bool],
        timeout_seconds: float,
        description: str,
    ) -> dict[str, Any]:
        step = self.step(f"wait_{label}_{description}")
        deadline = time.monotonic() + timeout_seconds
        last_error = ""
        last_status: dict[str, Any] = {}
        while time.monotonic() <= deadline:
            try:
                _, status = self.status(label, schema_id, socket_path)
                last_status = status
                if predicate(status):
                    step.finish(ok=True, status=status)
                    return status
            except (OSError, TimeoutError, json.JSONDecodeError, OrchestratorError) as exc:
                last_error = str(exc)
            time.sleep(self.args.poll_interval_seconds)
        step.finish(ok=False, last_error=last_error, last_status=last_status)
        raise OrchestratorError(f"timed out waiting for {label} {description}: {last_error}")

    def start_process(
        self,
        label: str,
        command: str,
        env_overlay: dict[str, str],
        log_path: str,
    ) -> None:
        if not command:
            return
        step = self.step(f"start_{label}_process")
        argv = shlex.split(command)
        if not argv:
            step.finish(ok=False, error="empty command")
            raise OrchestratorError(f"{label} command is empty")
        env = os.environ.copy()
        env.update(env_overlay)
        log_file = None
        stdout = None
        if log_path:
            path = Path(log_path)
            path.parent.mkdir(parents=True, exist_ok=True)
            log_file = path.open("ab")
            stdout = log_file
        try:
            process = subprocess.Popen(
                argv,
                env=env,
                stdout=stdout,
                stderr=subprocess.STDOUT if stdout else None,
                start_new_session=True,
            )
        finally:
            if log_file is not None:
                log_file.close()
        self.started_processes.append(
            {"label": label, "pid": process.pid, "command": argv, "log_path": log_path}
        )
        step.finish(ok=True, pid=process.pid, command=argv, env_overlay=env_overlay)

    def run(self) -> dict[str, Any]:
        final_citrus_status: dict[str, Any] = {}
        final_orange_status: dict[str, Any] = {}

        self.start_process(
            "orange",
            self.args.orange_command,
            {
                **parse_env_items(self.args.orange_env),
                "ORANGE_GUI_LOCAL_CONTROL_SOCKET": self.args.orange_socket,
                "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START": "1",
                "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP": "1",
            },
            self.args.orange_log,
        )
        final_orange_status = self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_ready_for_recording,
            self.args.timeout_seconds,
            "ready_for_recording_request",
        )

        citrus_env = {
            **parse_env_items(self.args.citrus_env),
            "CITRUS_GUI_LOCAL_CONTROL_SOCKET": self.args.citrus_socket,
        }
        if self.args.require_citrus_perf_jsonl:
            citrus_env["CITRUS_PERF_JSONL"] = "1"
        self.start_process(
            "citrus",
            self.args.citrus_command,
            citrus_env,
            self.args.citrus_log,
        )
        final_citrus_status = self.wait_for_status(
            "citrus",
            CITRUS_REQUEST_SCHEMA_ID,
            self.args.citrus_socket,
            citrus_ready_to_start,
            self.args.timeout_seconds,
            "ready_to_start",
        )
        if self.args.require_citrus_perf_jsonl and not citrus_perf_jsonl_enabled(final_citrus_status):
            raise OrchestratorError(
                "Citrus status does not report output.perf_jsonl_enabled=true"
            )

        step = self.step("orange_start_recording")
        orange_start_response = self.send(
            self.args.orange_socket,
            build_orange_start_request(
                self.args.operation_id,
                f"{self.args.operation_id}:orange:start_recording",
                self.args.source,
            ),
        )
        if not response_accepted(orange_start_response):
            step.finish(ok=False, response=orange_start_response)
            raise OrchestratorError(f"Orange start_recording was not accepted: {orange_start_response}")
        self.orange_recording_started = True
        step.finish(ok=True, response=orange_start_response)

        final_orange_status = self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_ready_for_citrus,
            self.args.timeout_seconds,
            "ready_for_citrus_experiment",
        )

        step = self.step("citrus_start_experiment")
        citrus_start_response = self.send(
            self.args.citrus_socket,
            build_citrus_start_request(
                self.args.operation_id,
                f"{self.args.operation_id}:citrus:start_experiment",
                self.args.source,
            ),
        )
        if not response_accepted(citrus_start_response):
            step.finish(ok=False, response=citrus_start_response)
            raise OrchestratorError(f"Citrus start_experiment was not accepted: {citrus_start_response}")
        step.finish(ok=True, response=citrus_start_response)

        final_citrus_status = self.wait_for_status(
            "citrus",
            CITRUS_REQUEST_SCHEMA_ID,
            self.args.citrus_socket,
            citrus_is_terminal,
            self.args.citrus_terminal_timeout_seconds,
            "terminal_state",
        )
        if self.args.require_citrus_perf_jsonl and not citrus_perf_jsonl_path_known(final_citrus_status):
            final_citrus_status = self.wait_for_status(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                citrus_perf_jsonl_path_known,
                self.args.timeout_seconds,
                "perf_jsonl_path_known",
            )

        if self.args.stop_policy != "none":
            final_orange_status = self.request_orange_stop(final_citrus_status)
            self.orange_recording_started = False

        return self.summary(
            "pass",
            final_orange_status=final_orange_status,
            final_citrus_status=final_citrus_status,
        )

    def request_orange_stop(self, citrus_status: dict[str, Any]) -> dict[str, Any]:
        if self.args.stop_policy == "stop_recording":
            request = build_orange_stop_request(
                self.args.operation_id,
                f"{self.args.operation_id}:orange:stop_recording",
                self.args.source,
                "orchestrator_stop",
                self.args.orange_stop_grace_seconds,
            )
        elif self.args.stop_policy == "citrus_completion":
            terminal_state = citrus_terminal_state(citrus_status) or "completed"
            terminal_reason = citrus_terminal_reason(citrus_status) or "orchestrator_completion"
            request = build_orange_citrus_completion_request(
                self.args.operation_id,
                f"citrus_completion:{self.args.operation_id}:{terminal_state}:{terminal_reason}",
                self.args.source,
                self.args.operation_id,
                terminal_state,
                terminal_reason,
                self.args.orange_stop_grace_seconds,
            )
        else:
            raise OrchestratorError(f"unsupported stop policy: {self.args.stop_policy}")

        step = self.step(f"orange_{request['method']}")
        response = self.send(self.args.orange_socket, request)
        if not response_accepted(response):
            step.finish(ok=False, response=response)
            raise OrchestratorError(f"Orange {request['method']} was not accepted: {response}")
        step.finish(ok=True, response=response)

        if self.args.skip_wait_orange_finalized:
            _, status = self.status("orange", ORANGE_REQUEST_SCHEMA_ID, self.args.orange_socket)
            return status
        return self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_recording_finalized,
            self.args.orange_finalize_timeout_seconds,
            "recording_finalized",
        )

    def best_effort_stop_after_failure(self) -> dict[str, Any] | None:
        if not self.args.stop_orange_on_failure or not self.orange_recording_started:
            return None
        try:
            request = build_orange_stop_request(
                self.args.operation_id,
                f"{self.args.operation_id}:orange:stop_recording:failure",
                self.args.source,
                "orchestrator_failure",
                0.0,
            )
            return self.send(self.args.orange_socket, request)
        except Exception as exc:  # pragma: no cover - failure path detail only.
            return {"ok": False, "error": str(exc)}

    def summary(
        self,
        result: str,
        *,
        error: str = "",
        final_orange_status: dict[str, Any] | None = None,
        final_citrus_status: dict[str, Any] | None = None,
        failure_stop_response: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        citrus_status = final_citrus_status or {}
        orange_status = final_orange_status or {}
        return {
            "schema_id": SUMMARY_SCHEMA_ID,
            "schema_version": SUMMARY_SCHEMA_VERSION,
            "generated_at_utc": utc_now(),
            "result": result,
            "error": error,
            "operation_id": self.args.operation_id,
            "mode": "execute" if self.args.execute else "dry_run",
            "orange": {
                "socket": self.args.orange_socket,
                "recording_folder": json_path(orange_status, ["recording", "folder"], ""),
                "phase": orange_status.get("phase", ""),
                "ready_for_recording_request": orange_ready_for_recording(orange_status),
                "ready_for_citrus_experiment": orange_ready_for_citrus(orange_status),
                "recording_finalized": orange_recording_finalized(orange_status),
            },
            "citrus": {
                "socket": self.args.citrus_socket,
                "terminal_state": citrus_terminal_state(citrus_status),
                "terminal_reason": citrus_terminal_reason(citrus_status),
                "perf_jsonl_enabled": citrus_perf_jsonl_enabled(citrus_status),
                "perf_jsonl_path_known": citrus_perf_jsonl_path_known(citrus_status),
                "perf_jsonl_path": json_path(citrus_status, ["output", "perf_jsonl_path"], ""),
            },
            "started_processes": self.started_processes,
            "steps": [step.__dict__ for step in self.steps],
            "failure_stop_response": failure_stop_response,
        }


def default_operation_id() -> str:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"orange_citrus_{stamp}_{uuid.uuid4().hex[:8]}"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true", help="Run the orchestration. Default prints a dry-run plan.")
    parser.add_argument("--operation-id", default=default_operation_id(), help="Idempotent operation id for Orange/Citrus mutating requests.")
    parser.add_argument("--source", default="orange_citrus_orchestrator", help="Source label used in local-control requests.")
    parser.add_argument("--orange-socket", default=os.environ.get("ORANGE_GUI_LOCAL_CONTROL_SOCKET", "/tmp/orange_local_control.sock"))
    parser.add_argument("--citrus-socket", default=os.environ.get("CITRUS_GUI_LOCAL_CONTROL_SOCKET", "/tmp/citrus_local_control.sock"))
    parser.add_argument("--orange-command", default="", help="Optional Orange command to start before polling its socket.")
    parser.add_argument("--citrus-command", default="", help="Optional Citrus command to start before polling its socket.")
    parser.add_argument("--orange-env", action="append", default=[], help="Extra Orange process env override, KEY=VALUE. Repeatable.")
    parser.add_argument("--citrus-env", action="append", default=[], help="Extra Citrus process env override, KEY=VALUE. Repeatable.")
    parser.add_argument("--orange-log", default="/tmp/orange_citrus_orchestrator_orange.log")
    parser.add_argument("--citrus-log", default="/tmp/orange_citrus_orchestrator_citrus.log")
    parser.add_argument("--summary-json", default="", help="Optional path for the combined run summary JSON.")
    parser.add_argument("--poll-interval-seconds", type=positive_float, default=0.25)
    parser.add_argument("--socket-timeout-seconds", type=positive_float, default=2.0)
    parser.add_argument("--timeout-seconds", type=positive_float, default=120.0)
    parser.add_argument("--citrus-terminal-timeout-seconds", type=positive_float, default=300.0)
    parser.add_argument("--orange-finalize-timeout-seconds", type=positive_float, default=180.0)
    parser.add_argument("--orange-stop-grace-seconds", type=nonnegative_float, default=0.0)
    parser.add_argument("--require-citrus-perf-jsonl", action="store_true")
    parser.add_argument("--skip-wait-orange-finalized", action="store_true")
    parser.add_argument("--no-stop-orange-on-failure", dest="stop_orange_on_failure", action="store_false")
    parser.set_defaults(stop_orange_on_failure=True)
    parser.add_argument(
        "--stop-policy",
        choices=("stop_recording", "citrus_completion", "none"),
        default="stop_recording",
        help="How to ask Orange to stop after Citrus terminal state.",
    )
    return parser.parse_args(argv)


def dry_run_summary(args: argparse.Namespace) -> dict[str, Any]:
    return {
        "schema_id": SUMMARY_SCHEMA_ID,
        "schema_version": SUMMARY_SCHEMA_VERSION,
        "generated_at_utc": utc_now(),
        "result": "dry_run",
        "operation_id": args.operation_id,
        "mode": "dry_run",
        "execute_required": True,
        "orange": {
            "socket": args.orange_socket,
            "command": shlex.split(args.orange_command) if args.orange_command else [],
            "env_overlay": {
                **parse_env_items(args.orange_env),
                "ORANGE_GUI_LOCAL_CONTROL_SOCKET": args.orange_socket,
                "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START": "1",
                "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP": "1",
            },
            "start_request": build_orange_start_request(
                args.operation_id,
                f"{args.operation_id}:orange:start_recording",
                args.source,
            ),
            "stop_policy": args.stop_policy,
        },
        "citrus": {
            "socket": args.citrus_socket,
            "command": shlex.split(args.citrus_command) if args.citrus_command else [],
            "env_overlay": {
                **parse_env_items(args.citrus_env),
                "CITRUS_GUI_LOCAL_CONTROL_SOCKET": args.citrus_socket,
                **({"CITRUS_PERF_JSONL": "1"} if args.require_citrus_perf_jsonl else {}),
            },
            "start_request": build_citrus_start_request(
                args.operation_id,
                f"{args.operation_id}:citrus:start_experiment",
                args.source,
            ),
            "require_perf_jsonl": args.require_citrus_perf_jsonl,
        },
    }


def write_summary(path: str, summary: dict[str, Any]) -> None:
    if not path:
        return
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.execute:
        try:
            summary = dry_run_summary(args)
            write_summary(args.summary_json, summary)
            print(json.dumps(summary, indent=2, sort_keys=True))
            return 0
        except Exception as exc:
            print(f"orchestrator dry-run failed: {exc}", file=sys.stderr)
            return 2

    orchestrator = Orchestrator(args)
    try:
        summary = orchestrator.run()
        write_summary(args.summary_json, summary)
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except Exception as exc:
        failure_stop_response = orchestrator.best_effort_stop_after_failure()
        summary = orchestrator.summary(
            "fail",
            error=str(exc),
            failure_stop_response=failure_stop_response,
        )
        write_summary(args.summary_json, summary)
        print(json.dumps(summary, indent=2, sort_keys=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
