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


def test_stream_and_telemetry_options_update_config() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"
        path.write_text(
            json.dumps(
                {
                    "schema_id": "orange.app.config",
                    "schema_version": 1,
                    "gui": {
                        "stream": {"downsample": 2},
                        "telemetry": {"show_speed_graphs": True},
                    },
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "fast",
                "--stream-downsample",
                "4",
                "--hide-speed-graphs",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        gui = json.loads(path.read_text(encoding="utf-8"))["gui"]
        require(gui["stream"]["downsample"] == 4, "stream downsample should update")
        require(
            gui["telemetry"]["show_speed_graphs"] is False,
            "speed graphs should be hidden",
        )


def test_local_control_options_update_config() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"
        path.write_text(
            json.dumps(
                {
                    "schema_id": "orange.app.config",
                    "schema_version": 1,
                    "gui": {
                        "local_control": {
                            "recording_start_enabled": True,
                            "recording_stop_enabled": False,
                            "drain_timeout_seconds": 60,
                        }
                    },
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "citrus_safe",
                "--disable-local-control-recording-start",
                "--enable-local-control-recording-stop",
                "--enable-citrus-completion-stop",
                "--enable-local-control-exit-after-finalize",
                "--local-control-drain-timeout-seconds",
                "75",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        local_control = json.loads(path.read_text(encoding="utf-8"))["gui"][
            "local_control"
        ]
        require(
            local_control["recording_start_enabled"] is False,
            "local-control start should disable",
        )
        require(
            local_control["recording_stop_enabled"] is True,
            "local-control stop should enable",
        )
        require(
            local_control["citrus_completion_stop_enabled"] is True,
            "Citrus completion stop should enable",
        )
        require(
            local_control["exit_after_finalize"] is True,
            "exit-after-finalize should enable",
        )
        require(
            local_control["drain_timeout_seconds"] == 75,
            "drain timeout should update",
        )


def test_manual_citrus_completion_control_profile_updates_config() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"
        path.write_text(
            json.dumps(
                {
                    "schema_id": "orange.app.config",
                    "schema_version": 1,
                    "gui": {
                        "local_control": {
                            "recording_start_enabled": True,
                            "recording_stop_enabled": True,
                            "citrus_completion_stop_enabled": False,
                            "exit_after_finalize": True,
                            "drain_timeout_seconds": 5,
                        }
                    },
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "citrus_safe",
                "--manual-citrus-completion-control",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        local_control = json.loads(path.read_text(encoding="utf-8"))["gui"][
            "local_control"
        ]
        require(
            local_control["recording_start_enabled"] is False,
            "manual profile should disable socket recording start",
        )
        require(
            local_control["recording_stop_enabled"] is False,
            "manual profile should disable generic stop_recording",
        )
        require(
            local_control["citrus_completion_stop_enabled"] is True,
            "manual profile should enable Citrus completion stop",
        )
        require(
            local_control["exit_after_finalize"] is False,
            "manual profile should keep GUI open after finalization",
        )
        require(
            local_control["drain_timeout_seconds"] == 60,
            "manual profile should set the default drain timeout",
        )


def test_manual_citrus_completion_control_allows_explicit_drain_timeout() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "citrus_safe",
                "--manual-citrus-completion-control",
                "--local-control-drain-timeout-seconds",
                "90",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        local_control = json.loads(path.read_text(encoding="utf-8"))["gui"][
            "local_control"
        ]
        require(
            local_control["citrus_completion_stop_enabled"] is True,
            "manual profile should still enable Citrus completion stop",
        )
        require(
            local_control["drain_timeout_seconds"] == 90,
            "explicit drain timeout should override the manual profile default",
        )


def test_clear_local_control_drain_timeout() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "fast",
                "--clear-local-control-drain-timeout-seconds",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        local_control = json.loads(path.read_text(encoding="utf-8"))["gui"][
            "local_control"
        ]
        require(
            local_control["drain_timeout_seconds"] is None,
            "drain timeout should clear to null",
        )


def test_crop_options_update_config() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"
        path.write_text(
            json.dumps(
                {
                    "schema_id": "orange.app.config",
                    "schema_version": 1,
                    "recording": {
                        "sink_mode": "external_ipc",
                        "crop": {
                            "sink_mode": "in_process",
                            "frame_pool_size": None,
                            "external_ipc": {"encode_queue_depth": 64},
                        },
                    },
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "citrus_safe",
                "--crop-recording-sink-mode",
                "external_ipc",
                "--crop-external-encode-queue-depth",
                "128",
                "--crop-external-recorder-gpu-id",
                "4",
                "--crop-external-recorder-gpu",
                "2010095=8",
                "--crop-external-recorder-gpu",
                "2010096=6",
                "--crop-frame-pool-size",
                "256",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        crop = json.loads(path.read_text(encoding="utf-8"))["recording"]["crop"]
        require(crop["sink_mode"] == "external_ipc", "crop sink mode should update")
        require(crop["frame_pool_size"] == 256, "crop frame pool should update")
        require(
            crop["external_ipc"]["encode_queue_depth"] == 128,
            "crop external queue depth should update",
        )
        require(
            crop["external_ipc"]["recorder_gpu_id"] == 4,
            "crop external global recorder GPU should update",
        )
        require(
            crop["external_ipc"]["recorder_gpu_ids_by_serial"] == {
                "2010095": 8,
                "2010096": 6,
            },
            "crop external per-camera recorder GPUs should update",
        )


def test_clear_crop_recorder_gpu_config() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"
        path.write_text(
            json.dumps(
                {
                    "schema_id": "orange.app.config",
                    "schema_version": 1,
                    "recording": {
                        "crop": {
                            "external_ipc": {
                                "recorder_gpu_id": 4,
                                "recorder_gpu_ids_by_serial": {
                                    "2010093": 4,
                                    "2010094": 2,
                                },
                            }
                        }
                    },
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "citrus_safe",
                "--clear-crop-external-recorder-gpu-id",
                "--clear-crop-external-recorder-gpus",
                "--crop-external-recorder-gpu",
                "2010095=8",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        external_ipc = json.loads(path.read_text(encoding="utf-8"))["recording"]["crop"][
            "external_ipc"
        ]
        require(
            external_ipc["recorder_gpu_id"] is None,
            "global crop recorder GPU should clear to null",
        )
        require(
            external_ipc["recorder_gpu_ids_by_serial"] == {"2010095": 8},
            "per-camera crop recorder GPUs should clear then apply new entry",
        )


def test_clear_crop_frame_pool_size() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "fast",
                "--clear-crop-frame-pool-size",
            ]
        )

        require(result.returncode == 0, f"update failed: {result.stderr}")
        crop = json.loads(path.read_text(encoding="utf-8"))["recording"]["crop"]
        require(crop["frame_pool_size"] is None, "crop frame pool should clear to null")


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


def test_invalid_stream_downsample_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "fast",
                "--stream-downsample",
                "3",
            ]
        )

        require(result.returncode != 0, "invalid downsample should fail")
        require(
            "must be one of 1, 2, 4, 8, or 16" in result.stderr,
            "failure should explain allowed values",
        )


def test_invalid_local_control_drain_timeout_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "fast",
                "--local-control-drain-timeout-seconds",
                "86401",
            ]
        )

        require(result.returncode != 0, "invalid drain timeout should fail")
        require("must be in [0,86400]" in result.stderr, "failure should explain range")


def test_invalid_crop_queue_depth_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "fast",
                "--crop-external-encode-queue-depth",
                "0",
            ]
        )

        require(result.returncode != 0, "invalid crop queue depth should fail")
        require("must be in [1,4096]" in result.stderr, "failure should explain range")


def test_invalid_crop_recorder_gpu_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "default.json"

        result = run_update(
            [
                "--config",
                str(path),
                "--profile",
                "fast",
                "--crop-external-recorder-gpu",
                "2010095=256",
            ]
        )

        require(result.returncode != 0, "invalid crop recorder GPU should fail")
        require("GPU must be in [0,255]" in result.stderr, "failure should explain range")


def main() -> int:
    tests = [
        test_citrus_safe_profile_updates_existing_config,
        test_explicit_values_override_profile_defaults,
        test_stream_and_telemetry_options_update_config,
        test_local_control_options_update_config,
        test_manual_citrus_completion_control_profile_updates_config,
        test_manual_citrus_completion_control_allows_explicit_drain_timeout,
        test_clear_local_control_drain_timeout,
        test_crop_options_update_config,
        test_clear_crop_recorder_gpu_config,
        test_clear_crop_frame_pool_size,
        test_dry_run_does_not_write,
        test_invalid_value_fails,
        test_invalid_stream_downsample_fails,
        test_invalid_local_control_drain_timeout_fails,
        test_invalid_crop_queue_depth_fails,
        test_invalid_crop_recorder_gpu_fails,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("update_app_config_display_profile_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
