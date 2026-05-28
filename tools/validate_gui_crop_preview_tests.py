#!/usr/bin/env python3
"""Focused tests for GUI crop-preview validation helpers."""

from __future__ import annotations

import csv
import json
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

import validate_gui_ptp_recording as validator  # noqa: E402
from recording_output_validation import recording_output_contract_errors  # noqa: E402


HEADER = [
    "camera_serial",
    "gpu_id",
    "worker",
    "queue_size",
    "producer_jobs_offered",
    "producer_jobs_enqueued",
    "producer_queue_full_drops",
    "producer_blank_jobs_offered",
    "producer_blank_jobs_enqueued",
    "producer_dropped_jobs_offered",
    "producer_dropped_jobs_enqueued",
    "consumer_jobs_enqueued",
    "consumer_queue_full_drops",
    "consumer_queue_high_water",
    "crop_frame_pool_size",
    "producer_recording_crop_frame_offered",
    "producer_recording_crop_frame_accepted",
    "producer_recording_crop_frame_dropped",
    "producer_preview_crop_frame_offered",
    "producer_preview_crop_frame_accepted",
    "producer_preview_crop_frame_dropped",
    "producer_pose_crop_frame_offered",
    "producer_pose_crop_frame_accepted",
    "producer_pose_crop_frame_dropped",
    "producer_frames_produced_total",
    "producer_frames_recycled_total",
    "producer_crop_frame_release_total",
    "producer_crop_frame_pool_misses_total",
    "producer_source_release_event_misses_total",
    "producer_pending_source_releases",
    "producer_pending_crop_frame_recycles",
    "preview_max_fps",
    "preview_disabled",
    "preview_display_enabled_final",
    "preview_frames_offered",
    "preview_frames_updated",
    "preview_frames_skipped_by_cadence",
    "preview_clears_updated",
    "preview_queue_full_drops",
    "preview_queue_high_water",
    "preview_serial_final",
]

CROP_META_HEADER = [
    "recording_frame_id",
    "local_frame_id",
    "camera_frame_id",
    "timestamp",
    "timestamp_sys",
    "has_detection",
    "blank_frame",
    "detection_confidence",
    "crop_x",
    "crop_y",
    "crop_w",
    "crop_h",
    "detection_x",
    "detection_y",
    "detection_w",
    "detection_h",
]

CROP_PERF_HEADER = [
    "recording_frame_id",
    "local_frame_id",
    "camera_frame_id",
    "dropped",
    "drop_reason",
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_sidecar(
    recording_folder: Path,
    serial: str,
    *,
    preview_max_fps: int = 15,
    preview_disabled: int = 0,
    preview_display_enabled: int = 1,
    offered: int = 100,
    updated: int = 15,
    skipped: int = 85,
    clears: int = 1,
    serial_final: int = 16,
    crop_frame_pool_size: int = 32,
    recording_offered: int = 100,
    recording_accepted: int = 100,
    recording_dropped: int = 0,
    preview_crop_offered: int | None = None,
    preview_crop_accepted: int | None = None,
    preview_crop_dropped: int = 0,
) -> None:
    if preview_crop_offered is None:
        preview_crop_offered = updated
    if preview_crop_accepted is None:
        preview_crop_accepted = updated
    row = {
        "camera_serial": serial,
        "gpu_id": "5",
        "worker": "CropAndEncodeWorker",
        "queue_size": "40",
        "producer_jobs_offered": "100",
        "producer_jobs_enqueued": "100",
        "producer_queue_full_drops": "0",
        "producer_blank_jobs_offered": "0",
        "producer_blank_jobs_enqueued": "0",
        "producer_dropped_jobs_offered": "0",
        "producer_dropped_jobs_enqueued": "0",
        "consumer_jobs_enqueued": "100",
        "consumer_queue_full_drops": "0",
        "consumer_queue_high_water": "1",
        "crop_frame_pool_size": str(crop_frame_pool_size),
        "producer_recording_crop_frame_offered": str(recording_offered),
        "producer_recording_crop_frame_accepted": str(recording_accepted),
        "producer_recording_crop_frame_dropped": str(recording_dropped),
        "producer_preview_crop_frame_offered": str(preview_crop_offered),
        "producer_preview_crop_frame_accepted": str(preview_crop_accepted),
        "producer_preview_crop_frame_dropped": str(preview_crop_dropped),
        "producer_pose_crop_frame_offered": "0",
        "producer_pose_crop_frame_accepted": "0",
        "producer_pose_crop_frame_dropped": "0",
        "producer_frames_produced_total": "100",
        "producer_frames_recycled_total": "100",
        "producer_crop_frame_release_total": "100",
        "producer_crop_frame_pool_misses_total": "0",
        "producer_source_release_event_misses_total": "0",
        "producer_pending_source_releases": "0",
        "producer_pending_crop_frame_recycles": "0",
        "preview_max_fps": str(preview_max_fps),
        "preview_disabled": str(preview_disabled),
        "preview_display_enabled_final": str(preview_display_enabled),
        "preview_frames_offered": str(offered),
        "preview_frames_updated": str(updated),
        "preview_frames_skipped_by_cadence": str(skipped),
        "preview_clears_updated": str(clears),
        "preview_queue_full_drops": "0",
        "preview_queue_high_water": "1",
        "preview_serial_final": str(serial_final),
    }
    path = recording_folder / f"Cam{serial}_crop_sidecar_perf.csv"
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=HEADER)
        writer.writeheader()
        writer.writerow(row)


def check(
    recording_folder: Path,
    snapshot: dict,
    cameras: list[str],
    *,
    expected_preview_max_fps: int | None = None,
    expected_preview_display_enabled: int | None = None,
    expected_preview_disabled: int | None = None,
    min_crop_frame_pool_size: int | None = None,
    require_sampling: bool = False,
    require_counters: bool = False,
) -> tuple[validator.Reporter, dict]:
    reporter = validator.Reporter(verbose=False)
    summary = validator.check_crop_preview_counters(
        reporter,
        recording_folder,
        snapshot,
        cameras,
        expected_preview_max_fps,
        expected_preview_display_enabled,
        expected_preview_disabled,
        min_crop_frame_pool_size,
        require_sampling,
        require_counters,
    )
    return reporter, summary


def test_requires_only_crop_enabled_cameras() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095")
        snapshot = {
            "crop_outputs": {
                "2010095": {"enabled": True},
                "2010096": {"enabled": False},
            }
        }

        reporter, summary = check(
            root,
            snapshot,
            ["2010095", "2010096"],
            expected_preview_max_fps=15,
            require_counters=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(set(summary) == {"2010095"}, "only crop-enabled camera should be checked")
        require(summary["2010095"]["preview_frames_updated"] == 15, "updated count should parse")
        require(
            summary["2010095"]["producer_recording_crop_frame_accepted"] == 100,
            "recording crop-frame fanout count should parse",
        )
        require(
            summary["2010095"]["producer_preview_crop_frame_accepted"] == 15,
            "preview crop-frame fanout count should parse",
        )


def test_missing_crop_enabled_counter_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, _ = check(root, snapshot, ["2010095"], require_counters=True)
        require(reporter.failures, "missing required counter should fail")
        require("missing" in reporter.failures[0], "failure should describe missing sidecar")


def test_expected_preview_max_fps_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095", preview_max_fps=30)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, _ = check(
            root,
            snapshot,
            ["2010095"],
            expected_preview_max_fps=15,
            require_counters=True,
        )
        require(
            any("expected 15" in failure for failure in reporter.failures),
            "preview max FPS mismatch should fail",
        )


