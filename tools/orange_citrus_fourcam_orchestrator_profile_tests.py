#!/usr/bin/env python3
"""Focused tests for the Orange/Citrus four-camera orchestrator profile."""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "run_orange_citrus_fourcam_orchestrator.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_profile(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(SCRIPT), *args],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_default_dry_run_builds_live_profile() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        summary_path = Path(tmp) / "summary.json"
        result = run_profile(
            [
                "--operation-id",
                "profile-dry",
                "--summary-json",
                str(summary_path),
                "--display",
                ":77",
                "--xauthority",
                "/tmp/xauthority-test",
                "--xdg-runtime-dir",
                "/tmp/runtime-test",
                "--xdg-session-type",
                "x11",
            ]
        )
        require(result.returncode == 0, f"profile dry-run failed: {result.stderr}")
        payload = json.loads(result.stdout)

        require(payload["result"] == "dry_run", "profile default should be dry-run")
        require(payload["operation_id"] == "profile-dry", "operation id should pass through")
        require(summary_path.exists(), "profile should write requested summary")

        orange = payload["orange"]
        citrus = payload["citrus"]
        require(
            orange["log_path"] == "/tmp/profile-dry_orange.log",
            "profile should default Orange log to an operation-specific path",
        )
        require(
            citrus["log_path"] == "/tmp/profile-dry_citrus.log",
            "profile should default Citrus log to an operation-specific path",
        )
        require(
            orange["command"]
            == [
                str(REPO_ROOT / "scripts" / "run_gui_fourcam_external_ipc_validation.sh"),
                "--citrus-display-safe",
            ],
            "profile should launch Orange with the Citrus-safe four-camera command",
        )
        require(
            citrus["command"] == ["/home/jeremy/citrus/targets/citrus"],
            "profile should launch the local Citrus GUI binary by default",
        )
        require(orange["preflight_existing_socket"], "profile should preflight Orange launch socket")
        require(citrus["preflight_existing_socket"], "profile should preflight Citrus launch socket")

        orange_env = orange["env_overlay"]
        citrus_env = citrus["env_overlay"]
        for env in (orange_env, citrus_env):
            require(env["DISPLAY"] == ":77", "display should pass through")
            require(env["XAUTHORITY"] == "/tmp/xauthority-test", "xauthority should pass through")
            require(env["XDG_RUNTIME_DIR"] == "/tmp/runtime-test", "runtime dir should pass through")
            require(env["XDG_SESSION_TYPE"] == "x11", "session type should pass through")

        require(
            orange_env["ORANGE_GUI_AUTORUN_START_RECORDING"] == "0",
            "orchestrator should keep Orange autorun in stream-only mode",
        )
        require(
            orange_env["ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE"] == "0",
            "orchestrator should not use autorun finalize as its exit trigger",
        )
        require(
            orange_env["ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE"] == "1",
            "orchestrator should close launched Orange after local-control finalization",
        )
        require(
            citrus_env["CITRUS_GUI_AUTORUN"] == "1",
            "profile should use Citrus autorun as the setup loader",
        )
        require(
            citrus_env["CITRUS_GUI_AUTORUN_START_DELAY_SECONDS"] == "86400",
            "profile should keep Citrus autorun from racing local-control start",
        )
        require(
            citrus_env["CITRUS_GUI_AUTORUN_RIG"] == "omnifin0",
            "profile should default to the omnifin0 rig",
        )
        require(
            citrus_env["CITRUS_GUI_AUTORUN_CANVAS"] == "shadow",
            "profile should default to the shadow canvas",
        )
        require(
            citrus_env["CITRUS_GUI_AUTORUN_PROTOCOL"] == "good_cop_bad_cop_demo.json",
            "profile should default to the GoodCop/BadCop demo protocol",
        )
        require(
            citrus_env["CITRUS_PERF_JSONL"] == "1",
            "profile should require Citrus perf JSONL by default",
        )
        require(
            citrus_env["CITRUS_ORANGE_COMPLETION_NOTIFY"] == "0",
            "profile should keep Citrus completion notifier disabled by default",
        )
        require(len(payload["validations"]) == 1, "profile should include default Orange validation")
        validation = payload["validations"][0]
        require(validation["label"] == "orange_validation_1", "default validator should be Orange labeled")
        require(
            validation["artifact_paths"] == ["/tmp/profile-dry_orange_gui_validation.json"],
            "profile should preserve Orange validator JSON as an orchestrator artifact",
        )
        require(
            "validate_gui_ptp_recording.py" in validation["command"],
            "default validator should run the GUI PTP validator",
        )
        require(
            "{orange_recording_folder}" in validation["command"],
            "default validator should target the exact Orange recording folder placeholder",
        )
        require(
            "--latest-complete" not in validation["command"],
            "default validator should not rely on newest-artifact discovery",
        )
        require(
            "--expect-gui-frame-max-fps 30" in validation["command"],
            "default validator should match the Citrus-safe frame cap",
        )
        require(
            "--expect-display-preview-max-fps 10" in validation["command"],
            "default validator should match the Citrus-safe display preview cap",
        )
        require(
            "--expect-external-crop-encode-queue-depth 128" in validation["command"],
            "default validator should match the four-camera crop external queue",
        )
        require(
            "--min-crop-frame-pool-size 256" in validation["command"],
            "default validator should match the derived four-camera crop frame pool",
        )


def test_attach_mode_does_not_launch_processes() -> None:
    result = run_profile(
        [
            "--operation-id",
            "profile-attach",
            "--attach-orange",
            "--attach-citrus",
            "--allow-missing-citrus-perf-jsonl",
            "--skip-orange-validation",
            "--orange-socket",
            "/tmp/orange-attach.sock",
            "--citrus-socket",
            "/tmp/citrus-attach.sock",
        ]
    )
    require(result.returncode == 0, f"profile attach dry-run failed: {result.stderr}")
    payload = json.loads(result.stdout)
    require(payload["orange"]["command"] == [], "attach mode should not launch Orange")
    require(payload["citrus"]["command"] == [], "attach mode should not launch Citrus")
    require(
        payload["orange"]["socket"] == "/tmp/orange-attach.sock",
        "Orange attach socket should pass through",
    )
    require(
        payload["citrus"]["socket"] == "/tmp/citrus-attach.sock",
        "Citrus attach socket should pass through",
    )
    require(
        "CITRUS_GUI_AUTORUN" not in payload["citrus"]["env_overlay"],
        "attach mode should not describe Citrus loader envs",
    )
    require(
        "CITRUS_PERF_JSONL" not in payload["citrus"]["env_overlay"],
        "allow-missing perf option should avoid injecting Citrus perf env",
    )
    require(payload["validations"] == [], "skip validation should avoid profile validation commands")


def test_allow_preexisting_sockets_disables_launch_preflight() -> None:
    result = run_profile(
        [
            "--operation-id",
            "profile-allow-preexisting",
            "--allow-preexisting-sockets",
            "--skip-orange-validation",
        ]
    )
    require(result.returncode == 0, f"profile allow-preexisting dry-run failed: {result.stderr}")
    payload = json.loads(result.stdout)
    require(
        not payload["orange"]["preflight_existing_socket"],
        "profile override should disable Orange socket preflight",
    )
    require(
        not payload["citrus"]["preflight_existing_socket"],
        "profile override should disable Citrus socket preflight",
    )


def main() -> int:
    tests = [
        test_default_dry_run_builds_live_profile,
        test_attach_mode_does_not_launch_processes,
        test_allow_preexisting_sockets_disables_launch_preflight,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("orange_citrus_fourcam_orchestrator_profile_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
