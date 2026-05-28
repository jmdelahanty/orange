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
GUI_WRAPPER_SCRIPT = REPO_ROOT / "scripts" / "orange_gui_validation_wrapper.sh"


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
            "ORANGE_YOLO_AFFINITY_CAM_*=2010095=10,2010096=12" in result.stdout,
            "launcher output should show the Citrus-safe default two-camera YOLO affinities",
        )
        require(
            "ORANGE_GUI_REQUIRE_ISOLATED_CPUS=<not set>" in result.stdout,
            "launcher output should show isolated CPU validation is unset by default",
        )
        require(
            "--expect-yolo-affinity 2010095=10" in result.stdout
            and "--expect-yolo-affinity 2010096=12" in result.stdout,
            "launcher validation commands should check default YOLO affinity telemetry",
        )
        require(
            "--require-isolated-cpus" not in result.stdout,
            "launcher validation commands should not require isolated CPUs by default",
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
            "ORANGE_GUI_SWAP_INTERVAL=0" in result.stdout,
            "launcher output should show the default GUI swap interval",
        )
        require(
            "ORANGE_GUI_FRAME_MAX_FPS=60" in result.stdout,
            "launcher output should show the default GUI frame cap",
        )
        require(
            "ORANGE_GUI_SHOW_SPEED_GRAPHS=0" in result.stdout,
            "launcher output should show the default speed graph setting",
        )
        require(
            "ORANGE_GUI_AUTORUN=0" in result.stdout,
            "launcher output should show autorun disabled by default",
        )
        require(
            "ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=3" in result.stdout,
            "launcher output should show the default autorun stream warmup",
        )
        require(
            "ORANGE_GUI_AUTORUN_RECORD_SECONDS=10" in result.stdout,
            "launcher output should show the default autorun recording duration",
        )
        require(
            "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=0" in result.stdout,
            "launcher output should show autorun exit disabled by default",
        )
        require(
            "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=0" in result.stdout,
            "launcher output should show autorun crop preview hiding disabled by default",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_STREAM=1" in result.stdout,
            "launcher output should show autorun streaming enabled by default",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_RECORD=1" in result.stdout,
            "launcher output should show autorun recording enabled by default",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_YOLO=1" in result.stdout,
            "launcher output should show autorun YOLO enabled by default",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_CROP=1" in result.stdout,
            "launcher output should show autorun crop recording enabled by default",
        )
        require(
            "ORANGE_GUI_RECORD_FOR_SECONDS=<app config/disabled>" in result.stdout,
            "launcher output should show GUI recording control disabled by default",
        )
        require(
            "ORANGE_GUI_CLIP_SECONDS=<app config/disabled>" in result.stdout,
            "launcher output should show GUI clip rollover disabled by default",
        )
        require(
            "ORANGE_GUI_PTP_STACK_MODE=off" in result.stdout,
            "launcher output should show host PTP preflight off by default for manual runs",
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
            "ORANGE_GUI_USE_PRIVILEGE_WRAPPER=auto" in result.stdout,
            "launcher output should show privilege-wrapper auto mode by default",
        )
        require(
            "ORANGE_GUI_PRIVILEGE_WRAPPER=/usr/local/bin/orange-gui-validation" in result.stdout,
            "launcher output should show the default GUI privilege wrapper path",
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
            "ORANGE_GUI_REQUIRE_SOURCE_VERSION=0" in result.stdout,
            "launcher should show source-version validation is off by default",
        )
        require(
            "--require-source-version" not in result.stdout,
            "launcher should not require source provenance unless requested",
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
            "--require-matching-yolo-runtime-config" in result.stdout,
            "launcher comparison command should require matching YOLO runtime config",
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
            "--require-external-recorder-status" in result.stdout,
            "external recorder validation commands should require status sidecars",
        )
        require(
            "--require-external-recorder-storage-preflight" in result.stdout,
            "external recorder validation commands should require storage preflight telemetry",
        )
        require(
            "--require-external-recorder-protocol-hello" in result.stdout,
            "external recorder validation commands should require protocol hello telemetry",
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


def test_source_version_validation_flags_are_printed() -> None:
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
                "ORANGE_GUI_REQUIRE_SOURCE_VERSION": "1",
                "ORANGE_GUI_EXPECT_SOURCE_GIT_COMMAND_USER_MODE": "sudo_invoking_user",
                "ORANGE_GUI_EXPECT_SOURCE_DIRTY_TRACKED": "1",
                "ORANGE_GUI_REQUIRE_ISOLATED_CPUS": "6,10",
                "ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS": "6,10,38,40",
                "ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_OPTIONS": "isolcpus,nohz_full,rcu_nocbs",
                "ORANGE_GUI_MAX_YOLO_ENQUEUE_TO_DEQUEUE_P95_MS": "0.5",
                "ORANGE_GUI_MAX_YOLO_SAME_CAMERA_SERVICE_GAP_P95_MS": "15",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "ORANGE_GUI_REQUIRE_SOURCE_VERSION=1" in result.stdout,
            "launcher output should show source-version validation is enabled",
        )
        require(
            "ORANGE_GUI_EXPECT_SOURCE_GIT_COMMAND_USER_MODE=sudo_invoking_user" in result.stdout,
            "launcher output should show expected git command user mode",
        )
        require(
            "ORANGE_GUI_EXPECT_SOURCE_DIRTY_TRACKED=1" in result.stdout,
            "launcher output should show expected tracked dirty-state",
        )
        require(
            "--require-source-version" in result.stdout,
            "validation commands should require source provenance",
        )
        require(
            "--expect-source-git-command-user-mode sudo_invoking_user" in result.stdout,
            "validation commands should check git command user mode",
        )
        require(
            "--expect-source-dirty-tracked 1" in result.stdout,
            "validation commands should check tracked dirty-state",
        )
        require(
            "ORANGE_GUI_REQUIRE_ISOLATED_CPUS=6,10" in result.stdout,
            "launcher output should show isolated CPU validation when requested",
        )
        require(
            "--require-isolated-cpus 6,10" in result.stdout,
            "validation commands should check requested isolated CPUs",
        )
        require(
            "ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS=6,10,38,40" in result.stdout,
            "launcher output should show required kernel cmdline CPUs",
        )
        for option in ("isolcpus", "nohz_full", "rcu_nocbs"):
            require(
                f"--require-kernel-cmdline-cpus {option}=6,10,38,40" in result.stdout,
                f"validation commands should check {option} kernel CPU list",
            )
        require(
            "ORANGE_GUI_MAX_YOLO_ENQUEUE_TO_DEQUEUE_P95_MS=0.5" in result.stdout,
            "launcher output should show YOLO enqueue/dequeue validation threshold",
        )
        require(
            "--max-yolo-enqueue-to-dequeue-p95-ms 0.5" in result.stdout,
            "validation commands should check YOLO enqueue/dequeue latency",
        )
        require(
            "--max-yolo-same-camera-service-gap-p95-ms 15" in result.stdout,
            "validation commands should check YOLO service-gap latency",
        )


