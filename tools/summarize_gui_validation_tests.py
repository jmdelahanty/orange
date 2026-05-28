#!/usr/bin/env python3
"""Focused tests for summarize_gui_validation crop/fanout summaries."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

import summarize_gui_validation as summarize  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_text(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")


def write_external_recorder_status_fixture(
    recording_folder: Path,
    serial: str,
    *,
    crop: bool = False,
    rows: int = 3,
    heartbeat_sequence: int = 4,
    runtime_heartbeat_sequence: int | None = None,
) -> None:
    artifact_root = recording_folder / (
        "external_crop_recorder" if crop else "external_recorder"
    )
    artifact_root.mkdir(exist_ok=True)
    name_prefix = f"Cam{serial}_crop_external" if crop else f"Cam{serial}_external"
    stream_id = f"{serial}_crop" if crop else serial
    contract_path = recording_folder / (
        "external_crop_recorder_contract.json"
        if crop
        else "external_recorder_contract.json"
    )
    summary_path = artifact_root / f"{name_prefix}_summary.json"
    status_path = artifact_root / f"{name_prefix}_status.json"
    runtime_path = artifact_root / "external_recorder_supervisor_runtime.json"
    write_text(
        summary_path,
        json.dumps(
            {
                "frames_received": rows,
                "frames_encoded": rows,
                "acks_sent": rows,
            }
        )
        + "\n",
    )
    write_text(
        status_path,
        json.dumps(
            {
                "schema_id": "orange.external_recorder.status",
                "schema_version": 1,
                "status": "completed",
                "heartbeat_sequence": heartbeat_sequence,
                "frames_received": rows,
                "frames_encoded": rows,
                "acks_sent": rows,
                "worker_failed": False,
            }
        )
        + "\n",
    )
    write_text(
        runtime_path,
        json.dumps(
            {
                "schema_id": "orange.external_recorder.supervisor_runtime",
                "schema_version": 1,
                "processes": [
                    {
                        "status_json_path": str(status_path),
                        "recorder_status": {
                            "present": True,
                            "valid": True,
                            "status": "completed",
                            "heartbeat_sequence": (
                                runtime_heartbeat_sequence
                                if runtime_heartbeat_sequence is not None
                                else heartbeat_sequence
                            ),
                        },
                    }
                ],
            }
        )
        + "\n",
    )
    write_text(
        contract_path,
        json.dumps(
            {
                "schema_id": "orange.external_recorder.contract",
                "schema_version": 1,
                "artifact_root": str(artifact_root),
                "streams": {
                    stream_id: {
                        "stream_id": stream_id,
                        "camera_serial": stream_id,
                        "summary_json": str(summary_path),
                        "status_json": str(status_path),
                    }
                },
            }
        )
        + "\n",
    )


def test_crop_summary_reads_rows_preview_and_fanout() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010093"
        snapshot = {
            "schema_version": 2,
            "session": {
                "gui_display_frame_rate": {
                    "timings": {
                        "frame_total_ms": {"sample_count": 10, "p95_ms": 20.0},
                        "main_texture_upload_ms": {"sample_count": 10, "p95_ms": 2.0},
                        "crop_texture_upload_ms": {"sample_count": 10, "p95_ms": 0.5},
                        "camera_window_draw_ms": {"sample_count": 10, "p95_ms": 7.0},
                        "crop_window_draw_ms": {"sample_count": 10, "p95_ms": 1.0},
                        "speed_graph_draw_ms": {"sample_count": 10, "p95_ms": 0.0},
                        "render_present_ms": {"sample_count": 10, "p95_ms": 5.0},
                    }
                }
            },
            "recording_outputs": {
                serial: {
                    "crop": {
                        "output_kind": "crop",
                        "role": "sidecar",
                        "backend": "external_ipc",
                        "status": "completed",
                        "frame_count": 3,
                        "metadata": f"Cam{serial}_crop_meta.csv",
                        "perf": f"Cam{serial}_crop_perf.csv",
                        "sidecar_perf": f"Cam{serial}_crop_sidecar_perf.csv",
                        "summary": f"Cam{serial}_crop_external_summary.json",
                    }
                }
            },
        }
        session = {
            "recording_outputs": snapshot["recording_outputs"],
            "recording_backend": {
                "crop_recording": {
                    "stream_config": {
                        serial: {
                            "stream_id": f"{serial}_crop",
                            "analytics_gpu_id": 5,
                            "recorder_gpu_id": 6,
                            "socket_path": f"/tmp/orange_external_crop_recorder_{serial}.sock",
                            "encode_queue_depth": 64,
                        }
                    }
                }
            },
        }
        write_text(root / "recording_snapshot.json", json.dumps(snapshot) + "\n")
        write_text(root / "recording_session.json", json.dumps(session) + "\n")
        write_text(
            root / f"Cam{serial}_crop_meta.csv",
            "\n".join(
                [
                    "recording_frame_id,has_detection",
                    "1,1",
                    "2,0",
                    "3,1",
                    "",
                ]
            ),
        )
        write_text(
            root / f"Cam{serial}_crop_perf.csv",
            "\n".join(
                [
                    "recording_frame_id,dropped",
                    "1,0",
                    "2,0",
                    "3,1",
                    "",
                ]
            ),
        )
        write_text(
            root / f"Cam{serial}_crop_sidecar_perf.csv",
            "\n".join(
                [
                    "crop_frame_pool_size,preview_max_fps,preview_disabled,"
                    "preview_display_enabled_final,preview_frames_offered,"
                    "preview_frames_updated,preview_frames_skipped_by_cadence,"
                    "preview_queue_full_drops,producer_recording_crop_frame_offered,"
                    "producer_recording_crop_frame_accepted,"
                    "producer_recording_crop_frame_dropped,"
                    "producer_preview_crop_frame_offered,"
                    "producer_preview_crop_frame_accepted,"
                    "producer_preview_crop_frame_dropped,"
                    "producer_pose_crop_frame_offered,"
                    "producer_pose_crop_frame_accepted,"
                    "producer_pose_crop_frame_dropped",
                    "32,15,0,1,12,3,9,0,2,2,0,3,3,0,0,0,0",
                    "",
                ]
            ),
        )
        write_text(
            root / f"Cam{serial}_crop_external_summary.json",
            json.dumps(
                {
                    "frames_received": 3,
                    "frames_encoded": 3,
                    "encode_dropped": 0,
                    "encode_queue_depth": 256,
                    "encode_queue_high_water": 7,
                    "external_encode": {
                        "frames_dropped": 0,
                        "enqueue_age_p95_ms": 1.75,
                        "encode_total_p95_ms": 1.25,
                        "lock_bitstream_p95_ms": 0.5,
                    },
                }
            )
            + "\n",
        )

        summary = summarize.summarize(root, steady_after_frame=50, ffprobe="ffprobe")
        crop = summary["crop"][serial]
        require(crop["backend"] == "external_ipc", "crop backend should come from descriptor")
        require(crop["metadata_rows"] == 3, "metadata rows should count")
        require(crop["metadata_detection_rows"] == 2, "detection rows should count")
        require(crop["perf_rows"] == 3, "perf rows should count")
        require(crop["perf_dropped_rows"] == 1, "perf dropped rows should count")
        require(crop["crop_frame_pool_size"] == 32, "pool size should parse")
        require(crop["preview_frames_updated"] == 3, "preview updated should parse")
        require(crop["preview_frames_offered"] == 12, "preview offered should parse")
        require(
            crop["producer_recording_crop_frame_accepted"] == 2,
            "recording fanout accepted should parse",
        )
        require(
            crop["producer_preview_crop_frame_accepted"] == 3,
            "preview fanout accepted should parse",
        )
        require(crop["external_frames_received"] == 3, "external received count should parse")
        require(crop["external_frames_encoded"] == 3, "external encoded count should parse")
        require(crop["external_frames_dropped"] == 0, "external dropped count should parse")
        require(crop["external_encode_dropped"] == 0, "external encode dropped count should parse")
        require(crop["external_encode_queue_depth"] == 256, "external queue depth should parse")
        require(crop["external_encode_queue_high_water"] == 7, "external queue high-water should parse")
        require(crop["external_enqueue_age_p95_ms"] == 1.75, "external enqueue age p95 should parse")
        require(crop["external_encode_total_p95_ms"] == 1.25, "external encode p95 should parse")
        require(crop["external_lock_bitstream_p95_ms"] == 0.5, "external lock p95 should parse")
        require(
            crop["external_stream_config"]["recorder_gpu_id"] == 6,
            "external stream config should be copied from recording_backend",
        )
        require(
            crop["external_stream_config_source"] == "recording_backend.crop_recording.stream_config",
            "recording_backend stream config should be identified as the source",
        )
        require(crop["external_analytics_gpu_id"] == 5, "analytics GPU should parse")
        require(crop["external_recorder_gpu_id"] == 6, "recorder GPU should parse")
        require(crop["external_gpu_mapping"] == "5->6", "external GPU mapping should parse")
        require(
            crop["external_same_gpu_as_analytics"] is False,
            "separate GPU mapping should not use the analytics GPU",
        )
        diagnosis = summary["gui_display_diagnosis"]
        require(
            diagnosis["dominant_timing_bucket"] == "camera_window_draw_ms",
            "GUI timing diagnosis should identify the largest p95 bucket",
        )
        require(
            diagnosis["dominant_timing_fraction_of_frame_total_p95"] == 7.0 / 20.0,
            "GUI timing diagnosis should compute dominant p95 share",
        )
        output = summary["outputs"][serial]["crop"]
        require(
            "sidecar_perf" in output.get("paths", {}),
            "recording output summary should include sidecar_perf path",
        )


def test_crop_summary_uses_recording_backend_external_fallbacks() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010094"
        snapshot = {
            "schema_version": 2,
            "recording_outputs": {
                serial: {
                    "crop": {
                        "output_kind": "crop",
                        "role": "sidecar",
                        "backend": "external_ipc",
                        "status": "completed",
                        "metadata": f"Cam{serial}_crop_meta.csv",
                        "perf": f"Cam{serial}_crop_perf.csv",
                        "sidecar_perf": f"Cam{serial}_crop_sidecar_perf.csv",
                    }
                }
            },
        }
        session = {
            "recording_outputs": snapshot["recording_outputs"],
            "recording_backend": {
                "crop_recording": {
                    "stream_config": {
                        serial: {
                            "stream_id": f"{serial}_crop",
                            "analytics_gpu_id": 7,
                            "recorder_gpu_id": 8,
                            "socket_path": f"/tmp/orange_external_crop_recorder_{serial}.sock",
                            "encode_queue_depth": 64,
                        }
                    },
                    "frames_received": {serial: 11},
                    "frames_encoded": {serial: 11},
                    "encode_dropped": {serial: 0},
                    "external_frames_dropped": {serial: 0},
                    "encode_queue_high_water": {serial: 9},
                    "enqueue_age_p95_ms": {serial: 3.5},
                }
            },
        }
        write_text(root / "recording_snapshot.json", json.dumps(snapshot) + "\n")
        write_text(root / "recording_session.json", json.dumps(session) + "\n")
        write_text(root / f"Cam{serial}_crop_meta.csv", "recording_frame_id,has_detection\n1,1\n")
        write_text(root / f"Cam{serial}_crop_perf.csv", "recording_frame_id,dropped\n1,0\n")
        write_text(root / f"Cam{serial}_crop_sidecar_perf.csv", "crop_frame_pool_size\n32\n")

        summary = summarize.summarize(root, steady_after_frame=50, ffprobe="ffprobe")
        crop = summary["crop"][serial]
        require(crop["external_frames_received"] == 11, "backend received fallback should parse")
        require(crop["external_frames_encoded"] == 11, "backend encoded fallback should parse")
        require(crop["external_encode_dropped"] == 0, "backend encode dropped fallback should parse")
        require(crop["external_frames_dropped"] == 0, "backend frame dropped fallback should parse")
        require(crop["external_encode_queue_depth"] == 64, "stream config queue depth fallback should parse")
        require(crop["external_encode_queue_high_water"] == 9, "backend high-water fallback should parse")
        require(crop["external_enqueue_age_p95_ms"] == 3.5, "backend enqueue p95 fallback should parse")
        require(
            crop["external_stream_config"]["recorder_gpu_id"] == 8,
            "backend stream config should remain visible",
        )
        require(crop["external_gpu_mapping"] == "7->8", "backend GPU mapping should parse")
        require(
            crop["external_same_gpu_as_analytics"] is False,
            "backend GPU mapping should not use the analytics GPU",
        )


def test_crop_summary_marks_same_external_gpu_as_analytics() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        snapshot = {
            "schema_version": 2,
            "recording_outputs": {
                serial: {
                    "crop": {
                        "output_kind": "crop",
                        "role": "sidecar",
                        "backend": "external_ipc",
                        "status": "completed",
                        "metadata": f"Cam{serial}_crop_meta.csv",
                        "perf": f"Cam{serial}_crop_perf.csv",
                        "sidecar_perf": f"Cam{serial}_crop_sidecar_perf.csv",
                    }
                }
            },
        }
        session = {
            "recording_outputs": snapshot["recording_outputs"],
            "recording_backend": {
                "crop_recording": {
                    "stream_config": {
                        serial: {
                            "stream_id": f"{serial}_crop",
                            "analytics_gpu_id": 5,
                            "recorder_gpu_id": 5,
                            "socket_path": f"/tmp/orange_external_crop_recorder_{serial}.sock",
                            "encode_queue_depth": 64,
                        }
                    }
                }
            },
        }
        write_text(root / "recording_snapshot.json", json.dumps(snapshot) + "\n")
        write_text(root / "recording_session.json", json.dumps(session) + "\n")
        write_text(root / f"Cam{serial}_crop_meta.csv", "recording_frame_id,has_detection\n1,1\n")
        write_text(root / f"Cam{serial}_crop_perf.csv", "recording_frame_id,dropped\n1,0\n")
        write_text(root / f"Cam{serial}_crop_sidecar_perf.csv", "crop_frame_pool_size\n32\n")

        summary = summarize.summarize(root, steady_after_frame=50, ffprobe="ffprobe")
        crop = summary["crop"][serial]
        require(crop["external_gpu_mapping"] == "5->5", "same-GPU mapping should parse")
        require(
            crop["external_same_gpu_as_analytics"] is True,
            "same-GPU mapping should be marked",
        )


def test_crop_summary_uses_external_crop_contract_stream_config_fallback() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010096"
        snapshot = {
            "schema_version": 2,
            "recording_outputs": {
                serial: {
                    "crop": {
                        "output_kind": "crop",
                        "role": "sidecar",
                        "backend": "external_ipc",
                        "status": "completed",
                        "metadata": f"Cam{serial}_crop_meta.csv",
                        "perf": f"Cam{serial}_crop_perf.csv",
                        "sidecar_perf": f"Cam{serial}_crop_sidecar_perf.csv",
                        "details": {
                            "stream_id": f"{serial}_crop",
                            "video_backend": "external_ipc",
                        },
                    }
                }
            },
        }
        contract = {
            "streams": {
                f"{serial}_crop": {
                    "stream_id": f"{serial}_crop",
                    "analytics_gpu_id": 5,
                    "recorder_gpu_id": 8,
                    "socket_path": f"/tmp/orange_external_recorder_{serial}_crop.sock",
                    "encode_queue_depth": 64,
                }
            }
        }
        write_text(root / "recording_snapshot.json", json.dumps(snapshot) + "\n")
        write_text(root / "recording_session.json", json.dumps({"recording_outputs": snapshot["recording_outputs"]}) + "\n")
        write_text(root / "external_crop_recorder_contract.json", json.dumps(contract) + "\n")
        write_text(root / f"Cam{serial}_crop_meta.csv", "recording_frame_id,has_detection\n1,1\n")
        write_text(root / f"Cam{serial}_crop_perf.csv", "recording_frame_id,dropped\n1,0\n")
        write_text(root / f"Cam{serial}_crop_sidecar_perf.csv", "crop_frame_pool_size\n32\n")

        summary = summarize.summarize(root, steady_after_frame=50, ffprobe="ffprobe")
        crop = summary["crop"][serial]
        require(
            crop["external_stream_config_source"] == "external_crop_recorder_contract.json",
            "external crop contract should be identified as the fallback source",
        )
        require(crop["external_gpu_mapping"] == "5->8", "contract GPU mapping should parse")
        require(
            crop["external_same_gpu_as_analytics"] is False,
            "contract GPU mapping should mark separate CUDA devices",
        )
        require(
            crop["external_stream_config"]["socket_path"] == f"/tmp/orange_external_recorder_{serial}_crop.sock",
            "contract socket path should be used as stream config",
        )


def test_external_recorder_status_summary_reads_full_and_crop_sidecars() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_text(root / "recording_snapshot.json", "{}\n")
        write_text(root / "recording_session.json", "{}\n")
        write_external_recorder_status_fixture(
            root,
            "2010095",
            crop=False,
            rows=3,
            heartbeat_sequence=4,
        )
        write_external_recorder_status_fixture(
            root,
            "2010095",
            crop=True,
            rows=2,
            heartbeat_sequence=5,
            runtime_heartbeat_sequence=6,
        )

        summary = summarize.summarize(root, steady_after_frame=50, ffprobe="ffprobe")
        full = summary["external_recorder_status"]["full"]["2010095"]
        crop = summary["external_recorder_status"]["crop"]["2010095"]

        require(full["status"] == "completed", "full recorder status should parse")
        require(full["status_json_exists"] is True, "full status sidecar should be marked present")
        require(full["runtime_valid"] is True, "full runtime status should parse")
        require(full["counts_match_summary"] is True, "full status counts should match summary")
        require(full["heartbeat_sequence"] == 4, "full heartbeat should parse")
        require(full["runtime_heartbeat_sequence"] == 4, "full runtime heartbeat should parse")
        require(crop["frames_received"] == 2, "crop received count should parse")
        require(crop["frames_encoded"] == 2, "crop encoded count should parse")
        require(crop["acks_sent"] == 2, "crop ACK count should parse")
        require(crop["runtime_heartbeat_sequence"] == 6, "crop runtime heartbeat should parse")


def test_latest_complete_selects_newest_complete_recording() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        incomplete = root / "2026_05_27_10_00_00"
        complete = root / "2026_05_27_11_00_00"
        incomplete.mkdir()
        complete.mkdir()
        for folder in (incomplete, complete):
            write_text(folder / "recording_snapshot.json", "{}\n")
        serial = "2010093"
        write_text(complete / f"Cam{serial}.mp4", "video")
        write_text(complete / f"Cam{serial}_pipeline_perf.csv", "frame_id\n1\n")
        write_text(complete / f"Cam{serial}_yolo_perf.csv", "frame_id\n1\n")

        selected = summarize.resolve_latest_recording_folder(root, require_complete=True)
        require(selected == complete.resolve(), "latest complete should skip incomplete folders")


def test_latest_complete_accepts_external_camera_artifact_video() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        complete = root / "2026_05_27_12_00_00"
        complete.mkdir()
        serial = "2010093"
        video = complete / "external_recorder" / f"Cam{serial}_external.mp4"
        video.parent.mkdir()
        write_text(video, "video")
        write_text(
            complete / "recording_session.json",
            json.dumps({"camera_artifacts": {serial: {"video": str(video)}}}) + "\n",
        )
        write_text(complete / "recording_snapshot.json", "{}\n")
        write_text(complete / f"Cam{serial}_pipeline_perf.csv", "frame_id\n1\n")
        write_text(complete / f"Cam{serial}_yolo_perf.csv", "frame_id\n1\n")

        selected = summarize.resolve_latest_recording_folder(root, require_complete=True)
        require(selected == complete.resolve(), "external camera_artifacts video should count as complete")


def test_yolo_summary_reports_affinity() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010093"
        write_text(root / "recording_snapshot.json", "{}\n")
        write_text(root / "recording_session.json", "{}\n")
        write_text(
            root / f"Cam{serial}_yolo_perf.csv",
            "\n".join(
                [
                    "frame_id,ok,acquisition_to_detect_done_ms,"
                    "acquisition_to_worker_start_ms,yolo_enqueue_to_dequeue_ms,"
                    "yolo_dequeue_to_worker_start_ms,yolo_queue_wait_ms,"
                    "same_camera_service_gap_ms,"
                    "yolo_affinity_configured,yolo_affinity_applied,"
                    "yolo_affinity_env_key,yolo_affinity_requested_cpus,"
                    "yolo_affinity_effective_cpus",
                    f"1,1,4.0,0.10,0.02,0.01,0.03,10.0,1,1,"
                    f"ORANGE_YOLO_AFFINITY_CAM_{serial},6,6",
                    "",
                ]
            ),
        )

        summary = summarize.summarize(root, steady_after_frame=1, ffprobe="ffprobe")
        yolo = summary["yolo"][serial]
        affinity = yolo["affinity"]
        require(affinity["configured"] == 1, "YOLO affinity configured flag should parse")
        require(affinity["applied"] == 1, "YOLO affinity applied flag should parse")
        require(
            affinity["env_key"] == f"ORANGE_YOLO_AFFINITY_CAM_{serial}",
            "YOLO affinity env key should parse",
        )
        require(affinity["requested_cpus"] == "6", "YOLO requested affinity should parse")
        require(affinity["effective_cpus"] == "6", "YOLO effective affinity should parse")
        metrics = yolo["metrics"]
        require(
            metrics["acquisition_to_worker_start_ms"]["p95"] == 0.10,
            "YOLO acquisition-to-worker metric should parse",
        )
        require(
            metrics["yolo_enqueue_to_dequeue_ms"]["p95"] == 0.02,
            "YOLO enqueue-to-dequeue metric should parse",
        )
        require(
            metrics["same_camera_service_gap_ms"]["p95"] == 10.0,
            "YOLO service-gap metric should parse",
        )


def test_system_cpu_summary_reports_isolation() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        snapshot = {
            "session": {
                "system_cpu": {
                    "schema_version": 1,
                    "isolated_cpus": {
                        "available": True,
                        "parse_ok": True,
                        "raw": "1-2,6,8,10,12",
                        "cpus": [1, 2, 6, 8, 10, 12],
                    },
                    "kernel_cmdline": {
                        "available": True,
                        "options": {
                            "isolcpus": "managed_irq,domain,1,2,6,8,10,12,38,40,42,44",
                            "nohz_full": "1,2,6,8,10,12,38,40,42,44",
                            "rcu_nocbs": "1,2,6,8,10,12,38,40,42,44",
                        },
                    },
                }
            }
        }
        write_text(root / "recording_snapshot.json", json.dumps(snapshot) + "\n")
        write_text(root / "recording_session.json", "{}\n")

        summary = summarize.summarize(root, steady_after_frame=50, ffprobe="ffprobe")
        system_cpu = summary["system_cpu"]
        require(
            system_cpu["isolated_cpus"]["cpus"] == [1, 2, 6, 8, 10, 12],
            "system CPU isolation list should be carried into summary",
        )
        require(
            system_cpu["kernel_cmdline"]["options"]["nohz_full"]
            == "1,2,6,8,10,12,38,40,42,44",
            "kernel cmdline nohz_full option should be visible in summary",
        )
        require(
            summary["system_cpu_kernel_cmdline_cpu_option_values"] == [
                "isolcpus=cpus:1-2,6,8,10,12,38,40,42,44;flags:domain|managed_irq",
                "nohz_full=cpus:1-2,6,8,10,12,38,40,42,44",
                "rcu_nocbs=cpus:1-2,6,8,10,12,38,40,42,44",
            ],
            "normalized kernel cmdline CPU options should be visible in summary",
        )


def main() -> int:
    test_crop_summary_reads_rows_preview_and_fanout()
    test_crop_summary_uses_recording_backend_external_fallbacks()
    test_crop_summary_marks_same_external_gpu_as_analytics()
    test_crop_summary_uses_external_crop_contract_stream_config_fallback()
    test_external_recorder_status_summary_reads_full_and_crop_sidecars()
    test_latest_complete_selects_newest_complete_recording()
    test_latest_complete_accepts_external_camera_artifact_video()
    test_yolo_summary_reports_affinity()
    test_system_cpu_summary_reports_isolation()
    print("summarize_gui_validation_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
