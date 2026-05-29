#!/usr/bin/env python3
"""Focused tests for the four-camera GUI external-IPC validation profile."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "run_gui_fourcam_external_ipc_validation.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_camera_config(config_dir: Path, serial: str, source_gpu_id: int) -> None:
    payload = {
        "schema_version": 4,
        "camera_serial": serial,
        "source_gpu_id": source_gpu_id,
        "sync_mode": "ptp_gate",
        "ptp": {
            "enabled": True,
            "mode": "TwoStep",
        },
        "recording": {
            "encode": {
                "aq": "off",
                "temporal_aq": "off",
            }
        },
    }
    (config_dir / f"{serial}.json").write_text(
        json.dumps(payload, indent=2) + "\n",
        encoding="utf-8")


def run_profile(args: list[str] | None = None, extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    args = args or []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        detect_engine = root / "detect.engine"
        detect_engine.write_bytes(b"engine")
        config_dir = root / "config"
        config_dir.mkdir()
        for serial, source_gpu_id in {
            "2010093": 3,
            "2010094": 1,
            "2010095": 7,
            "2010096": 5,
        }.items():
            write_camera_config(config_dir, serial, source_gpu_id)

        env = os.environ.copy()
        env.update(
            {
                "ORANGE_BIN": "/bin/true",
                "ORANGE_GUI_CONFIG_DIR": str(config_dir),
                "ORANGE_GUI_DETECT_ENGINE": str(detect_engine),
                "ORANGE_GUI_APP_CONFIG_PATH": str(config_dir / "missing_app_config.json"),
            }
        )
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


def test_default_hidden_profile_validate_only() -> None:
    result = run_profile(["--validate-only"])
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require("ORANGE_GUI_AUTORUN=1" in result.stdout, "profile should enable autorun")
    require(
        "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=1" in result.stdout,
        "default profile should hide crop previews",
    )
    require(
        "ORANGE_GUI_RECORDING_SINK_MODE=external_ipc" in result.stdout,
        "profile should use full-frame external IPC",
    )
    require(
        "ORANGE_YOLO_AFFINITY_CAM_*=2010093=6,2010094=8,2010095=10,2010096=12"
        in result.stdout,
        "profile should show the Citrus-safe per-camera YOLO affinities",
    )
    for serial, cpu in {
        "2010093": 6,
        "2010094": 8,
        "2010095": 10,
        "2010096": 12,
    }.items():
        require(
            f"--expect-yolo-affinity {serial}={cpu}" in result.stdout,
            f"printed validator command should check {serial} YOLO affinity",
        )
    require(
        "ORANGE_GUI_REQUIRE_ISOLATED_CPUS=6,8,10,12,38,40,42,44" in result.stdout,
        "profile should require the Orange YOLO CPU isolation set plus SMT siblings",
    )
    require(
        "ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS=6,8,10,12,38,40,42,44" in result.stdout,
        "profile should require matching kernel boot CPU options",
    )
    require(
        "ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_OPTIONS=isolcpus,nohz_full,rcu_nocbs" in result.stdout,
        "profile should require all low-jitter kernel boot options",
    )
    require(
        "--require-isolated-cpus 6,8,10,12,38,40,42,44" in result.stdout,
        "printed validator command should check the kernel isolated CPU set",
    )
    for option in ("isolcpus", "nohz_full", "rcu_nocbs"):
        require(
            f"--require-kernel-cmdline-cpus {option}=6,8,10,12,38,40,42,44" in result.stdout,
            f"printed validator command should check {option} boot CPU set",
        )
    require(
        "ORANGE_CROP_RECORDING_SINK_MODE=external_ipc" in result.stdout,
        "profile should use crop external IPC",
    )
    require(
        "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=128" in result.stdout,
        "four-camera profile should increase the crop external queue for shared NVENC contention",
    )
    require(
        "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*=2010093=4,2010094=2,2010095=8,2010096=6"
        in result.stdout,
        "profile should use the expected per-camera crop recorder GPUs",
    )
    require("ORANGE_GUI_SWAP_INTERVAL=0" in result.stdout, "profile should disable vsync")
    require("ORANGE_GUI_FRAME_MAX_FPS=60" in result.stdout, "profile should cap GUI frame rate")
    require(
        "ORANGE_DISPLAY_PREVIEW_MAX_FPS=15" in result.stdout,
        "fast profile should keep the default display preview cap",
    )
    require(
        "--expect-gui-frame-max-fps 60" in result.stdout,
        "printed validator command should assert frame cap telemetry",
    )
    require(
        "ORANGE_GUI_REQUIRE_SOURCE_VERSION=1" in result.stdout,
        "four-camera profile should enable source-version validation",
    )
    require(
        "ORANGE_GUI_EXPECT_SOURCE_GIT_COMMAND_USER_MODE=sudo_invoking_user" in result.stdout,
        "four-camera profile should expect sudo-invoking-user git provenance",
    )
    require(
        "--require-source-version" in result.stdout,
        "printed validator command should require source provenance",
    )
    require(
        "--expect-source-git-command-user-mode sudo_invoking_user" in result.stdout,
        "printed validator command should check git command user mode",
    )
    require(
        "ORANGE_CROP_FRAME_POOL_SIZE=256 (auto for external_ipc)" in result.stdout,
        "profile should auto-size crop frame pool for external IPC",
    )


def test_visible_profile_validate_only() -> None:
    result = run_profile(["--visible-crop-preview", "--validate-only"])
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require(
        "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=0" in result.stdout,
        "visible profile should leave crop preview windows visible",
    )
    require(
        "ORANGE_CROP_PREVIEW_DISABLE=0" in result.stdout,
        "visible profile should keep crop preview generation enabled",
    )


def test_citrus_display_safe_profile_validate_only() -> None:
    result = run_profile(["--citrus-display-safe", "--validate-only"])
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require(
        "ORANGE_DISPLAY_PREVIEW_MAX_FPS=10" in result.stdout,
        "citrus-safe profile should lower display preview cadence",
    )
    require(
        "ORANGE_GUI_SWAP_INTERVAL=1" in result.stdout,
        "citrus-safe profile should restore vsync",
    )
    require(
        "ORANGE_GUI_FRAME_MAX_FPS=30" in result.stdout,
        "citrus-safe profile should lower GUI frame cap",
    )
    require(
        "--expect-display-preview-max-fps 10" in result.stdout,
        "printed validator command should assert citrus-safe preview cap",
    )
    require(
        "--expect-gui-swap-interval 1" in result.stdout,
        "printed validator command should assert citrus-safe swap interval",
    )
    require(
        "--expect-gui-frame-max-fps 30" in result.stdout,
        "printed validator command should assert citrus-safe frame cap",
    )


def test_display_pacing_options_override_profile_defaults() -> None:
    result = run_profile(
        [
            "--citrus-display-safe",
            "--display-preview-max-fps",
            "5",
            "--swap-interval",
            "0",
            "--gui-frame-max-fps",
            "20",
            "--validate-only",
        ]
    )
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require(
        "ORANGE_DISPLAY_PREVIEW_MAX_FPS=5" in result.stdout,
        "explicit display preview cap should override the selected profile",
    )
    require(
        "ORANGE_GUI_SWAP_INTERVAL=0" in result.stdout,
        "explicit swap interval should override the selected profile",
    )
    require(
        "ORANGE_GUI_FRAME_MAX_FPS=20" in result.stdout,
        "explicit GUI frame cap should override the selected profile",
    )
    require(
        "--expect-display-preview-max-fps 5" in result.stdout,
        "printed validator command should assert the explicit preview cap",
    )
    require(
        "--expect-gui-swap-interval 0" in result.stdout,
        "printed validator command should assert the explicit swap interval",
    )
    require(
        "--expect-gui-frame-max-fps 20" in result.stdout,
        "printed validator command should assert the explicit frame cap",
    )


def test_disabled_preview_profile_validate_only() -> None:
    result = run_profile(["--disable-crop-preview", "--validate-only"])
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require(
        "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=1" in result.stdout,
        "disabled profile should hide crop preview windows",
    )
    require(
        "ORANGE_CROP_PREVIEW_DISABLE=1" in result.stdout,
        "disabled profile should disable crop preview generation",
    )


def test_print_exec_env_only_contains_profile_env() -> None:
    result = run_profile(["--print-exec-env-only"])
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    for expected in [
        "ORANGE_GUI_AUTORUN=1",
        "ORANGE_GUI_AUTORUN_ENABLE_STREAM=1",
        "ORANGE_GUI_AUTORUN_ENABLE_RECORD=1",
        "ORANGE_GUI_AUTORUN_ENABLE_YOLO=1",
        "ORANGE_GUI_AUTORUN_ENABLE_CROP=1",
        "ORANGE_GUI_AUTORUN_START_RECORDING=1",
        "ORANGE_GUI_RECORDING_SINK_MODE=external_ipc",
        "ORANGE_CROP_RECORDING_SINK_MODE=external_ipc",
        "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=128",
        "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=1",
        "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010093=4",
        "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010094=2",
        "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095=8",
        "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010096=6",
        "ORANGE_YOLO_AFFINITY_CAM_2010093=6",
        "ORANGE_YOLO_AFFINITY_CAM_2010094=8",
        "ORANGE_YOLO_AFFINITY_CAM_2010095=10",
        "ORANGE_YOLO_AFFINITY_CAM_2010096=12",
        "ORANGE_GUI_FRAME_MAX_FPS=60",
    ]:
        require(expected in result.stdout, f"exec env should include {expected}")


def test_overrides_are_preserved() -> None:
    result = run_profile(
        ["--validate-only"],
        extra_env={
            "ORANGE_GUI_FRAME_MAX_FPS": "45",
            "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH": "64",
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095": "6",
            "ORANGE_GUI_REQUIRE_ISOLATED_CPUS": "6",
        },
    )
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require("ORANGE_GUI_FRAME_MAX_FPS=45" in result.stdout, "frame cap override should be preserved")
    require(
        "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=64" in result.stdout,
        "external crop queue override should be preserved",
    )
    require(
        "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*=2010093=4,2010094=2,2010095=6,2010096=6"
        in result.stdout,
        "per-camera crop recorder GPU override should be preserved",
    )
    require(
        "--expect-external-crop-recorder-gpu 2010095=6" in result.stdout,
        "validator command should reflect the per-camera override",
    )
    require(
        "--require-isolated-cpus 6" in result.stdout,
        "validator command should reflect the isolated CPU override",
    )
    require(
        "--require-kernel-cmdline-cpus isolcpus=6" in result.stdout,
        "validator command should derive kernel CPU checks from the isolated CPU override",
    )


def test_empty_isolation_overrides_disable_validation_gates() -> None:
    result = run_profile(
        ["--validate-only"],
        extra_env={
            "ORANGE_GUI_REQUIRE_ISOLATED_CPUS": "",
            "ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS": "",
        },
    )
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require(
        "ORANGE_GUI_REQUIRE_ISOLATED_CPUS=<not set>" in result.stdout,
        "explicit empty isolated CPU override should disable isolation validation",
    )
    require(
        "ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS=<not set>" in result.stdout,
        "explicit empty kernel cmdline CPU override should disable boot-option validation",
    )
    require(
        "--require-isolated-cpus" not in result.stdout,
        "validator command should omit isolated CPU gate when explicitly disabled",
    )
    require(
        "--require-kernel-cmdline-cpus" not in result.stdout,
        "validator command should omit boot-option gates when explicitly disabled",
    )


def test_yolo_affinity_overrides_are_preserved_in_exec_env() -> None:
    result = run_profile(
        ["--print-exec-env-only"],
        extra_env={
            "ORANGE_YOLO_AFFINITY_CAM_2010093": "14",
            "ORANGE_YOLO_AFFINITY_CAM_2010095": "16",
        },
    )
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require(
        "ORANGE_YOLO_AFFINITY_CAM_2010093=14" in result.stdout,
        "arbitrary per-camera YOLO affinity override should be preserved",
    )
    require(
        "ORANGE_YOLO_AFFINITY_CAM_2010095=16" in result.stdout,
        "default two-camera YOLO affinity override should be preserved",
    )


def test_no_lens_camera_content_allowance_is_printed() -> None:
    result = run_profile(
        ["--allow-main-video-content-failure", "2010093", "--validate-only"],
    )
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require(
        "ORANGE_GUI_ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS=2010093" in result.stdout,
        "profile should forward the explicit no-lens camera allowance",
    )
    require(
        "--allow-main-video-content-failure 2010093" in result.stdout,
        "printed validator command should allow the known no-lens camera content failure",
    )


def test_rolling_clip_seconds_option_is_printed() -> None:
    result = run_profile(
        ["--record-seconds", "6", "--clip-seconds", "2", "--validate-only"],
    )
    require(result.returncode == 0, f"profile failed: {result.stderr}")
    require(
        "ORANGE_GUI_AUTORUN_RECORD_SECONDS=6" in result.stdout,
        "profile should preserve the requested autorun recording duration",
    )
    require(
        "ORANGE_GUI_CLIP_SECONDS=2" in result.stdout,
        "profile should enable GUI rolling clips with the requested clip duration",
    )
    require(
        "--expect-recording-mode rolling_clips" in result.stdout,
        "printed validator command should expect rolling mode when clip seconds are set",
    )
    require(
        "--expect-record-for-seconds 6" in result.stdout,
        "printed validator command should expect the autorun recording duration",
    )
    require(
        "--expect-clip-seconds 2" in result.stdout,
        "printed validator command should expect the requested clip duration",
    )


def main() -> int:
    tests = [
        test_default_hidden_profile_validate_only,
        test_visible_profile_validate_only,
        test_citrus_display_safe_profile_validate_only,
        test_display_pacing_options_override_profile_defaults,
        test_disabled_preview_profile_validate_only,
        test_print_exec_env_only_contains_profile_env,
        test_overrides_are_preserved,
        test_empty_isolation_overrides_disable_validation_gates,
        test_yolo_affinity_overrides_are_preserved_in_exec_env,
        test_no_lens_camera_content_allowance_is_printed,
        test_rolling_clip_seconds_option_is_printed,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("run_gui_fourcam_external_ipc_validation_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