def test_main_video_content_failure_validation_flags_are_printed() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010093")

        result = run_launcher(
            config_dir,
            detect_engine,
            extra_env={
                "ORANGE_GUI_ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS": "2010093",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "ORANGE_GUI_ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS=2010093" in result.stdout,
            "launcher output should show explicitly allowed main-video content failures",
        )
        require(
            "--allow-main-video-content-failure 2010093" in result.stdout,
            "validation commands should pass through allowed main-video content failures",
        )


def test_rolling_recording_validation_flags_are_printed() -> None:
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
                "ORANGE_GUI_AUTORUN": "1",
                "ORANGE_GUI_AUTORUN_RECORD_SECONDS": "7",
                "ORANGE_GUI_CLIP_SECONDS": "2",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "--expect-recording-mode rolling_clips" in result.stdout,
            "validation commands should require rolling mode when GUI clip_seconds is set",
        )
        require(
            "--expect-record-for-seconds 7" in result.stdout,
            "validation commands should use autorun record seconds for rolling record_for_seconds",
        )
        require(
            "--expect-clip-seconds 2" in result.stdout,
            "validation commands should check GUI clip_seconds",
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
                "ORANGE_GUI_AUTORUN": "maybe",
                "ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS": "-1",
                "ORANGE_GUI_AUTORUN_RECORD_SECONDS": "0",
                "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE": "yes",
                "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW": "nope",
                "ORANGE_GUI_AUTORUN_ENABLE_STREAM": "maybe",
                "ORANGE_GUI_AUTORUN_ENABLE_RECORD": "maybe",
                "ORANGE_GUI_AUTORUN_ENABLE_YOLO": "maybe",
                "ORANGE_GUI_AUTORUN_ENABLE_CROP": "maybe",
                "ORANGE_GUI_RECORD_FOR_SECONDS": "-1",
                "ORANGE_GUI_CLIP_SECONDS": "many",
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
        require(
            "ORANGE_GUI_AUTORUN must be 0 or 1" in result.stderr,
            "autorun enable flag should explain the boolean requirement",
        )
        require(
            "ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS must be >= 0" in result.stderr,
            "autorun warmup error should explain the nonnegative requirement",
        )
        require(
            "ORANGE_GUI_AUTORUN_RECORD_SECONDS must be >= 1" in result.stderr,
            "autorun record duration error should explain the positive requirement",
        )
        require(
            "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE must be 0 or 1" in result.stderr,
            "autorun exit flag should explain the boolean requirement",
        )
        require(
            "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW must be 0 or 1" in result.stderr,
            "autorun crop preview flag should explain the boolean requirement",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_STREAM must be 0 or 1" in result.stderr,
            "autorun stream enable flag should explain the boolean requirement",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_RECORD must be 0 or 1" in result.stderr,
            "autorun record enable flag should explain the boolean requirement",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_YOLO must be 0 or 1" in result.stderr,
            "autorun YOLO enable flag should explain the boolean requirement",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_CROP must be 0 or 1" in result.stderr,
            "autorun crop enable flag should explain the boolean requirement",
        )
        require(
            "ORANGE_GUI_RECORD_FOR_SECONDS must be >= 0" in result.stderr,
            "GUI record_for_seconds override should explain the nonnegative requirement",
        )
        require(
            "ORANGE_GUI_CLIP_SECONDS must be an integer" in result.stderr,
            "GUI clip_seconds override should explain the integer requirement",
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


def test_yolo_worker_scheduling_env_controls_are_forwarded_to_exec_env() -> None:
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
                "ORANGE_YOLO_AFFINITY_CAM_2010093": "6",
                "ORANGE_YOLO_AFFINITY_CAM_2010095": "10",
                "ORANGE_YOLO_RT_PRIORITY": "10",
                "ORANGE_YOLO_RT_PRIORITY_CAM_2010093": "12",
                "ORANGE_YOLO_RT_POLICY": "fifo",
                "ORANGE_RECORDING_DETECT_PRIORITY": "1",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "ORANGE_YOLO_AFFINITY_CAM_2010093=6" in result.stdout,
            "per-camera YOLO affinity should be forwarded through sudo env",
        )
        require(
            "ORANGE_YOLO_AFFINITY_CAM_2010095=10" in result.stdout,
            "default two-camera YOLO affinity should honor overrides",
        )
        require(
            "ORANGE_YOLO_RT_PRIORITY=10" in result.stdout,
            "YOLO realtime priority should be forwarded through sudo env",
        )
        require(
            "ORANGE_YOLO_RT_PRIORITY_CAM_2010093=12" in result.stdout,
            "per-camera YOLO realtime priority should be forwarded through sudo env",
        )
        require(
            "ORANGE_YOLO_RT_POLICY=fifo" in result.stdout,
            "YOLO realtime policy should be forwarded through sudo env",
        )
        require(
            "ORANGE_RECORDING_DETECT_PRIORITY=1" in result.stdout,
            "recording detect-priority flag should be forwarded through sudo env",
        )


def test_external_crop_ipc_autosizes_crop_frame_pool() -> None:
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
                "ORANGE_CROP_RECORDING_SINK_MODE": "external_ipc",
                "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH": "64",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        require(
            "ORANGE_CROP_FRAME_POOL_SIZE=128" in result.stdout,
            "external crop IPC should auto-forward a crop pool sized above the encode queue",
        )

        validation_result = run_launcher(
            config_dir,
            detect_engine,
            extra_env={
                "ORANGE_CROP_RECORDING_SINK_MODE": "external_ipc",
                "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH": "64",
            },
        )

        require(validation_result.returncode == 0, f"launcher failed: {validation_result.stderr}")
        require(
            "ORANGE_CROP_FRAME_POOL_SIZE=128 (auto for external_ipc)" in validation_result.stdout,
            "launcher output should explain the auto-sized external crop pool",
        )
        require(
            "--min-crop-frame-pool-size 128" in validation_result.stdout,
            "validation commands should require the auto-sized crop pool",
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
                "DISPLAY": ":77",
                "XAUTHORITY": "/tmp/orange_test.Xauthority",
                "WAYLAND_DISPLAY": "wayland-test",
                "XDG_RUNTIME_DIR": "/run/user/1000",
                "XDG_SESSION_TYPE": "wayland",
                "ORANGE_GUI_STREAM_DOWNSAMPLE": "8",
                "ORANGE_DISPLAY_PREVIEW_MAX_FPS": "20",
                "ORANGE_GUI_SWAP_INTERVAL": "1",
                "ORANGE_GUI_FRAME_MAX_FPS": "30",
                "ORANGE_GUI_SHOW_SPEED_GRAPHS": "1",
                "ORANGE_GUI_AUTORUN": "1",
                "ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS": "2",
                "ORANGE_GUI_AUTORUN_RECORD_SECONDS": "7",
                "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE": "1",
                "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW": "1",
                "ORANGE_GUI_AUTORUN_ENABLE_STREAM": "1",
                "ORANGE_GUI_AUTORUN_ENABLE_RECORD": "1",
                "ORANGE_GUI_AUTORUN_ENABLE_YOLO": "1",
                "ORANGE_GUI_AUTORUN_ENABLE_CROP": "1",
                "ORANGE_GUI_RECORD_FOR_SECONDS": "7",
                "ORANGE_GUI_CLIP_SECONDS": "2",
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
            "ORANGE_GUI_SWAP_INTERVAL=1" in result.stdout,
            "GUI swap interval should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_FRAME_MAX_FPS=30" in result.stdout,
            "GUI frame cap should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_SHOW_SPEED_GRAPHS=1" in result.stdout,
            "YOLO speed graph toggle should be forwarded through sudo env",
        )
        require(
            f"ORANGE_GUI_CONFIG_DIR={config_dir}" in result.stdout,
            "GUI config folder should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN=1" in result.stdout,
            "GUI autorun enable flag should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=2" in result.stdout,
            "GUI autorun warmup duration should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN_RECORD_SECONDS=7" in result.stdout,
            "GUI autorun recording duration should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=1" in result.stdout,
            "GUI autorun exit flag should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=1" in result.stdout,
            "GUI autorun crop-preview flag should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_STREAM=1" in result.stdout,
            "GUI autorun stream-enable flag should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_RECORD=1" in result.stdout,
            "GUI autorun record-enable flag should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_YOLO=1" in result.stdout,
            "GUI autorun YOLO-enable flag should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_AUTORUN_ENABLE_CROP=1" in result.stdout,
            "GUI autorun crop-enable flag should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_RECORD_FOR_SECONDS=7" in result.stdout,
            "GUI record_for_seconds should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_CLIP_SECONDS=2" in result.stdout,
            "GUI clip_seconds should be forwarded through sudo env",
        )
        require(
            "ORANGE_GUI_PTP_STACK_MODE=auto" in result.stdout,
            "GUI autorun PTP-gate runs should default to auto-starting/checking the host PTP stack",
        )
        require(
            "DISPLAY=:77" in result.stdout,
            "DISPLAY should be forwarded through sudo env",
        )
        require(
            "XAUTHORITY=/tmp/orange_test.Xauthority" in result.stdout,
            "XAUTHORITY should be forwarded through sudo env",
        )
        require(
            "WAYLAND_DISPLAY=wayland-test" in result.stdout,
            "WAYLAND_DISPLAY should be forwarded through sudo env when set",
        )
        require(
            "XDG_RUNTIME_DIR=/run/user/1000" in result.stdout,
            "XDG_RUNTIME_DIR should be forwarded through sudo env when set",
        )
        require(
            "XDG_SESSION_TYPE=wayland" in result.stdout,
            "XDG_SESSION_TYPE should be forwarded through sudo env when set",
        )


def test_default_autorun_enable_flags_are_forwarded_to_exec_env() -> None:
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
                "DISPLAY": ":77",
                "XAUTHORITY": "/tmp/orange_test.Xauthority",
                "XDG_RUNTIME_DIR": "/run/user/1000",
                "XDG_SESSION_TYPE": "x11",
            },
        )

        require(result.returncode == 0, f"launcher failed: {result.stderr}")
        for expected in [
            "ORANGE_GUI_AUTORUN_ENABLE_STREAM=1",
            "ORANGE_GUI_AUTORUN_ENABLE_RECORD=1",
            "ORANGE_GUI_AUTORUN_ENABLE_YOLO=1",
            "ORANGE_GUI_AUTORUN_ENABLE_CROP=1",
        ]:
            require(expected in result.stdout, f"default exec env should include {expected}")


