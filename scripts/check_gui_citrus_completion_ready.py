#!/usr/bin/env python3
"""Preflight a manual Orange GUI session for Citrus completion-stop control."""

from __future__ import annotations

import argparse
import json
import os
import pwd
import socket
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


def default_orange_root() -> Path:
    sudo_user = os.environ.get("SUDO_USER", "")
    if sudo_user:
        try:
            home = pwd.getpwnam(sudo_user).pw_dir
        except KeyError:
            home = ""
        if home:
            return Path(home) / "orange_data"
    home_env = os.environ.get("HOME", "")
    if home_env:
        return Path(home_env) / "orange_data"
    return Path.home() / "orange_data"


def default_config_path() -> Path:
    for name in ("ORANGE_APP_CONFIG_PATH", "ORANGE_GUI_APP_CONFIG_PATH"):
        value = os.environ.get(name, "").strip()
        if value:
            return Path(value)
    return default_orange_root() / "config" / "app" / "default.json"


def default_socket_path() -> str:
    for name in ("ORANGE_GUI_LOCAL_CONTROL_SOCKET", "ORANGE_LOCAL_CONTROL_SOCKET"):
        value = os.environ.get(name, "").strip()
        if value:
            return value
    return "/tmp/orange_local_control.sock"


def utc_now() -> str:
    return (
        datetime.now(timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        type=Path,
        default=default_config_path(),
        help="Orange app config path. Default: %(default)s",
    )
    parser.add_argument(
        "--socket",
        default=default_socket_path(),
        help="Orange local-control socket path. Default: %(default)s",
    )
    socket_group = parser.add_mutually_exclusive_group()
    socket_group.add_argument(
        "--check-socket",
        action="store_true",
        help="If the socket exists, query status and validate the live GUI gates.",
    )
    socket_group.add_argument(
        "--require-live-socket",
        action="store_true",
        help="Require a live socket and validate the running GUI gates.",
    )
    socket_group.add_argument(
        "--require-manual-citrus-ready",
        action="store_true",
        help=(
            "Require the full manual Orange+Citrus readiness profile: live "
            "socket, active recording, Citrus experiment readiness, and "
            "non-empty recording folder."
        ),
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=2.0,
        help="Socket connect/read timeout in seconds. Default: %(default)s",
    )
    parser.add_argument(
        "--wait-seconds",
        type=float,
        default=0.0,
        help=(
            "Poll until the requested checks pass or this many seconds elapse. "
            "Useful with --require-manual-citrus-ready while the operator starts "
            "recording in the GUI."
        ),
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=0.5,
        help="Polling interval for --wait-seconds. Default: %(default)s",
    )
    parser.add_argument(
        "--require-recording-active",
        action="store_true",
        help="Fail unless live status reports readiness.recording_active=true.",
    )
    parser.add_argument(
        "--require-ready-for-citrus-experiment",
        action="store_true",
        help=(
            "Fail unless live status reports "
            "readiness.ready_for_citrus_experiment=true."
        ),
    )
    parser.add_argument(
        "--require-recording-folder",
        action="store_true",
        help="Fail unless live status reports a non-empty recording.folder path.",
    )
    parser.add_argument(
        "--write-handoff",
        type=Path,
        help=(
            "Write a machine-readable JSON handoff after successful manual "
            "Orange+Citrus readiness. Implies the full manual Citrus readiness "
            "requirements."
        ),
    )
    output_group = parser.add_mutually_exclusive_group()
    output_group.add_argument(
        "--json",
        action="store_true",
        help="Print machine-readable preflight JSON.",
    )
    output_group.add_argument(
        "--print-recording-folder",
        action="store_true",
        help=(
            "After successful live checks, print only status.recording.folder. "
            "Implies a live socket check and a non-empty recording folder."
        ),
    )
    args = parser.parse_args()
    args.config = args.config.expanduser()
    if args.write_handoff is not None:
        args.write_handoff = args.write_handoff.expanduser()
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    if args.wait_seconds < 0:
        parser.error("--wait-seconds must be >= 0")
    if args.poll_interval <= 0:
        parser.error("--poll-interval must be > 0")
    return args


def read_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"missing app config: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"failed to parse app config {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise ValueError(f"app config root must be a JSON object: {path}")
    return payload


def nested_dict(payload: dict[str, Any], *keys: str) -> dict[str, Any]:
    current: Any = payload
    for key in keys:
        if not isinstance(current, dict):
            return {}
        current = current.get(key)
    return current if isinstance(current, dict) else {}


def check_expected_bool(
    payload: dict[str, Any],
    key: str,
    expected: bool,
    failures: list[str],
    passes: list[str],
    prefix: str,
) -> None:
    actual = payload.get(key)
    if actual is expected:
        passes.append(f"{prefix}.{key}={str(expected).lower()}")
    else:
        failures.append(
            f"{prefix}.{key} expected {str(expected).lower()}, got {actual!r}"
        )


def check_expected_number(
    payload: dict[str, Any],
    key: str,
    minimum: float,
    failures: list[str],
    passes: list[str],
    prefix: str,
) -> None:
    actual = payload.get(key)
    try:
        value = float(actual)
    except (TypeError, ValueError):
        failures.append(f"{prefix}.{key} expected number >= {minimum}, got {actual!r}")
        return
    if value >= minimum:
        passes.append(f"{prefix}.{key}>={minimum:g}")
    else:
        failures.append(f"{prefix}.{key} expected >= {minimum}, got {actual!r}")


def check_app_config(payload: dict[str, Any]) -> tuple[list[str], list[str]]:
    failures: list[str] = []
    passes: list[str] = []
    local_control = nested_dict(payload, "gui", "local_control")
    check_expected_bool(
        local_control,
        "recording_start_enabled",
        False,
        failures,
        passes,
        "app_config.gui.local_control",
    )
    check_expected_bool(
        local_control,
        "recording_stop_enabled",
        False,
        failures,
        passes,
        "app_config.gui.local_control",
    )
    check_expected_bool(
        local_control,
        "citrus_completion_stop_enabled",
        True,
        failures,
        passes,
        "app_config.gui.local_control",
    )
    check_expected_bool(
        local_control,
        "exit_after_finalize",
        False,
        failures,
        passes,
        "app_config.gui.local_control",
    )
    check_expected_number(
        local_control,
        "drain_timeout_seconds",
        1.0,
        failures,
        passes,
        "app_config.gui.local_control",
    )
    return failures, passes


def status_request() -> dict[str, Any]:
    return {
        "schema_id": "orange.local_control.request",
        "schema_version": 1,
        "method": "status",
        "request_id": str(uuid.uuid4()),
        "source": "orange_citrus_completion_ready_check",
        "sent_at_utc": utc_now(),
        "params": {},
    }


def send_status(socket_path: str, timeout: float) -> dict[str, Any]:
    rendered = json.dumps(status_request(), separators=(",", ":")) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(socket_path)
        client.sendall(rendered.encode("utf-8"))
        client.shutdown(socket.SHUT_WR)
        chunks: list[bytes] = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    response = json.loads(b"".join(chunks).decode("utf-8"))
    if not isinstance(response, dict):
        raise ValueError("Orange status response was not a JSON object")
    return response


def recording_folder_from_response(response: dict[str, Any]) -> str:
    status = response.get("status")
    if not isinstance(status, dict):
        return ""
    recording = nested_dict(status, "recording")
    folder = recording.get("folder")
    return folder if isinstance(folder, str) else ""


def check_socket_status(
    response: dict[str, Any],
    *,
    require_recording_active: bool = False,
    require_ready_for_citrus_experiment: bool = False,
    require_recording_folder: bool = False,
) -> tuple[list[str], list[str]]:
    failures: list[str] = []
    passes: list[str] = []
    if response.get("ok") is not True:
        failures.append(f"socket status ok expected true, got {response.get('ok')!r}")
        return failures, passes
    status = response.get("status")
    if not isinstance(status, dict):
        failures.append("socket response missing status object")
        return failures, passes
    local_control = nested_dict(status, "local_control")
    recording_start = nested_dict(local_control, "recording_start")
    recording_stop = nested_dict(local_control, "recording_stop")
    citrus_stop = nested_dict(local_control, "citrus_completion_stop")
    check_expected_bool(
        recording_start,
        "enabled",
        False,
        failures,
        passes,
        "socket.status.local_control.recording_start",
    )
    check_expected_bool(
        recording_stop,
        "enabled",
        False,
        failures,
        passes,
        "socket.status.local_control.recording_stop",
    )
    check_expected_bool(
        citrus_stop,
        "enabled",
        True,
        failures,
        passes,
        "socket.status.local_control.citrus_completion_stop",
    )
    readiness = nested_dict(status, "readiness")
    passes.append(
        "socket.status.readiness.recording_active="
        f"{str(readiness.get('recording_active')).lower()}"
    )
    passes.append(
        "socket.status.readiness.ready_for_citrus_experiment="
        f"{str(readiness.get('ready_for_citrus_experiment')).lower()}"
    )
    if require_recording_active:
        check_expected_bool(
            readiness,
            "recording_active",
            True,
            failures,
            passes,
            "socket.status.readiness",
        )
    if require_ready_for_citrus_experiment:
        check_expected_bool(
            readiness,
            "ready_for_citrus_experiment",
            True,
            failures,
            passes,
            "socket.status.readiness",
        )
    recording = nested_dict(status, "recording")
    recording_folder = recording.get("folder")
    if isinstance(recording_folder, str) and recording_folder:
        passes.append(f"socket.status.recording.folder={recording_folder}")
    elif require_recording_folder:
        failures.append(
            "socket.status.recording.folder expected non-empty string, "
            f"got {recording_folder!r}"
        )
    passes.append(f"socket.status.phase={status.get('phase', '<missing>')}")
    return failures, passes


def effective_socket_requirements(args: argparse.Namespace) -> tuple[bool, bool, bool, bool]:
    require_manual_ready = args.require_manual_citrus_ready or args.write_handoff is not None
    require_live_socket = args.require_live_socket or require_manual_ready
    require_recording_active = (
        args.require_recording_active
        or require_manual_ready
    )
    require_ready_for_citrus_experiment = (
        args.require_ready_for_citrus_experiment
        or require_manual_ready
    )
    require_recording_folder = (
        args.require_recording_folder
        or require_manual_ready
        or args.print_recording_folder
    )
    return (
        require_live_socket,
        require_recording_active,
        require_ready_for_citrus_experiment,
        require_recording_folder,
    )


def evaluate_preflight(
    args: argparse.Namespace,
    *,
    send_status_fn: Callable[[str, float], dict[str, Any]] = send_status,
    socket_exists_fn: Callable[[str], bool] | None = None,
) -> dict[str, Any]:
    socket_present = (
        socket_exists_fn(args.socket)
        if socket_exists_fn is not None
        else Path(args.socket).exists()
    )
    result: dict[str, Any] = {
        "ok": False,
        "config": str(args.config.resolve()),
        "socket": args.socket,
        "recording_folder": "",
        "passes": [],
        "failures": [],
        "socket_checked": False,
        "socket_present": socket_present,
    }
    (
        require_live_socket,
        require_recording_active,
        require_ready_for_citrus_experiment,
        require_recording_folder,
    ) = effective_socket_requirements(args)

    try:
        app_config = read_json(args.config)
        failures, passes = check_app_config(app_config)
        result["failures"].extend(failures)
        result["passes"].extend(passes)
    except ValueError as exc:
        result["failures"].append(str(exc))

    should_check_socket = (
        require_live_socket
        or args.print_recording_folder
        or (args.check_socket and (result["socket_present"] or args.wait_seconds > 0))
    )
    require_socket_present = (
        require_live_socket
        or args.print_recording_folder
        or (args.check_socket and args.wait_seconds > 0)
    )
    if require_socket_present and not result["socket_present"]:
        result["failures"].append(f"required live socket missing: {args.socket}")
    elif should_check_socket:
        result["socket_checked"] = True
        try:
            response = send_status_fn(args.socket, args.timeout)
            result["status_response"] = response
            result["recording_folder"] = recording_folder_from_response(response)
            failures, passes = check_socket_status(
                response,
                require_recording_active=require_recording_active,
                require_ready_for_citrus_experiment=require_ready_for_citrus_experiment,
                require_recording_folder=require_recording_folder,
            )
            result["failures"].extend(failures)
            result["passes"].extend(passes)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            result["failures"].append(f"failed to query socket {args.socket}: {exc}")

    result["ok"] = not result["failures"]
    return result


def evaluate_preflight_until_ready(
    args: argparse.Namespace,
    *,
    send_status_fn: Callable[[str, float], dict[str, Any]] = send_status,
    socket_exists_fn: Callable[[str], bool] | None = None,
    sleep_fn: Callable[[float], None] = time.sleep,
    monotonic_fn: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    deadline = monotonic_fn() + args.wait_seconds
    attempts = 0
    while True:
        attempts += 1
        result = evaluate_preflight(
            args,
            send_status_fn=send_status_fn,
            socket_exists_fn=socket_exists_fn,
        )
        result["attempts"] = attempts
        result["wait_seconds"] = args.wait_seconds
        now = monotonic_fn()
        if result["ok"] or args.wait_seconds <= 0 or now >= deadline:
            result["wait_timed_out"] = bool(args.wait_seconds > 0 and not result["ok"])
            return result
        sleep_fn(min(args.poll_interval, max(0.0, deadline - now)))


def build_handoff_payload(args: argparse.Namespace, result: dict[str, Any]) -> dict[str, Any]:
    recording_folder = result.get("recording_folder")
    handoff_path = (
        str(args.write_handoff.resolve()) if args.write_handoff is not None else ""
    )
    app_config_path = str(args.config.resolve())
    return {
        "schema_id": "orange.manual_citrus_completion_handoff",
        "schema_version": 1,
        "created_at_utc": utc_now(),
        "handoff_path": handoff_path,
        "recording_folder": recording_folder,
        "orange_local_control_socket": args.socket,
        "orange_app_config": app_config_path,
        "readiness": {
            "ok": result.get("ok") is True,
            "attempts": result.get("attempts", 1),
            "wait_seconds": result.get("wait_seconds", args.wait_seconds),
            "passes": result.get("passes", []),
            "status_response": result.get("status_response"),
        },
        "citrus_env": {
            "CITRUS_ORANGE_COMPLETION_NOTIFY": "1",
            "CITRUS_ORANGE_LOCAL_CONTROL_SOCKET": args.socket,
            "CITRUS_ORANGE_COMPLETION_GRACE_SECONDS": "10",
        },
        "validation": {
            "stop_all": [
                "scripts/validate_gui_citrus_completion_recording.py",
                "--handoff",
                handoff_path,
                "--stop-all",
            ],
            "natural_completion": [
                "scripts/validate_gui_citrus_completion_recording.py",
                "--handoff",
                handoff_path,
                "--natural-completion",
            ],
        },
    }


def write_handoff_if_requested(args: argparse.Namespace, result: dict[str, Any]) -> None:
    if args.write_handoff is None:
        return
    if not result.get("ok"):
        return
    if not result.get("recording_folder"):
        result["ok"] = False
        result.setdefault("failures", []).append(
            "--write-handoff requires a non-empty recording folder"
        )
        return
    payload = build_handoff_payload(args, result)
    try:
        args.write_handoff.parent.mkdir(parents=True, exist_ok=True)
        args.write_handoff.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        result["handoff_path"] = payload["handoff_path"]
        result.setdefault("passes", []).append(
            f"handoff.written={payload['handoff_path']}"
        )
    except OSError as exc:
        result["ok"] = False
        result.setdefault("failures", []).append(
            f"failed to write handoff {args.write_handoff}: {exc}"
        )


def main() -> int:
    args = parse_args()
    result = evaluate_preflight_until_ready(args)
    write_handoff_if_requested(args, result)
    if args.print_recording_folder:
        if result["ok"] and result["recording_folder"]:
            print(result["recording_folder"])
        else:
            for message in result["failures"]:
                print(f"[FAIL] {message}", file=sys.stderr)
        return 0 if result["ok"] else 1
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        for message in result["passes"]:
            print(f"[PASS] {message}")
        if (
            args.check_socket
            and not result["socket_checked"]
            and not args.require_live_socket
            and args.wait_seconds <= 0
        ):
            print(f"[WARN] socket not present; skipped live socket check: {args.socket}")
        for message in result["failures"]:
            print(f"[FAIL] {message}")
        if args.wait_seconds > 0:
            print(
                "[INFO] readiness attempts="
                f"{result.get('attempts', 1)} wait_seconds={args.wait_seconds:g}"
            )
        print("Result: PASS" if result["ok"] else "Result: FAIL")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