def test_updated_frames_cannot_exceed_offered_frames() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095", offered=10, updated=11)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, _ = check(root, snapshot, ["2010095"], require_counters=True)
        require(
            any("exceed offered" in failure for failure in reporter.failures),
            "updated > offered should fail",
        )


def test_expected_display_enabled_passes_for_hidden_preview() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095", preview_display_enabled=0)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, summary = check(
            root,
            snapshot,
            ["2010095"],
            expected_preview_display_enabled=0,
            require_counters=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(
            summary["2010095"]["preview_display_enabled_final"] == 0,
            "hidden preview final state should parse",
        )


def test_expected_preview_disabled_passes_for_disabled_preview() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095", preview_disabled=1, offered=0, updated=0, skipped=0)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, summary = check(
            root,
            snapshot,
            ["2010095"],
            expected_preview_disabled=1,
            require_counters=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(summary["2010095"]["preview_disabled"] == 1, "preview disabled state should parse")


def test_expected_preview_disabled_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095", preview_disabled=0)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, _ = check(
            root,
            snapshot,
            ["2010095"],
            expected_preview_disabled=1,
            require_counters=True,
        )
        require(
            any("expected 1" in failure for failure in reporter.failures),
            "preview disabled mismatch should fail",
        )


def test_min_crop_frame_pool_size_passes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095", crop_frame_pool_size=32)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, summary = check(
            root,
            snapshot,
            ["2010095"],
            min_crop_frame_pool_size=32,
            require_counters=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(summary["2010095"]["crop_frame_pool_size"] == 32, "crop pool size should parse")


def test_min_crop_frame_pool_size_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095", crop_frame_pool_size=8)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, _ = check(
            root,
            snapshot,
            ["2010095"],
            min_crop_frame_pool_size=32,
            require_counters=True,
        )
        require(
            any("expected >= 32" in failure for failure in reporter.failures),
            "small crop frame pool should fail",
        )