def test_live_launcher_fails_fast_without_display_env() -> None:
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
            print_exec_env=False,
            extra_env={
                "DISPLAY": "",
                "WAYLAND_DISPLAY": "",
                "XDG_SESSION_TYPE": "tty",
            },
        )

        require(result.returncode != 0, "live launcher should fail without a display")
        require(
            "No GUI display session detected" in result.stderr,
            "live launcher should explain missing DISPLAY/WAYLAND_DISPLAY",
        )
        require(
            "tmux set-environment -g DISPLAY" in result.stderr,
            "live launcher should include tmux display refresh guidance",
        )


def test_invalid_gui_privilege_wrapper_mode_fails() -> None:
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
                "ORANGE_GUI_USE_PRIVILEGE_WRAPPER": "sometimes",
            },
        )

        require(result.returncode != 0, "bad GUI privilege-wrapper mode should fail")
        require(
            "ORANGE_GUI_USE_PRIVILEGE_WRAPPER must be auto, 0, or 1" in result.stderr,
            "bad GUI privilege-wrapper mode should explain accepted values",
        )


def test_live_launcher_rejects_stale_privilege_wrapper_for_autorun_ptp() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")
        fake_wrapper = root / "orange-gui-validation"
        fake_wrapper.write_text(
            "#!/usr/bin/env bash\n"
            "if [[ \"${1:-}\" == \"--help\" ]]; then\n"
            "  echo 'Usage: stale-wrapper --orange-bin --env KEY=VALUE'\n"
            "  exit 0\n"
            "fi\n"
            "echo 'fake wrapper should not be executed' >&2\n"
            "exit 99\n",
            encoding="utf-8",
        )
        fake_wrapper.chmod(0o755)

        result = run_launcher(
            config_dir,
            detect_engine,
            validate_only=False,
            extra_env={
                "DISPLAY": ":77",
                "ORANGE_GUI_AUTORUN": "1",
                "ORANGE_GUI_USE_PRIVILEGE_WRAPPER": "1",
                "ORANGE_GUI_PRIVILEGE_WRAPPER": str(fake_wrapper),
            },
        )

        require(result.returncode != 0, "stale wrapper should fail before sudo execution")
        require(
            "Installed GUI privilege wrapper does not support --ptp-stack-mode" in result.stderr,
            "launcher should explain that the installed wrapper must be refreshed",
        )
        require(
            "install_orange_gui_validation_wrapper.sh --install-sudoers" in result.stderr,
            "launcher should include the reinstall command",
        )
        require(
            "fake wrapper should not be executed" not in result.stderr,
            "launcher should only inspect wrapper help and not execute the stale wrapper",
        )


