#!/usr/bin/env python3
"""Coordinate a local Orange/Citrus experiment through local-control sockets."""

from __future__ import annotations

import argparse
import errno
import json
import os
import signal
import shlex
import shutil
import socket
import subprocess
import sys
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


REPO_ROOT = Path(__file__).resolve().parents[1]
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


def socket_absent_or_stale_error(exc: OSError) -> bool:
    if isinstance(exc, (FileNotFoundError, ConnectionRefusedError)):
        return True
    return exc.errno in {
        errno.ENOENT,
        errno.ECONNREFUSED,
        errno.ENOTSOCK,
    }


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


def parse_labeled_command_items(items: list[str]) -> list[dict[str, str]]:
    parsed: list[dict[str, str]] = []
    for item in items:
        if "=" not in item:
            raise OrchestratorError(f"validation command must be LABEL=COMMAND: {item}")
        label, command = item.split("=", 1)
        if not label:
            raise OrchestratorError(f"validation command has empty label: {item}")
        if not command.strip():
            raise OrchestratorError(f"validation command has empty command: {item}")
        parsed.append({"label": label, "command": command})
    return parsed


def parse_labeled_path_items(items: list[str]) -> dict[str, list[str]]:
    parsed: dict[str, list[str]] = {}
    for item in items:
        if "=" not in item:
            raise OrchestratorError(f"validation artifact must be LABEL=PATH: {item}")
        label, path = item.split("=", 1)
        if not label:
            raise OrchestratorError(f"validation artifact has empty label: {item}")
        if not path.strip():
            raise OrchestratorError(f"validation artifact has empty path: {item}")
        parsed.setdefault(label, []).append(path)
    return parsed


def validation_commands_from_args(args: argparse.Namespace) -> list[dict[str, str]]:
    commands = parse_labeled_command_items(args.validation_command)
    for index, command in enumerate(args.orange_validation_command, start=1):
        if not command.strip():
            raise OrchestratorError("--orange-validation-command cannot be empty")
        commands.append({"label": f"orange_validation_{index}", "command": command})
    for index, command in enumerate(args.citrus_validation_command, start=1):
        if not command.strip():
            raise OrchestratorError("--citrus-validation-command cannot be empty")
        commands.append({"label": f"citrus_validation_{index}", "command": command})
    return commands


def validation_artifacts_from_args(args: argparse.Namespace) -> dict[str, list[str]]:
    return parse_labeled_path_items(args.validation_artifact)


def tail_text(value: str, max_chars: int) -> tuple[str, bool]:
    if max_chars <= 0 or len(value) <= max_chars:
        return value, False
    return value[-max_chars:], True


def safe_artifact_name(value: str) -> str:
    safe = "".join(
        char if char.isalnum() or char in "._-" else "_"
        for char in value
    ).strip("._-")
    return safe or "artifact"


def render_validation_command(
    command: str,
    *,
    operation_id: str,
    orange_status: dict[str, Any],
    citrus_status: dict[str, Any],
) -> str:
    raw_values = {
        "{operation_id}": operation_id,
        "{orange_recording_folder}": json_path(orange_status, ["recording", "folder"], ""),
        "{citrus_perf_jsonl_path}": json_path(citrus_status, ["output", "perf_jsonl_path"], ""),
    }
    for placeholder, value in raw_values.items():
        if placeholder not in command:
            continue
        if value is None or str(value) == "":
            raise OrchestratorError(
                f"validation command requires {placeholder}, but its status value is missing"
            )
    replacements = {
        placeholder: shlex.quote(str(value))
        for placeholder, value in raw_values.items()
    }
    rendered = command
    for needle, value in replacements.items():
        rendered = rendered.replace(needle, value)
    return rendered


def orange_ready_for_recording(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["readiness", "ready_for_recording_request"], False))


def orange_ready_for_citrus(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["readiness", "ready_for_citrus_experiment"], False))


def orange_recording_finalized(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["readiness", "recording_finalized"], False))


def orange_local_control_recording_stop(status: dict[str, Any]) -> dict[str, Any]:
    value = json_path(status, ["local_control", "recording_stop"], {})
    return value if isinstance(value, dict) else {}


