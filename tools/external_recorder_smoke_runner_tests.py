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


def test_one_camera_runner_defaults_to_supervised_full_rate_split_gop() -> None:
    runner = RUNNERS[0]
    source = runner.read_text(encoding="utf-8")
    require('ENCODE_FPS=100' in source, "one-camera smoke must default to 100 FPS")
    require('ENCODE_MAX_FPS=0' in source, "one-camera smoke must default to uncapped encode")
    require('QUEUE_DEPTH=32' in source, "one-camera smoke must default to a GOP-burst-safe queue")
    require(
        'SHARD_GPU_IDS="5,6"' in source,
        "default camera/GPU topology must materialize two split-GOP shards",
    )
    require(
        '"supervise_processes": True' in source,
        "Orange must supervise the recorder lifecycle",
    )
    require(
        'fixed["recording_control"]' in source,
        "smoke must request a timed single-clip recording so Orange writes the manifest",
    )
    require(
        '"clip_seconds": 0' in source,
        "one-camera acceptance smoke must remain non-rolling",
    )
    require(
        '"$RECORDER_TOOL" "${RECORDER_ARGS[@]}"' not in source,
        "runner must not independently launch a competing recorder process",
    )


def main() -> int:
    tests = [
        test_runner_shell_syntax,
        test_runner_help_lists_queue_verifier_options,
        test_one_camera_runner_defaults_to_supervised_full_rate_split_gop,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("external_recorder_smoke_runner_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