def test_live_launcher_rejects_stale_privilege_wrapper_for_frame_cap_env() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")
        fake_wrapper = root / "orange-gui-validation"
        fake_wrapper.write_text(
            "#!/usr/bin/env bash\n"
            "if [[ \"${1:-}\" == \"--help\" ]]; then\n"
            "  echo 'Usage: wrapper --orange-bin --env KEY=VALUE --ptp-stack-mode --dry-run'\n"
            "  exit 0\n"
            "fi\n"
            "for arg in \"$@\"; do\n"
            "  if [[ \"$arg\" == ORANGE_GUI_FRAME_MAX_FPS=* ]]; then\n"
            "    echo 'Refusing unsupported env key: ORANGE_GUI_FRAME_MAX_FPS' >&2\n"
            "    exit 2\n"
            "  fi\n"
            "done\n"
            "if [[ \" $* \" == *\" --dry-run \"* ]]; then\n"
            "  exit 0\n"
            "fi\n"
            "echo 'fake wrapper should not launch orange' >&2\n"
            "exit 99\n",
            encoding="utf-8",
        )
        fake_wrapper.chmod(0o755)

        result = run_launcher(
            config_dir,
            detect_engine,
            validate_only=False,
            extra_env={
                "DISPLAY": ":77",
                "ORANGE_GUI_USE_PRIVILEGE_WRAPPER": "1",
                "ORANGE_GUI_PRIVILEGE_WRAPPER": str(fake_wrapper),
            },
        )

        require(result.returncode != 0, "stale wrapper should fail before sudo execution")
        require(
            "Installed GUI privilege wrapper does not support ORANGE_GUI_FRAME_MAX_FPS" in result.stderr,
            "launcher should explain that the wrapper lacks frame-cap env support",
        )
        require(
            "install_orange_gui_validation_wrapper.sh --install-sudoers" in result.stderr,
            "launcher should include the reinstall command",
        )
        require(
            "fake wrapper should not launch orange" not in result.stderr,
            "launcher should only dry-run the stale wrapper",
        )


