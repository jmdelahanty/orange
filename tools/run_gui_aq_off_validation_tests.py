#!/usr/bin/env python3
"""Focused tests for the GUI AQ-off validation launcher preflight."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "run_gui_aq_off_validation.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_camera_config(
    config_dir: Path,
    serial: str,
    *,
    schema_version: int = 4,
    sync_mode: str = "ptp_gate",
    ptp_enabled: bool = True,
    aq: str = "off",
    temporal_aq: str = "off",
    source_gpu_id: int = 5,
) -> None:
    payload = {
        "schema_version": schema_version,
        "camera_serial": serial,
        "source_gpu_id": source_gpu_id,
        "sync_mode": sync_mode,
        "ptp": {
            "enabled": ptp_enabled,
            "mode": "TwoStep",
        },
        "recording": {
            "encode": {
                "aq": aq,
                "temporal_aq": temporal_aq,
            }
        },
    }
    (config_dir / f"{serial}.json").write_text(
        json.dumps(payload, indent=2) + "\n",
        encoding="utf-8")


def run_launcher(
    config_dir: Path,
    detect_engine: Path,
    *,
    expect_cameras: str = "",
    validate_only: bool = True,
    print_exec_env: bool = False,
    extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env.update(
        {
            "ORANGE_BIN": "/bin/true",
            "ORANGE_GUI_VALIDATE_ONLY": "1" if validate_only else "0",
            "ORANGE_GUI_CONFIG_DIR": str(config_dir),
            "ORANGE_GUI_DETECT_ENGINE": str(detect_engine),
            "ORANGE_GUI_APP_CONFIG_PATH": str(config_dir / "missing_app_config.json"),
            "ORANGE_GUI_EXPECT_CAMERAS": expect_cameras,
            "ORANGE_GUI_PRINT_EXEC_ENV_ONLY": "1" if print_exec_env else "0",
        }
    )
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        [str(SCRIPT)],
        cwd=REPO_ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_discovers_all_camera_json_files() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        for serial in ["2010096", "2010093", "2010095", "2010094"]:
            write_camera_config(config_dir, serial)

        result = run_launcher(config_dir, detect_engine)

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "validated camera configs: 2010093, 2010094, 2010095, 2010096" in result.stdout,
            "launcher should validate all camera JSON files in sorted order",
        )
        require(
            "ORANGE_GUI_EXPECT_CAMERAS=<all JSON files in config folder>" in result.stdout,
            "launcher output should describe the default camera discovery mode",
        )
        require(
            "ORANGE_GUI_STREAM_DOWNSAMPLE=4" in result.stdout,
            "launcher output should show the default GUI display downsample",
        )
        require(
            "ORANGE_DISPLAY_PREVIEW_MAX_FPS=15" in result.stdout,
            "launcher output should show the default display preview FPS cap",
        )
        require(
            "ORANGE_GUI_SHOW_SPEED_GRAPHS=0" in result.stdout,
            "launcher output should show the default speed graph setting",
        )
        require(
            "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=64" in result.stdout,
            "launcher output should show the default external crop queue depth",
        )
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID=<camera GPU/default>" in result.stdout,
            "launcher output should describe the default external crop recorder GPU placement",
        )
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*=<none>" in result.stdout,
            "launcher output should show no per-camera external crop recorder GPU overrides by default",
        )
        require(
            "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=0" in result.stdout,
            "launcher output should show the default external crop GPU separation gate",
        )
        require(
            "ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER=<not set>" in result.stdout,
            "launcher output should show queue high-water validation is unset by default",
        )
        require(
            "--max-external-crop-encode-queue-high-water" not in result.stdout,
            "launcher validation commands should not add a queue high-water limit by default",
        )
        require(
            "scripts/summarize_gui_validation.py --latest-complete" in result.stdout,
            "launcher output should include the compact latest-complete summary command",
        )
        require(
            "--expect-external-crop-encode-queue-depth 64" in result.stdout,
            "launcher validation commands should check the external crop queue depth",
        )
        require(
            "--require-external-crop-backend-metadata" not in result.stdout,
            "launcher should only require external crop backend metadata for external crop runs",
        )
        require(
            "--min-gui-visible-p05-fps 45" in result.stdout,
            "launcher comparison command should enforce visible GUI FPS when samples exist",
        )
        require(
            "--require-visible-samples" in result.stdout,
            "launcher comparison command should require at least one visible-sample run",
        )
        require(
            "--require-hidden-samples" in result.stdout,
            "launcher comparison command should require at least one hidden-sample run",
        )
        require(
            "--require-matching-cameras" in result.stdout,
            "launcher comparison command should require matching camera sets",
        )
        require(
            "--require-matching-display-config" in result.stdout,
            "launcher comparison command should require matching display config",
        )
        require(
            "--require-matching-crop-config" in result.stdout,
            "launcher comparison command should require matching crop config",
        )
        require(
            "--min-gui-hidden-p05-fps 45" in result.stdout,
            "launcher comparison command should enforce hidden GUI FPS when samples exist",
        )


def test_external_crop_queue_validation_limits_are_printed() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")

        result = run_launcher(
            config_dir,
            detect_engine,
            extra_env={
                "ORANGE_CROP_RECORDING_SINK_MODE": "external_ipc",
                "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH": "64",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID": "8",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095": "6",
                "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU": "1",
                "ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER": "48",
                "ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS": "80",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER=48" in result.stdout,
            "launcher output should show the selected external crop queue high-water limit",
        )
        require(
            "ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS=80" in result.stdout,
            "launcher output should show the selected external crop enqueue-age limit",
        )
        require(
            "--expect-external-crop-encode-queue-depth 64" in result.stdout,
            "launcher validation commands should use the selected external crop queue depth",
        )
        require(
            "--require-external-crop-backend-metadata" in result.stdout,
            "external crop validation commands should require backend metadata",
        )
        require(
            "--expect-external-crop-recorder-gpu-id 8" in result.stdout,
            "launcher validation commands should include the global crop recorder GPU expectation",
        )
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*=2010095=6" in result.stdout,
            "launcher output should show selected per-camera crop recorder GPU overrides",
        )
        require(
            "--expect-external-crop-recorder-gpu 2010095=6" in result.stdout,
            "launcher validation commands should include the per-camera crop recorder GPU expectation",
        )
        require(
            "--require-external-crop-recorder-gpu-separate-from-analytics" in result.stdout,
            "launcher validation commands should include the optional external crop GPU separation gate",
        )
        require(
            "--max-external-crop-encode-queue-high-water 48" in result.stdout,
            "launcher validation commands should include the selected queue high-water limit",
        )
        require(
            "--max-external-crop-enqueue-age-p95-ms 80" in result.stdout,
            "launcher validation commands should include the selected enqueue-age limit",
        )
        require(
            "--max-external-crop-queue-high-water 48" in result.stdout,
            "launcher comparison command should include the selected queue high-water limit",
        )
        require(
            "--max-external-crop-enqueue-age-p95-ms 80" in result.stdout,
            "launcher comparison command should include the selected enqueue-age limit",
        )


def test_external_crop_queue_validation_rejects_bad_values() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")

        result = run_launcher(
            config_dir,
            detect_engine,
            extra_env={
                "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH": "0",
                "ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER": "many",
                "ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS": "-1",
                "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU": "yes",
            },
        )

        require(result.returncode != 0, "bad external crop queue validation values should fail preflight")
        require(
            "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH must be >= 1" in result.stderr,
            "queue depth error should explain the minimum",
        )
        require(
            "ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER must be an integer" in result.stderr,
            "queue high-water error should explain the integer requirement",
        )
        require(
            "ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS must be >= 0" in result.stderr,
            "enqueue-age error should explain the nonnegative requirement",
        )
        require(
            "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU must be 0 or 1" in result.stderr,
            "external crop GPU separation gate should explain the boolean requirement",
        )


def test_external_crop_recorder_gpu_validation_rejects_bad_values() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")

        result = run_launcher(
            config_dir,
            detect_engine,
            extra_env={
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID": "gpu8",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095": "-1",
            },
        )

        require(result.returncode != 0, "bad external crop GPU overrides should fail preflight")
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID must be an integer" in result.stderr,
            "global crop GPU override error should explain integer requirement",
        )
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095 must be >= 0" in result.stderr,
            "per-camera crop GPU override error should explain nonnegative requirement",
        )


def test_external_crop_separate_gpu_gate_rejects_default_same_gpu() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095", source_gpu_id=5)

        result = run_launcher(
            config_dir,
            detect_engine,
            extra_env={
                "ORANGE_CROP_RECORDING_SINK_MODE": "external_ipc",
                "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU": "1",
            },
        )

        require(result.returncode != 0, "separate-GPU gate should reject default same-GPU placement")
        require(
            "external crop recorder GPU would be 5, matching source_gpu_id 5" in result.stderr,
            "separate-GPU preflight should name the same-GPU mapping",
        )
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095" in result.stderr,
            "separate-GPU preflight should suggest the per-camera override",
        )


def test_external_crop_separate_gpu_gate_accepts_override() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095", source_gpu_id=5)

        result = run_launcher(
            config_dir,
            detect_engine,
            extra_env={
                "ORANGE_CROP_RECORDING_SINK_MODE": "external_ipc",
                "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU": "1",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID": "6",
            },
        )

        require(result.returncode == 0, f"separate-GPU override should pass: {result.stderr}")
        require(
            "--require-external-crop-recorder-gpu-separate-from-analytics" in result.stdout,
            "launcher output should keep the separate-GPU validation gate",
        )


def test_external_crop_four_camera_pix_local_mapping_accepts_per_camera_overrides() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        camera_source_gpus = {
            "2010093": 3,
            "2010094": 1,
            "2010095": 7,
            "2010096": 5,
        }
        expected_crop_gpus = {
            "2010093": 4,
            "2010094": 2,
            "2010095": 8,
            "2010096": 6,
        }
        for serial, source_gpu_id in camera_source_gpus.items():
            write_camera_config(config_dir, serial, source_gpu_id=source_gpu_id)

        result = run_launcher(
            config_dir,
            detect_engine,
            expect_cameras="2010093,2010094,2010095,2010096",
            extra_env={
                "ORANGE_CROP_RECORDING_SINK_MODE": "external_ipc",
                "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU": "1",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010093": "4",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010094": "2",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095": "8",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010096": "6",
            },
        )

        require(result.returncode == 0, f"four-camera per-camera mapping should pass: {result.stderr}")
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID=<camera GPU/default>" in result.stdout,
            "launcher should not require a global crop recorder GPU override",
        )
        require(
            (
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*="
                "2010093=4,2010094=2,2010095=8,2010096=6"
            ) in result.stdout,
            "launcher output should show the selected four-camera crop recorder GPU overrides",
        )
        require(
            "--expect-external-crop-recorder-gpu-id" not in result.stdout,
            "launcher validation commands should not emit a global crop recorder GPU expectation",
        )
        for serial, expected_gpu_id in expected_crop_gpus.items():
            require(
                f"--expect-external-crop-recorder-gpu {serial}={expected_gpu_id}" in result.stdout,
                f"launcher validation commands should include the {serial} crop recorder GPU expectation",
            )
        require(
            "--require-external-crop-recorder-gpu-separate-from-analytics" in result.stdout,
            "launcher validation commands should keep the separate-GPU gate for the live run shape",
        )


def test_expected_camera_gate_fails_when_missing() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")

        result = run_launcher(config_dir, detect_engine, expect_cameras="2010095,2010096")

        require(result.returncode != 0, "missing expected camera should fail preflight")
        require("missing config:" in result.stderr, "failure should explain missing camera config")
        require("2010096.json" in result.stderr, "failure should name the missing config")


def test_expected_camera_subset_validates_only_requested_files() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")
        write_camera_config(config_dir, "2010096", aq="on")

        result = run_launcher(config_dir, detect_engine, expect_cameras="2010095")

        require(result.returncode == 0, f"launcher should ignore unrequested configs: {result.stderr}")
        require(
            "validated camera configs: 2010095" in result.stdout,
            "launcher should report only explicitly requested configs",
        )


def test_bad_config_fails_preflight() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095", temporal_aq="on")

        result = run_launcher(config_dir, detect_engine)

        require(result.returncode != 0, "invalid recording defaults should fail preflight")
        require(
            "recording.encode.temporal_aq is not 'off'" in result.stderr,
            "failure should explain the stale recording default",
        )


def test_crop_preview_env_controls_are_forwarded_to_exec_env() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")

        result = run_launcher(
            config_dir,
            detect_engine,
            validate_only=False,
            print_exec_env=True,
            extra_env={
                "ORANGE_CROP_PREVIEW_MAX_FPS": "30",
                "ORANGE_CROP_PREVIEW_DISABLE": "1",
                "ORANGE_CROP_FRAME_POOL_SIZE": "64",
                "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH": "64",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID": "8",
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095": "6",
                "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU": "1",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "ORANGE_CROP_PREVIEW_MAX_FPS=30" in result.stdout,
            "crop preview max FPS should be forwarded through sudo env",
        )
        require(
            "ORANGE_CROP_PREVIEW_DISABLE=1" in result.stdout,
            "crop preview disable flag should be forwarded through sudo env",
        )
        require(
            "ORANGE_CROP_FRAME_POOL_SIZE=64" in result.stdout,
            "crop frame pool size should be forwarded through sudo env",
        )
        require(
            "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=64" in result.stdout,
            "external crop encode queue depth should be forwarded through sudo env",
        )
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID=8" in result.stdout,
            "global external crop recorder GPU override should be forwarded through sudo env",
        )
        require(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095=6" in result.stdout,
            "per-camera external crop recorder GPU override should be forwarded through sudo env",
        )
        require(
            "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=1" in result.stdout,
            "external crop separate-GPU gate should be forwarded through sudo env",
        )


def test_display_env_controls_are_forwarded_to_exec_env() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")

        result = run_launcher(
            config_dir,
            detect_engine,
            validate_only=False,
            print_exec_env=True,
            extra_env={
                "ORANGE_GUI_STREAM_DOWNSAMPLE": "8",
                "ORANGE_DISPLAY_PREVIEW_MAX_FPS": "20",
                "ORANGE_GUI_SHOW_SPEED_GRAPHS": "1",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "ORANGE_GUI_STREAM_DOWNSAMPLE=8" in result.stdout,
            "GUI stream downsample should be forwarded through sudo env",
        )
        require(
            "ORANGE_DISPLAY_PREVIEW_MAX_FPS=20" in result.stdout,
            "display preview FPS cap should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_SHOW_SPEED_GRAPHS=1" in result.stdout,
            "YOLO speed graph toggle should be forwarded through sudo env",
        )


def main() -> int:
    tests = [
        test_discovers_all_camera_json_files,
        test_external_crop_queue_validation_limits_are_printed,
        test_external_crop_queue_validation_rejects_bad_values,
        test_external_crop_recorder_gpu_validation_rejects_bad_values,
        test_external_crop_separate_gpu_gate_rejects_default_same_gpu,
        test_external_crop_separate_gpu_gate_accepts_override,
        test_external_crop_four_camera_pix_local_mapping_accepts_per_camera_overrides,
        test_expected_camera_gate_fails_when_missing,
        test_expected_camera_subset_validates_only_requested_files,
        test_bad_config_fails_preflight,
        test_crop_preview_env_controls_are_forwarded_to_exec_env,
        test_display_env_controls_are_forwarded_to_exec_env,
    ]
    for test in tests:
        test()
    print("run_gui_aq_off_validation_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