def test_recording_fanout_matches_detection_rows() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_sidecar(
            root,
            serial,
            recording_offered=2,
            recording_accepted=2,
            recording_dropped=0,
        )
        write_crop_recording_artifacts(root, serial, rows=4, detection_rows=2)
        reporter, summary = check(
            root,
            crop_snapshot(serial),
            [serial],
            require_counters=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(
            summary[serial]["crop_metadata_detection_rows"] == 2,
            "detection crop-row count should parse",
        )


def test_recording_fanout_detection_row_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_sidecar(
            root,
            serial,
            recording_offered=3,
            recording_accepted=3,
            recording_dropped=0,
        )
        write_crop_recording_artifacts(root, serial, rows=4, detection_rows=2)
        reporter, _ = check(
            root,
            crop_snapshot(serial),
            [serial],
            require_counters=True,
        )
        require(
            any("has_detection rows" in failure for failure in reporter.failures),
            "recording fanout/detection-row mismatch should fail",
        )


def test_recording_fanout_drops_fail() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_sidecar(
            root,
            serial,
            recording_offered=3,
            recording_accepted=2,
            recording_dropped=1,
        )
        write_crop_recording_artifacts(root, serial, rows=4, detection_rows=2)
        reporter, _ = check(
            root,
            crop_snapshot(serial),
            [serial],
            require_counters=True,
        )
        require(
            any("fanout drops=1" in failure for failure in reporter.failures),
            "recording fanout drops should fail",
        )


def test_preview_fanout_cannot_exceed_preview_updates() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_sidecar(
            root,
            serial,
            updated=3,
            preview_crop_offered=4,
            preview_crop_accepted=4,
        )
        reporter, _ = check(
            root,
            {"crop_outputs": {serial: {"enabled": True}}},
            [serial],
            require_counters=True,
        )
        require(
            any("exceeds preview updates" in failure for failure in reporter.failures),
            "preview fanout accepted > preview updates should fail",
        )


def crop_snapshot(serial: str, *, crop_size: int = 256, enabled: bool = True) -> dict:
    return {
        "crop_outputs": {
            serial: {
                "enabled": enabled,
                "runtime": {
                    "crop_size_px": crop_size,
                    "width": crop_size,
                    "height": crop_size,
                    "files": {
                        "video": f"Cam{serial}_crop.mp4",
                        "metadata": f"Cam{serial}_crop_meta.csv",
                        "keyframes": f"Cam{serial}_crop_keyframe.json",
                        "perf": f"Cam{serial}_crop_perf.csv",
                    },
                },
            }
        }
    }


def write_crop_recording_artifacts(
    recording_folder: Path,
    serial: str,
    *,
    rows: int = 3,
    keyframe_total_frames: int | None = None,
    crop_size: int = 256,
    perf_rows: int | None = None,
    dropped_row: int | None = None,
    detection_rows: int | None = None,
) -> None:
    (recording_folder / f"Cam{serial}_crop.mp4").write_bytes(b"not-a-real-mp4")
    (recording_folder / f"Cam{serial}_crop_keyframe.json").write_text(
        json.dumps({"total_frames": rows if keyframe_total_frames is None else keyframe_total_frames}) + "\n",
        encoding="utf-8",
    )

    with (recording_folder / f"Cam{serial}_crop_meta.csv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=CROP_META_HEADER)
        writer.writeheader()
        actual_detection_rows = rows if detection_rows is None else detection_rows
        for frame_id in range(1, rows + 1):
            has_detection = frame_id <= actual_detection_rows
            writer.writerow(
                {
                    "recording_frame_id": frame_id,
                    "local_frame_id": frame_id,
                    "camera_frame_id": 1000 + frame_id,
                    "timestamp": frame_id * 10,
                    "timestamp_sys": frame_id * 20,
                    "has_detection": 1 if has_detection else 0,
                    "blank_frame": 0 if has_detection else 1,
                    "detection_confidence": 0.9 if has_detection else 0.0,
                    "crop_x": 10 if has_detection else 0,
                    "crop_y": 12 if has_detection else 0,
                    "crop_w": crop_size if has_detection else 0,
                    "crop_h": crop_size if has_detection else 0,
                    "detection_x": 20 if has_detection else 0,
                    "detection_y": 22 if has_detection else 0,
                    "detection_w": 30 if has_detection else 0,
                    "detection_h": 32 if has_detection else 0,
                }
            )

    actual_perf_rows = rows if perf_rows is None else perf_rows
    with (recording_folder / f"Cam{serial}_crop_perf.csv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=CROP_PERF_HEADER)
        writer.writeheader()
        for frame_id in range(1, actual_perf_rows + 1):
            writer.writerow(
                {
                    "recording_frame_id": frame_id,
                    "local_frame_id": frame_id,
                    "camera_frame_id": 1000 + frame_id,
                    "dropped": 1 if dropped_row == frame_id else 0,
                    "drop_reason": "test_drop" if dropped_row == frame_id else "",
                }
            )


def check_crop_recording(
    recording_folder: Path,
    snapshot: dict,
    cameras: list[str],
    *,
    yolo_rows: int | None = None,
    expected_external_queue_depth: int | None = None,
    expected_external_recorder_gpu_id: int | None = None,
    expected_external_recorder_gpu_by_serial: dict[str, int] | None = None,
    require_external_recorder_gpu_separate_from_analytics: bool = False,
    max_external_queue_high_water: int | None = None,
    max_external_enqueue_age_p95_ms: float | None = None,
    require_external_crop_backend_metadata: bool = False,
) -> tuple[validator.Reporter, dict]:
    reporter = validator.Reporter(verbose=False)
    summary = {"yolo": {}}
    if yolo_rows is not None:
        summary["yolo"] = {cameras[0]: {"rows": yolo_rows}}
    crop_summary = validator.check_crop_recording_artifacts(
        reporter,
        recording_folder,
        snapshot,
        summary,
        cameras,
        True,
        "ffprobe",
        probe_video=False,
        expected_external_queue_depth=expected_external_queue_depth,
        expected_external_recorder_gpu_id=expected_external_recorder_gpu_id,
        expected_external_recorder_gpu_by_serial=expected_external_recorder_gpu_by_serial,
        require_external_recorder_gpu_separate_from_analytics=(
            require_external_recorder_gpu_separate_from_analytics
        ),
        max_external_queue_high_water=max_external_queue_high_water,
        max_external_enqueue_age_p95_ms=max_external_enqueue_age_p95_ms,
        require_external_crop_backend_metadata=require_external_crop_backend_metadata,
    )
    return reporter, crop_summary


def write_external_crop_summary(
    path: Path,
    rows: int,
    *,
    dropped: int = 0,
    queue_depth: int = 256,
    queue_high_water: int = 4,
    enqueue_age_p95_ms: float = 1.5,
) -> None:
    path.write_text(
        json.dumps(
            {
                "frames_received": rows,
                "frames_encoded": rows - dropped,
                "encode_dropped": dropped,
                "encode_queue_depth": queue_depth,
                "encode_queue_high_water": queue_high_water,
                "external_encode": {
                    "frames_dropped": dropped,
                    "enqueue_age_p95_ms": enqueue_age_p95_ms,
                },
            }
        )
        + "\n",
        encoding="utf-8",
    )


def write_external_crop_detach_csv(path: Path, depths: list[int]) -> None:
    path.write_text(
        "frame_index,encode_queue_depth\n"
        + "".join(f"{index},{depth}\n" for index, depth in enumerate(depths)),
        encoding="utf-8",
    )


def write_external_crop_recording_session_manifest(
    recording_folder: Path,
    serial: str,
    *,
    frames_received: int = 3,
    frames_encoded: int = 3,
    encode_dropped: int = 0,
    external_frames_dropped: int = 0,
    queue_depth: int = 64,
    queue_high_water: int = 12,
    enqueue_age_p95_ms: float = 2.25,
    analytics_gpu_id: int = 5,
    recorder_gpu_id: int = 5,
) -> None:
    summary_json = recording_folder / "external_crop_recorder" / f"Cam{serial}_crop_external_summary.json"
    status_json = recording_folder / "external_crop_recorder" / f"Cam{serial}_crop_external_status.json"
    (recording_folder / "recording_session.json").write_text(
        json.dumps(
            {
                "recording_backend": {
                    "crop_recording": {
                        "mode": "external_ipc",
                        "status": "completed",
                        "stream_config": {
                            serial: {
                                "stream_id": f"{serial}_crop",
                                "analytics_gpu_id": analytics_gpu_id,
                                "recorder_gpu_id": recorder_gpu_id,
                                "socket_path": f"/tmp/orange_external_recorder_{serial}_crop.sock",
                                "encode_queue_depth": queue_depth,
                                "summary_json": str(summary_json),
                                "status_json": str(status_json),
                            }
                        },
                        "frames_received": {serial: frames_received},
                        "frames_encoded": {serial: frames_encoded},
                        "encode_dropped": {serial: encode_dropped},
                        "external_frames_dropped": {serial: external_frames_dropped},
                        "encode_queue_depth": {serial: queue_depth},
                        "encode_queue_high_water": {serial: queue_high_water},
                        "enqueue_age_p95_ms": {serial: enqueue_age_p95_ms},
                    }
                }
            }
        )
        + "\n",
        encoding="utf-8",
    )


def write_external_crop_contract(
    recording_folder: Path,
    serial: str,
    *,
    queue_depth: int = 64,
    analytics_gpu_id: int = 5,
    recorder_gpu_id: int = 6,
) -> None:
    artifact_root = recording_folder / "external_crop_recorder"
    summary_json = artifact_root / f"Cam{serial}_crop_external_summary.json"
    status_json = artifact_root / f"Cam{serial}_crop_external_status.json"
    (recording_folder / "external_crop_recorder_contract.json").write_text(
        json.dumps(
            {
                "schema_id": "orange.external_recorder.contract",
                "schema_version": 1,
                "artifact_root": str(artifact_root),
                "require_status": True,
                "require_status_runtime": True,
                "streams": {
                    f"{serial}_crop": {
                        "stream_id": f"{serial}_crop",
                        "camera_serial": f"{serial}_crop",
                        "analytics_gpu_id": analytics_gpu_id,
                        "recorder_gpu_id": recorder_gpu_id,
                        "socket_path": f"/tmp/orange_external_recorder_{serial}_crop.sock",
                        "encode_queue_depth": queue_depth,
                        "summary_json": str(summary_json),
                        "status_json": str(status_json),
                    }
                }
            }
        )
        + "\n",
        encoding="utf-8",
    )


def write_external_recorder_status_fixture(
    recording_folder: Path,
    serial: str,
    *,
    crop: bool = False,
    rows: int = 3,
    heartbeat_sequence: int = 4,
    status: str = "completed",
    worker_failed: bool = False,
    error: str = "",
    runtime_heartbeat_sequence: int | None = None,
) -> tuple[Path, Path]:
    artifact_root = recording_folder / (
        "external_crop_recorder" if crop else "external_recorder"
    )
    artifact_root.mkdir(exist_ok=True)
    name_prefix = f"Cam{serial}_crop_external" if crop else f"Cam{serial}_external"
    stream_id = f"{serial}_crop" if crop else serial
    summary_path = artifact_root / f"{name_prefix}_summary.json"
    status_path = artifact_root / f"{name_prefix}_status.json"
    contract_path = recording_folder / (
        "external_crop_recorder_contract.json"
        if crop
        else "external_recorder_contract.json"
    )
    runtime_path = artifact_root / "external_recorder_supervisor_runtime.json"

    summary_path.write_text(
        json.dumps(
            {
                "frames_received": rows,
                "frames_encoded": rows,
                "acks_sent": rows,
            }
        )
        + "\n",
        encoding="utf-8",
    )
    status_payload = {
        "schema_id": "orange.external_recorder.status",
        "schema_version": 1,
        "status": status,
        "heartbeat_sequence": heartbeat_sequence,
        "frames_received": rows,
        "frames_encoded": rows,
        "acks_sent": rows,
        "worker_failed": worker_failed,
    }
    if error:
        status_payload["error"] = error
    status_path.write_text(json.dumps(status_payload) + "\n", encoding="utf-8")
    runtime_path.write_text(
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
                            "status": status,
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
        encoding="utf-8",
    )
    contract_path.write_text(
        json.dumps(
            {
                "schema_id": "orange.external_recorder.contract",
                "schema_version": 1,
                "artifact_root": str(artifact_root),
                "require_status": True,
                "require_status_runtime": True,
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
        encoding="utf-8",
    )
    return summary_path, status_path


def gui_fps_snapshot(
    *,
    overall_p05: float = 60.0,
    visible_p05: float = 55.0,
    hidden_p05: float = 70.0,
    stream_downsample: int = 4,
    display_preview_max_fps: int = 30,
    swap_interval: int = 0,
    frame_max_fps: int = 60,
    yolo_speed_graphs_enabled: bool = False,
) -> dict:
    return {
        "session": {
            "gui_display_frame_rate": {
                "schema_version": 1,
                "source": "imgui_io_delta_time",
                "stream_downsample": stream_downsample,
                "display_preview_max_fps": display_preview_max_fps,
                "swap_interval": swap_interval,
                "frame_max_fps": frame_max_fps,
                "yolo_speed_graphs_enabled": yolo_speed_graphs_enabled,
                "overall": {"sample_count": 100, "p05_fps": overall_p05},
                "crop_preview_visible": {"sample_count": 80, "p05_fps": visible_p05},
                "crop_preview_hidden": {"sample_count": 20, "p05_fps": hidden_p05},
                "timings": {
                    "frame_total_ms": {"sample_count": 100, "p50_ms": 12.0, "p95_ms": 18.0},
                    "main_texture_upload_ms": {"sample_count": 100, "p50_ms": 1.0, "p95_ms": 2.0},
                    "crop_texture_upload_ms": {"sample_count": 100, "p50_ms": 0.1, "p95_ms": 0.2},
                    "camera_window_draw_ms": {"sample_count": 100, "p50_ms": 2.0, "p95_ms": 3.0},
                    "crop_window_draw_ms": {"sample_count": 100, "p50_ms": 0.0, "p95_ms": 0.0},
                    "speed_graph_draw_ms": {"sample_count": 100, "p50_ms": 0.0, "p95_ms": 0.0},
                    "render_present_ms": {"sample_count": 100, "p50_ms": 4.0, "p95_ms": 5.0},
                    "main_texture_upload_count": 50,
                    "crop_texture_upload_count": 10,
                },
            }
        }
    }


def check_gui_fps(
    snapshot: dict,
    *,
    min_overall: float | None = None,
    min_visible: float | None = None,
    min_hidden: float | None = None,
    expected_stream_downsample: int | None = None,
    expected_display_preview_max_fps: int | None = None,
    expected_swap_interval: int | None = None,
    expected_frame_max_fps: int | None = None,
    expected_yolo_speed_graphs_enabled: int | None = None,
    require_timing_telemetry: bool = False,
) -> tuple[validator.Reporter, dict]:
    reporter = validator.Reporter(verbose=False)
    summary = validator.check_gui_display_frame_rate(
        reporter,
        snapshot,
        min_overall,
        min_visible,
        min_hidden,
        expected_stream_downsample,
        expected_display_preview_max_fps,
        expected_swap_interval,
        expected_frame_max_fps,
        expected_yolo_speed_graphs_enabled,
        require_timing_telemetry,
    )
    return reporter, summary


def test_expected_display_enabled_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095", preview_display_enabled=1)
        snapshot = {"crop_outputs": {"2010095": {"enabled": True}}}
        reporter, _ = check(
            root,
            snapshot,
            ["2010095"],
            expected_preview_display_enabled=0,
            require_counters=True,
        )
        require(
            any("display_enabled_final" in failure and "expected 0" in failure for failure in reporter.failures),
            "display enabled mismatch should fail",
        )


def test_gui_display_frame_rate_threshold_passes() -> None:
    reporter, summary = check_gui_fps(
        gui_fps_snapshot(),
        min_overall=45.0,
        min_visible=45.0,
        min_hidden=45.0,
        expected_stream_downsample=4,
        expected_display_preview_max_fps=30,
        expected_swap_interval=0,
        expected_frame_max_fps=60,
        expected_yolo_speed_graphs_enabled=0,
        require_timing_telemetry=True,
    )
    require(not reporter.failures, f"unexpected failures: {reporter.failures}")
    require(summary["crop_preview_visible"]["p05_fps"] == 55.0, "visible p05 should be summarized")
    require(summary["swap_interval"] == 0, "swap interval should be summarized")
    require(summary["frame_max_fps"] == 60, "GUI frame cap should be summarized")
    require(summary["yolo_speed_graphs_enabled"] is False, "speed graph state should be summarized")
    require(summary["timings"]["frame_total_ms"]["sample_count"] == 100, "timing samples should be summarized")
    diagnosis = summary["timing_diagnosis"]
    require(
        diagnosis["dominant_timing_bucket"] == "render_present_ms",
        "render/present should be the dominant GUI timing bucket in the fixture",
    )
    require(
        round(diagnosis["dominant_timing_fraction_of_frame_total_p95"], 3) == round(5.0 / 18.0, 3),
        "dominant GUI timing share should be computed from p95 frame total",
    )


def test_gui_display_frame_rate_threshold_fails() -> None:
    reporter, _ = check_gui_fps(
        gui_fps_snapshot(visible_p05=20.0),
        min_visible=45.0,
    )
    require(
        any("below 45.0" in failure for failure in reporter.failures),
        "low GUI FPS p05 should fail threshold",
    )


def test_gui_display_frame_rate_missing_fails_when_required() -> None:
    reporter, _ = check_gui_fps({}, min_overall=45.0)
    require(
        any("telemetry missing" in failure for failure in reporter.failures),
        "missing GUI FPS telemetry should fail when threshold is requested",
    )


def test_gui_display_frame_rate_display_config_mismatch_fails() -> None:
    reporter, _ = check_gui_fps(
        gui_fps_snapshot(stream_downsample=1, display_preview_max_fps=60, swap_interval=1, frame_max_fps=120),
        expected_stream_downsample=4,
        expected_display_preview_max_fps=30,
        expected_swap_interval=0,
        expected_frame_max_fps=60,
    )
    require(
        any("stream downsample=1" in failure for failure in reporter.failures),
        "stream downsample mismatch should fail",
    )
    require(
        any("display preview max FPS=60" in failure for failure in reporter.failures),
        "display preview max FPS mismatch should fail",
    )
    require(
        any("swap interval=1" in failure for failure in reporter.failures),
        "swap interval mismatch should fail",
    )
    require(
        any("frame max FPS=120" in failure for failure in reporter.failures),
        "GUI frame cap mismatch should fail",
    )


def test_gui_display_frame_rate_speed_graph_mismatch_fails() -> None:
    reporter, _ = check_gui_fps(
        gui_fps_snapshot(yolo_speed_graphs_enabled=True),
        expected_yolo_speed_graphs_enabled=0,
    )
    require(
        any("YOLO speed graphs enabled=1" in failure for failure in reporter.failures),
        "YOLO speed graph mismatch should fail",
    )


def test_gui_display_frame_rate_missing_timing_fails_when_required() -> None:
    snapshot = gui_fps_snapshot()
    snapshot["session"]["gui_display_frame_rate"].pop("timings")
    reporter, _ = check_gui_fps(snapshot, require_timing_telemetry=True)
    require(
        any("timing telemetry missing" in failure for failure in reporter.failures),
        "missing GUI timing telemetry should fail when required",
    )


def test_preview_sampling_passes_when_visible_bounded_and_skipped() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_sidecar(root, serial, offered=100, updated=15, skipped=85)
        write_crop_recording_artifacts(root, serial, rows=100)
        reporter, summary = check(
            root,
            crop_snapshot(serial),
            [serial],
            require_sampling=True,
            require_counters=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(summary[serial]["crop_metadata_rows"] == 100, "crop row count should be summarized")


def test_preview_sampling_fails_without_cadence_skips() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_sidecar(root, serial, offered=100, updated=100, skipped=0)
        write_crop_recording_artifacts(root, serial, rows=100)
        reporter, _ = check(
            root,
            crop_snapshot(serial),
            [serial],
            require_sampling=True,
            require_counters=True,
        )
        require(
            any("did not skip" in failure or "updated/offered" in failure for failure in reporter.failures),
            "sampling should fail when no frames were skipped by cadence",
        )


def test_preview_sampling_fails_when_preview_hidden() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_sidecar(root, serial, preview_display_enabled=0, offered=0, updated=0, skipped=0)
        write_crop_recording_artifacts(root, serial, rows=100)
        reporter, _ = check(
            root,
            crop_snapshot(serial),
            [serial],
            require_sampling=True,
            require_counters=True,
        )
        require(
            any("preview_display_enabled_final=0" in failure for failure in reporter.failures),
            "sampling should fail when preview was hidden at finalization",
        )


def test_external_recorder_status_validation_checks_full_and_crop_contracts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
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
            rows=3,
            heartbeat_sequence=5,
        )

        reporter = validator.Reporter(verbose=False)
        summary = validator.check_external_recorder_status(reporter, root, True)

        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(summary["full"]["2010095"]["heartbeat_sequence"] == 4, "full heartbeat should parse")
        require(summary["crop"]["2010095"]["heartbeat_sequence"] == 5, "crop heartbeat should parse")
        require(summary["crop"]["2010095"]["frames_encoded"] == 3, "crop encoded count should parse")


def test_external_recorder_status_validation_fails_on_bad_sidecar_or_runtime() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_external_recorder_status_fixture(
            root,
            "2010095",
            runtime_heartbeat_sequence=3,
        )

        reporter = validator.Reporter(verbose=False)
        validator.check_external_recorder_status(reporter, root, True)

        require(
            any("runtime heartbeat" in failure for failure in reporter.failures),
            "runtime heartbeat mismatch should fail",
        )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_external_recorder_status_fixture(
            root,
            "2010095",
            status="failed",
            worker_failed=True,
            error="simulated failure",
        )

        reporter = validator.Reporter(verbose=False)
        validator.check_external_recorder_status(reporter, root, True)

        require(
            any("expected completed" in failure for failure in reporter.failures),
            "failed recorder status should fail",
        )
        require(
            any("worker_failed=True" in failure for failure in reporter.failures),
            "worker_failed=true should fail",
        )
        require(
            any("simulated failure" in failure for failure in reporter.failures),
            "status sidecar error should fail",
        )


def test_external_recorder_status_validation_requires_contract_flags() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_external_recorder_status_fixture(root, "2010095")
        contract_path = root / "external_recorder_contract.json"
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
        contract["require_status"] = False
        del contract["require_status_runtime"]
        contract_path.write_text(json.dumps(contract) + "\n", encoding="utf-8")

        reporter = validator.Reporter(verbose=False)
        validator.check_external_recorder_status(reporter, root, True)

        require(
            any("require_status=False" in failure for failure in reporter.failures),
            "strict status validation should fail when contract disables require_status",
        )
        require(
            any("require_status_runtime=None" in failure for failure in reporter.failures),
            "strict status validation should fail when contract omits require_status_runtime",
        )


def test_external_recorder_status_validation_derives_status_path_from_summary() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_external_recorder_status_fixture(root, "2010095")
        contract_path = root / "external_recorder_contract.json"
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
        del contract["streams"]["2010095"]["status_json"]
        contract_path.write_text(json.dumps(contract) + "\n", encoding="utf-8")

        reporter = validator.Reporter(verbose=False)
        validator.check_external_recorder_status(reporter, root, True)

        require(not reporter.failures, f"summary-derived status path should pass: {reporter.failures}")

        del contract["streams"]["2010095"]["summary_json"]
        contract_path.write_text(json.dumps(contract) + "\n", encoding="utf-8")
        reporter = validator.Reporter(verbose=False)
        validator.check_external_recorder_status(reporter, root, True)

        require(
            any("recorder status sidecar missing" in failure for failure in reporter.failures),
            "missing status and summary paths should fail instead of resolving to cwd",
        )


def test_crop_recording_artifacts_pass_when_aligned() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        reporter, summary = check_crop_recording(
            root,
            crop_snapshot(serial),
            [serial],
            yolo_rows=3,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(summary[serial]["metadata_rows"] == 3, "metadata row count should be summarized")
        require(summary[serial]["perf_rows"] == 3, "perf row count should be summarized")


def test_crop_recording_artifacts_use_recording_output_descriptor_paths() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(external_summary, 3)

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                    "details": {
                        "stream_id": f"{serial}_crop",
                        "analytics_gpu_id": 5,
                        "recorder_gpu_id": 5,
                        "socket_path": f"/tmp/orange_external_recorder_{serial}_crop.sock",
                        "encode_queue_depth": 64,
                        "summary_json": str(external_summary),
                        "status_json": str(external_summary).replace("_summary.json", "_status.json"),
                    },
                }
            }
        }
        reporter, summary = check_crop_recording(root, snapshot, [serial], yolo_rows=3)
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(
            summary[serial]["backend"] == "external_ipc",
            "external crop descriptor backend should be summarized",
        )
        require(
            summary[serial]["video"] == str(external_video),
            "external crop descriptor video path should be used",
        )
        require(
            summary[serial]["external_encode_queue_depth"] == 256,
            "external crop queue depth should be summarized",
        )
        require(
            summary[serial]["external_encode_queue_high_water"] == 4,
            "external crop queue high-water should be summarized",
        )
        require(
            summary[serial]["external_enqueue_age_p95_ms"] == 1.5,
            "external crop enqueue age p95 should be summarized",
        )


def test_crop_recording_artifacts_external_queue_expectations() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(
            external_summary,
            3,
            queue_depth=64,
            queue_high_water=12,
            enqueue_age_p95_ms=2.25,
        )

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                    "details": {
                        "stream_id": f"{serial}_crop",
                        "analytics_gpu_id": 5,
                        "recorder_gpu_id": 5,
                        "socket_path": f"/tmp/orange_external_recorder_{serial}_crop.sock",
                        "encode_queue_depth": 64,
                        "summary_json": str(external_summary),
                        "status_json": str(external_summary).replace("_summary.json", "_status.json"),
                    },
                }
            }
        }
        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            expected_external_queue_depth=64,
            max_external_queue_high_water=16,
            max_external_enqueue_age_p95_ms=3.0,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")

        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            expected_external_queue_depth=32,
            max_external_queue_high_water=8,
            max_external_enqueue_age_p95_ms=2.0,
        )
        require(
            any("encode_queue_depth" in failure for failure in reporter.failures),
            "external crop queue-depth mismatch should fail",
        )
        require(
            any("encode_queue_high_water" in failure for failure in reporter.failures),
            "external crop queue high-water threshold should fail",
        )
        require(
            any("enqueue_age_p95_ms" in failure for failure in reporter.failures),
            "external crop enqueue-age threshold should fail",
        )


