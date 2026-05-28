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


def main() -> int:
    test_crop_summary_reads_rows_preview_and_fanout()
    test_crop_summary_uses_recording_backend_external_fallbacks()
    test_crop_summary_marks_same_external_gpu_as_analytics()
    test_crop_summary_uses_external_crop_contract_stream_config_fallback()
    test_latest_complete_selects_newest_complete_recording()
    test_latest_complete_accepts_external_camera_artifact_video()
    print("summarize_gui_validation_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
