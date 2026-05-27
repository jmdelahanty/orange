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
        session = {"recording_outputs": snapshot["recording_outputs"]}
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
        output = summary["outputs"][serial]["crop"]
        require(
            "sidecar_perf" in output.get("paths", {}),
            "recording output summary should include sidecar_perf path",
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
    test_latest_complete_selects_newest_complete_recording()
    test_latest_complete_accepts_external_camera_artifact_video()
    print("summarize_gui_validation_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