def test_crop_recording_artifacts_external_queue_high_water_cannot_exceed_depth() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(
            external_summary,
            3,
            queue_depth=64,
            queue_high_water=65,
        )

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                    "details": {
                        "stream_id": f"{serial}_crop",
                        "analytics_gpu_id": 5,
                        "recorder_gpu_id": 5,
                        "socket_path": f"/tmp/orange_external_recorder_{serial}_crop.sock",
                        "encode_queue_depth": 64,
                        "summary_json": str(external_summary),
                        "status_json": str(external_summary).replace("_summary.json", "_status.json"),
                    },
                }
            }
        }
        reporter, _ = check_crop_recording(root, snapshot, [serial], yolo_rows=3)
        require(
            any("exceeds encode_queue_depth" in failure for failure in reporter.failures),
            "external crop queue high-water greater than depth should fail",
        )


def test_crop_recording_artifacts_require_external_backend_metadata() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(
            external_summary,
            3,
            queue_depth=64,
            queue_high_water=12,
            enqueue_age_p95_ms=2.25,
        )

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                    "details": {
                        "stream_id": f"{serial}_crop",
                        "analytics_gpu_id": 5,
                        "recorder_gpu_id": 5,
                        "socket_path": f"/tmp/orange_external_recorder_{serial}_crop.sock",
                        "encode_queue_depth": 64,
                        "summary_json": str(external_summary),
                        "status_json": str(external_summary).replace("_summary.json", "_status.json"),
                    },
                }
            }
        }
        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            require_external_crop_backend_metadata=True,
        )
        require(
            any("recording_backend.crop_recording.mode" in failure for failure in reporter.failures),
            "missing external crop backend metadata should fail when required",
        )

        write_external_crop_recording_session_manifest(root, serial)
        reporter, summary = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            require_external_crop_backend_metadata=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(summary[serial]["external_stream_id"] == f"{serial}_crop", "summary should expose stream id")
        require(summary[serial]["external_analytics_gpu_id"] == 5, "summary should expose analytics GPU")
        require(summary[serial]["external_recorder_gpu_id"] == 5, "summary should expose recorder GPU")
        require(
            summary[serial]["external_socket_path"] == f"/tmp/orange_external_recorder_{serial}_crop.sock",
            "summary should expose external crop socket path",
        )
        require(
            summary[serial]["external_stream_config"]["encode_queue_depth"] == 64,
            "summary should expose external crop stream config",
        )

        snapshot["recording_outputs"][serial]["crop"]["details"]["recorder_gpu_id"] = 6
        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            require_external_crop_backend_metadata=True,
        )
        require(
            any("recording_outputs.crop.details.recorder_gpu_id" in failure for failure in reporter.failures),
            "external crop descriptor details should match stream config when required",
        )