def orange_recording_stop_drain_timed_out(status: dict[str, Any]) -> bool:
    return bool(orange_local_control_recording_stop(status).get("drain_timed_out", False))


def citrus_ready_to_start(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["readiness", "ready_to_start"], False))


def citrus_active_or_armed(status: dict[str, Any]) -> bool:
    return bool(json_path(status, ["experiment", "active"], False)) or bool(
        json_path(status, ["experiment", "armed"], False)
    )


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
        self.processes: dict[str, subprocess.Popen[Any]] = {}
        self.validation_results: list[dict[str, Any]] = []
        self.orange_recording_started = False
        self.citrus_control_complete = False
        self.last_orange_status: dict[str, Any] = {}
        self.last_citrus_status: dict[str, Any] = {}

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
        if label == "orange":
            self.last_orange_status = status
        elif label == "citrus":
            self.last_citrus_status = status
        return response, status

    def preflight_launch_socket(
        self,
        label: str,
        schema_id: str,
        socket_path: str,
        allow_preexisting: bool,
    ) -> None:
        if allow_preexisting or not socket_path:
            return
        step = self.step(f"preflight_{label}_launch_socket")
        request_id = f"{self.args.operation_id}:{label}:prelaunch_status:{uuid.uuid4()}"
        try:
            response = send_unix_json(
                socket_path,
                build_status_request(schema_id, request_id, self.args.source),
                self.args.launch_socket_preflight_timeout_seconds,
            )
        except OSError as exc:
            if socket_absent_or_stale_error(exc):
                step.finish(
                    ok=True,
                    socket_path=socket_path,
                    existing_server=False,
                    error=str(exc),
                )
                return
            step.finish(ok=False, socket_path=socket_path, existing_server=True, error=str(exc))
            raise OrchestratorError(
                f"{label} local-control socket {socket_path} exists before launch "
                f"but could not be preflighted safely: {exc}"
            ) from exc
        except (json.JSONDecodeError, TimeoutError) as exc:
            step.finish(ok=False, socket_path=socket_path, existing_server=True, error=str(exc))
            raise OrchestratorError(
                f"{label} local-control socket {socket_path} answered before launch "
                f"but did not return a clean status response; refusing to launch over it"
            ) from exc

        step.finish(ok=False, socket_path=socket_path, existing_server=True, response=response)
        raise OrchestratorError(
            f"{label} local-control socket {socket_path} is already answering before launch; "
            f"use attach mode or --allow-preexisting-{label}-socket if this is intentional"
        )

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
            self.raise_if_started_process_exited(f"waiting for {label} {description}")
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

    def wait_for_citrus_terminal_or_run_duration(self) -> dict[str, Any]:
        if self.args.citrus_run_seconds <= 0.0:
            return self.wait_for_status(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                citrus_is_terminal,
                self.args.citrus_terminal_timeout_seconds,
                "terminal_state",
            )

        step = self.step("wait_citrus_terminal_or_run_duration")
        deadline = time.monotonic() + self.args.citrus_terminal_timeout_seconds
        active_since: float | None = None
        last_error = ""
        last_status: dict[str, Any] = {}
        while time.monotonic() <= deadline:
            self.raise_if_started_process_exited("waiting for Citrus terminal state or run duration")
            try:
                _, status = self.status("citrus", CITRUS_REQUEST_SCHEMA_ID, self.args.citrus_socket)
                last_status = status
                if citrus_is_terminal(status):
                    step.finish(ok=True, outcome="terminal", status=status)
                    return status
                now = time.monotonic()
                if citrus_active_or_armed(status):
                    if active_since is None:
                        active_since = now
                    active_elapsed_s = now - active_since
                    if active_elapsed_s >= self.args.citrus_run_seconds:
                        step.finish(
                            ok=True,
                            outcome="run_duration_elapsed",
                            run_seconds=self.args.citrus_run_seconds,
                            active_elapsed_s=active_elapsed_s,
                            status=status,
                        )
                        return status
            except (OSError, TimeoutError, json.JSONDecodeError, OrchestratorError) as exc:
                last_error = str(exc)
            time.sleep(self.args.poll_interval_seconds)
        step.finish(ok=False, last_error=last_error, last_status=last_status)
        raise OrchestratorError(
            "timed out waiting for Citrus terminal state or "
            f"{self.args.citrus_run_seconds}s active run duration: {last_error}"
        )

    def request_citrus_stop_for_run_duration(self) -> dict[str, Any]:
        step = self.step("citrus_stop_experiment_run_duration")
        response = self.send(
            self.args.citrus_socket,
            build_citrus_stop_request(
                self.args.operation_id,
                f"{self.args.operation_id}:citrus:stop_experiment:run_duration",
                self.args.source,
            ),
        )
        if not response_accepted(response):
            step.finish(ok=False, response=response)
            raise OrchestratorError(f"Citrus stop_experiment was not accepted: {response}")
        step.finish(ok=True, response=response)
        return response

    def refresh_started_processes(self) -> None:
        for info in self.started_processes:
            label = str(info.get("label", ""))
            process = self.processes.get(label)
            if process is not None:
                info["returncode"] = process.poll()

    def raise_if_started_process_exited(self, context: str) -> None:
        self.refresh_started_processes()
        for info in self.started_processes:
            returncode = info.get("returncode")
            if returncode is None:
                continue
            label = info.get("label", "<unknown>")
            if label == "citrus" and self.citrus_control_complete:
                continue
            pid = info.get("pid", "<unknown>")
            log_path = info.get("log_path", "")
            raise OrchestratorError(
                f"{label} process exited while {context}: "
                f"pid={pid} returncode={returncode} log_path={log_path}"
            )

    def cleanup_started_processes(
        self,
        *,
        labels: set[str] | None = None,
        terminate_timeout_seconds: float = 5.0,
    ) -> None:
        self.refresh_started_processes()
        for info in self.started_processes:
            label = str(info.get("label", ""))
            if labels is not None and label not in labels:
                continue
            process = self.processes.get(label)
            if process is None or process.poll() is not None:
                if process is not None:
                    info["returncode"] = process.returncode
                continue

            step = self.step(f"cleanup_{label}_process")
            pid = process.pid
            action = "none"
            try:
                try:
                    os.killpg(pid, signal.SIGTERM)
                    action = "terminate_process_group"
                except ProcessLookupError:
                    action = "already_exited"
                except OSError:
                    process.terminate()
                    action = "terminate_process"

                try:
                    process.wait(timeout=terminate_timeout_seconds)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(pid, signal.SIGKILL)
                        action = f"{action}_then_kill_process_group"
                    except ProcessLookupError:
                        action = f"{action}_then_already_exited"
                    except OSError:
                        process.kill()
                        action = f"{action}_then_kill_process"
                    process.wait(timeout=terminate_timeout_seconds)
                info["returncode"] = process.returncode
                info["cleanup_action"] = action
                step.finish(ok=True, pid=pid, action=action, returncode=process.returncode)
            except Exception as exc:  # pragma: no cover - best-effort process cleanup.
                info["cleanup_action"] = action
                info["cleanup_error"] = str(exc)
                info["returncode"] = process.poll()
                step.finish(ok=False, pid=pid, action=action, error=str(exc))

    def cleanup_launched_socket_files(self) -> None:
        launched_labels = {
            str(info.get("label", ""))
            for info in self.started_processes
            if (
                self.processes.get(str(info.get("label", ""))) is not None
                and self.processes[str(info.get("label", ""))].poll() is not None
            )
        }
        socket_by_label = {
            "orange": self.args.orange_socket,
            "citrus": self.args.citrus_socket,
        }
        for label in sorted(launched_labels):
            socket_path = socket_by_label.get(label, "")
            if not socket_path:
                continue
            step = self.step(f"cleanup_{label}_socket")
            path = Path(socket_path)
            try:
                if not path.exists():
                    step.finish(ok=True, socket_path=socket_path, removed=False, reason="absent")
                    continue
                path.unlink()
                step.finish(ok=True, socket_path=socket_path, removed=True)
            except Exception as exc:  # pragma: no cover - filesystem permissions detail only.
                step.finish(ok=False, socket_path=socket_path, removed=False, error=str(exc))

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
            log_file = path.open("wb")
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
            {
                "label": label,
                "pid": process.pid,
                "command": argv,
                "log_path": log_path,
                "returncode": None,
            }
        )
        self.processes[label] = process
        step.finish(ok=True, pid=process.pid, command=argv, env_overlay=env_overlay)

    def run(self) -> dict[str, Any]:
        final_citrus_status: dict[str, Any] = {}
        final_orange_status: dict[str, Any] = {}

        if self.args.orange_command:
            self.preflight_launch_socket(
                "orange",
                ORANGE_REQUEST_SCHEMA_ID,
                self.args.orange_socket,
                self.args.allow_preexisting_orange_socket,
            )
        if self.args.citrus_command:
            self.preflight_launch_socket(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                self.args.allow_preexisting_citrus_socket,
            )
        self.start_process(
            "orange",
            self.args.orange_command,
            {
                **parse_env_items(self.args.orange_env),
                "ORANGE_GUI_LOCAL_CONTROL_SOCKET": self.args.orange_socket,
                "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START": "1",
                "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP": "1",
                "ORANGE_GUI_AUTORUN_START_RECORDING": "0",
                "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE": "0",
                "ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE": "1",
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

        final_citrus_status = self.wait_for_citrus_terminal_or_run_duration()
        if not citrus_is_terminal(final_citrus_status):
            self.request_citrus_stop_for_run_duration()
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
        self.citrus_control_complete = True

        if self.args.stop_policy != "none":
            final_orange_status = self.request_orange_stop(final_citrus_status)
            self.orange_recording_started = False

        self.validation_results = self.run_validations(
            final_orange_status,
            final_citrus_status,
        )
        self.cleanup_started_processes()
        self.cleanup_launched_socket_files()

        return self.summary(
            "pass",
            final_orange_status=final_orange_status,
            final_citrus_status=final_citrus_status,
        )

    def run_validations(
        self,
        orange_status: dict[str, Any],
        citrus_status: dict[str, Any],
    ) -> list[dict[str, Any]]:
        commands = validation_commands_from_args(self.args)
        validation_artifacts = validation_artifacts_from_args(self.args)
        if not commands:
            return []

        results: list[dict[str, Any]] = []
        env = os.environ.copy()
        env.update(parse_env_items(self.args.validation_env))
        cwd = self.args.validation_cwd or str(REPO_ROOT)

        for item in commands:
            label = item["label"]
            rendered_command = render_validation_command(
                item["command"],
                operation_id=self.args.operation_id,
                orange_status=orange_status,
                citrus_status=citrus_status,
            )
            argv = shlex.split(rendered_command)
            step = self.step(f"validation_{label}")
            started = time.monotonic()
            result: dict[str, Any] = {
                "label": label,
                "command": argv,
                "command_text": rendered_command,
                "cwd": cwd,
                "returncode": None,
                "timed_out": False,
                "duration_seconds": None,
                "stdout": "",
                "stderr": "",
                "stdout_truncated": False,
                "stderr_truncated": False,
                "artifact_paths": validation_artifacts.get(label, []),
            }
            try:
                completed = subprocess.run(
                    argv,
                    cwd=cwd,
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=self.args.validation_timeout_seconds,
                    check=False,
                    errors="replace",
                )
                duration = time.monotonic() - started
                stdout, stdout_truncated = tail_text(
                    completed.stdout,
                    self.args.validation_output_max_chars,
                )
                stderr, stderr_truncated = tail_text(
                    completed.stderr,
                    self.args.validation_output_max_chars,
                )
                result.update(
                    {
                        "returncode": completed.returncode,
                        "duration_seconds": duration,
                        "stdout": stdout,
                        "stderr": stderr,
                        "stdout_truncated": stdout_truncated,
                        "stderr_truncated": stderr_truncated,
                    }
                )
                step.finish(ok=completed.returncode == 0, result=result)
                results.append(result)
                if completed.returncode != 0:
                    self.validation_results = results
                    raise OrchestratorError(
                        f"validation command {label} failed with exit code {completed.returncode}"
                    )
            except subprocess.TimeoutExpired as exc:
                duration = time.monotonic() - started
                stdout_raw = exc.stdout or ""
                stderr_raw = exc.stderr or ""
                if isinstance(stdout_raw, bytes):
                    stdout_raw = stdout_raw.decode("utf-8", errors="replace")
                if isinstance(stderr_raw, bytes):
                    stderr_raw = stderr_raw.decode("utf-8", errors="replace")
                stdout, stdout_truncated = tail_text(
                    stdout_raw,
                    self.args.validation_output_max_chars,
                )
                stderr, stderr_truncated = tail_text(
                    stderr_raw,
                    self.args.validation_output_max_chars,
                )
                result.update(
                    {
                        "timed_out": True,
                        "duration_seconds": duration,
                        "stdout": stdout,
                        "stderr": stderr,
                        "stdout_truncated": stdout_truncated,
                        "stderr_truncated": stderr_truncated,
                    }
                )
                step.finish(ok=False, result=result)
                results.append(result)
                self.validation_results = results
                raise OrchestratorError(
                    f"validation command {label} timed out after "
                    f"{self.args.validation_timeout_seconds}s"
                ) from exc
        return results

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
            self.require_orange_drain_not_timed_out(status)
            return status
        status = self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_recording_finalized,
            self.args.orange_finalize_timeout_seconds,
            "recording_finalized",
        )
        self.require_orange_drain_not_timed_out(status)
        return status

    def require_orange_drain_not_timed_out(self, status: dict[str, Any]) -> None:
        if self.args.allow_orange_drain_timeout or not orange_recording_stop_drain_timed_out(status):
            return
        stop_status = orange_local_control_recording_stop(status)
        raise OrchestratorError(
            "Orange recording drain timed out before finalization: "
            f"request_id={stop_status.get('request_id', '')} "
            f"operation_id={stop_status.get('operation_id', '')} "
            f"elapsed_seconds={stop_status.get('drain_elapsed_seconds', '')} "
            f"timeout_seconds={stop_status.get('drain_timeout_seconds', '')} "
            f"last_event={stop_status.get('last_event', '')}"
        )

    def persist_artifacts(self, summary: dict[str, Any]) -> None:
        artifacts: dict[str, Any] = {
            "copy_to_recording_enabled": bool(self.args.copy_artifacts_to_recording),
            "recording_folder": json_path(summary, ["orange", "recording_folder"], ""),
            "artifact_dir": "",
            "logs": {},
            "validations": {},
        }
        summary["artifacts"] = artifacts
        if not self.args.copy_artifacts_to_recording:
            artifacts["reason"] = "disabled"
            return

        recording_folder = str(artifacts["recording_folder"] or "")
        if not recording_folder:
            artifacts["reason"] = "missing_orange_recording_folder"
            return

        artifact_dir = Path(recording_folder) / self.args.artifact_dir_name
        artifact_dir.mkdir(parents=True, exist_ok=True)
        artifacts["artifact_dir"] = str(artifact_dir)
        artifacts["logs"]["orange"] = self.copy_artifact_file(
            self.args.orange_log,
            artifact_dir / "orange.log",
        )
        artifacts["logs"]["citrus"] = self.copy_artifact_file(
            self.args.citrus_log,
            artifact_dir / "citrus.log",
        )
        for validation in summary.get("validations", []):
            if not isinstance(validation, dict):
                continue
            label = str(validation.get("label") or "validation")
            safe_label = safe_artifact_name(label)
            copied_items: list[dict[str, Any]] = []
            for path in validation.get("artifact_paths", []):
                if not isinstance(path, str):
                    continue
                source_path = Path(path)
                target_name = f"{safe_label}_{source_path.name}" if source_path.name else safe_label
                copied_items.append(
                    self.copy_artifact_file(
                        str(source_path),
                        artifact_dir / target_name,
                    )
                )
            if copied_items:
                artifacts["validations"][label] = copied_items
        artifact_summary = artifact_dir / "orchestrator_summary.json"
        artifacts["summary_json_artifact"] = str(artifact_summary)
        artifact_summary.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    @staticmethod
    def copy_artifact_file(source: str, target: Path) -> dict[str, Any]:
        if not source:
            return {"source": source, "path": str(target), "copied": False, "reason": "empty_source"}
        source_path = Path(source)
        result: dict[str, Any] = {
            "source": str(source_path),
            "path": str(target),
            "copied": False,
        }
        if not source_path.exists():
            result["reason"] = "missing_source"
            return result
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            if source_path.resolve() == target.resolve():
                result["reason"] = "source_is_target"
            else:
                shutil.copy2(source_path, target)
                result["copied"] = True
        except Exception as exc:  # pragma: no cover - filesystem detail only.
            result["reason"] = "copy_failed"
            result["error"] = str(exc)
            return result
        if target.exists():
            result["bytes"] = target.stat().st_size
        return result

    def best_effort_stop_after_failure(self) -> dict[str, Any] | None:
        if not self.args.stop_orange_on_failure or not self.orange_recording_started:
            return None
        if orange_recording_finalized(self.last_orange_status):
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
        citrus_status = final_citrus_status or self.last_citrus_status or {}
        orange_status = final_orange_status or self.last_orange_status or {}
        self.refresh_started_processes()
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
                "log_path": self.args.orange_log,
                "recording_folder": json_path(orange_status, ["recording", "folder"], ""),
                "phase": orange_status.get("phase", ""),
                "ready_for_recording_request": orange_ready_for_recording(orange_status),
                "ready_for_citrus_experiment": orange_ready_for_citrus(orange_status),
                "recording_finalized": orange_recording_finalized(orange_status),
                "local_control_recording_stop": orange_local_control_recording_stop(orange_status),
                "local_control_stop_drain_timed_out": orange_recording_stop_drain_timed_out(orange_status),
                "allow_drain_timeout": self.args.allow_orange_drain_timeout,
            },
            "citrus": {
                "socket": self.args.citrus_socket,
                "log_path": self.args.citrus_log,
                "run_seconds": self.args.citrus_run_seconds,
                "terminal_state": citrus_terminal_state(citrus_status),
                "terminal_reason": citrus_terminal_reason(citrus_status),
                "perf_jsonl_enabled": citrus_perf_jsonl_enabled(citrus_status),
                "perf_jsonl_path_known": citrus_perf_jsonl_path_known(citrus_status),
                "perf_jsonl_path": json_path(citrus_status, ["output", "perf_jsonl_path"], ""),
            },
            "validations": self.validation_results,
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
    parser.add_argument(
        "--artifact-dir-name",
        default="orchestrator",
        help="Subdirectory name under the Orange recording folder for orchestrator logs/summaries.",
    )
    parser.add_argument(
        "--no-copy-artifacts-to-recording",
        dest="copy_artifacts_to_recording",
        action="store_false",
        help="Do not copy orchestrator process logs and summary into the Orange recording folder.",
    )
    parser.set_defaults(copy_artifacts_to_recording=True)
    parser.add_argument("--poll-interval-seconds", type=positive_float, default=0.25)
    parser.add_argument("--socket-timeout-seconds", type=positive_float, default=2.0)
    parser.add_argument("--launch-socket-preflight-timeout-seconds", type=positive_float, default=0.2)
    parser.add_argument("--timeout-seconds", type=positive_float, default=120.0)
    parser.add_argument("--citrus-terminal-timeout-seconds", type=positive_float, default=300.0)
    parser.add_argument(
        "--citrus-run-seconds",
        type=nonnegative_float,
        default=0.0,
        help=(
            "If positive, stop Citrus after this many seconds of active/armed "
            "experiment time, then wait for terminal state."
        ),
    )
    parser.add_argument("--orange-finalize-timeout-seconds", type=positive_float, default=180.0)
    parser.add_argument("--orange-stop-grace-seconds", type=nonnegative_float, default=0.0)
    parser.add_argument(
        "--allow-orange-drain-timeout",
        action="store_true",
        help=(
            "Do not fail the orchestrator if Orange local-control status reports "
            "recording_stop.drain_timed_out=true."
        ),
    )
    parser.add_argument("--require-citrus-perf-jsonl", action="store_true")
    parser.add_argument(
        "--validation-command",
        action="append",
        default=[],
        help="Post-run validation command as LABEL=COMMAND. Repeatable. Non-zero exits fail the orchestrator.",
    )
    parser.add_argument(
        "--validation-artifact",
        action="append",
        default=[],
        help="Validation output artifact as LABEL=PATH. Repeatable. Copied into the Orange recording folder.",
    )
    parser.add_argument(
        "--orange-validation-command",
        action="append",
        default=[],
        help="Post-run Orange validation command. Repeatable. Label is orange_validation_N.",
    )
    parser.add_argument(
        "--citrus-validation-command",
        action="append",
        default=[],
        help="Post-run Citrus validation command. Repeatable. Label is citrus_validation_N.",
    )
    parser.add_argument(
        "--validation-env",
        action="append",
        default=[],
        help="Extra env override for validation commands, KEY=VALUE. Repeatable.",
    )
    parser.add_argument("--validation-cwd", default=str(REPO_ROOT))
    parser.add_argument("--validation-timeout-seconds", type=positive_float, default=120.0)
    parser.add_argument("--validation-output-max-chars", type=int, default=20000)
    parser.add_argument("--skip-wait-orange-finalized", action="store_true")
    parser.add_argument("--allow-preexisting-orange-socket", action="store_true")
    parser.add_argument("--allow-preexisting-citrus-socket", action="store_true")
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
            "log_path": args.orange_log,
            "command": shlex.split(args.orange_command) if args.orange_command else [],
            "env_overlay": {
                **parse_env_items(args.orange_env),
                "ORANGE_GUI_LOCAL_CONTROL_SOCKET": args.orange_socket,
                "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START": "1",
                "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP": "1",
                "ORANGE_GUI_AUTORUN_START_RECORDING": "0",
                "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE": "0",
                "ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE": "1",
            },
            "start_request": build_orange_start_request(
                args.operation_id,
                f"{args.operation_id}:orange:start_recording",
                args.source,
            ),
            "stop_policy": args.stop_policy,
            "allow_drain_timeout": args.allow_orange_drain_timeout,
            "preflight_existing_socket": bool(args.orange_command)
            and not args.allow_preexisting_orange_socket,
        },
        "citrus": {
            "socket": args.citrus_socket,
            "log_path": args.citrus_log,
            "command": shlex.split(args.citrus_command) if args.citrus_command else [],
            "run_seconds": args.citrus_run_seconds,
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
            "stop_request": build_citrus_stop_request(
                args.operation_id,
                f"{args.operation_id}:citrus:stop_experiment:run_duration",
                args.source,
            ) if args.citrus_run_seconds > 0.0 else None,
            "require_perf_jsonl": args.require_citrus_perf_jsonl,
            "preflight_existing_socket": bool(args.citrus_command)
            and not args.allow_preexisting_citrus_socket,
        },
        "validations": [
            {
                **item,
                "cwd": args.validation_cwd,
                "timeout_seconds": args.validation_timeout_seconds,
                "artifact_paths": validation_artifacts_from_args(args).get(item["label"], []),
            }
            for item in validation_commands_from_args(args)
        ],
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
        orchestrator.persist_artifacts(summary)
        write_summary(args.summary_json, summary)
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except Exception as exc:
        failure_stop_response = orchestrator.best_effort_stop_after_failure()
        cleanup_labels = {"citrus"}
        if (
            not orchestrator.orange_recording_started
            or orange_recording_finalized(orchestrator.last_orange_status)
        ):
            cleanup_labels.add("orange")
        orchestrator.cleanup_started_processes(labels=cleanup_labels)
        orchestrator.cleanup_launched_socket_files()
        summary = orchestrator.summary(
            "fail",
            error=str(exc),
            failure_stop_response=failure_stop_response,
        )
        orchestrator.persist_artifacts(summary)
        write_summary(args.summary_json, summary)
        print(json.dumps(summary, indent=2, sort_keys=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
