#!/usr/bin/env python3
"""Focused tests for update_app_config_display_profile.py."""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "update_app_config_display_profile.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_update(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(SCRIPT), *args],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_citrus_safe_profile_updates_existing_config() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"
        path.write_text(
            json.dumps(
                {
                    "schema_id": "orange.app.config",
                    "schema_version": 1,
                    "models": {"default_detect_engine": "/tmp/detect.engine"},
                    "recording": {"sink_mode": "external_ipc"},
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

        result = run_update(["--config", str(path), "--profile", "citrus_safe"])

        require(result.returncode == 0, f"update failed: {result.stderr}")
        payload = json.loads(path.read_text(encoding="utf-8"))
        require(
            payload["models"]["default_detect_engine"] == "/tmp/detect.engine",
            "existing model config should be preserved",
        )
        display = payload["gui"]["display"]
        require(display["profile"] == "citrus_safe", "profile should be citrus_safe")
        require(display["display_preview_max_fps"] == 10, "citrus-safe preview cap")
        require(display["swap_interval"] == 1, "citrus-safe swap interval")
        require(display["frame_max_fps"] == 30, "citrus-safe frame cap")


def test_explicit_values_override_profile_defaults() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "citrus_safe",
                "--display-preview-max-fps",
                "5",
                "--swap-interval",
                "0",
                "--gui-frame-max-fps",
                "20",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        display = json.loads(path.read_text(encoding="utf-8"))["gui"]["display"]
        require(display["display_preview_max_fps"] == 5, "explicit preview cap")
        require(display["swap_interval"] == 0, "explicit swap interval")
        require(display["frame_max_fps"] == 20, "explicit frame cap")


def test_dry_run_does_not_write() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(["--config", str(path), "--profile", "fast", "--dry-run"])

        require(result.returncode == 0, f"dry-run failed: {result.stderr}")
        require(not path.exists(), "dry-run should not create the target file")
        payload = json.loads(result.stdout)
        display = payload["gui"]["display"]
        require(display["profile"] == "fast", "dry-run should print requested profile")
        require(display["swap_interval"] == 0, "dry-run should print fast defaults")


def test_invalid_value_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "citrus_safe",
                "--swap-interval",
                "5",
            ]
        )

        require(result.returncode != 0, "out-of-range swap interval should fail")
        require("must be in [0,4]" in result.stderr, "failure should explain range")


def main() -> int:
    tests = [
        test_citrus_safe_profile_updates_existing_config,
        test_explicit_values_override_profile_defaults,
        test_dry_run_does_not_write,
        test_invalid_value_fails,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("update_app_config_display_profile_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