def test_crop_recording_artifacts_external_backend_manifest_matches_summary() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(
            external_summary,
            3,
            queue_depth=64,
            queue_high_water=12,
            enqueue_age_p95_ms=2.25,
        )
        write_external_crop_recording_session_manifest(root, serial)

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                }
            }
        }
        reporter, _ = check_crop_recording(root, snapshot, [serial], yolo_rows=3)
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")

        write_external_crop_recording_session_manifest(
            root,
            serial,
            frames_encoded=2,
            queue_depth=32,
        )
        reporter, _ = check_crop_recording(root, snapshot, [serial], yolo_rows=3)
        require(
            any("recording_backend.crop_recording.frames_encoded" in failure for failure in reporter.failures),
            "recording_backend crop frames_encoded mismatch should fail",
        )
        require(
            any("stream_config encode_queue_depth" in failure for failure in reporter.failures),
            "recording_backend crop stream_config queue-depth mismatch should fail",
        )


def test_crop_recording_artifacts_external_recorder_gpu_expectations() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(
            external_summary,
            3,
            queue_depth=64,
            queue_high_water=12,
            enqueue_age_p95_ms=2.25,
        )
        write_external_crop_recording_session_manifest(root, serial, recorder_gpu_id=6)

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                    "details": {
                        "stream_id": f"{serial}_crop",
                        "analytics_gpu_id": 5,
                        "recorder_gpu_id": 6,
                        "socket_path": f"/tmp/orange_external_recorder_{serial}_crop.sock",
                        "encode_queue_depth": 64,
                        "summary_json": str(external_summary),
                        "status_json": str(external_summary).replace("_summary.json", "_status.json"),
                    },
                }
            }
        }

        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            expected_external_recorder_gpu_id=6,
            require_external_crop_backend_metadata=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")

        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            expected_external_recorder_gpu_id=8,
        )
        require(
            any("external crop recorder_gpu_id" in failure for failure in reporter.failures),
            "wrong global external crop recorder GPU expectation should fail",
        )

        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            expected_external_recorder_gpu_id=8,
            expected_external_recorder_gpu_by_serial={serial: 6},
        )
        require(
            not reporter.failures,
            "per-camera external crop recorder GPU expectation should override global expectation",
        )


