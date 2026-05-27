#!/usr/bin/env python3
"""Lightweight checks for external recorder smoke runner CLIs."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RUNNERS = [
    REPO_ROOT / "scripts" / "run_external_recorder_smoke.sh",
    REPO_ROOT / "scripts" / "run_external_recorder_two_camera_ptp_smoke.sh",
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_runner_shell_syntax() -> None:
    for runner in RUNNERS:
        result = run(["bash", "-n", str(runner)])
        require(result.returncode == 0, f"shell syntax failed for {runner}: {result.stderr}")


def test_runner_help_lists_queue_verifier_options() -> None:
    for runner in RUNNERS:
        result = run([str(runner), "--help"])
        require(result.returncode == 0, f"--help failed for {runner}: {result.stderr}")
        require(
            "--max-encode-queue-high-water" in result.stdout,
            f"{runner.name} help should expose queue high-water verifier threshold",
        )
        require(
            "--max-enqueue-age-p95-ms" in result.stdout,
            f"{runner.name} help should expose enqueue-age verifier threshold",
        )


def main() -> int:
    tests = [
        test_runner_shell_syntax,
        test_runner_help_lists_queue_verifier_options,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("external_recorder_smoke_runner_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
