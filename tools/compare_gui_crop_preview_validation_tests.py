#!/usr/bin/env python3
"""Focused tests for GUI crop-preview comparison summaries."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "scripts"
SCRIPT = SCRIPTS_DIR / "compare_gui_crop_preview_validation.py"
sys.path.insert(0, str(SCRIPTS_DIR))

import compare_gui_crop_preview_validation as compare  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def add_external_protocol(status: dict) -> dict:
    status.update(
        {
            "ipc_protocol_name": "orange.external_recorder.ipc",
            "ipc_protocol_version": 1,
            "recorder_hello_sent": True,
            "client_hello_received": True,
            "client_control_messages_received": 2,
            "client_drain_messages_received": 1,
            "client_finalize_messages_received": 1,
            "client_drain_received": True,
            "client_finalize_received": True,
            "client_drain_first_frame_count": 3,
            "client_finalize_frame_count": 3,
            "client_control_state": "finalize_requested",
            "descriptor_intake_end_reason": "client_finalize",
            "descriptor_intake_completed_cleanly": True,
        }
    )
    return status


def sample_payload(
    *,
    status: str = "pass",
    visible_p05: float = 58.0,
    drops: int = 0,
    external_drops: int = 0,
) -> dict:
    payload = {
        "status": status,
        "recording_folder": "/tmp/run",
        "failures": [] if status == "pass" else ["failure"],
        "warnings": ["warning"],
        "system_cpu": {
            "isolated_cpus": {
                "available": True,
                "parse_ok": True,
                "cpus": [1, 2, 6, 8, 10, 12],
            },
            "kernel_cmdline": {
                "available": True,
                "options": {
                    "isolcpus": "domain,managed_irq,1-2,6,8,10,12,38,40,42,44",
                    "nohz_full": "1-2,6,8,10,12,38,40,42,44",
                    "rcu_nocbs": "1-2,6,8,10,12,38,40,42,44",
                },
            },
        },
        "summary": {
            "2010095": {
                "yolo_affinity": {
                    "configured": 1,
                    "applied": 1,
                    "requested_cpus": "10",
                    "effective_cpus": "10",
                },
                "detect_steady_p95_ms": 3.8,
                "acquisition_to_worker_start_p95_ms": 0.12,
                "acquisition_to_worker_start_steady_p95_ms": 0.10,
                "yolo_enqueue_to_dequeue_p95_ms": 0.04,
                "yolo_enqueue_to_dequeue_steady_p95_ms": 0.03,
                "yolo_dequeue_to_worker_start_p95_ms": 0.02,
                "yolo_dequeue_to_worker_start_steady_p95_ms": 0.01,
                "queue_p95_ms": 0.02,
                "queue_steady_p95_ms": 0.015,
                "same_camera_service_gap_p95_ms": 10.5,
                "same_camera_service_gap_steady_p95_ms": 10.1,
            },
            "2010096": {
                "yolo_affinity": {
                    "configured": 1,
                    "applied": 1,
                    "requested_cpus": "12",
                    "effective_cpus": "12",
                },
                "detect_steady_p95_ms": 4.2,
                "acquisition_to_worker_start_p95_ms": 0.14,
                "acquisition_to_worker_start_steady_p95_ms": 0.12,
                "yolo_enqueue_to_dequeue_p95_ms": 0.06,
                "yolo_enqueue_to_dequeue_steady_p95_ms": 0.05,
                "yolo_dequeue_to_worker_start_p95_ms": 0.04,
                "yolo_dequeue_to_worker_start_steady_p95_ms": 0.03,
                "queue_p95_ms": 0.03,
                "queue_steady_p95_ms": 0.025,
                "same_camera_service_gap_p95_ms": 10.7,
                "same_camera_service_gap_steady_p95_ms": 10.3,
            },
        },
        "crop_preview": {
            "2010095": {
                "preview_max_fps": 15,
                "preview_disabled": 0,
                "preview_display_enabled_final": 1,
                "preview_frames_offered": 100,
                "preview_frames_updated": 15,
                "preview_frames_skipped_by_cadence": 85,
                "preview_clears_updated": 1,
                "crop_frame_pool_size": 32,
                "crop_metadata_detection_rows": 90,
                "producer_recording_crop_frame_offered": 90,
                "producer_recording_crop_frame_accepted": 90,
                "producer_recording_crop_frame_dropped": 0,
                "producer_preview_crop_frame_offered": 15,
                "producer_preview_crop_frame_accepted": 15,
                "producer_preview_crop_frame_dropped": 0,
                "producer_pose_crop_frame_offered": 0,
                "producer_pose_crop_frame_accepted": 0,
                "producer_pose_crop_frame_dropped": 0,
            },
            "2010096": {
                "preview_max_fps": 15,
                "preview_disabled": 0,
                "preview_display_enabled_final": 1,
                "preview_frames_offered": 80,
                "preview_frames_updated": 12,
                "preview_frames_skipped_by_cadence": 68,
                "preview_clears_updated": 0,
                "crop_frame_pool_size": 32,
                "crop_metadata_detection_rows": 70,
                "producer_recording_crop_frame_offered": 70,
                "producer_recording_crop_frame_accepted": 70,
                "producer_recording_crop_frame_dropped": 0,
                "producer_preview_crop_frame_offered": 12,
                "producer_preview_crop_frame_accepted": 12,
                "producer_preview_crop_frame_dropped": 0,
                "producer_pose_crop_frame_offered": 0,
                "producer_pose_crop_frame_accepted": 0,
                "producer_pose_crop_frame_dropped": 0,
            },
        },
        "crop_recording": {
            "2010095": {
                "backend": "external_ipc",
                "metadata_rows": 100,
                "video_frames": 100,
                "dropped_rows": drops,
                "external_frames_dropped": external_drops,
                "external_encode_queue_depth": 64,
                "external_encode_queue_high_water": 12,
                "external_enqueue_age_p95_ms": 2.5,
                "external_stream_id": "2010095_crop",
                "external_analytics_gpu_id": 5,
                "external_recorder_gpu_id": 5,
                "external_socket_path": "/tmp/orange_external_recorder_2010095_crop.sock",
            },
            "2010096": {
                "backend": "external_ipc",
                "metadata_rows": 80,
                "video_frames": 80,
                "dropped_rows": 0,
                "external_encode_queue_depth": 64,
                "external_stream_id": "2010096_crop",
                "external_analytics_gpu_id": 7,
                "external_recorder_gpu_id": 7,
                "external_socket_path": "/tmp/orange_external_recorder_2010096_crop.sock",
            },
        },
        "external_recorder_status": {
            "full": {
                "2010095": {
                    "status": "completed",
                    "heartbeat_sequence": 4,
                    "frames_received": 100,
                    "frames_encoded": 100,
                    "runtime_present": True,
                    "runtime_valid": True,
                    "storage_checked": True,
                    "storage_ok": True,
                    "storage_low_space": False,
                    "storage_path_count": 1,
                    "runtime_storage_checked": True,
                    "runtime_storage_ok": True,
                    "runtime_storage_low_space": False,
                },
                "2010096": {
                    "status": "completed",
                    "heartbeat_sequence": 5,
                    "frames_received": 80,
                    "frames_encoded": 80,
                    "runtime_present": True,
                    "runtime_valid": True,
                    "storage_checked": True,
                    "storage_ok": True,
                    "storage_low_space": False,
                    "storage_path_count": 1,
                    "runtime_storage_checked": True,
                    "runtime_storage_ok": True,
                    "runtime_storage_low_space": False,
                },
            },
            "crop": {
                "2010095": {
                    "status": "completed",
                    "heartbeat_sequence": 6,
                    "frames_received": 100,
                    "frames_encoded": 100,
                    "runtime_present": True,
                    "runtime_valid": True,
                    "storage_checked": True,
                    "storage_ok": True,
                    "storage_low_space": False,
                    "storage_path_count": 1,
                    "runtime_storage_checked": True,
                    "runtime_storage_ok": True,
                    "runtime_storage_low_space": False,
                },
                "2010096": {
                    "status": "completed",
                    "heartbeat_sequence": 7,
                    "frames_received": 80,
                    "frames_encoded": 80,
                    "runtime_present": True,
                    "runtime_valid": True,
                    "storage_checked": True,
                    "storage_ok": True,
                    "storage_low_space": False,
                    "storage_path_count": 1,
                    "runtime_storage_checked": True,
                    "runtime_storage_ok": True,
                    "runtime_storage_low_space": False,
                },
            },
        },
        "gui_display_frame_rate": {
            "stream_downsample": 4,
            "display_preview_max_fps": 30,
            "swap_interval": 0,
            "frame_max_fps": 60,
            "yolo_speed_graphs_enabled": False,
            "overall": {"sample_count": 120, "p05_fps": 57.0},
            "crop_preview_visible": {"sample_count": 80, "p05_fps": visible_p05},
            "crop_preview_hidden": {"sample_count": 0},
            "timings": {
                "frame_total_ms": {"sample_count": 120, "p95_ms": 18.5},
                "main_texture_upload_ms": {"sample_count": 120, "p95_ms": 2.25},
                "crop_texture_upload_ms": {"sample_count": 120, "p95_ms": 0.5},
                "camera_window_draw_ms": {"sample_count": 120, "p95_ms": 3.5},
                "crop_window_draw_ms": {"sample_count": 120, "p95_ms": 0.75},
                "speed_graph_draw_ms": {"sample_count": 120, "p95_ms": 1.25},
                "render_present_ms": {"sample_count": 120, "p95_ms": 4.0},
                "main_texture_upload_count": 240,
                "crop_texture_upload_count": 27,
            },
        },
    }
    for group in payload["external_recorder_status"].values():
        for status_payload in group.values():
            add_external_protocol(status_payload)
    return payload


def test_summarize_validation_aggregates_crop_preview_and_fps() -> None:
    summary = compare.summarize_validation("visible", sample_payload())
    require(summary["camera_count"] == 2, "camera count should include summary cameras")
    require(summary["crop_rows_total"] == 180, "crop metadata rows should aggregate")
    require(summary["crop_dropped_rows_total"] == 0, "crop drops should aggregate")
    require(summary["external_crop_dropped_total"] == 0, "external crop drops should aggregate")
    require(summary["crop_backend_values"] == ["external_ipc"], "crop backend values should aggregate")
    require(summary["external_crop_queue_depth_values"] == [64], "external queue depths should aggregate")
    require(
        summary["external_crop_gpu_mapping_values"] == ["2010095:5->5", "2010096:7->7"],
        "external crop GPU placement should aggregate by camera",
    )
    require(
        summary["external_crop_same_gpu_mapping_values"] == ["2010095:5->5", "2010096:7->7"],
        "same-GPU external crop placement should aggregate by camera",
    )
    require(summary["external_crop_queue_high_water_max"] == 12, "external queue high-water should aggregate")
    require(summary["external_crop_enqueue_age_p95_max_ms"] == 2.5, "external enqueue age should aggregate")
    require(summary["external_recorder_status_streams_total"] == 4, "external status streams should aggregate")
    require(summary["external_recorder_status_values"] == ["completed"], "external status values should aggregate")
    require(
        summary["external_recorder_status_failed_streams"] == [],
        "healthy external status streams should not be marked failed",
    )
    require(
        summary["external_recorder_protocol_failed_streams"] == [],
        "healthy external protocol streams should not be marked failed",
    )
    require(summary["external_recorder_heartbeat_min"] == 4, "external heartbeat min should aggregate")
    require(summary["external_recorder_heartbeat_max"] == 7, "external heartbeat max should aggregate")
    require(
        summary["external_recorder_frames_received_total"] == 360,
        "external status received counts should aggregate",
    )
    require(
        summary["external_recorder_frames_encoded_total"] == 360,
        "external status encoded counts should aggregate",
    )
    require(summary["preview_offered_total"] == 180, "preview offered should aggregate")
    require(summary["preview_updated_total"] == 27, "preview updates should aggregate")
    require(summary["preview_skipped_total"] == 153, "preview skipped should aggregate")
    require(summary["crop_metadata_detection_rows_total"] == 160, "detection rows should aggregate")
    require(
        summary["producer_recording_crop_frame_offered_total"] == 160,
        "recording fanout offered should aggregate",
    )
    require(
        summary["producer_recording_crop_frame_accepted_total"] == 160,
        "recording fanout accepted should aggregate",
    )
    require(
        summary["producer_recording_crop_frame_dropped_total"] == 0,
        "recording fanout drops should aggregate",
    )
    require(
        summary["producer_preview_crop_frame_offered_total"] == 27,
        "preview fanout offered should aggregate",
    )
    require(
        summary["producer_preview_crop_frame_accepted_total"] == 27,
        "preview fanout accepted should aggregate",
    )
    require(summary["preview_max_fps_values"] == [15], "preview max FPS set should collapse")
    require(summary["crop_frame_pool_size_values"] == [32], "crop pool size set should collapse")
    require(summary["preview_disabled_values"] == [0], "preview disabled set should collapse")
    require(summary["preview_display_enabled_values"] == [1], "preview display state set should collapse")
    require(summary["gui_visible_p05_fps"] == 58.0, "visible GUI p05 should parse")
    require(summary["gui_stream_downsample"] == 4, "stream downsample should parse")
    require(summary["display_preview_max_fps"] == 30, "display preview FPS should parse")
    require(summary["gui_swap_interval"] == 0, "GUI swap interval should parse")
    require(summary["gui_frame_max_fps"] == 60, "GUI frame cap should parse")
    require(summary["yolo_speed_graphs_enabled"] == 0, "speed graph state should parse")
    require(
        summary["isolated_cpu_values"] == [1, 2, 6, 8, 10, 12],
        "isolated CPU values should parse",
    )
    require(
        summary["kernel_cmdline_cpu_option_values"] == [
            "isolcpus=cpus:1-2,6,8,10,12,38,40,42,44;flags:domain|managed_irq",
            "nohz_full=cpus:1-2,6,8,10,12,38,40,42,44",
            "rcu_nocbs=cpus:1-2,6,8,10,12,38,40,42,44",
        ],
        "kernel cmdline CPU option values should parse",
    )
    require(
        summary["yolo_affinity_mapping_values"] == ["2010095:10->10", "2010096:12->12"],
        "YOLO affinity mappings should parse",
    )
    require(summary["gui_frame_total_p95_ms"] == 18.5, "GUI frame timing p95 should parse")
    require(summary["gui_main_texture_upload_p95_ms"] == 2.25, "GUI upload timing p95 should parse")
    require(summary["gui_crop_texture_upload_p95_ms"] == 0.5, "GUI crop upload timing p95 should parse")
    require(abs(summary["detect_steady_p95_avg_ms"] - 4.0) < 0.00001, "detect p95 average")
    require(abs(summary["acq_worker_p95_avg_ms"] - 0.13) < 0.00001, "acq-worker p95 average")
    require(abs(summary["acq_worker_steady_p95_avg_ms"] - 0.11) < 0.00001, "acq-worker steady p95 average")
    require(abs(summary["enqueue_dequeue_p95_avg_ms"] - 0.05) < 0.00001, "enqueue-dequeue p95 average")
    require(
        abs(summary["enqueue_dequeue_steady_p95_avg_ms"] - 0.04) < 0.00001,
        "enqueue-dequeue steady p95 average",
    )
    require(abs(summary["dequeue_worker_p95_avg_ms"] - 0.03) < 0.00001, "dequeue-worker p95 average")
    require(abs(summary["queue_steady_p95_avg_ms"] - 0.02) < 0.00001, "queue steady p95 average")
    require(abs(summary["service_gap_p95_avg_ms"] - 10.6) < 0.00001, "service-gap p95 average")
    require(
        abs(summary["service_gap_steady_p95_avg_ms"] - 10.2) < 0.00001,
        "service-gap steady p95 average",
    )
    require(summary["gui_crop_window_draw_p95_ms"] == 0.75, "crop window draw p95 should parse")
    require(summary["gui_speed_graph_draw_p95_ms"] == 1.25, "speed graph draw p95 should parse")
    require(
        summary["gui_dominant_timing_bucket"] == "render_present_ms",
        "dominant timing should be computed from raw timing buckets",
    )
    require(summary["gui_dominant_timing_p95_ms"] == 4.0, "dominant timing p95 should parse")
    require(
        round(summary["gui_dominant_timing_share"], 3) == round(4.0 / 18.5, 3),
        "dominant timing share should use frame-total p95",
    )
    require(summary["gui_main_texture_upload_count"] == 240, "main upload count should parse")
    require(summary["gui_crop_texture_upload_count"] == 27, "crop upload count should parse")


def test_compare_adds_deltas_against_first_run() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        baseline = root / "baseline.json"
        visible = root / "visible.json"
        baseline.write_text(json.dumps(sample_payload(visible_p05=50.0)) + "\n", encoding="utf-8")
        visible.write_text(json.dumps(sample_payload(visible_p05=58.0)) + "\n", encoding="utf-8")

        summaries = compare.compare([f"baseline={baseline}", f"visible={visible}"])
        require(summaries[0]["label"] == "baseline", "explicit label should parse")
        require(summaries[1]["delta_vs_first"]["gui_visible_p05_fps"] == 8.0, "visible FPS delta")


def test_require_zero_crop_drops_would_fail_for_drops() -> None:
    summary = compare.summarize_validation("drop", sample_payload(drops=2))
    require(summary["crop_dropped_rows_total"] == 2, "drop count should be visible")
    summary = compare.summarize_validation("external-drop", sample_payload(external_drops=3))
    require(summary["external_crop_dropped_total"] == 3, "external drop count should be visible")


def test_threshold_failures_cover_fps_and_external_queue_pressure() -> None:
    summary = compare.summarize_validation("visible", sample_payload(visible_p05=42.0))
    args = SimpleNamespace(
        require_pass=True,
        require_zero_crop_drops=True,
        require_visible_samples=True,
        require_hidden_samples=True,
        require_matching_cameras=False,
        require_matching_display_config=False,
        require_matching_crop_config=False,
        min_gui_overall_p05_fps=45.0,
        min_gui_visible_p05_fps=45.0,
        min_gui_hidden_p05_fps=45.0,
        max_external_crop_queue_high_water=8,
        max_external_crop_enqueue_age_p95_ms=2.0,
    )
    failures = compare.threshold_failures(args, [summary])
    require(
        any("visible GUI p05 FPS=42.0 below 45.0" in failure for failure in failures),
        "visible FPS threshold should fail",
    )
    require(
        any("external crop queue high-water=12.0 > 8" in failure for failure in failures),
        "external queue high-water threshold should fail",
    )
    require(
        any("external crop enqueue-age p95=2.5 ms > 2.000 ms" in failure for failure in failures),
        "external enqueue-age threshold should fail",
    )
    require(
        not any("hidden GUI p05" in failure for failure in failures),
        "hidden FPS threshold should ignore runs with no hidden samples",
    )
    require(
        any("no compared run has hidden crop-preview GUI FPS samples" in failure for failure in failures),
        "hidden sample presence gate should fail when every run lacks hidden samples",
    )


def test_threshold_failures_cover_mismatched_camera_and_display_config() -> None:
    baseline = compare.summarize_validation("baseline", sample_payload())
    changed_payload = sample_payload()
    del changed_payload["summary"]["2010096"]
    del changed_payload["crop_preview"]["2010096"]
    del changed_payload["crop_recording"]["2010096"]
    changed_payload["gui_display_frame_rate"]["display_preview_max_fps"] = 15
    changed = compare.summarize_validation("changed", changed_payload)
    args = SimpleNamespace(
        require_pass=False,
        require_zero_crop_drops=False,
        require_visible_samples=False,
        require_hidden_samples=False,
        require_matching_cameras=True,
        require_matching_display_config=True,
        require_matching_crop_config=False,
        min_gui_overall_p05_fps=None,
        min_gui_visible_p05_fps=None,
        min_gui_hidden_p05_fps=None,
        max_external_crop_queue_high_water=None,
        max_external_crop_enqueue_age_p95_ms=None,
    )

    failures = compare.threshold_failures(args, [baseline, changed])
    require(
        any("camera set" in failure and "does not match" in failure for failure in failures),
        "mismatched camera sets should fail",
    )
    require(
        any("display config" in failure and "does not match" in failure for failure in failures),
        "mismatched display config should fail",
    )


def test_threshold_failures_cover_mismatched_crop_config() -> None:
    baseline = compare.summarize_validation("baseline", sample_payload())
    changed_payload = sample_payload()
    changed_payload["crop_recording"]["2010095"]["backend"] = "in_process"
    changed_payload["crop_recording"]["2010095"]["external_encode_queue_depth"] = 32
    changed = compare.summarize_validation("changed", changed_payload)
    args = SimpleNamespace(
        require_pass=False,
        require_zero_crop_drops=False,
        require_visible_samples=False,
        require_hidden_samples=False,
        require_matching_cameras=False,
        require_matching_display_config=False,
        require_matching_crop_config=True,
        min_gui_overall_p05_fps=None,
        min_gui_visible_p05_fps=None,
        min_gui_hidden_p05_fps=None,
        max_external_crop_queue_high_water=None,
        max_external_crop_enqueue_age_p95_ms=None,
    )

    failures = compare.threshold_failures(args, [baseline, changed])
    require(
        any("crop config" in failure and "does not match" in failure for failure in failures),
        "mismatched crop backend or queue depth should fail",
    )


def test_threshold_failures_cover_mismatched_yolo_runtime_config() -> None:
    baseline = compare.summarize_validation("baseline", sample_payload())
    changed_payload = sample_payload()
    changed_payload["summary"]["2010095"]["yolo_affinity"]["effective_cpus"] = "8"
    changed_payload["system_cpu"]["isolated_cpus"]["cpus"] = [1, 2, 6]
    changed_payload["system_cpu"]["kernel_cmdline"]["options"]["nohz_full"] = "1-2,6"
    changed = compare.summarize_validation("changed", changed_payload)
    args = SimpleNamespace(
        require_pass=False,
        require_zero_crop_drops=False,
        require_visible_samples=False,
        require_hidden_samples=False,
        require_matching_cameras=False,
        require_matching_display_config=False,
        require_matching_crop_config=False,
        require_matching_yolo_runtime_config=True,
        min_gui_overall_p05_fps=None,
        min_gui_visible_p05_fps=None,
        min_gui_hidden_p05_fps=None,
        max_external_crop_queue_high_water=None,
        max_external_crop_enqueue_age_p95_ms=None,
    )

    failures = compare.threshold_failures(args, [baseline, changed])
    require(
        any("YOLO runtime config" in failure and "does not match" in failure for failure in failures),
        "mismatched YOLO affinity, isolated CPU, or kernel boot CPU config should fail",
    )


def test_threshold_failures_accept_equivalent_kernel_cpu_list_syntax() -> None:
    baseline = compare.summarize_validation("baseline", sample_payload())
    changed_payload = sample_payload()
    changed_payload["system_cpu"]["kernel_cmdline"]["options"] = {
        "isolcpus": "managed_irq,domain,1,2,6,8,10,12,38,40,42,44",
        "nohz_full": "1,2,6,8,10,12,38,40,42,44",
        "rcu_nocbs": "1,2,6,8,10,12,38,40,42,44",
    }
    changed = compare.summarize_validation("changed", changed_payload)
    args = SimpleNamespace(
        require_pass=False,
        require_zero_crop_drops=False,
        require_visible_samples=False,
        require_hidden_samples=False,
        require_matching_cameras=False,
        require_matching_display_config=False,
        require_matching_crop_config=False,
        require_matching_yolo_runtime_config=True,
        min_gui_overall_p05_fps=None,
        min_gui_visible_p05_fps=None,
        min_gui_hidden_p05_fps=None,
        max_external_crop_queue_high_water=None,
        max_external_crop_enqueue_age_p95_ms=None,
    )

    failures = compare.threshold_failures(args, [baseline, changed])
    require(
        not any("YOLO runtime config" in failure for failure in failures),
        f"equivalent kernel CPU-list syntax should not fail runtime matching: {failures}",
    )


def test_summarize_validation_prefers_validator_normalized_kernel_options() -> None:
    payload = sample_payload()
    payload["system_cpu"]["kernel_cmdline"]["options"]["nohz_full"] = "1-2,6"
    payload["system_cpu_kernel_cmdline_cpu_option_values"] = [
        "isolcpus=cpus:1-2,6,8,10,12,38,40,42,44;flags:domain|managed_irq",
        "nohz_full=cpus:1-2,6,8,10,12,38,40,42,44",
        "rcu_nocbs=cpus:1-2,6,8,10,12,38,40,42,44",
    ]

    summary = compare.summarize_validation("validator_json", payload)

    require(
        summary["kernel_cmdline_cpu_option_values"]
        == payload["system_cpu_kernel_cmdline_cpu_option_values"],
        "compare summary should prefer validator-provided normalized kernel options",
    )


def test_threshold_failures_cover_mismatched_external_crop_gpu_mapping() -> None:
    baseline = compare.summarize_validation("baseline", sample_payload())
    changed_payload = sample_payload()
    changed_payload["crop_recording"]["2010096"]["external_recorder_gpu_id"] = 8
    changed = compare.summarize_validation("changed", changed_payload)
    args = SimpleNamespace(
        require_pass=False,
        require_zero_crop_drops=False,
        require_visible_samples=False,
        require_hidden_samples=False,
        require_matching_cameras=False,
        require_matching_display_config=False,
        require_matching_crop_config=True,
        min_gui_overall_p05_fps=None,
        min_gui_visible_p05_fps=None,
        min_gui_hidden_p05_fps=None,
        max_external_crop_queue_high_water=None,
        max_external_crop_enqueue_age_p95_ms=None,
    )

    failures = compare.threshold_failures(args, [baseline, changed])
    require(
        any(
            "crop config" in failure
            and "external_crop_gpu_mapping_values" in failure
            and "does not match" in failure
            for failure in failures
        ),
        "mismatched external crop GPU placement should fail matched crop config",
    )


def test_threshold_failures_cover_same_external_crop_gpu_mapping() -> None:
    same_gpu = compare.summarize_validation("same-gpu", sample_payload())
    separate_payload = sample_payload()
    separate_payload["crop_recording"]["2010095"]["external_recorder_gpu_id"] = 6
    separate_payload["crop_recording"]["2010096"]["external_recorder_gpu_id"] = 8
    separate = compare.summarize_validation("separate-gpu", separate_payload)
    args = SimpleNamespace(
        require_pass=False,
        require_zero_crop_drops=False,
        require_visible_samples=False,
        require_hidden_samples=False,
        require_matching_cameras=False,
        require_matching_display_config=False,
        require_matching_crop_config=False,
        require_external_crop_recorder_gpu_separate_from_analytics=True,
        require_external_recorder_status=False,
        min_gui_overall_p05_fps=None,
        min_gui_visible_p05_fps=None,
        min_gui_hidden_p05_fps=None,
        max_external_crop_queue_high_water=None,
        max_external_crop_enqueue_age_p95_ms=None,
    )

    failures = compare.threshold_failures(args, [same_gpu])
    require(
        any("uses analytics GPU" in failure for failure in failures),
        "same analytics/recorder GPU should fail the separation threshold",
    )
    require(
        not compare.threshold_failures(args, [separate]),
        "separate analytics/recorder GPU placement should pass the separation threshold",
    )


def test_threshold_failures_cover_missing_external_crop_gpu_mapping() -> None:
    payload = sample_payload()
    for item in payload["crop_recording"].values():
        item.pop("external_analytics_gpu_id", None)
        item.pop("external_recorder_gpu_id", None)
    summary = compare.summarize_validation("missing-gpu", payload)
    args = SimpleNamespace(
        require_pass=False,
        require_zero_crop_drops=False,
        require_visible_samples=False,
        require_hidden_samples=False,
        require_matching_cameras=False,
        require_matching_display_config=False,
        require_matching_crop_config=False,
        require_external_crop_recorder_gpu_separate_from_analytics=True,
        require_external_recorder_status=False,
        min_gui_overall_p05_fps=None,
        min_gui_visible_p05_fps=None,
        min_gui_hidden_p05_fps=None,
        max_external_crop_queue_high_water=None,
        max_external_crop_enqueue_age_p95_ms=None,
    )

    failures = compare.threshold_failures(args, [summary])
    require(
        any("GPU placement metadata missing" in failure for failure in failures),
        "missing external crop GPU metadata should fail the separation threshold",
    )


def test_table_contains_expected_columns_and_values() -> None:
    summary = compare.summarize_validation("visible", sample_payload())
    table = compare.render_table([summary])
    require("visible p05" in table, "table should include visible FPS column")
    require("27/180" in table, "table should include preview update ratio")
    require("160/160" in table, "table should include recording fanout ratio")
    require("detect rows" in table, "table should include detection row column")
    require("preview fanout" in table, "table should include preview fanout column")
    require("pose fanout" in table, "table should include pose fanout column")
    require("85.0" in table, "table should include preview skip percentage")
    require("preview disabled" in table, "table should include preview disabled column")
    require("crop pool" in table, "table should include crop pool column")
    require("stream ds" in table, "table should include display downsample column")
    require("display fps" in table, "table should include display preview FPS column")
    require("main upload p95" in table, "table should include main upload timing column")
    require("crop upload p95" in table, "table should include crop upload timing column")
    require("crop draw p95" in table, "table should include crop draw timing column")
    require("speed graph p95" in table, "table should include speed graph timing column")
    require("dominant p95" in table, "table should include dominant timing column")
    require("render-present 4.00" in table, "table should include dominant timing value")
    require("dom share" in table, "table should include dominant timing share column")
    require("main uploads" in table, "table should include main upload count column")
    require("crop uploads" in table, "table should include crop upload count column")
    require("crop backend" in table, "table should include crop backend column")
    require("external_ipc" in table, "table should include crop backend value")
    require("ext drops" in table, "table should include external crop drop column")
    require("ext q depth" in table, "table should include external queue depth column")
    require("ext gpus" in table, "table should include external crop GPU placement column")
    require("2010096:7->7" in table, "table should include external crop GPU placement values")
    require("ext same-gpu" in table, "table should include same-GPU external crop column")
    require("2010095:5->5" in table, "table should include same-GPU external crop values")
    require("ext q high" in table, "table should include external queue high-water column")
    require("ext q age p95" in table, "table should include external enqueue-age column")
    require("ext status" in table, "table should include external recorder status column")
    require("ext hb min" in table, "table should include external recorder heartbeat column")
    require("ok" in table, "table should include healthy external recorder status")


def test_threshold_failures_cover_external_recorder_status() -> None:
    healthy = compare.summarize_validation("healthy", sample_payload())
    missing_payload = sample_payload()
    missing_payload.pop("external_recorder_status", None)
    missing = compare.summarize_validation("missing", missing_payload)
    failed_payload = sample_payload()
    failed_payload["external_recorder_status"]["crop"]["2010096"]["status"] = "failed"
    failed_payload["external_recorder_status"]["crop"]["2010096"]["runtime_valid"] = False
    failed = compare.summarize_validation("failed", failed_payload)
    storage_payload = sample_payload()
    storage_payload["external_recorder_status"]["full"]["2010095"]["storage_ok"] = True
    storage_payload["external_recorder_status"]["full"]["2010095"]["storage_low_space"] = True
    storage = compare.summarize_validation("storage", storage_payload)
    missing_storage_payload = sample_payload()
    missing_storage_payload["external_recorder_status"]["full"]["2010096"].pop(
        "storage_checked",
        None,
    )
    missing_storage = compare.summarize_validation(
        "missing-storage",
        missing_storage_payload,
    )
    args = SimpleNamespace(
        require_pass=False,
        require_zero_crop_drops=False,
        require_visible_samples=False,
        require_hidden_samples=False,
        require_matching_cameras=False,
        require_matching_display_config=False,
        require_matching_crop_config=False,
        require_external_crop_recorder_gpu_separate_from_analytics=False,
        require_external_recorder_status=True,
        require_external_recorder_storage_preflight=True,
        min_gui_overall_p05_fps=None,
        min_gui_visible_p05_fps=None,
        min_gui_hidden_p05_fps=None,
        max_external_crop_queue_high_water=None,
        max_external_crop_enqueue_age_p95_ms=None,
    )

    require(not compare.threshold_failures(args, [healthy]), "healthy recorder status should pass")
    failures = compare.threshold_failures(args, [missing, failed, storage, missing_storage])
    require(
        any("external recorder status missing" in failure for failure in failures),
        "missing external status should fail",
    )
    require(
        any("crop:2010096" in failure for failure in failures),
        "failed external status stream should be named",
    )
    require(
        any("full:2010095" in failure for failure in failures),
        "low-space external status stream should be named",
    )
    require(
        any("full:2010096" in failure for failure in failures),
        "missing storage preflight stream should be named",
    )


def test_uses_validator_timing_diagnosis_when_present() -> None:
    payload = sample_payload()
    payload["gui_display_frame_rate"]["timing_diagnosis"] = {
        "dominant_timing_bucket": "main_texture_upload_ms",
        "dominant_timing_label": "main-texture-upload",
        "dominant_timing_p95_ms": 9.0,
        "dominant_timing_fraction_of_frame_total_p95": 0.5,
    }
    summary = compare.summarize_validation("diagnosed", payload)
    require(
        summary["gui_dominant_timing_bucket"] == "main_texture_upload_ms",
        "validator-provided timing diagnosis should take precedence",
    )
    require(summary["gui_dominant_timing_p95_ms"] == 9.0, "validator diagnosis p95 should parse")


def test_table_marks_absent_fanout_as_unavailable() -> None:
    payload = sample_payload()
    for item in payload["crop_preview"].values():
        for key in list(item):
            if key.startswith("producer_"):
                del item[key]
    summary = compare.summarize_validation("legacy", payload)
    require(
        summary["producer_recording_crop_frame_accepted_total"] is None,
        "missing recording fanout should stay unavailable",
    )
    table = compare.render_table([summary])
    require(" - " in table, "table should render unavailable fanout fields as dashes")
    require("0/0" not in table, "missing fanout should not look like real zero fanout")


def test_absent_timing_counts_stay_unavailable() -> None:
    payload = sample_payload()
    timings = payload["gui_display_frame_rate"]["timings"]
    del timings["main_texture_upload_count"]
    del timings["crop_texture_upload_count"]
    summary = compare.summarize_validation("legacy-timing", payload)
    require(
        summary["gui_main_texture_upload_count"] is None,
        "missing main upload count should stay unavailable",
    )
    require(
        summary["gui_crop_texture_upload_count"] is None,
        "missing crop upload count should stay unavailable",
    )


def test_cli_thresholds_fail_with_clear_stderr() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / "visible.json"
        path.write_text(json.dumps(sample_payload(visible_p05=42.0)) + "\n", encoding="utf-8")

        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                f"visible={path}",
                "--require-pass",
                "--require-zero-crop-drops",
                "--require-visible-samples",
                "--require-matching-cameras",
                "--require-matching-display-config",
                "--require-matching-crop-config",
                "--min-gui-visible-p05-fps",
                "45",
                "--max-external-crop-queue-high-water",
                "8",
                "--max-external-crop-enqueue-age-p95-ms",
                "2",
                "--require-external-crop-recorder-gpu-separate-from-analytics",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        require(result.returncode == 1, "CLI should fail when thresholds are missed")
        require("visible GUI p05 FPS=42.0 below 45.0" in result.stderr, "CLI stderr should name FPS miss")
        require("external crop queue high-water=12.0 > 8" in result.stderr, "CLI stderr should name queue miss")
        require(
            "uses analytics GPU" in result.stderr,
            "CLI stderr should name same-GPU external crop placement",
        )
        require("dominant p95" in result.stdout, "CLI table should still include dominant timing column")


def test_cli_thresholds_pass_with_zero_sample_hidden_bucket() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / "visible.json"
        path.write_text(json.dumps(sample_payload(visible_p05=58.0)) + "\n", encoding="utf-8")

        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                f"visible={path}",
                "--require-pass",
                "--require-zero-crop-drops",
                "--require-visible-samples",
                "--min-gui-visible-p05-fps",
                "45",
                "--min-gui-hidden-p05-fps",
                "45",
                "--max-external-crop-queue-high-water",
                "64",
                "--max-external-crop-enqueue-age-p95-ms",
                "80",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        require(result.returncode == 0, f"CLI should pass: {result.stderr}")
        require("hidden GUI p05" not in result.stderr, "zero-sample hidden bucket should not fail")
        require("render-present 4.00" in result.stdout, "CLI table should show dominant timing value")


def test_cli_sample_presence_gate_fails_without_matching_run() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / "visible.json"
        path.write_text(json.dumps(sample_payload(visible_p05=58.0)) + "\n", encoding="utf-8")

        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                f"visible={path}",
                "--require-hidden-samples",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        require(result.returncode == 1, "CLI should fail when required hidden samples are absent")
        require(
            "no compared run has hidden crop-preview GUI FPS samples" in result.stderr,
            "CLI stderr should explain missing hidden samples",
        )


def test_cli_json_includes_threshold_failures_and_status() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / "visible.json"
        path.write_text(json.dumps(sample_payload(visible_p05=42.0)) + "\n", encoding="utf-8")

        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                f"visible={path}",
                "--json",
                "--min-gui-visible-p05-fps",
                "45",
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        require(result.returncode == 1, "JSON CLI should still fail when thresholds are missed")
        payload = json.loads(result.stdout)
        require(payload["status"] == "fail", "JSON output should expose comparison status")
        require(
            any("visible GUI p05 FPS=42.0 below 45.0" in item for item in payload["threshold_failures"]),
            "JSON output should include threshold failure detail",
        )
        require(payload["runs"][0]["label"] == "visible", "JSON output should preserve run summaries")


def test_cli_rejects_negative_numeric_thresholds() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "dummy.json",
            "--min-gui-visible-p05-fps",
            "-1",
        ],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    require(result.returncode == 2, "negative numeric threshold should fail argparse")
    require("must be >= 0" in result.stderr, "argparse error should explain nonnegative requirement")


def main() -> int:
    tests = [
        test_summarize_validation_aggregates_crop_preview_and_fps,
        test_compare_adds_deltas_against_first_run,
        test_require_zero_crop_drops_would_fail_for_drops,
        test_threshold_failures_cover_fps_and_external_queue_pressure,
        test_threshold_failures_cover_mismatched_camera_and_display_config,
        test_threshold_failures_cover_mismatched_crop_config,
        test_threshold_failures_cover_mismatched_yolo_runtime_config,
        test_threshold_failures_accept_equivalent_kernel_cpu_list_syntax,
        test_summarize_validation_prefers_validator_normalized_kernel_options,
        test_threshold_failures_cover_mismatched_external_crop_gpu_mapping,
        test_threshold_failures_cover_same_external_crop_gpu_mapping,
        test_threshold_failures_cover_missing_external_crop_gpu_mapping,
        test_table_contains_expected_columns_and_values,
        test_threshold_failures_cover_external_recorder_status,
        test_uses_validator_timing_diagnosis_when_present,
        test_table_marks_absent_fanout_as_unavailable,
        test_absent_timing_counts_stay_unavailable,
        test_cli_thresholds_fail_with_clear_stderr,
        test_cli_thresholds_pass_with_zero_sample_hidden_bucket,
        test_cli_sample_presence_gate_fails_without_matching_run,
        test_cli_json_includes_threshold_failures_and_status,
        test_cli_rejects_negative_numeric_thresholds,
    ]
    for test in tests:
        test()
    print("compare_gui_crop_preview_validation_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