def test_crop_recording_artifacts_external_recorder_gpu_uses_contract_fallback() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(
            external_summary,
            3,
            queue_depth=64,
            queue_high_water=12,
            enqueue_age_p95_ms=2.25,
        )
        write_external_crop_contract(
            root,
            serial,
            analytics_gpu_id=5,
            recorder_gpu_id=6,
            queue_depth=64,
        )
        (root / "recording_session.json").write_text(
            json.dumps(
                {
                    "recording_backend": {
                        "crop_recording": {
                            "mode": "external_ipc",
                            "status": "completed",
                        }
                    }
                }
            )
            + "\n",
            encoding="utf-8",
        )

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                }
            }
        }

        reporter, crop_summary = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            expected_external_recorder_gpu_id=6,
            require_external_recorder_gpu_separate_from_analytics=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(
            crop_summary[serial]["external_stream_config_source"] == "external_crop_recorder_contract.json",
            "validator should identify external crop contract as GPU metadata fallback",
        )

        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            expected_external_recorder_gpu_id=6,
            require_external_crop_backend_metadata=True,
        )
        require(
            any("recording_backend.crop_recording.stream_config missing" in failure for failure in reporter.failures),
            "strict backend metadata gate should still fail when recording_session stream_config is missing",
        )


def test_crop_recording_artifacts_external_recorder_gpu_separation_gate() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(
            external_summary,
            3,
            queue_depth=64,
            queue_high_water=12,
            enqueue_age_p95_ms=2.25,
        )

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                }
            }
        }

        write_external_crop_recording_session_manifest(
            root,
            serial,
            analytics_gpu_id=5,
            recorder_gpu_id=5,
        )
        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            require_external_recorder_gpu_separate_from_analytics=True,
        )
        require(
            any("uses the same CUDA device" in failure for failure in reporter.failures),
            "same analytics/recorder GPU should fail the separation gate",
        )

        write_external_crop_recording_session_manifest(
            root,
            serial,
            analytics_gpu_id=5,
            recorder_gpu_id=6,
        )
        reporter, _ = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            require_external_recorder_gpu_separate_from_analytics=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")


