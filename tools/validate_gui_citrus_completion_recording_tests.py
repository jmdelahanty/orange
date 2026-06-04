#!/usr/bin/env python3
"""Tests for the Citrus-completion GUI validation shortcut."""

from __future__ import annotations

import json
import shlex
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "validate_gui_citrus_completion_recording.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def dry_run(args: list[str]) -> list[str]:
    result = subprocess.run(
        [str(SCRIPT), "--dry-run", *args],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    require(result.returncode == 0, result.stderr)
    return shlex.split(result.stdout.strip())


def valid_handoff_payload(
    recording_folder: str = "/tmp/orange_from_handoff",
    handoff_path: str | None = None,
) -> dict:
    payload = {
        "schema_id": "orange.manual_citrus_completion_handoff",
        "schema_version": 1,
        "created_at_utc": "2026-05-29T00:00:00Z",
        "recording_folder": recording_folder,
        "orange_app_config": "/home/jeremy/orange_data/config/app/default.json",
        "orange_local_control_socket": "/tmp/orange_local_control.sock",
        "readiness": {
            "ok": True,
            "status_response": {
                "ok": True,
                "status": {
                    "readiness": {
                        "recording_active": True,
                        "ready_for_citrus_experiment": True,
                    },
                    "recording": {
                        "folder": recording_folder,
                    },
                    "local_control": {
                        "recording_start": {"enabled": False},
                        "recording_stop": {"enabled": False},
                        "citrus_completion_stop": {"enabled": True},
                    },
                },
            },
        },
        "citrus_env": {
            "CITRUS_ORANGE_COMPLETION_NOTIFY": "1",
            "CITRUS_ORANGE_LOCAL_CONTROL_SOCKET": "/tmp/orange_local_control.sock",
            "CITRUS_ORANGE_COMPLETION_GRACE_SECONDS": "10",
        },
    }
    if handoff_path:
        payload["handoff_path"] = handoff_path
        payload["validation"] = {
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
        }
    return payload


def test_stop_all_validates_latest_complete_with_strict_citrus_gates() -> None:
    cmd = dry_run(["--stop-all"])
    require(
        "scripts/validate_gui_ptp_recording.py" in cmd[1],
        "shortcut should dispatch to validate_gui_ptp_recording.py",
    )
    require("--latest-complete" in cmd, "default target should be latest complete")
    expected_pairs = {
        "--expect-local-control-stop-method": "citrus_completion",
        "--expect-local-control-stop-command-source": "citrus",
        "--expect-local-control-stop-ack-state": "executed",
        "--expect-local-control-generic-stop-enabled": "0",
        "--expect-local-control-citrus-stop-enabled": "1",
    }
    for flag, value in expected_pairs.items():
        require(flag in cmd, f"missing {flag}")
        require(cmd[cmd.index(flag) + 1] == value, f"{flag} should be {value}")
    require(
        "--require-orange-local-control-event-log" in cmd,
        "shortcut should require local-control event-log evidence",
    )


def test_stop_all_mode_sets_terminal_expectations() -> None:
    cmd = dry_run(["--stop-all"])
    require(
        cmd[cmd.index("--expect-local-control-stop-terminal-state") + 1] == "stopped",
        "STOP ALL mode should expect stopped terminal state",
    )
    require(
        cmd[cmd.index("--expect-local-control-stop-reason") + 1]
        == "stopped_by_local_control",
        "STOP ALL mode should expect stopped_by_local_control reason",
    )


def test_natural_completion_mode_sets_terminal_expectations() -> None:
    cmd = dry_run(["--natural-completion"])
    require(
        cmd[cmd.index("--expect-local-control-stop-terminal-state") + 1] == "completed",
        "natural completion mode should expect completed terminal state",
    )
    require(
        cmd[cmd.index("--expect-local-control-stop-reason") + 1]
        == "protocol_finished",
        "natural completion mode should expect protocol_finished reason",
    )


def test_explicit_folder_and_passthrough_args() -> None:
    cmd = dry_run(
        [
            "/tmp/recording",
            "--terminal-state",
            "failed",
            "--reason",
            "protocol_error",
            "--operation-id",
            "op-123",
            "--",
            "--expected-cameras",
            "2010093",
        ]
    )
    require("/tmp/recording" in cmd, "explicit recording folder should be forwarded")
    require("--latest-complete" not in cmd, "folder target should not add latest flag")
    require(
        cmd[cmd.index("--expect-local-control-stop-operation-id") + 1] == "op-123",
        "operation id should be forwarded as an expectation",
    )
    require(
        cmd[cmd.index("--expected-cameras") + 1] == "2010093",
        "extra validator args should pass through",
    )


def test_handoff_targets_exact_recording_folder() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        handoff.write_text(
            json.dumps(valid_handoff_payload(handoff_path=str(handoff))),
            encoding="utf-8",
        )
        cmd = dry_run(["--handoff", str(handoff), "--natural-completion"])
    require(
        "/tmp/orange_from_handoff" in cmd,
        "handoff recording folder should be forwarded as exact validation target",
    )
    require("--latest-complete" not in cmd, "handoff target should not add latest flag")
    require(
        cmd[cmd.index("--expect-local-control-stop-terminal-state") + 1]
        == "completed",
        "handoff should still preserve terminal-mode expectations",
    )


def test_handoff_conflicts_with_latest_targeting() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        handoff.write_text(
            json.dumps(valid_handoff_payload(handoff_path=str(handoff))),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                str(SCRIPT),
                "--handoff",
                str(handoff),
                "--latest-complete",
                "--stop-all",
                "--dry-run",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "handoff should conflict with latest targeting")
    require(
        "--handoff cannot be combined" in result.stderr,
        "handoff conflict should explain invalid target combination",
    )


def test_handoff_requires_schema_and_folder() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        handoff.write_text(json.dumps({"recording_folder": "/tmp/recording"}), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--stop-all", "--dry-run"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "invalid handoff schema should fail")
    require(
        "handoff schema_id expected" in result.stderr,
        "invalid handoff failure should explain schema mismatch",
    )


def test_handoff_requires_schema_version_one() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["schema_version"] = 2
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--stop-all", "--dry-run"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "wrong handoff schema version should fail")
    require(
        "handoff schema_version expected 1" in result.stderr,
        "wrong schema version failure should be explicit",
    )


def test_handoff_requires_generated_metadata_fields() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        for field, expected_message in [
            ("created_at_utc", "handoff created_at_utc must be a non-empty string"),
            ("handoff_path", "handoff handoff_path must be a non-empty string"),
            ("orange_app_config", "handoff orange_app_config must be a non-empty string"),
        ]:
            handoff = Path(tmp) / f"{field}.json"
            payload = valid_handoff_payload(handoff_path=str(handoff))
            del payload[field]
            handoff.write_text(json.dumps(payload), encoding="utf-8")
            result = subprocess.run(
                [str(SCRIPT), "--handoff", str(handoff), "--stop-all", "--dry-run"],
                cwd=REPO_ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            require(result.returncode != 0, f"missing {field} should fail")
            require(
                expected_message in result.stderr,
                f"missing {field} failure should be explicit",
            )


def test_handoff_requires_successful_readiness() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        handoff.write_text(
            json.dumps(
                {
                    "schema_id": "orange.manual_citrus_completion_handoff",
                    "schema_version": 1,
                    "created_at_utc": "2026-05-29T00:00:00Z",
                    "handoff_path": str(handoff),
                    "recording_folder": "/tmp/recording",
                    "orange_app_config": "/home/jeremy/orange_data/config/app/default.json",
                    "readiness": {"ok": False},
                }
            ),
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--stop-all", "--dry-run"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "handoff with failed readiness should fail")
    require(
        "handoff readiness.ok must be true" in result.stderr,
        "failed readiness should be rejected explicitly",
    )


def test_handoff_rejects_stale_top_level_handoff_path() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["handoff_path"] = str(Path(tmp) / "old_handoff.json")
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--stop-all", "--dry-run"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "stale top-level handoff path should fail")
    require(
        "handoff handoff_path expected" in result.stderr,
        "stale top-level handoff path failure should be explicit",
    )


def test_handoff_requires_status_response() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        del payload["readiness"]["status_response"]
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--stop-all", "--dry-run"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "handoff without status response should fail")
    require(
        "handoff readiness.status_response must be an object" in result.stderr,
        "missing status response should be rejected explicitly",
    )


def test_handoff_rejects_mismatched_recording_folder() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(
            "/tmp/top_level_folder", handoff_path=str(handoff)
        )
        payload["readiness"]["status_response"]["status"]["recording"]["folder"] = (
            "/tmp/different_folder"
        )
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--stop-all", "--dry-run"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "handoff folder mismatch should fail")
    require(
        "status_response.status.recording.folder expected" in result.stderr,
        "folder mismatch should be rejected explicitly",
    )


def test_handoff_rejects_enabled_generic_stop_gate() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["readiness"]["status_response"]["status"]["local_control"][
            "recording_stop"
        ]["enabled"] = True
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--stop-all", "--dry-run"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "handoff with generic stop enabled should fail")
    require(
        "status_response.status.local_control.recording_stop.enabled expected false"
        in result.stderr,
        "generic stop gate mismatch should be rejected explicitly",
    )


def test_handoff_prints_citrus_env_exports() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        handoff.write_text(json.dumps(valid_handoff_payload(handoff_path=str(handoff))), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--print-citrus-env"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode == 0, result.stderr)
    require(
        "export CITRUS_ORANGE_COMPLETION_NOTIFY=1" in result.stdout,
        "Citrus env export should include completion notify",
    )
    require(
        "export CITRUS_ORANGE_LOCAL_CONTROL_SOCKET=/tmp/orange_local_control.sock"
        in result.stdout,
        "Citrus env export should include Orange socket",
    )
    require(
        "export CITRUS_ORANGE_COMPLETION_GRACE_SECONDS=10" in result.stdout,
        "Citrus env export should include grace seconds",
    )


def test_handoff_print_citrus_env_requires_handoff() -> None:
    result = subprocess.run(
        [str(SCRIPT), "--print-citrus-env"],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    require(result.returncode != 0, "printing Citrus env should require handoff")
    require(
        "--print-citrus-env requires --handoff" in result.stderr,
        "missing handoff failure should explain requirement",
    )


def test_handoff_print_citrus_env_requires_env_payload() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        del payload["citrus_env"]
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--print-citrus-env"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "missing citrus_env should fail")
    require(
        "handoff citrus_env must be an object" in result.stderr,
        "missing citrus_env failure should be explicit",
    )


def test_handoff_print_citrus_env_rejects_notify_disabled() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["citrus_env"]["CITRUS_ORANGE_COMPLETION_NOTIFY"] = "0"
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--print-citrus-env"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "disabled Citrus notify should fail")
    require(
        "CITRUS_ORANGE_COMPLETION_NOTIFY expected '1'" in result.stderr,
        "disabled Citrus notify should be rejected explicitly",
    )


def test_handoff_print_citrus_env_rejects_socket_mismatch() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["citrus_env"]["CITRUS_ORANGE_LOCAL_CONTROL_SOCKET"] = (
            "/tmp/different.sock"
        )
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--print-citrus-env"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "Citrus socket mismatch should fail")
    require(
        "CITRUS_ORANGE_LOCAL_CONTROL_SOCKET expected" in result.stderr,
        "Citrus socket mismatch should be rejected explicitly",
    )


def test_handoff_print_citrus_env_rejects_invalid_grace_seconds() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["citrus_env"]["CITRUS_ORANGE_COMPLETION_GRACE_SECONDS"] = "-1"
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--print-citrus-env"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "negative grace seconds should fail")
    require(
        "CITRUS_ORANGE_COMPLETION_GRACE_SECONDS must be >= 0" in result.stderr,
        "negative grace seconds should be rejected explicitly",
    )


def test_handoff_prints_validation_command() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        handoff.write_text(
            json.dumps(valid_handoff_payload(handoff_path=str(handoff))),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                str(SCRIPT),
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
    require(result.returncode == 0, result.stderr)
    cmd = shlex.split(result.stdout.strip())
    require(
        cmd
        == [
            "scripts/validate_gui_citrus_completion_recording.py",
            "--handoff",
            str(handoff),
            "--natural-completion",
        ],
        "printed validation command should come from the handoff",
    )


def test_handoff_print_validation_command_rejects_stale_handoff_path() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["validation"]["natural_completion"][2] = str(
            Path(tmp) / "different_handoff.json"
        )
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [
                str(SCRIPT),
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
    require(result.returncode != 0, "stale handoff path should fail")
    require(
        "validation.natural_completion --handoff expected" in result.stderr,
        "stale handoff path failure should identify the mismatched handoff target",
    )


def test_handoff_print_validation_command_rejects_mixed_terminal_flags() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["validation"]["natural_completion"].append("--stop-all")
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [
                str(SCRIPT),
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
    require(result.returncode != 0, "mixed terminal flags should fail")
    require(
        "validation.natural_completion must not include --stop-all" in result.stderr,
        "mixed terminal flag failure should be explicit",
    )


def test_handoff_print_validation_command_rejects_duplicate_terminal_flag() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["validation"]["natural_completion"].append("--natural-completion")
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [
                str(SCRIPT),
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
    require(result.returncode != 0, "duplicate terminal flag should fail")
    require(
        "validation.natural_completion must include exactly one --natural-completion"
        in result.stderr,
        "duplicate terminal flag failure should be explicit",
    )


def test_handoff_print_validation_command_rejects_any_terminal_escape() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["validation"]["natural_completion"].append("--any-terminal")
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [
                str(SCRIPT),
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
    require(result.returncode != 0, "any-terminal escape should fail")
    require(
        "validation.natural_completion must not include --any-terminal" in result.stderr,
        "any-terminal escape failure should be explicit",
    )


def test_handoff_print_validation_command_rejects_extra_passthrough_args() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        payload["validation"]["natural_completion"].extend(
            ["--expected-cameras", "2010093"]
        )
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [
                str(SCRIPT),
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
    require(result.returncode != 0, "extra passthrough args should fail")
    require(
        "validation.natural_completion must exactly match" in result.stderr,
        "extra passthrough arg failure should be explicit",
    )


def test_handoff_print_validation_command_requires_mode() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        handoff.write_text(
            json.dumps(valid_handoff_payload(handoff_path=str(handoff))),
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(SCRIPT), "--handoff", str(handoff), "--print-validation-command"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "validation command printing should require mode")
    require(
        "--print-validation-command requires --stop-all or --natural-completion"
        in result.stderr,
        "missing mode failure should be explicit",
    )


def test_handoff_print_validation_command_requires_validation_payload() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        handoff = Path(tmp) / "handoff.json"
        payload = valid_handoff_payload(handoff_path=str(handoff))
        del payload["validation"]
        handoff.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [
                str(SCRIPT),
                "--handoff",
                str(handoff),
                "--print-validation-command",
                "--stop-all",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    require(result.returncode != 0, "missing validation payload should fail")
    require(
        "handoff validation must be an object" in result.stderr,
        "missing validation payload should be rejected explicitly",
    )


def test_terminal_state_requires_reason() -> None:
    result = subprocess.run(
        [str(SCRIPT), "--terminal-state", "completed", "--dry-run"],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    require(result.returncode != 0, "--terminal-state without reason should fail")
    require(
        "--terminal-state requires --reason" in result.stderr,
        "failure should explain the missing reason",
    )


def test_terminal_mode_is_required_by_default() -> None:
    result = subprocess.run(
        [str(SCRIPT), "--dry-run"],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    require(result.returncode != 0, "terminal mode should be required by default")
    require(
        "choose --stop-all" in result.stderr,
        "failure should explain the required terminal-mode choice",
    )


def test_any_terminal_escape_hatch_keeps_gate_checks() -> None:
    cmd = dry_run(["--any-terminal"])
    require("--latest-complete" in cmd, "any-terminal mode should still target latest complete")
    require(
        "--expect-local-control-stop-terminal-state" not in cmd,
        "any-terminal mode should omit terminal-state expectation",
    )
    require(
        "--expect-local-control-stop-method" in cmd,
        "any-terminal mode should still enforce Citrus completion method",
    )


def main() -> None:
    tests = [
        test_stop_all_validates_latest_complete_with_strict_citrus_gates,
        test_stop_all_mode_sets_terminal_expectations,
        test_natural_completion_mode_sets_terminal_expectations,
        test_explicit_folder_and_passthrough_args,
        test_handoff_targets_exact_recording_folder,
        test_handoff_conflicts_with_latest_targeting,
        test_handoff_requires_schema_and_folder,
        test_handoff_requires_schema_version_one,
        test_handoff_requires_generated_metadata_fields,
        test_handoff_requires_successful_readiness,
        test_handoff_rejects_stale_top_level_handoff_path,
        test_handoff_requires_status_response,
        test_handoff_rejects_mismatched_recording_folder,
        test_handoff_rejects_enabled_generic_stop_gate,
        test_handoff_prints_citrus_env_exports,
        test_handoff_print_citrus_env_requires_handoff,
        test_handoff_print_citrus_env_requires_env_payload,
        test_handoff_print_citrus_env_rejects_notify_disabled,
        test_handoff_print_citrus_env_rejects_socket_mismatch,
        test_handoff_print_citrus_env_rejects_invalid_grace_seconds,
        test_handoff_prints_validation_command,
        test_handoff_print_validation_command_rejects_stale_handoff_path,
        test_handoff_print_validation_command_rejects_mixed_terminal_flags,
        test_handoff_print_validation_command_rejects_duplicate_terminal_flag,
        test_handoff_print_validation_command_rejects_any_terminal_escape,
        test_handoff_print_validation_command_rejects_extra_passthrough_args,
        test_handoff_print_validation_command_requires_mode,
        test_handoff_print_validation_command_requires_validation_payload,
        test_terminal_state_requires_reason,
        test_terminal_mode_is_required_by_default,
        test_any_terminal_escape_hatch_keeps_gate_checks,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("validate_gui_citrus_completion_recording_tests passed")


if __name__ == "__main__":
    main()