def test_live_launcher_rejects_stale_privilege_wrapper_for_yolo_affinity_env() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        write_camera_config(config_dir, "2010095")
        fake_wrapper = root / "orange-gui-validation"
        fake_wrapper.write_text(
            "#!/usr/bin/env bash\n"
            "if [[ \"${1:-}\" == \"--help\" ]]; then\n"
            "  echo 'Usage: wrapper --orange-bin --env KEY=VALUE --ptp-stack-mode --dry-run'\n"
            "  exit 0\n"
            "fi\n"
            "for arg in \"$@\"; do\n"
            "  if [[ \"$arg\" == ORANGE_YOLO_AFFINITY_CAM_*=* ]]; then\n"
            "    echo 'Refusing unsupported env key: ORANGE_YOLO_AFFINITY_CAM_2010095' >&2\n"
            "    exit 2\n"
            "  fi\n"
            "done\n"
            "if [[ \" $* \" == *\" --dry-run \"* ]]; then\n"
            "  exit 0\n"
            "fi\n"
            "echo 'fake wrapper should not launch orange' >&2\n"
            "exit 99\n",
            encoding="utf-8",
        )
        fake_wrapper.chmod(0o755)

        result = run_launcher(
            config_dir,
            detect_engine,
            validate_only=False,
            extra_env={
                "DISPLAY": ":77",
                "ORANGE_GUI_USE_PRIVILEGE_WRAPPER": "1",
                "ORANGE_GUI_PRIVILEGE_WRAPPER": str(fake_wrapper),
            },
        )

        require(result.returncode != 0, "stale wrapper should fail before sudo execution")
        require(
            "Installed GUI privilege wrapper does not support ORANGE_YOLO_AFFINITY_CAM_*"
            in result.stderr,
            "launcher should explain that the wrapper lacks YOLO affinity env support",
        )
        require(
            "install_orange_gui_validation_wrapper.sh --install-sudoers" in result.stderr,
            "launcher should include the reinstall command",
        )
        require(
            "fake wrapper should not launch orange" not in result.stderr,
            "launcher should only dry-run the stale wrapper",
        )