def test_crop_recording_artifacts_external_queue_high_water_falls_back_to_detach_csv() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_detach = external_root / f"Cam{serial}_crop_external_detach.csv"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(external_summary, 3, queue_high_water=None)
        write_external_crop_detach_csv(external_detach, [0, 5, 3])

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                }
            }
        }
        reporter, summary = check_crop_recording(
            root,
            snapshot,
            [serial],
            yolo_rows=3,
            max_external_queue_high_water=5,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(
            summary[serial]["external_encode_queue_high_water"] == 5,
            "external crop queue high-water should fall back to detach CSV",
        )


def test_crop_recording_artifacts_use_incomplete_external_descriptor_paths() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(external_summary, 3)

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "status": "incomplete",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                    "details": {
                        "status_reason": "external crop recorder output incomplete",
                    },
                }
            }
        }
        reporter, summary = check_crop_recording(root, snapshot, [serial], yolo_rows=3)
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(
            summary[serial]["video"] == str(external_video),
            "incomplete external crop descriptors should still override legacy root paths",
        )


def test_crop_recording_artifacts_fail_on_external_crop_drops() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        (root / f"Cam{serial}_crop.mp4").unlink()
        (root / f"Cam{serial}_crop_keyframe.json").unlink()

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_summary = external_root / f"Cam{serial}_crop_external_summary.json"
        external_video.write_bytes(b"external-crop-mp4")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")
        write_external_crop_summary(external_summary, 3, dropped=1)

        snapshot = crop_snapshot(serial)
        snapshot["recording_outputs"] = {
            serial: {
                "crop": {
                    "backend": "external_ipc",
                    "video": str(external_video),
                    "metadata": f"Cam{serial}_crop_meta.csv",
                    "keyframes": str(external_keyframes),
                    "perf": f"Cam{serial}_crop_perf.csv",
                    "summary": str(external_summary),
                }
            }
        }
        reporter, summary = check_crop_recording(root, snapshot, [serial], yolo_rows=3)
        require(
            any("external crop frames_encoded" in failure for failure in reporter.failures),
            f"external encoded-count mismatch should fail: {reporter.failures}",
        )
        require(
            any("external crop recorder dropped frames" in failure for failure in reporter.failures),
            f"external dropped-frame counter should fail: {reporter.failures}",
        )
        require(
            summary[serial]["external_frames_dropped"] == 1,
            "external dropped-frame count should be summarized",
        )


def test_recording_output_contract_allows_external_crop_video_paths() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        for name in (
            f"Cam{serial}.mp4",
            f"Cam{serial}_meta.csv",
            f"Cam{serial}_keyframe.json",
            f"Cam{serial}_crop_meta.csv",
            f"Cam{serial}_crop_perf.csv",
        ):
            (root / name).write_text("x\n", encoding="utf-8")

        external_root = root / "external_crop_recorder"
        external_root.mkdir()
        external_video = external_root / f"Cam{serial}_crop_external.mp4"
        external_keyframes = external_root / f"Cam{serial}_crop_external_keyframe.json"
        external_video.write_text("external video\n", encoding="utf-8")
        external_keyframes.write_text(json.dumps({"total_frames": 3}) + "\n", encoding="utf-8")

        full_output = {
            "output_kind": "full",
            "role": "ingest_authoritative",
            "backend": "external_ipc",
            "status": "completed",
            "video": f"Cam{serial}.mp4",
            "metadata": f"Cam{serial}_meta.csv",
            "keyframes": f"Cam{serial}_keyframe.json",
            "frame_count": 3,
            "packet_count": 3,
        }
        crop_output = {
            "output_kind": "crop",
            "role": "sidecar",
            "backend": "external_ipc",
            "status": "completed",
            "video": str(external_video),
            "metadata": f"Cam{serial}_crop_meta.csv",
            "keyframes": str(external_keyframes),
            "perf": f"Cam{serial}_crop_perf.csv",
        }
        manifest = {
            "camera_artifacts": {
                serial: {
                    "video": f"Cam{serial}.mp4",
                    "metadata": f"Cam{serial}_meta.csv",
                    "keyframes": f"Cam{serial}_keyframe.json",
                    "frame_count": 3,
                    "packet_count": 3,
                }
            },
            "recording_outputs": {serial: {"full": full_output, "crop": crop_output}},
        }
        snapshot = {
            "schema_version": 2,
            "recording_outputs": {serial: {"full": full_output, "crop": crop_output}},
            "encoders": {
                serial: {
                    "outputs": {
                        "full": {
                            "output_kind": "full",
                            "role": "ingest_authoritative",
                            "backend": "external_ipc",
                        },
                        "crop": {
                            "output_kind": "crop",
                            "role": "sidecar",
                            "backend": "external_ipc",
                        },
                    }
                }
            },
            "crop_outputs": {
                serial: {
                    "enabled": True,
                    "runtime": {
                        "files": {
                            "video": f"Cam{serial}_crop.mp4",
                            "metadata": f"Cam{serial}_crop_meta.csv",
                            "keyframes": f"Cam{serial}_crop_keyframe.json",
                            "perf": f"Cam{serial}_crop_perf.csv",
                        }
                    },
                }
            },
        }

        errors = recording_output_contract_errors(root, manifest, snapshot, [serial])
        require(not errors, f"external crop output contract should pass: {errors}")


