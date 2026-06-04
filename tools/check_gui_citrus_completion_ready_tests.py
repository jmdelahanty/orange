#!/usr/bin/env python3
"""Tests for the manual Citrus completion readiness preflight."""

from __future__ import annotations

import json
import importlib.util
import os
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "check_gui_citrus_completion_ready.py"
VALIDATOR_SCRIPT = REPO_ROOT / "scripts" / "validate_gui_citrus_completion_recording.py"
SPEC = importlib.util.spec_from_file_location("ready_check", SCRIPT)
require_spec = SPEC is not None and SPEC.loader is not None
if not require_spec:
    raise RuntimeError(f"failed to load module spec for {SCRIPT}")
ready_check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ready_check)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_config(path: Path, *, citrus_stop: bool = True, generic_stop: bool = False) -> None:
    payload = {
        "schema_id": "orange.app.config",
        "schema_version": 1,
        "gui": {
            "local_control": {
                "recording_start_enabled": False,
                "recording_stop_enabled": generic_stop,
                "citrus_completion_stop_enabled": citrus_stop,
                "exit_after_finalize": False,
                "drain_timeout_seconds": 60,
            }
        },
    }
    path.write_text(json.dumps(payload), encoding="utf-8")


def run_checker(
    args: list[str],
    *,
    extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        [str(SCRIPT), *args],
        cwd=REPO_ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def checker_args(config: Path, **overrides: Any) -> SimpleNamespace:
    defaults: dict[str, Any] = {
        "config": config,
        "socket": "/tmp/orange_test.sock",
        "check_socket": False,
        "require_live_socket": False,
        "require_manual_citrus_ready": False,
        "timeout": 2.0,
        "require_recording_active": False,
        "require_ready_for_citrus_experiment": False,
        "require_recording_folder": False,
        "json": False,
        "print_recording_folder": False,
        "write_handoff": None,
        "wait_seconds": 0.0,
        "poll_interval": 0.5,
    }
    defaults.update(overrides)
    return SimpleNamespace(**defaults)


def test_good_app_config_passes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        config = Path(tmp) / "default.json"
        write_config(config)
        result = run_checker(["--config", str(config)])
        require(result.returncode == 0, result.stdout + result.stderr)
        require("Result: PASS" in result.stdout, "good config should pass")


def test_checked_in_default_example_supports_manual_citrus_completion() -> None:
    result = run_checker(["--config", str(REPO_ROOT / "config/app/default.example.json")])
    require(result.returncode == 0, result.stdout + result.stderr)
    require(
        "app_config.gui.local_control.citrus_completion_stop_enabled=true"
        in result.stdout,
        "checked-in app config example should enable Citrus completion stop",
    )


def test_bad_app_config_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        config = Path(tmp) / "default.json"
        write_config(config, citrus_stop=False, generic_stop=True)
        result = run_checker(["--config", str(config)])
        require(result.returncode != 0, "bad config should fail")
        require(
            "recording_stop_enabled expected false" in result.stdout,
            "failure should explain generic stop gate mismatch",
        )
        require(
            "citrus_completion_stop_enabled expected true" in result.stdout,
            "failure should explain Citrus gate mismatch",
        )


def socket_response(
    *,
    start_enabled: bool,
    stop_enabled: bool,
    citrus_enabled: bool,
    recording_active: bool = True,
    ready_for_citrus_experiment: bool = True,
    recording_folder: str = "/tmp/orange_recording",
) -> dict[str, Any]:
    return {
        "schema_id": "orange.local_control.response",
        "schema_version": 1,
        "ok": True,
        "accepted": True,
        "method": "status",
        "request_id": "test",
        "operation_id": "",
        "responded_at_utc": "2026-05-29T00:00:00Z",
        "status": {
            "phase": "recording" if recording_active else "streaming",
            "readiness": {
                "recording_active": recording_active,
                "ready_for_citrus_experiment": ready_for_citrus_experiment,
            },
            "recording": {
                "folder": recording_folder,
            },
            "local_control": {
                "recording_start": {"enabled": start_enabled},
                "recording_stop": {"enabled": stop_enabled},
                "citrus_completion_stop": {"enabled": citrus_enabled},
            },
        },
    }


def test_socket_gate_status_check_passes() -> None:
    failures, passes = ready_check.check_socket_status(
        socket_response(
            start_enabled=False,
            stop_enabled=False,
            citrus_enabled=True,
        )
    )
    require(not failures, f"valid socket status should pass: {failures}")
    require(
        "socket.status.local_control.citrus_completion_stop.enabled=true" in passes,
        "socket status check should report Citrus stop gate",
    )


def test_socket_gate_status_check_fails_on_wrong_running_gate() -> None:
    failures, _passes = ready_check.check_socket_status(
        socket_response(
            start_enabled=False,
            stop_enabled=True,
            citrus_enabled=True,
        )
    )
    require(failures, "wrong live gate should fail")
    require(
        any(
            "socket.status.local_control.recording_stop.enabled expected false"
            in failure
            for failure in failures
        ),
        "failure should identify live generic stop gate mismatch",
    )


def test_socket_readiness_requirement_passes_when_ready_for_citrus() -> None:
    failures, passes = ready_check.check_socket_status(
        socket_response(
            start_enabled=False,
            stop_enabled=False,
            citrus_enabled=True,
            recording_active=True,
            ready_for_citrus_experiment=True,
        ),
        require_ready_for_citrus_experiment=True,
        require_recording_folder=True,
    )
    require(not failures, f"ready Citrus status should pass: {failures}")
    require(
        "socket.status.readiness.ready_for_citrus_experiment=true" in passes,
        "readiness pass should include ready_for_citrus_experiment=true",
    )
    require(
        "socket.status.recording.folder=/tmp/orange_recording" in passes,
        "strict readiness should report active recording folder",
    )


def test_socket_readiness_requirement_fails_when_not_recording() -> None:
    failures, _passes = ready_check.check_socket_status(
        socket_response(
            start_enabled=False,
            stop_enabled=False,
            citrus_enabled=True,
            recording_active=False,
            ready_for_citrus_experiment=False,
        ),
        require_recording_active=True,
        require_ready_for_citrus_experiment=True,
    )
    require(failures, "not-recording status should fail strict readiness")
    require(
        any("socket.status.readiness.recording_active expected true" in failure for failure in failures),
        "failure should identify inactive recording",
    )
    require(
        any("socket.status.readiness.ready_for_citrus_experiment expected true" in failure for failure in failures),
        "failure should identify Citrus readiness mismatch",
    )


def test_recording_folder_requirement_fails_when_missing() -> None:
    failures, _passes = ready_check.check_socket_status(
        socket_response(
            start_enabled=False,
            stop_enabled=False,
            citrus_enabled=True,
            recording_active=True,
            ready_for_citrus_experiment=True,
            recording_folder="",
        ),
        require_recording_folder=True,
    )
    require(failures, "missing recording folder should fail strict readiness")
    require(
        any("socket.status.recording.folder expected non-empty string" in failure for failure in failures),
        "failure should identify missing recording folder",
    )


def test_recording_folder_extraction() -> None:
    folder = ready_check.recording_folder_from_response(
        socket_response(
            start_enabled=False,
            stop_enabled=False,
            citrus_enabled=True,
            recording_folder="/tmp/specific_recording",
        )
    )
    require(folder == "/tmp/specific_recording", "recording folder should extract from status")


def test_require_live_socket_fails_when_missing() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = root / "default.json"
        sock = root / "missing.sock"
        write_config(config)
        result = run_checker(
            ["--config", str(config), "--socket", str(sock), "--require-live-socket"]
        )
        require(result.returncode != 0, "missing required socket should fail")
        require("required live socket missing" in result.stdout, result.stdout)


def test_manual_citrus_ready_profile_requires_live_socket() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = root / "default.json"
        sock = root / "missing.sock"
        write_config(config)
        result = run_checker(
            [
                "--config",
                str(config),
                "--socket",
                str(sock),
                "--require-manual-citrus-ready",
            ]
        )
        require(result.returncode != 0, "manual Citrus profile should require socket")
        require("required live socket missing" in result.stdout, result.stdout)


def test_manual_citrus_ready_profile_requires_active_recording() -> None:
    requirements = ready_check.effective_socket_requirements(
        SimpleNamespace(
            require_live_socket=False,
            require_recording_active=False,
            require_ready_for_citrus_experiment=False,
            require_recording_folder=False,
            require_manual_citrus_ready=True,
            print_recording_folder=False,
            write_handoff=None,
        )
    )
    require(
        requirements == (True, True, True, True),
        "manual Citrus profile should imply live socket, active recording, "
        "Citrus readiness, and recording folder",
    )


def test_print_recording_folder_requires_live_socket() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = root / "default.json"
        sock = root / "missing.sock"
        write_config(config)
        result = run_checker(
            [
                "--config",
                str(config),
                "--socket",
                str(sock),
                "--print-recording-folder",
            ]
        )
        require(result.returncode != 0, "recording folder output requires live socket")
        require("required live socket missing" in result.stderr, result.stderr)


def test_wait_manual_citrus_ready_retries_until_status_passes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        config = Path(tmp) / "default.json"
        write_config(config)
        responses = [
            socket_response(
                start_enabled=False,
                stop_enabled=False,
                citrus_enabled=True,
                recording_active=False,
                ready_for_citrus_experiment=False,
                recording_folder="",
            ),
            socket_response(
                start_enabled=False,
                stop_enabled=False,
                citrus_enabled=True,
                recording_active=True,
                ready_for_citrus_experiment=True,
                recording_folder="/tmp/orange_ready",
            ),
        ]

        def send_status(_socket_path: str, _timeout: float) -> dict[str, Any]:
            return responses.pop(0)

        args = checker_args(
            config,
            require_manual_citrus_ready=True,
            wait_seconds=30.0,
            poll_interval=0.1,
        )
        result = ready_check.evaluate_preflight_until_ready(
            args,
            send_status_fn=send_status,
            socket_exists_fn=lambda _path: True,
            sleep_fn=lambda _seconds: None,
            monotonic_fn=lambda: 0.0,
        )
        require(result["ok"], f"waited readiness should pass: {result}")
        require(result["attempts"] == 2, "readiness wait should retry until status passes")
        require(
            result["recording_folder"] == "/tmp/orange_ready",
            "readiness wait should preserve the passing recording folder",
        )


def test_wait_manual_citrus_ready_times_out_with_last_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        config = Path(tmp) / "default.json"
        write_config(config)
        clock = iter([0.0, 1.0])
        args = checker_args(
            config,
            require_manual_citrus_ready=True,
            wait_seconds=1.0,
            poll_interval=0.1,
        )
        result = ready_check.evaluate_preflight_until_ready(
            args,
            send_status_fn=lambda _socket_path, _timeout: socket_response(
                start_enabled=False,
                stop_enabled=False,
                citrus_enabled=True,
                recording_active=False,
                ready_for_citrus_experiment=False,
                recording_folder="",
            ),
            socket_exists_fn=lambda _path: True,
            sleep_fn=lambda _seconds: None,
            monotonic_fn=lambda: next(clock),
        )
        require(not result["ok"], "timeout should fail when readiness never passes")
        require(result["wait_timed_out"], "timeout result should report wait_timed_out")
        require(
            any("recording_active expected true" in failure for failure in result["failures"]),
            "timeout should preserve the last readiness failure",
        )


def test_write_handoff_implies_manual_readiness_requirements() -> None:
    requirements = ready_check.effective_socket_requirements(
        SimpleNamespace(
            require_live_socket=False,
            require_recording_active=False,
            require_ready_for_citrus_experiment=False,
            require_recording_folder=False,
            require_manual_citrus_ready=False,
            print_recording_folder=False,
            write_handoff=Path("/tmp/handoff.json"),
        )
    )
    require(
        requirements == (True, True, True, True),
        "writing a handoff should imply the full manual Citrus readiness profile",
    )


def test_write_handoff_payload_after_ready() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = root / "default.json"
        handoff = root / "nested" / "handoff.json"
        write_config(config)
        args = checker_args(config, write_handoff=handoff)
        result = ready_check.evaluate_preflight_until_ready(
            args,
            send_status_fn=lambda _socket_path, _timeout: socket_response(
                start_enabled=False,
                stop_enabled=False,
                citrus_enabled=True,
                recording_active=True,
                ready_for_citrus_experiment=True,
                recording_folder="/tmp/orange_ready",
            ),
            socket_exists_fn=lambda _path: True,
        )
        ready_check.write_handoff_if_requested(args, result)
        require(result["ok"], f"handoff write should preserve a passing result: {result}")
        require(handoff.exists(), "handoff JSON should be written")
        payload = json.loads(handoff.read_text(encoding="utf-8"))
        require(
            payload["schema_id"] == "orange.manual_citrus_completion_handoff",
            "handoff schema id should identify the manual Citrus handoff",
        )
        require(
            payload["recording_folder"] == "/tmp/orange_ready",
            "handoff should contain the exact Orange recording folder",
        )
        require(
            payload["handoff_path"] == str(handoff),
            "handoff should record its own path",
        )
        require(
            payload["orange_app_config"] == str(config),
            "handoff should record the absolute app config path",
        )
        require(
            payload["readiness"]["ok"] is True,
            "handoff should record the readiness result",
        )
        require(
            payload["readiness"]["status_response"]["status"]["readiness"][
                "ready_for_citrus_experiment"
            ]
            is True,
            "handoff should include the Orange status response that proved readiness",
        )
        require(
            payload["citrus_env"]["CITRUS_ORANGE_COMPLETION_NOTIFY"] == "1",
            "handoff should include Citrus completion notify env",
        )
        require(
            payload["validation"]["natural_completion"]
            == [
                "scripts/validate_gui_citrus_completion_recording.py",
                "--handoff",
                str(handoff),
                "--natural-completion",
            ],
            "handoff should include handoff-based natural-completion validation",
        )


def test_write_handoff_payload_uses_absolute_path_for_relative_input() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = root / "default.json"
        handoff = Path("relative_handoff.json")
        expected_handoff = Path.cwd() / handoff
        write_config(config)
        args = checker_args(config, write_handoff=handoff)
        result = ready_check.evaluate_preflight_until_ready(
            args,
            send_status_fn=lambda _socket_path, _timeout: socket_response(
                start_enabled=False,
                stop_enabled=False,
                citrus_enabled=True,
                recording_active=True,
                ready_for_citrus_experiment=True,
                recording_folder="/tmp/orange_ready",
            ),
            socket_exists_fn=lambda _path: True,
        )
        payload = ready_check.build_handoff_payload(args, result)
        require(
            payload["handoff_path"] == str(expected_handoff),
            "handoff should store an absolute path even when input is relative",
        )
        require(
            payload["validation"]["stop_all"][2] == str(expected_handoff),
            "handoff validation commands should use the absolute handoff path",
        )


def test_write_handoff_payload_uses_absolute_config_path_for_relative_input() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = Path("relative_config.json")
        handoff = root / "handoff.json"
        args = checker_args(config, write_handoff=handoff)
        result = {
            "ok": True,
            "recording_folder": "/tmp/orange_ready",
            "status_response": socket_response(
                start_enabled=False,
                stop_enabled=False,
                citrus_enabled=True,
                recording_active=True,
                ready_for_citrus_experiment=True,
                recording_folder="/tmp/orange_ready",
            ),
        }
        payload = ready_check.build_handoff_payload(args, result)
        require(
            payload["orange_app_config"] == str(Path.cwd() / config),
            "handoff should store an absolute app config path even when input is relative",
        )


def test_written_handoff_is_accepted_by_validator_public_modes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = root / "default.json"
        handoff = root / "handoff.json"
        recording_folder = "/tmp/orange_ready"
        write_config(config)
        args = checker_args(config, write_handoff=handoff)
        result = ready_check.evaluate_preflight_until_ready(
            args,
            send_status_fn=lambda _socket_path, _timeout: socket_response(
                start_enabled=False,
                stop_enabled=False,
                citrus_enabled=True,
                recording_active=True,
                ready_for_citrus_experiment=True,
                recording_folder=recording_folder,
            ),
            socket_exists_fn=lambda _path: True,
        )
        ready_check.write_handoff_if_requested(args, result)
        require(result["ok"], f"handoff write should pass: {result}")

        env_result = subprocess.run(
            [
                str(VALIDATOR_SCRIPT),
                "--handoff",
                str(handoff),
                "--print-citrus-env",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(env_result.returncode == 0, env_result.stderr)
        require(
            "export CITRUS_ORANGE_COMPLETION_NOTIFY=1" in env_result.stdout,
            "validator should accept generated handoff for Citrus env export",
        )

        command_result = subprocess.run(
            [
                str(VALIDATOR_SCRIPT),
                "--handoff",
                str(handoff),
                "--print-validation-command",
                "--natural-completion",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(command_result.returncode == 0, command_result.stderr)
        require(
            command_result.stdout.strip()
            == (
                "scripts/validate_gui_citrus_completion_recording.py "
                f"--handoff {handoff} --natural-completion"
            ),
            "validator should print the generated natural-completion command",
        )

        dry_run_result = subprocess.run(
            [
                str(VALIDATOR_SCRIPT),
                "--handoff",
                str(handoff),
                "--natural-completion",
                "--dry-run",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(dry_run_result.returncode == 0, dry_run_result.stderr)
        require(recording_folder in dry_run_result.stdout, "dry-run should target handoff folder")
        require(
            "--expect-local-control-stop-method citrus_completion"
            in dry_run_result.stdout,
            "dry-run should preserve strict Citrus completion expectations",
        )
        require(
            "--expect-local-control-stop-terminal-state completed"
            in dry_run_result.stdout,
            "dry-run should preserve natural-completion terminal expectation",
        )


def test_check_socket_conflicts_with_manual_citrus_ready_profile() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        config = Path(tmp) / "default.json"
        write_config(config)
        result = run_checker(
            [
                "--config",
                str(config),
                "--check-socket",
                "--require-manual-citrus-ready",
            ]
        )
        require(result.returncode != 0, "socket modes should be mutually exclusive")
        require(
            "not allowed with argument" in result.stderr,
            "argparse should explain socket mode conflict",
        )


def test_json_and_print_recording_folder_conflict() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        config = Path(tmp) / "default.json"
        write_config(config)
        result = run_checker(
            [
                "--config",
                str(config),
                "--json",
                "--print-recording-folder",
            ]
        )
        require(result.returncode != 0, "JSON and folder-only output should conflict")
        require(
            "not allowed with argument" in result.stderr,
            "argparse should explain mutually exclusive output modes",
        )


def test_app_config_env_override_matches_orange_precedence() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        app_config = root / "app_config.json"
        gui_config = root / "gui_config.json"
        write_config(app_config)
        write_config(gui_config, citrus_stop=False)
        result = run_checker(
            ["--json"],
            extra_env={
                "ORANGE_APP_CONFIG_PATH": str(app_config),
                "ORANGE_GUI_APP_CONFIG_PATH": str(gui_config),
            },
        )
        require(result.returncode == 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        require(
            payload["config"] == str(app_config),
            "ORANGE_APP_CONFIG_PATH should win over ORANGE_GUI_APP_CONFIG_PATH",
        )


def test_json_reports_absolute_config_path_for_relative_input() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = Path("relative_ready_config.json")
        write_config(REPO_ROOT / config)
        try:
            result = run_checker(["--config", str(config), "--json"])
        finally:
            (REPO_ROOT / config).unlink(missing_ok=True)
        require(result.returncode == 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        require(
            payload["config"] == str((REPO_ROOT / config).resolve()),
            "JSON preflight output should report the absolute app config path",
        )


def test_socket_env_override_matches_orange_client_precedence() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = root / "default.json"
        write_config(config)
        result = run_checker(
            ["--config", str(config), "--json"],
            extra_env={
                "ORANGE_GUI_LOCAL_CONTROL_SOCKET": "/tmp/gui.sock",
                "ORANGE_LOCAL_CONTROL_SOCKET": "/tmp/generic.sock",
            },
        )
        require(result.returncode == 0, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        require(
            payload["socket"] == "/tmp/gui.sock",
            "GUI-specific socket env should win over generic socket env",
        )


def main() -> None:
    tests = [
        test_good_app_config_passes,
        test_checked_in_default_example_supports_manual_citrus_completion,
        test_bad_app_config_fails,
        test_socket_gate_status_check_passes,
        test_socket_gate_status_check_fails_on_wrong_running_gate,
        test_socket_readiness_requirement_passes_when_ready_for_citrus,
        test_socket_readiness_requirement_fails_when_not_recording,
        test_recording_folder_requirement_fails_when_missing,
        test_recording_folder_extraction,
        test_require_live_socket_fails_when_missing,
        test_manual_citrus_ready_profile_requires_live_socket,
        test_manual_citrus_ready_profile_requires_active_recording,
        test_print_recording_folder_requires_live_socket,
        test_wait_manual_citrus_ready_retries_until_status_passes,
        test_wait_manual_citrus_ready_times_out_with_last_failure,
        test_write_handoff_implies_manual_readiness_requirements,
        test_write_handoff_payload_after_ready,
        test_write_handoff_payload_uses_absolute_path_for_relative_input,
        test_write_handoff_payload_uses_absolute_config_path_for_relative_input,
        test_written_handoff_is_accepted_by_validator_public_modes,
        test_check_socket_conflicts_with_manual_citrus_ready_profile,
        test_json_and_print_recording_folder_conflict,
        test_app_config_env_override_matches_orange_precedence,
        test_json_reports_absolute_config_path_for_relative_input,
        test_socket_env_override_matches_orange_client_precedence,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("check_gui_citrus_completion_ready_tests passed")


if __name__ == "__main__":
    main()