def test_invalid_gui_ptp_stack_mode_fails() -> None:
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
                "ORANGE_GUI_PTP_STACK_MODE": "sometimes",
            },
        )

        require(result.returncode != 0, "bad GUI PTP stack mode should fail")
        require(
            "ORANGE_GUI_PTP_STACK_MODE must be off, require, or auto" in result.stderr,
            "bad GUI PTP stack mode should explain accepted values",
        )


def test_gui_privilege_wrapper_help_documents_contract() -> None:
    result = subprocess.run(
        [str(GUI_WRAPPER_SCRIPT), "--help"],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    require(result.returncode == 0, f"wrapper help failed: {result.stderr}")
    require("--orange-bin" in result.stdout, "wrapper help should document --orange-bin")
    require("--env KEY=VALUE" in result.stdout, "wrapper help should document --env")
    require(
        "--ptp-stack-mode" in result.stdout,
        "wrapper help should document the PTP stack preflight mode",
    )
    require(
        "ORANGE_GUI_AUTORUN" in result.stdout,
        "wrapper help should mention the GUI autorun env contract",
    )


def test_gui_privilege_wrapper_rejects_bad_ptp_stack_mode() -> None:
    result = subprocess.run(
        [str(GUI_WRAPPER_SCRIPT), "--dry-run", "--ptp-stack-mode", "sometimes"],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    require(result.returncode != 0, "wrapper should reject invalid PTP stack modes")
    require(
        "--ptp-stack-mode must be off, require, or auto" in result.stderr,
        "wrapper should explain accepted PTP stack mode values",
    )

def test_gui_privilege_wrapper_accepts_recording_control_envs() -> None:
    result = subprocess.run(
        [
            str(GUI_WRAPPER_SCRIPT),
            "--dry-run",
            "--env",
            "ORANGE_GUI_RECORD_FOR_SECONDS=10",
            "--env",
            "ORANGE_GUI_CLIP_SECONDS=2",
            "--env",
            "ORANGE_YOLO_AFFINITY_CAM_2010093=6",
            "--env",
            "ORANGE_YOLO_RT_PRIORITY=10",
            "--env",
            "ORANGE_YOLO_RT_PRIORITY_CAM_2010093=12",
            "--env",
            "ORANGE_YOLO_RT_POLICY=fifo",
            "--env",
            "ORANGE_RECORDING_DETECT_PRIORITY=1",
        ],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    require(result.returncode == 0, f"wrapper should accept GUI recording control envs: {result.stderr}")
    require(
        "ORANGE_GUI_RECORD_FOR_SECONDS=10" in result.stdout,
        "wrapper dry-run should include GUI record_for_seconds env",
    )
    require(
        "ORANGE_GUI_CLIP_SECONDS=2" in result.stdout,
        "wrapper dry-run should include GUI clip_seconds env",
    )
    require(
        "ORANGE_YOLO_AFFINITY_CAM_2010093=6" in result.stdout,
        "wrapper dry-run should include per-camera YOLO affinity env",
    )
    require(
        "ORANGE_YOLO_RT_PRIORITY=10" in result.stdout,
        "wrapper dry-run should include YOLO realtime priority env",
    )
    require(
        "ORANGE_YOLO_RT_PRIORITY_CAM_2010093=12" in result.stdout,
        "wrapper dry-run should include per-camera YOLO realtime priority env",
    )
    require(
        "ORANGE_YOLO_RT_POLICY=fifo" in result.stdout,
        "wrapper dry-run should include YOLO realtime policy env",
    )
    require(
        "ORANGE_RECORDING_DETECT_PRIORITY=1" in result.stdout,
        "wrapper dry-run should include recording detect-priority env",
    )


def test_gui_privilege_wrapper_accepts_app_config_envs() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        app_config_path = root / "default.json"
        app_config_path.write_text(
            json.dumps(
                {
                    "schema_id": "orange.app.config",
                    "schema_version": 1,
                    "gui": {
                        "display": {
                            "profile": "citrus_safe",
                        }
                    },
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

        result = subprocess.run(
            [
                str(GUI_WRAPPER_SCRIPT),
                "--dry-run",
                "--env",
                f"ORANGE_GUI_APP_CONFIG_PATH={app_config_path}",
                "--env",
                f"ORANGE_APP_CONFIG_PATH={app_config_path}",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        require(result.returncode == 0, f"wrapper should accept app config envs: {result.stderr}")
        require(
            f"ORANGE_GUI_APP_CONFIG_PATH={app_config_path}" in result.stdout,
            "wrapper dry-run should include GUI app config path env",
        )
        require(
            f"ORANGE_APP_CONFIG_PATH={app_config_path}" in result.stdout,
            "wrapper dry-run should include generic app config path env",
        )


def test_gui_privilege_wrapper_rejects_missing_app_config_env() -> None:
    missing_path = Path("/tmp/orange_missing_app_config_for_wrapper_test.json")
    result = subprocess.run(
        [
            str(GUI_WRAPPER_SCRIPT),
            "--dry-run",
            "--env",
            f"ORANGE_GUI_APP_CONFIG_PATH={missing_path}",
        ],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    require(result.returncode != 0, "wrapper should reject a missing app config path")
    require(
        "App config path is outside allowed roots or missing" in result.stderr,
        "wrapper should explain app config path validation failures",
    )


def main() -> int:
    tests = [
        test_discovers_all_camera_json_files,
        test_invalid_gui_privilege_wrapper_mode_fails,
        test_live_launcher_rejects_stale_privilege_wrapper_for_autorun_ptp,
        test_live_launcher_rejects_stale_privilege_wrapper_for_frame_cap_env,
        test_live_launcher_rejects_stale_privilege_wrapper_for_yolo_affinity_env,
        test_invalid_gui_ptp_stack_mode_fails,
        test_gui_privilege_wrapper_help_documents_contract,
        test_gui_privilege_wrapper_rejects_bad_ptp_stack_mode,
        test_gui_privilege_wrapper_accepts_recording_control_envs,
        test_gui_privilege_wrapper_accepts_app_config_envs,
        test_gui_privilege_wrapper_rejects_missing_app_config_env,
        test_external_crop_queue_validation_limits_are_printed,
        test_source_version_validation_flags_are_printed,
        test_main_video_content_failure_validation_flags_are_printed,
        test_rolling_recording_validation_flags_are_printed,
        test_external_crop_queue_validation_rejects_bad_values,
        test_external_crop_recorder_gpu_validation_rejects_bad_values,
        test_external_crop_separate_gpu_gate_rejects_default_same_gpu,
        test_external_crop_separate_gpu_gate_accepts_override,
        test_external_crop_four_camera_pix_local_mapping_accepts_per_camera_overrides,
        test_expected_camera_gate_fails_when_missing,
        test_expected_camera_subset_validates_only_requested_files,
        test_bad_config_fails_preflight,
        test_crop_preview_env_controls_are_forwarded_to_exec_env,
        test_yolo_worker_scheduling_env_controls_are_forwarded_to_exec_env,
        test_external_crop_ipc_autosizes_crop_frame_pool,
        test_display_env_controls_are_forwarded_to_exec_env,
        test_default_autorun_enable_flags_are_forwarded_to_exec_env,
        test_live_launcher_fails_fast_without_display_env,
    ]
    for test in tests:
        test()
    print("run_gui_aq_off_validation_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