def test_recording_output_contract_allows_external_crop_sidecar_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        for name in (
            f"Cam{serial}.mp4",
            f"Cam{serial}_meta.csv",
            f"Cam{serial}_keyframe.json",
            f"Cam{serial}_crop_meta.csv",
            f"Cam{serial}_crop_perf.csv",
        ):
            (root / name).write_text("x\n", encoding="utf-8")

        full_output = {
            "output_kind": "full",
            "role": "ingest_authoritative",
            "backend": "external_ipc",
            "status": "completed",
            "video": f"Cam{serial}.mp4",
            "metadata": f"Cam{serial}_meta.csv",
            "keyframes": f"Cam{serial}_keyframe.json",
            "frame_count": 3,
            "packet_count": 3,
        }
        crop_output = {
            "output_kind": "crop",
            "role": "sidecar",
            "backend": "external_ipc",
            "status": "incomplete",
            "metadata": f"Cam{serial}_crop_meta.csv",
            "perf": f"Cam{serial}_crop_perf.csv",
            "details": {
                "status_reason": "external crop recorder output incomplete",
            },
        }
        manifest = {
            "status": "completed",
            "recording_backend": {
                "mode": "external_ipc",
                "status": "completed",
                "crop_recording": {
                    "mode": "external_ipc",
                    "status": "incomplete",
                    "error": "external crop recorder output incomplete",
                },
            },
            "camera_artifacts": {
                serial: {
                    "video": f"Cam{serial}.mp4",
                    "metadata": f"Cam{serial}_meta.csv",
                    "keyframes": f"Cam{serial}_keyframe.json",
                    "frame_count": 3,
                    "packet_count": 3,
                }
            },
            "recording_outputs": {serial: {"full": full_output, "crop": crop_output}},
        }
        snapshot = {
            "schema_version": 2,
            "recording_session_status": "completed",
            "recording_outputs": {serial: {"full": full_output, "crop": crop_output}},
            "encoders": {
                serial: {
                    "outputs": {
                        "full": {
                            "output_kind": "full",
                            "role": "ingest_authoritative",
                            "backend": "external_ipc",
                        },
                        "crop": {
                            "output_kind": "crop",
                            "role": "sidecar",
                            "backend": "external_ipc",
                        },
                    }
                }
            },
            "crop_outputs": {
                serial: {
                    "enabled": True,
                    "runtime": {
                        "files": {
                            "video": f"Cam{serial}_crop.mp4",
                            "metadata": f"Cam{serial}_crop_meta.csv",
                            "keyframes": f"Cam{serial}_crop_keyframe.json",
                            "perf": f"Cam{serial}_crop_perf.csv",
                        }
                    },
                }
            },
        }

        errors = recording_output_contract_errors(root, manifest, snapshot, [serial])
        require(not errors, f"external crop sidecar failure should not poison full output: {errors}")


def test_crop_recording_artifacts_fail_on_perf_row_mismatch() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3, perf_rows=2)
        reporter, _ = check_crop_recording(root, crop_snapshot(serial), [serial])
        require(
            any("crop perf rows" in failure and "metadata rows" in failure for failure in reporter.failures),
            "crop perf row mismatch should fail",
        )


def test_crop_recording_artifacts_fail_on_keyframe_row_mismatch() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3, keyframe_total_frames=2)
        reporter, _ = check_crop_recording(root, crop_snapshot(serial), [serial])
        require(
            any("keyframe total_frames" in failure for failure in reporter.failures),
            "crop keyframe total_frames mismatch should fail",
        )


def test_crop_recording_artifacts_fail_on_dropped_rows() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3, dropped_row=2)
        reporter, _ = check_crop_recording(root, crop_snapshot(serial), [serial])
        require(
            any("dropped crop frame" in failure for failure in reporter.failures),
            "crop perf dropped rows should fail",
        )


def test_crop_recording_artifacts_fail_on_yolo_row_mismatch() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010095"
        write_crop_recording_artifacts(root, serial, rows=3)
        reporter, _ = check_crop_recording(
            root,
            crop_snapshot(serial),
            [serial],
            yolo_rows=4,
        )
        require(
            any("YOLO rows" in failure for failure in reporter.failures),
            "crop/Yolo row mismatch should fail",
        )


def test_legacy_snapshot_without_crop_outputs_checks_requested_cameras() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_sidecar(root, "2010095")
        reporter, summary = check(
            root,
            {},
            ["2010095"],
            expected_preview_max_fps=15,
            require_counters=True,
        )
        require(not reporter.failures, f"unexpected failures: {reporter.failures}")
        require(set(summary) == {"2010095"}, "legacy snapshot should fall back to requested cameras")


def main() -> int:
    tests = [
        test_requires_only_crop_enabled_cameras,
        test_missing_crop_enabled_counter_fails,
        test_expected_preview_max_fps_mismatch_fails,
        test_updated_frames_cannot_exceed_offered_frames,
        test_expected_display_enabled_passes_for_hidden_preview,
        test_expected_display_enabled_mismatch_fails,
        test_expected_preview_disabled_passes_for_disabled_preview,
        test_expected_preview_disabled_mismatch_fails,
        test_min_crop_frame_pool_size_passes,
        test_min_crop_frame_pool_size_mismatch_fails,
        test_recording_fanout_matches_detection_rows,
        test_recording_fanout_detection_row_mismatch_fails,
        test_recording_fanout_drops_fail,
        test_preview_fanout_cannot_exceed_preview_updates,
        test_gui_display_frame_rate_threshold_passes,
        test_gui_display_frame_rate_threshold_fails,
        test_gui_display_frame_rate_missing_fails_when_required,
        test_gui_display_frame_rate_display_config_mismatch_fails,
        test_gui_display_frame_rate_speed_graph_mismatch_fails,
        test_gui_display_frame_rate_missing_timing_fails_when_required,
        test_preview_sampling_passes_when_visible_bounded_and_skipped,
        test_preview_sampling_fails_without_cadence_skips,
        test_preview_sampling_fails_when_preview_hidden,
        test_external_recorder_status_validation_checks_full_and_crop_contracts,
        test_external_recorder_status_validation_fails_on_bad_sidecar_or_runtime,
        test_external_recorder_status_validation_requires_contract_flags,
        test_external_recorder_status_validation_derives_status_path_from_summary,
        test_crop_recording_artifacts_pass_when_aligned,
        test_crop_recording_artifacts_use_recording_output_descriptor_paths,
        test_crop_recording_artifacts_external_queue_expectations,
        test_crop_recording_artifacts_external_queue_high_water_cannot_exceed_depth,
        test_crop_recording_artifacts_require_external_backend_metadata,
        test_crop_recording_artifacts_external_backend_manifest_matches_summary,
        test_crop_recording_artifacts_external_recorder_gpu_expectations,
        test_crop_recording_artifacts_external_recorder_gpu_uses_contract_fallback,
        test_crop_recording_artifacts_external_recorder_gpu_separation_gate,
        test_crop_recording_artifacts_external_queue_high_water_falls_back_to_detach_csv,
        test_crop_recording_artifacts_use_incomplete_external_descriptor_paths,
        test_crop_recording_artifacts_fail_on_external_crop_drops,
        test_recording_output_contract_allows_external_crop_video_paths,
        test_recording_output_contract_allows_external_crop_sidecar_failure,
        test_crop_recording_artifacts_fail_on_perf_row_mismatch,
        test_crop_recording_artifacts_fail_on_keyframe_row_mismatch,
        test_crop_recording_artifacts_fail_on_dropped_rows,
        test_crop_recording_artifacts_fail_on_yolo_row_mismatch,
        test_legacy_snapshot_without_crop_outputs_checks_requested_cameras,
    ]

    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("All GUI crop preview validation tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
