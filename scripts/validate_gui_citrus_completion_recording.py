#!/usr/bin/env python3
"""Validate a GUI recording stopped by Citrus completion local control."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = REPO_ROOT / "scripts" / "validate_gui_ptp_recording.py"


def nested_dict(payload: dict[str, object], *keys: str) -> dict[str, object]:
    current: object = payload
    for key in keys:
        if not isinstance(current, dict):
            return {}
        current = current.get(key)
    return current if isinstance(current, dict) else {}


def parse_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(
        description=(
            "Shortcut for validating a manual Orange GUI recording where Citrus "
            "sent Orange a citrus_completion local-control request."
        )
    )
    parser.add_argument(
        "recording_folder",
        nargs="?",
        help=(
            "Recording folder to validate. If omitted, validates the latest "
            "complete GUI recording."
        ),
    )
    parser.add_argument(
        "--handoff",
        type=Path,
        help=(
            "Manual Orange+Citrus handoff JSON written by "
            "check_gui_citrus_completion_ready.py --write-handoff. Uses its "
            "recording_folder as the exact validation target."
        ),
    )
    parser.add_argument(
        "--print-citrus-env",
        action="store_true",
        help=(
            "With --handoff, validate the handoff and print shell export lines "
            "for the Citrus completion-notify environment."
        ),
    )
    parser.add_argument(
        "--print-validation-command",
        action="store_true",
        help=(
            "With --handoff and --stop-all or --natural-completion, validate "
            "the handoff and print the handoff-stored validation command."
        ),
    )
    latest_group = parser.add_mutually_exclusive_group()
    latest_group.add_argument(
        "--latest",
        action="store_true",
        help="Use validate_gui_ptp_recording.py --latest instead of --latest-complete.",
    )
    latest_group.add_argument(
        "--latest-complete",
        action="store_true",
        help=(
            "Use validate_gui_ptp_recording.py --latest-complete. This is the "
            "default when recording_folder is omitted."
        ),
    )
    parser.add_argument("--root", help="Optional root passed to --latest or --latest-complete.")
    terminal_group = parser.add_mutually_exclusive_group()
    terminal_group.add_argument(
        "--stop-all",
        action="store_true",
        help="Expect terminal_state=stopped and reason=stopped_by_local_control.",
    )
    terminal_group.add_argument(
        "--natural-completion",
        action="store_true",
        help="Expect terminal_state=completed and reason=protocol_finished.",
    )
    terminal_group.add_argument(
        "--terminal-state",
        help="Explicit Citrus terminal_state expected in recording.control.",
    )
    terminal_group.add_argument(
        "--any-terminal",
        action="store_true",
        help=(
            "Do not assert terminal_state/reason. Prefer --stop-all or "
            "--natural-completion for acceptance validation."
        ),
    )
    parser.add_argument(
        "--reason",
        help=(
            "Explicit Citrus terminal reason expected in recording.control. "
            "Required when --terminal-state is used."
        ),
    )
    parser.add_argument(
        "--operation-id",
        help="Optional expected local-control operation_id in recording.control.",
    )
    parser.add_argument(
        "--orange-local-control-event-log",
        help="Optional source event log path to compare with the artifact copy.",
    )
    parser.add_argument("--json-out", help="Optional validation JSON output path.")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the validator command instead of executing it.",
    )
    args, extra_args = parser.parse_known_args(argv)
    if args.handoff is not None:
        args.handoff = args.handoff.expanduser()
    return args, extra_args


def load_handoff_payload(path: Path) -> dict[str, object]:
    expected_handoff_path = str(path.resolve())
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise SystemExit(f"handoff file missing: {path}") from exc
    except json.JSONDecodeError as exc:
        raise SystemExit(f"failed to parse handoff JSON {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise SystemExit(f"handoff root must be a JSON object: {path}")
    if payload.get("schema_id") != "orange.manual_citrus_completion_handoff":
        raise SystemExit(
            "handoff schema_id expected "
            "'orange.manual_citrus_completion_handoff', got "
            f"{payload.get('schema_id')!r}"
        )
    if payload.get("schema_version") != 1:
        raise SystemExit(
            "handoff schema_version expected 1, got "
            f"{payload.get('schema_version')!r}: {path}"
        )
    created_at_utc = payload.get("created_at_utc")
    if not isinstance(created_at_utc, str) or not created_at_utc:
        raise SystemExit(
            f"handoff created_at_utc must be a non-empty string: {path}"
        )
    stored_handoff_path = payload.get("handoff_path")
    if not isinstance(stored_handoff_path, str) or not stored_handoff_path:
        raise SystemExit(f"handoff handoff_path must be a non-empty string: {path}")
    if str(Path(stored_handoff_path).expanduser()) != expected_handoff_path:
        raise SystemExit(
            f"handoff handoff_path expected {expected_handoff_path!r}, "
            f"got {stored_handoff_path!r}: {path}"
        )
    app_config_path = payload.get("orange_app_config")
    if not isinstance(app_config_path, str) or not app_config_path:
        raise SystemExit(
            f"handoff orange_app_config must be a non-empty string: {path}"
        )
    if not Path(app_config_path).expanduser().is_absolute():
        raise SystemExit(
            f"handoff orange_app_config must be an absolute path, "
            f"got {app_config_path!r}: {path}"
        )
    readiness = payload.get("readiness")
    if not isinstance(readiness, dict) or readiness.get("ok") is not True:
        raise SystemExit(f"handoff readiness.ok must be true: {path}")
    folder = payload.get("recording_folder")
    if not isinstance(folder, str) or not folder:
        raise SystemExit(f"handoff recording_folder must be a non-empty string: {path}")
    status_response = readiness.get("status_response")
    if not isinstance(status_response, dict):
        raise SystemExit(f"handoff readiness.status_response must be an object: {path}")
    validate_handoff_status_response(status_response, folder, path)
    return payload


def recording_folder_from_handoff(path: Path) -> str:
    payload = load_handoff_payload(path)
    folder = payload.get("recording_folder")
    assert isinstance(folder, str)
    return folder


def citrus_env_from_handoff(path: Path) -> dict[str, str]:
    payload = load_handoff_payload(path)
    citrus_env = payload.get("citrus_env")
    if not isinstance(citrus_env, dict):
        raise SystemExit(f"handoff citrus_env must be an object: {path}")
    required_keys = [
        "CITRUS_ORANGE_COMPLETION_NOTIFY",
        "CITRUS_ORANGE_LOCAL_CONTROL_SOCKET",
        "CITRUS_ORANGE_COMPLETION_GRACE_SECONDS",
    ]
    env: dict[str, str] = {}
    for key in required_keys:
        value = citrus_env.get(key)
        if not isinstance(value, str) or not value:
            raise SystemExit(
                f"handoff citrus_env.{key} must be a non-empty string: {path}"
            )
        env[key] = value
    if env["CITRUS_ORANGE_COMPLETION_NOTIFY"] != "1":
        raise SystemExit(
            "handoff citrus_env.CITRUS_ORANGE_COMPLETION_NOTIFY expected '1', "
            f"got {env['CITRUS_ORANGE_COMPLETION_NOTIFY']!r}: {path}"
        )
    handoff_socket = payload.get("orange_local_control_socket")
    if not isinstance(handoff_socket, str) or not handoff_socket:
        raise SystemExit(
            f"handoff orange_local_control_socket must be a non-empty string: {path}"
        )
    if env["CITRUS_ORANGE_LOCAL_CONTROL_SOCKET"] != handoff_socket:
        raise SystemExit(
            "handoff citrus_env.CITRUS_ORANGE_LOCAL_CONTROL_SOCKET expected "
            f"{handoff_socket!r}, got "
            f"{env['CITRUS_ORANGE_LOCAL_CONTROL_SOCKET']!r}: {path}"
        )
    try:
        grace_seconds = float(env["CITRUS_ORANGE_COMPLETION_GRACE_SECONDS"])
    except ValueError as exc:
        raise SystemExit(
            "handoff citrus_env.CITRUS_ORANGE_COMPLETION_GRACE_SECONDS must be "
            f"numeric, got {env['CITRUS_ORANGE_COMPLETION_GRACE_SECONDS']!r}: {path}"
        ) from exc
    if grace_seconds < 0:
        raise SystemExit(
            "handoff citrus_env.CITRUS_ORANGE_COMPLETION_GRACE_SECONDS must be "
            f">= 0, got {env['CITRUS_ORANGE_COMPLETION_GRACE_SECONDS']!r}: {path}"
        )
    return env


def validation_command_from_handoff(path: Path, mode: str) -> list[str]:
    expected_handoff_path = str(path.resolve())
    payload = load_handoff_payload(path)
    validation = payload.get("validation")
    if not isinstance(validation, dict):
        raise SystemExit(f"handoff validation must be an object: {path}")
    command = validation.get(mode)
    if not isinstance(command, list) or not command:
        raise SystemExit(f"handoff validation.{mode} must be a non-empty list: {path}")
    if not all(isinstance(item, str) and item for item in command):
        raise SystemExit(f"handoff validation.{mode} must contain only strings: {path}")
    expected_flag = "--stop-all" if mode == "stop_all" else "--natural-completion"
    if command[0] != "scripts/validate_gui_citrus_completion_recording.py":
        raise SystemExit(
            f"handoff validation.{mode}[0] must be "
            "'scripts/validate_gui_citrus_completion_recording.py': {path}"
        )
    if "--handoff" not in command:
        raise SystemExit(f"handoff validation.{mode} must use --handoff: {path}")
    handoff_indexes = [
        index for index, item in enumerate(command) if item == "--handoff"
    ]
    if len(handoff_indexes) != 1:
        raise SystemExit(
            f"handoff validation.{mode} must contain exactly one --handoff: {path}"
        )
    handoff_index = handoff_indexes[0]
    if handoff_index + 1 >= len(command):
        raise SystemExit(
            f"handoff validation.{mode} --handoff must have a path value: {path}"
        )
    observed_handoff_path = str(Path(command[handoff_index + 1]).expanduser())
    if observed_handoff_path != expected_handoff_path:
        raise SystemExit(
            f"handoff validation.{mode} --handoff expected "
            f"{expected_handoff_path!r}, got {observed_handoff_path!r}: {path}"
        )
    if command.count(expected_flag) != 1:
        raise SystemExit(
            f"handoff validation.{mode} must include exactly one "
            f"{expected_flag}: {path}"
        )
    unexpected_flag = (
        "--natural-completion" if mode == "stop_all" else "--stop-all"
    )
    if unexpected_flag in command:
        raise SystemExit(
            f"handoff validation.{mode} must not include {unexpected_flag}: {path}"
        )
    if "--any-terminal" in command:
        raise SystemExit(
            f"handoff validation.{mode} must not include --any-terminal: {path}"
        )
    expected_command = [
        "scripts/validate_gui_citrus_completion_recording.py",
        "--handoff",
        expected_handoff_path,
        expected_flag,
    ]
    if command != expected_command:
        raise SystemExit(
            f"handoff validation.{mode} must exactly match "
            f"{expected_command!r}, got {command!r}: {path}"
        )
    return command


def require_handoff_bool(
    payload: dict[str, object],
    key: str,
    expected: bool,
    path: Path,
    prefix: str,
) -> None:
    actual = payload.get(key)
    if actual is not expected:
        raise SystemExit(
            f"handoff {prefix}.{key} expected {str(expected).lower()}, "
            f"got {actual!r}: {path}"
        )


def validate_handoff_status_response(
    response: dict[str, object],
    recording_folder: str,
    path: Path,
) -> None:
    require_handoff_bool(response, "ok", True, path, "status_response")
    status = response.get("status")
    if not isinstance(status, dict):
        raise SystemExit(f"handoff status_response.status must be an object: {path}")
    readiness = nested_dict(status, "readiness")
    require_handoff_bool(
        readiness,
        "recording_active",
        True,
        path,
        "status_response.status.readiness",
    )
    require_handoff_bool(
        readiness,
        "ready_for_citrus_experiment",
        True,
        path,
        "status_response.status.readiness",
    )
    local_control = nested_dict(status, "local_control")
    require_handoff_bool(
        nested_dict(local_control, "recording_start"),
        "enabled",
        False,
        path,
        "status_response.status.local_control.recording_start",
    )
    require_handoff_bool(
        nested_dict(local_control, "recording_stop"),
        "enabled",
        False,
        path,
        "status_response.status.local_control.recording_stop",
    )
    require_handoff_bool(
        nested_dict(local_control, "citrus_completion_stop"),
        "enabled",
        True,
        path,
        "status_response.status.local_control.citrus_completion_stop",
    )
    observed_folder = nested_dict(status, "recording").get("folder")
    if observed_folder != recording_folder:
        raise SystemExit(
            "handoff status_response.status.recording.folder expected "
            f"{recording_folder!r}, got {observed_folder!r}: {path}"
        )


def terminal_expectations(args: argparse.Namespace) -> tuple[str | None, str | None]:
    if args.stop_all:
        if args.reason:
            raise SystemExit("--reason cannot be combined with --stop-all")
        return "stopped", "stopped_by_local_control"
    if args.natural_completion:
        if args.reason:
            raise SystemExit("--reason cannot be combined with --natural-completion")
        return "completed", "protocol_finished"
    if args.terminal_state:
        if not args.reason:
            raise SystemExit("--terminal-state requires --reason")
        return args.terminal_state, args.reason
    if args.reason:
        raise SystemExit("--reason requires --terminal-state")
    if args.any_terminal:
        return None, None
    raise SystemExit(
        "choose --stop-all, --natural-completion, --terminal-state ... --reason ..., "
        "or --any-terminal"
    )


def build_command(args: argparse.Namespace, extra_args: list[str]) -> list[str]:
    terminal_state, reason = terminal_expectations(args)
    cmd = [sys.executable, str(VALIDATOR)]

    if args.handoff:
        if args.recording_folder or args.latest or args.latest_complete or args.root:
            raise SystemExit(
                "--handoff cannot be combined with recording_folder, --latest, "
                "--latest-complete, or --root"
            )
        cmd.append(recording_folder_from_handoff(args.handoff))
    elif args.recording_folder:
        if args.latest or args.latest_complete or args.root:
            raise SystemExit(
                "recording_folder cannot be combined with --latest, "
                "--latest-complete, or --root"
            )
        cmd.append(args.recording_folder)
    else:
        latest_flag = "--latest" if args.latest else "--latest-complete"
        cmd.append(latest_flag)
        if args.root:
            cmd.append(args.root)

    cmd.extend(
        [
            "--expect-local-control-stop-method",
            "citrus_completion",
            "--expect-local-control-stop-command-source",
            "citrus",
            "--expect-local-control-stop-ack-state",
            "executed",
            "--expect-local-control-generic-stop-enabled",
            "0",
            "--expect-local-control-citrus-stop-enabled",
            "1",
            "--require-orange-local-control-event-log",
        ]
    )
    if terminal_state:
        cmd.extend(["--expect-local-control-stop-terminal-state", terminal_state])
    if reason:
        cmd.extend(["--expect-local-control-stop-reason", reason])
    if args.operation_id:
        cmd.extend(["--expect-local-control-stop-operation-id", args.operation_id])
    if args.orange_local_control_event_log:
        cmd.extend(
            ["--orange-local-control-event-log", args.orange_local_control_event_log]
        )
    if args.json_out:
        cmd.extend(["--json-out", args.json_out])
    if extra_args and extra_args[0] == "--":
        extra_args = extra_args[1:]
    cmd.extend(extra_args)
    return cmd


def main(argv: list[str] | None = None) -> int:
    args, extra_args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.print_citrus_env and args.print_validation_command:
        raise SystemExit(
            "--print-citrus-env cannot be combined with --print-validation-command"
        )
    if args.print_citrus_env:
        if not args.handoff:
            raise SystemExit("--print-citrus-env requires --handoff")
        if args.recording_folder or args.latest or args.latest_complete or args.root:
            raise SystemExit(
                "--print-citrus-env cannot be combined with recording_folder, "
                "--latest, --latest-complete, or --root"
            )
        if extra_args:
            raise SystemExit("--print-citrus-env does not accept passthrough arguments")
        for key, value in citrus_env_from_handoff(args.handoff).items():
            print(f"export {key}={shlex.quote(value)}")
        return 0
    if args.print_validation_command:
        if not args.handoff:
            raise SystemExit("--print-validation-command requires --handoff")
        if args.recording_folder or args.latest or args.latest_complete or args.root:
            raise SystemExit(
                "--print-validation-command cannot be combined with "
                "recording_folder, --latest, --latest-complete, or --root"
            )
        if args.dry_run:
            raise SystemExit("--print-validation-command cannot be combined with --dry-run")
        if extra_args:
            raise SystemExit(
                "--print-validation-command does not accept passthrough arguments"
            )
        if args.stop_all:
            mode = "stop_all"
        elif args.natural_completion:
            mode = "natural_completion"
        else:
            raise SystemExit(
                "--print-validation-command requires --stop-all or --natural-completion"
            )
        if args.terminal_state or args.reason or args.any_terminal:
            raise SystemExit(
                "--print-validation-command only supports --stop-all or "
                "--natural-completion"
            )
        print(shlex.join(validation_command_from_handoff(args.handoff, mode)))
        return 0
    cmd = build_command(args, extra_args)
    if args.dry_run:
        print(shlex.join(cmd))
        return 0
    return subprocess.run(cmd, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
