#!/usr/bin/env python3
"""Focused tests for external recorder session verification helpers."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "verify_external_recorder_session.py"
sys.path.insert(0, str(REPO_ROOT / "scripts"))

spec = importlib.util.spec_from_file_location("verify_external_recorder_session", SCRIPT)
assert spec is not None and spec.loader is not None
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def storage_preflight_payload(
    *,
    ok: bool = True,
    low_space: bool = False,
    path_ok: bool = True,
    meets_min_free: bool = True,
    below_warning: bool = False,
) -> dict:
    return {
        "checked": True,
        "ok": ok,
        "low_space": low_space,
        "min_free_bytes": 1024,
        "low_space_warning_bytes": 2048,
        "paths": [
            {
                "path": "/tmp",
                "ok": path_ok,
                "meets_min_free": meets_min_free,
                "below_warning": below_warning,
                "available_bytes": 4096,
                "error": "" if path_ok and meets_min_free else "low space",
            }
        ],
    }


def ipc_protocol_payload() -> dict:
    return {
        "name": "orange.external_recorder.ipc",
        "version": 1,
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


def video_metadata_payload(serial: str, *, output_kind: str = "full") -> dict:
    contract_id = (
        "orange.crop.mono8.v1"
        if output_kind == "crop"
        else "orange.camera.mono8.full_frame.v1"
    )
    transform = "crop_mono8_to_nv12" if output_kind == "crop" else "mono8_to_nv12"
    title = f"Cam{serial}" + (" crop" if output_kind == "crop" else "")
    comment = (
        "nvenc codec=hevc; preset=p1; tuning=ll; res=256x256; fps=60; "
        f"color=0; gop=25; source_pixel_contract={contract_id}; "
        "source_pixel_format=mono8; source_pixel_dtype=uint8; "
        "source_pixel_range=0_255; source_color_space=linear_gray; "
        "source_channel_order=gray; source_memory_layout=HxW; "
        "source_width=256; source_height=256; "
        "source_coordinate_origin=top_left; source_origin=camera_dma; "
        f"source_transform_to_encoder={transform}; encoder_input_format=nv12; "
        f"encoded_pix_fmt=yuv420p; encoded_color_range=pc; output_kind={output_kind}; "
        "output_mode=factor; rc=vbr; bpp=0.100; target_bps=150000000"
    )
    if output_kind == "crop":
        comment = comment.replace("source_origin=camera_dma", "source_origin=analytics_crop")
    return {
        "schema_id": "orange.video_metadata",
        "schema_version": 1,
        "video_path": f"Cam{serial}_external.mp4",
        "stream_id": serial,
        "camera_serial": serial,
        "output_kind": output_kind,
        "role": "sidecar" if output_kind == "crop" else "ingest_authoritative",
        "encoder": {
            "name": "nvenc",
            "codec": "hevc",
            "preset": "p1",
            "tuning": "ll",
            "rate_control_mode": "vbr",
            "resolved_gop_length": 25,
            "fps": 60,
        },
        "source_pixel_contract": {
            "id": contract_id,
            "pixel_format": "mono8",
            "dtype": "uint8",
            "value_range": "0_255",
            "color_space": "linear_gray",
            "channel_order": "gray",
            "memory_layout": "HxW",
            "width": 256,
            "height": 256,
            "coordinate_origin": "top_left",
            "source_origin": "analytics_crop" if output_kind == "crop" else "camera_dma",
            "transform_to_encoder": transform,
            "encoder_input_format": "nv12",
            "encoded_pix_fmt": "yuv420p",
            "encoded_color_range": "pc",
        },
        "mp4_tags_expected": {
            "title": title,
            "comment": comment,
        },
        "mp4_metadata_embedding": {
            "attempted": True,
            "succeeded": True,
            "validated_with_ffprobe": False,
        },
    }


def write_summary(
    root: Path,
    serial: str,
    *,
    queue_depth: int = 64,
    queue_high_water: int | None = 12,
    enqueue_age_p95_ms: float = 2.5,
    detach_depths: list[int] | None = None,
    rolling: bool = False,
) -> tuple[Path, Path]:
    mp4_path = root / f"Cam{serial}_external.mp4"
    mp4_path.write_bytes(b"not-a-real-mp4-but-ffprobe-is-stubbed")
    detach_path = root / f"Cam{serial}_external_detach.csv"
    detach_path.write_text(
        "frame_index,encode_queue_depth\n"
        + "".join(
            f"{index},{depth}\n"
            for index, depth in enumerate(detach_depths if detach_depths is not None else [1, 2, 3])
        ),
        encoding="utf-8",
    )
    summary = {
        "schema_id": verifier.SUMMARY_SCHEMA_ID,
        "schema_version": 1,
        "tool": "external_recorder_ipc_probe",
        "stream_id": serial,
        "routing_policy": "single_shard",
        "shard_count": 1,
        "encode": True,
        "worker_failed": False,
        "frames_received": 3,
        "acks_sent": 3,
        "detach_copied": 3,
        "encode_enqueued": 3,
        "encode_skipped": 0,
        "encode_dropped": 0,
        "encode_queue_depth": queue_depth,
        "frames_encoded": 3,
        "external_encode": {
            "frames_dropped": 0,
            "enqueue_age_p95_ms": enqueue_age_p95_ms,
            "mp4_packets": 3,
        },
        "external_encode_shards": [
            {
                "assigned_gpu_id": 5,
                "assigned_shard_id": 0,
                "frames_encoded": 3,
                "frames_dropped": 0,
                "worker_failed": False,
            }
        ],
        "merged_output": {},
        "storage_preflight": storage_preflight_payload(),
        "ipc_protocol": ipc_protocol_payload(),
        "video_metadata": video_metadata_payload(serial),
        "outputs": {
            "detach_csv": str(detach_path),
            "mp4": str(mp4_path),
        },
    }
    if rolling:
        first_clip_dir = root / "clips" / "clip_000000"
        second_clip_dir = root / "clips" / "clip_000001"
        first_clip_dir.mkdir(parents=True, exist_ok=True)
        second_clip_dir.mkdir(parents=True, exist_ok=True)
        first_clip_mp4 = first_clip_dir / f"Cam{serial}_external.mp4"
        first_clip_metadata = first_clip_dir / f"Cam{serial}_external_meta.csv"
        first_clip_keyframes = first_clip_dir / f"Cam{serial}_external_keyframe.json"
        second_clip_mp4 = second_clip_dir / f"Cam{serial}_external.mp4"
        second_clip_metadata = second_clip_dir / f"Cam{serial}_external_meta.csv"
        second_clip_keyframes = second_clip_dir / f"Cam{serial}_external_keyframe.json"
        first_clip_mp4.write_bytes(b"rolling-clip-0")
        second_clip_mp4.write_bytes(b"rolling-clip-1")
        first_clip_metadata.write_text(
            "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id,"
            "gop_index,frame_index_within_gop,source_gpu_id,assigned_gpu_id,assigned_shard_id,bytes\n"
            "1,100,1000,1,1,0,0,5,5,0,256\n"
            "2,200,2000,2,2,0,1,5,5,0,256\n",
            encoding="utf-8",
        )
        second_clip_metadata.write_text(
            "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id,"
            "gop_index,frame_index_within_gop,source_gpu_id,assigned_gpu_id,assigned_shard_id,bytes\n"
            "3,300,3000,3,3,1,0,5,5,0,256\n",
            encoding="utf-8",
        )
        first_clip_keyframes.write_text(
            '{"total_frames":2,"keyframe_frames":[0]}\n',
            encoding="utf-8",
        )
        second_clip_keyframes.write_text(
            '{"total_frames":1,"keyframe_frames":[0]}\n',
            encoding="utf-8",
        )
        summary["recording_control"] = {
            "record_for_seconds": 6,
            "clip_seconds": 2,
        }
        summary["authoritative_video_output"] = {
            "mode": "rolling_clips",
            "session_mp4_written": False,
            "shard_mp4s_requested": False,
        }
        summary["merged_output"] = {
            "coordinator_enabled": True,
            "enabled": False,
            "failed": False,
            "pending_gops": 0,
            "packets_written": 3,
            "mp4": "",
            "mp4_keyframe": "",
        }
        summary["outputs"]["mp4"] = ""
        mp4_path.unlink()
        summary["rolling_output"] = {
            "enabled": True,
            "implementation": "external_recorder_gop_boundary_writer_rotation",
            "record_for_seconds": 6,
            "clip_seconds": 2,
            "clip_span_frames": 2,
            "clip_span_gops": 2,
            "target_frame_count": 3,
            "terminal_tail_coalesce_frames": 1,
            "terminal_tail_coalesced_frames": 0,
            "clip_count": 2,
            "clips": [
                {
                    "clip_index": 0,
                    "clip_id": "clip_000000",
                    "first_recording_frame_id": 1,
                    "last_recording_frame_id": 2,
                    "recording_frame_id_gaps": 0,
                    "frame_count": 2,
                    "packets_written": 2,
                    "failed": False,
                    "mp4": str(first_clip_mp4),
                    "metadata": str(first_clip_metadata),
                    "keyframes": str(first_clip_keyframes),
                },
                {
                    "clip_index": 1,
                    "clip_id": "clip_000001",
                    "first_recording_frame_id": 3,
                    "last_recording_frame_id": 3,
                    "recording_frame_id_gaps": 0,
                    "frame_count": 1,
                    "packets_written": 1,
                    "failed": False,
                    "mp4": str(second_clip_mp4),
                    "metadata": str(second_clip_metadata),
                    "keyframes": str(second_clip_keyframes),
                },
            ],
        }
    else:
        metadata_path = root / f"Cam{serial}_external_meta.csv"
        metadata_path.write_text(
            "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id,"
            "gop_index,frame_index_within_gop,source_gpu_id,assigned_gpu_id,assigned_shard_id,bytes\n"
            "1,1770000000000000037,1770000000000000000,1,1,0,0,5,5,0,256\n"
            "2,1770000000033333370,1770000000033333333,2,2,0,1,5,5,0,256\n"
            "3,1770000000066666703,1770000000066666666,3,3,0,2,5,5,0,256\n",
            encoding="utf-8",
        )
        summary["frame_metadata"] = {
            "path": str(metadata_path),
            "rows_written": 3,
            "first_recording_frame_id": 1,
            "last_recording_frame_id": 3,
            "recording_frame_id_gaps": 0,
            "zero_camera_timestamp_rows": 0,
            "zero_system_timestamp_rows": 0,
        }
        summary["outputs"]["metadata"] = str(metadata_path)
    if queue_high_water is not None:
        summary["encode_queue_high_water"] = queue_high_water
    summary_path = root / f"Cam{serial}_external_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary_path, mp4_path


def write_status(
    root: Path,
    serial: str,
    *,
    heartbeat_sequence: int = 7,
    status: str = "completed",
    frames_received: int = 3,
    acks_sent: int = 3,
    frames_encoded: int = 3,
    worker_failed: bool = False,
    rolling: bool = False,
    rolling_last_completed_clip_index: int = 1,
) -> Path:
    status_path = root / f"Cam{serial}_external_status.json"
    payload = {
        "schema_id": verifier.STATUS_SCHEMA_ID,
        "schema_version": 1,
        "tool": "external_recorder_ipc_probe",
        "status": status,
        "session_id": "test-session",
        "stream_id": serial,
        "status_json": str(status_path),
        "heartbeat_sequence": heartbeat_sequence,
        "frames_received": frames_received,
        "acks_sent": acks_sent,
        "detach_copied": frames_encoded,
        "encode_enqueued": frames_encoded,
        "encode_skipped": frames_received - frames_encoded,
        "encode_dropped": 0,
        "frames_encoded": frames_encoded,
        "worker_failed": worker_failed,
        "storage_preflight": storage_preflight_payload(),
        "ipc_protocol": ipc_protocol_payload(),
    }
    if rolling:
        payload["rolling"] = {
            "enabled": True,
            "implementation": "external_recorder_gop_boundary_writer_rotation",
            "record_for_seconds": 6,
            "clip_seconds": 2,
            "clip_span_frames": 2,
            "target_frame_count": 3,
            "current_clip_index": 1,
            "next_rollover_at_recording_frame_id": 5,
            "frames_until_next_rollover": 1,
            "completed_clip_count": 2,
            "last_completed_clip_index": rolling_last_completed_clip_index,
            "last_completed_clip_last_recording_frame_id": 3,
            "last_completed_clip_frame_count": 1,
            "last_rollover_status": "completed",
        }
    status_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return status_path


def rewrite_summary(summary_path: Path, mutator) -> None:
    payload = json.loads(summary_path.read_text(encoding="utf-8"))
    mutator(payload)
    summary_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def write_runtime(
    root: Path,
    serial: str,
    status_path: Path,
    *,
    heartbeat_sequence: int = 7,
    rolling: bool = False,
    rolling_current_clip_index: int = 1,
) -> Path:
    recorder_status = {
        "present": True,
        "valid": True,
        "status": "completed",
        "heartbeat_sequence": heartbeat_sequence,
        "frames_received": 3,
        "acks_sent": 3,
        "frames_encoded": 3,
        "storage_checked": True,
        "storage_ok": True,
        "storage_low_space": False,
        "storage_path_count": 1,
        "storage_paths_ok_count": 1,
        "storage_paths_low_space_count": 0,
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
    if rolling:
        recorder_status.update(
            {
                "rolling_enabled": True,
                "rolling_current_clip_index": rolling_current_clip_index,
                "rolling_next_rollover_at_recording_frame_id": 5,
                "rolling_frames_until_next_rollover": 1,
                "rolling_completed_clip_count": 2,
                "rolling_last_completed_clip_index": 1,
                "rolling_last_rollover_status": "completed",
            }
        )
    runtime_path = root / "external_recorder_supervisor_runtime.json"
    payload = {
        "schema_id": "orange.external_recorder.supervisor_runtime",
        "schema_version": 1,
        "processes": [
            {
                "stream_id": serial,
                "camera_serial": serial,
                "status_json_path": str(status_path),
                "recorder_status": recorder_status,
            }
        ],
    }
    runtime_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return runtime_path


def verify_one(
    root: Path,
    summary_path: Path,
    mp4_path: Path,
    *,
    stream_fields: dict | None = None,
    expected_depth: int | None = None,
    max_high_water: int | None = None,
    max_enqueue_age: float | None = None,
    require_status: bool = False,
    require_runtime_status: bool = False,
    require_storage_preflight: bool = False,
    require_protocol_hello: bool = False,
    ffprobe_packet_count: int | None = None,
) -> dict:
    serial = "2010096"
    stream = {
        "stream_id": serial,
        "summary_json": str(summary_path),
        "status_json": str(root / f"Cam{serial}_external_status.json"),
        "mp4": str(mp4_path),
        "routing_policy": "single_shard",
    }
    if stream_fields:
        stream.update(stream_fields)
    summary_payload = json.loads(summary_path.read_text(encoding="utf-8"))
    rolling_requested = int(
        summary_payload.get("recording_control", {}).get("clip_seconds", 0)
    ) > 0
    contract = {
        "require_gop_routing": False,
        "require_merged_mp4": not rolling_requested,
        "require_video_sanity": False,
    }
    original_ffprobe = verifier.ffprobe_video
    original_ffprobe_key_flags = verifier.ffprobe_packet_key_flags
    verifier.ffprobe_video = lambda path, ffprobe: {
        "tags": video_metadata_payload(serial)["mp4_tags_expected"],
        "packet_count": (
            int(ffprobe_packet_count)
            if ffprobe_packet_count is not None
            else int(summary_payload["frames_encoded"])
        ),
    }
    verifier.ffprobe_packet_key_flags = lambda path, ffprobe: ["K_", "K_", "K_"]
    try:
        return verifier.verify_summary(
            root,
            serial,
            stream,
            contract,
            "ffprobe",
            True,
            expected_depth,
            max_high_water,
            max_enqueue_age,
            require_status,
            require_runtime_status,
            require_storage_preflight,
            require_protocol_hello,
        )
    finally:
        verifier.ffprobe_video = original_ffprobe
        verifier.ffprobe_packet_key_flags = original_ffprobe_key_flags


def test_queue_thresholds_pass_and_summarize() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        result = verify_one(
            root,
            summary_path,
            mp4_path,
            expected_depth=64,
            max_high_water=16,
            max_enqueue_age=3.0,
        )
        require(result["encode_queue_depth"] == 64, "queue depth should be returned")
        require(result["encode_queue_high_water"] == 12, "queue high-water should be returned")
        require(result["enqueue_age_p95_ms"] == 2.5, "enqueue age p95 should be returned")
        require(result["frame_metadata"]["rows"] == 3, "frame metadata should be verified")


def test_single_clip_frame_metadata_is_required_and_complete() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        metadata_path = Path(summary["frame_metadata"]["path"])
        metadata_path.write_text(
            "frame_id,timestamp,timestamp_sys,recording_frame_id\n"
            "1,1770000000000000037,1770000000000000000,1\n"
            "2,1770000000033333370,1770000000033333333,2\n",
            encoding="utf-8",
        )
        try:
            verify_one(root, summary_path, mp4_path)
        except verifier.VerificationError as exc:
            require(
                "rows do not match frames_encoded" in str(exc),
                f"unexpected incomplete metadata failure: {exc}",
            )
        else:
            raise AssertionError("expected incomplete single-clip metadata to fail")


def test_single_clip_frame_metadata_identity_contract() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        metadata_path = Path(summary["frame_metadata"]["path"])
        original = metadata_path.read_text(encoding="utf-8")

        metadata_path.write_text(
            original.replace(
                "frame_id,timestamp,timestamp_sys",
                "recording_frame_id,timestamp,timestamp_sys",
                1,
            ),
            encoding="utf-8",
        )
        try:
            verify_one(root, summary_path, mp4_path)
        except verifier.VerificationError as exc:
            require(
                "legacy leading columns" in str(exc),
                f"unexpected legacy-column failure: {exc}",
            )
        else:
            raise AssertionError("expected legacy-column order mismatch to fail")

        metadata_path.write_text(
            original.replace(
                "2,1770000000033333370,1770000000033333333,2,",
                "9,1770000000033333370,1770000000033333333,2,",
                1,
            ),
            encoding="utf-8",
        )
        try:
            verify_one(root, summary_path, mp4_path)
        except verifier.VerificationError as exc:
            require(
                "frame_id != recording_frame_id" in str(exc),
                f"unexpected frame-identity failure: {exc}",
            )
        else:
            raise AssertionError("expected frame identity mismatch to fail")


def test_single_clip_frame_metadata_permits_intentional_rate_cap_gaps() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        metadata_path = Path(summary["frame_metadata"]["path"])
        metadata_path.write_text(
            "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id,"
            "gop_index,frame_index_within_gop,source_gpu_id,assigned_gpu_id,assigned_shard_id,bytes\n"
            "1,100,1000,1,1,0,0,5,5,0,256\n"
            "3,300,3000,3,3,0,2,5,5,0,256\n"
            "5,500,5000,5,5,0,4,5,5,0,256\n",
            encoding="utf-8",
        )
        summary.update(
            {
                "frames_received": 5,
                "acks_sent": 5,
                "encode_skipped": 2,
            }
        )
        summary["frame_metadata"].update(
            {
                "last_recording_frame_id": 5,
                "recording_frame_id_gaps": 2,
            }
        )
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

        result = verify_one(root, summary_path, mp4_path)
        require(
            result["frame_metadata"]["recording_frame_id_gaps"] == 2,
            "intentional rate-cap gaps should remain reported",
        )


def test_single_clip_packet_parity_is_required() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        rewrite_summary(
            summary_path,
            lambda payload: payload["external_encode"].update({"mp4_packets": 2}),
        )
        try:
            verify_one(root, summary_path, mp4_path)
        except verifier.VerificationError as exc:
            require(
                "authoritative packet count != frames_encoded" in str(exc),
                f"unexpected reported-packet failure: {exc}",
            )
        else:
            raise AssertionError("expected reported packet mismatch to fail")

        rewrite_summary(
            summary_path,
            lambda payload: payload["external_encode"].update({"mp4_packets": 3}),
        )
        try:
            verify_one(
                root,
                summary_path,
                mp4_path,
                ffprobe_packet_count=2,
            )
        except verifier.VerificationError as exc:
            require(
                "MP4 packet count != frames_encoded" in str(exc),
                f"unexpected actual-packet failure: {exc}",
            )
        else:
            raise AssertionError("expected actual MP4 packet mismatch to fail")


def test_queue_threshold_failures() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        checks = [
            ({"expected_depth": 32}, "encode_queue_depth mismatch"),
            ({"max_high_water": 8}, "encode_queue_high_water too high"),
            ({"max_enqueue_age": 1.0}, "enqueue_age_p95_ms too high"),
        ]
        for kwargs, expected in checks:
            try:
                verify_one(root, summary_path, mp4_path, **kwargs)
            except verifier.VerificationError as exc:
                require(expected in str(exc), f"unexpected failure for {kwargs}: {exc}")
            else:
                raise AssertionError(f"expected verification failure for {kwargs}")


def test_video_metadata_comment_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        rewrite_summary(
            summary_path,
            lambda payload: payload["video_metadata"]["mp4_tags_expected"].update(
                {
                    "comment": payload["video_metadata"]["mp4_tags_expected"]["comment"].replace(
                        "source_pixel_contract=orange.camera.mono8.full_frame.v1",
                        "source_pixel_contract=orange.bad_contract.v1",
                    )
                }
            ),
        )
        try:
            verify_one(root, summary_path, mp4_path)
        except verifier.VerificationError as exc:
            require(
                "source_pixel_contract" in str(exc),
                f"unexpected video metadata failure: {exc}",
            )
        else:
            raise AssertionError("expected video metadata mismatch to fail")


def test_queue_high_water_falls_back_to_detach_csv() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(
            root,
            "2010096",
            queue_high_water=None,
            detach_depths=[2, 9, 4],
        )
        result = verify_one(root, summary_path, mp4_path, max_high_water=9)
        require(result["encode_queue_high_water"] == 9, "queue high-water should fall back to detach CSV")


def test_mp4_queue_overflow_failures() -> None:
    checks = [
        (
            lambda payload: payload["external_encode"].update({"mp4_queue_overflowed": True}),
            "external_encode for 2010096 reports mp4_queue_overflowed=true",
        ),
        (
            lambda payload: payload["external_encode"].update({"mp4_queue_overflow_events": 1}),
            "external_encode for 2010096 mp4_queue_overflow_events=1",
        ),
        (
            lambda payload: payload["external_encode_shards"][0].update({"mp4_queue_overflowed": True}),
            "shard 0 for 2010096 reports mp4_queue_overflowed=true",
        ),
        (
            lambda payload: payload["merged_output"].update({"mp4_queue_overflow_events": 1}),
            "merged_output for 2010096 mp4_queue_overflow_events=1",
        ),
    ]
    for mutator, expected in checks:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            summary_path, mp4_path = write_summary(root, "2010096")
            rewrite_summary(summary_path, mutator)
            try:
                verify_one(root, summary_path, mp4_path)
            except verifier.VerificationError as exc:
                require(expected in str(exc), f"unexpected overflow failure: {exc}")
            else:
                raise AssertionError("expected MP4 queue overflow verification failure")


def test_storage_preflight_failures() -> None:
    checks = [
        (
            lambda payload: payload.pop("storage_preflight", None),
            "summary for 2010096 missing storage_preflight",
            True,
        ),
        (
            lambda payload: payload.update({"storage_preflight": storage_preflight_payload(ok=False)}),
            "summary for 2010096 storage_preflight.ok=false",
            False,
        ),
        (
            lambda payload: payload.update(
                {"storage_preflight": storage_preflight_payload(low_space=True)}
            ),
            "summary for 2010096 storage_preflight.low_space=true",
            False,
        ),
        (
            lambda payload: payload.update(
                {"storage_preflight": storage_preflight_payload(meets_min_free=False)}
            ),
            "summary for 2010096 storage path /tmp below min_free_bytes",
            False,
        ),
        (
            lambda payload: payload.update(
                {"storage_preflight": storage_preflight_payload(below_warning=True)}
            ),
            "summary for 2010096 storage path /tmp below low_space_warning_bytes",
            False,
        ),
    ]
    for mutator, expected, require_storage_preflight in checks:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            summary_path, mp4_path = write_summary(root, "2010096")
            rewrite_summary(summary_path, mutator)
            try:
                verify_one(
                    root,
                    summary_path,
                    mp4_path,
                    require_storage_preflight=require_storage_preflight,
                )
            except verifier.VerificationError as exc:
                require(expected in str(exc), f"unexpected storage failure: {exc}")
            else:
                raise AssertionError("expected storage preflight verification failure")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        status_path = write_status(root, "2010096")
        rewrite_summary(
            status_path,
            lambda payload: payload.update(
                {"storage_preflight": storage_preflight_payload(ok=False)}
            ),
        )
        try:
            verify_one(root, summary_path, mp4_path, require_status=True)
        except verifier.VerificationError as exc:
            require(
                "status for 2010096 storage_preflight.ok=false" in str(exc),
                f"unexpected status storage failure: {exc}",
            )
        else:
            raise AssertionError("expected status storage preflight failure")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        status_path = write_status(root, "2010096")
        rewrite_summary(status_path, lambda payload: payload.pop("storage_preflight", None))
        try:
            verify_one(
                root,
                summary_path,
                mp4_path,
                require_status=True,
                require_storage_preflight=True,
            )
        except verifier.VerificationError as exc:
            require(
                "status for 2010096 missing storage_preflight" in str(exc),
                f"unexpected missing status storage failure: {exc}",
            )
        else:
            raise AssertionError("expected missing status storage preflight failure")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        status_path = write_status(root, "2010096")
        runtime_path = write_runtime(root, "2010096", status_path)
        rewrite_summary(
            runtime_path,
            lambda payload: payload["processes"][0]["recorder_status"].update(
                {"storage_checked": False}
            ),
        )
        try:
            verify_one(
                root,
                summary_path,
                mp4_path,
                require_status=True,
                require_runtime_status=True,
                require_storage_preflight=True,
            )
        except verifier.VerificationError as exc:
            require(
                "runtime storage_checked is not true for 2010096" in str(exc),
                f"unexpected runtime storage failure: {exc}",
            )
        else:
            raise AssertionError("expected runtime storage preflight failure")


def test_rolling_output_uses_summary_recording_control() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096", rolling=True)
        result = verify_one(root, summary_path, mp4_path)
        require(result["rolling_clip_count"] == 2, "rolling clips should be verified")
        require(
            result["rolling_clips"][0]["first_recording_frame_id"] == 1,
            "first rolling clip should start at recording frame 1",
        )
        require(
            result["rolling_clips"][1]["last_recording_frame_id"] == 3,
            "second rolling clip should end at recording frame 3",
        )


def test_rolling_output_requires_keyframe_zero() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096", rolling=True)
        keyframes = root / "clips" / "clip_000000" / "Cam2010096_external_keyframe.json"
        keyframes.write_text(
            '{"total_frames":2,"keyframe_frames":[1]}\n',
            encoding="utf-8",
        )
        try:
            verify_one(root, summary_path, mp4_path)
        except verifier.VerificationError as exc:
            require(
                "does not start on keyframe 0" in str(exc),
                f"unexpected keyframe-zero failure: {exc}",
            )
        else:
            raise AssertionError("expected rolling keyframe-zero verification failure")


def test_rolling_full_frame_metadata_requires_canonical_leading_columns() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096", rolling=True)
        metadata = root / "clips" / "clip_000000" / "Cam2010096_external_meta.csv"
        metadata.write_text(
            "recording_frame_id,timestamp,timestamp_sys\n"
            "1,100,1000\n"
            "2,200,2000\n",
            encoding="utf-8",
        )
        try:
            verify_one(root, summary_path, mp4_path)
        except verifier.VerificationError as exc:
            require(
                "does not preserve legacy leading columns" in str(exc),
                f"unexpected rolling metadata schema failure: {exc}",
            )
        else:
            raise AssertionError("expected rolling metadata schema verification failure")


def test_status_sidecar_passes_and_summarizes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        write_status(root, "2010096", heartbeat_sequence=8)
        result = verify_one(root, summary_path, mp4_path, require_status=True)
        recorder_status = result["recorder_status"]
        require(recorder_status["status"] == "completed", "status should be summarized")
        require(recorder_status["heartbeat_sequence"] == 8, "heartbeat should be summarized")


def test_stream_kind_and_output_kind_match_contract() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        status_path = write_status(root, "2010096")
        for path in (summary_path, status_path):
            rewrite_summary(
                path,
                lambda payload: payload.update(
                    {"stream_kind": "full_frame", "output_kind": "full"}
                ),
            )

        stream_fields = {"stream_kind": "full_frame", "output_kind": "full"}
        verify_one(root, summary_path, mp4_path, stream_fields=stream_fields, require_status=True)

        rewrite_summary(summary_path, lambda payload: payload.update({"output_kind": "crop"}))
        try:
            verify_one(
                root,
                summary_path,
                mp4_path,
                stream_fields=stream_fields,
                require_status=True,
            )
        except verifier.VerificationError as exc:
            require("output_kind mismatch" in str(exc), f"unexpected output-kind failure: {exc}")
        else:
            raise AssertionError("expected summary output_kind mismatch to fail")


def test_crop_external_mp4_requires_all_packet_key_samples() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010096"
        summary_path, mp4_path = write_summary(root, serial)
        rewrite_summary(
            summary_path,
            lambda payload: payload.update(
                {
                    "output_kind": "crop",
                    "stream_kind": "crop",
                    "video_metadata": video_metadata_payload(serial, output_kind="crop"),
                }
            ),
        )
        stream = {
            "stream_id": serial,
            "stream_kind": "crop",
            "output_kind": "crop",
            "summary_json": str(summary_path),
            "mp4": str(mp4_path),
            "routing_policy": "single_shard",
        }
        contract = {
            "require_gop_routing": False,
            "require_merged_mp4": True,
            "require_video_sanity": False,
        }
        original_ffprobe = verifier.ffprobe_video
        original_ffprobe_key_flags = verifier.ffprobe_packet_key_flags
        verifier.ffprobe_video = lambda path, ffprobe: {
            "tags": video_metadata_payload(serial, output_kind="crop")["mp4_tags_expected"],
            "packet_count": 3,
        }
        verifier.ffprobe_packet_key_flags = lambda path, ffprobe: ["K_", "__", "K_"]
        try:
            try:
                verifier.verify_summary(
                    root,
                    serial,
                    stream,
                    contract,
                    "ffprobe",
                    True,
                    None,
                    None,
                    None,
                    False,
                    False,
                    False,
                    False,
                )
            except verifier.VerificationError as exc:
                require(
                    "expected every packet to be a key/sync sample" in str(exc),
                    f"unexpected crop key-sample failure: {exc}",
                )
            else:
                raise AssertionError("expected non-key crop packet to fail")
        finally:
            verifier.ffprobe_video = original_ffprobe
            verifier.ffprobe_packet_key_flags = original_ffprobe_key_flags


def test_status_sidecar_checks_rolling_progress() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096", rolling=True)
        write_status(root, "2010096", heartbeat_sequence=8, rolling=True)
        result = verify_one(root, summary_path, mp4_path, require_status=True)
        recorder_status = result["recorder_status"]
        require(result["rolling_clip_count"] == 2, "rolling clips should be verified with status")
        require(
            recorder_status["rolling_completed_clip_count"] == 2,
            "rolling completed clip count should be summarized",
        )
        require(
            recorder_status["rolling_last_completed_clip_index"] == 1,
            "rolling last completed clip should be summarized",
        )
        require(
            recorder_status["rolling_last_rollover_status"] == "completed",
            "rolling last rollover status should be summarized",
        )

        write_status(
            root,
            "2010096",
            heartbeat_sequence=9,
            rolling=True,
            rolling_last_completed_clip_index=0,
        )
        try:
            verify_one(root, summary_path, mp4_path, require_status=True)
        except verifier.VerificationError as exc:
            require(
                "last_completed_clip_index" in str(exc),
                f"unexpected rolling status mismatch failure: {exc}",
            )
        else:
            raise AssertionError("expected rolling status mismatch to fail")


def test_status_sidecar_failures() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        try:
            verify_one(root, summary_path, mp4_path, require_status=True)
        except verifier.VerificationError as exc:
            require("missing external recorder status JSON" in str(exc), f"unexpected missing-status failure: {exc}")
        else:
            raise AssertionError("expected missing status sidecar to fail")

        write_status(root, "2010096", status="running")
        try:
            verify_one(root, summary_path, mp4_path, require_status=True)
        except verifier.VerificationError as exc:
            require("status is not completed" in str(exc), f"unexpected bad-status failure: {exc}")
        else:
            raise AssertionError("expected unfinished status sidecar to fail")

        write_status(root, "2010096", frames_encoded=2)
        try:
            verify_one(root, summary_path, mp4_path, require_status=True)
        except verifier.VerificationError as exc:
            require("does not match summary" in str(exc), f"unexpected count failure: {exc}")
        else:
            raise AssertionError("expected mismatched status counts to fail")


def test_runtime_status_is_checked_when_required() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096")
        status_path = write_status(root, "2010096", heartbeat_sequence=9)

        try:
            verify_one(root, summary_path, mp4_path, require_runtime_status=True)
        except verifier.VerificationError as exc:
            require("missing external recorder supervisor runtime" in str(exc), f"unexpected runtime failure: {exc}")
        else:
            raise AssertionError("expected missing runtime to fail")

        write_runtime(root, "2010096", status_path, heartbeat_sequence=9)
        result = verify_one(root, summary_path, mp4_path, require_runtime_status=True)
        require(
            result["recorder_status"]["runtime_heartbeat_sequence"] == 9,
            "runtime heartbeat should be summarized",
        )

        write_runtime(root, "2010096", status_path, heartbeat_sequence=3)
        try:
            verify_one(root, summary_path, mp4_path, require_runtime_status=True)
        except verifier.VerificationError as exc:
            require("runtime heartbeat" in str(exc), f"unexpected runtime mismatch failure: {exc}")
        else:
            raise AssertionError("expected runtime heartbeat mismatch to fail")


def test_runtime_status_checks_rolling_progress() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary_path, mp4_path = write_summary(root, "2010096", rolling=True)
        status_path = write_status(root, "2010096", heartbeat_sequence=9, rolling=True)
        write_runtime(root, "2010096", status_path, heartbeat_sequence=9, rolling=True)
        result = verify_one(root, summary_path, mp4_path, require_runtime_status=True)
        require(
            result["recorder_status"]["runtime_heartbeat_sequence"] == 9,
            "runtime heartbeat should still be summarized",
        )

        write_runtime(
            root,
            "2010096",
            status_path,
            heartbeat_sequence=9,
            rolling=True,
            rolling_current_clip_index=0,
        )
        try:
            verify_one(root, summary_path, mp4_path, require_runtime_status=True)
        except verifier.VerificationError as exc:
            require(
                "runtime rolling_current_clip_index" in str(exc),
                f"unexpected runtime rolling mismatch failure: {exc}",
            )
        else:
            raise AssertionError("expected runtime rolling mismatch to fail")


def test_multi_shard_shard_mp4_retention_contract() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        serial = "2010096"
        mp4_path = root / f"Cam{serial}_external.mp4"
        mp4_path.write_bytes(b"merged-mp4")
        summary_path = root / f"Cam{serial}_external_summary.json"
        shard0 = root / f"Cam{serial}_external_shard0_gpu5.mp4"
        shard1 = root / f"Cam{serial}_external_shard1_gpu6.mp4"
        summary = {
            "schema_id": verifier.SUMMARY_SCHEMA_ID,
            "schema_version": 1,
            "tool": "external_recorder_ipc_probe",
            "stream_id": serial,
            "routing_policy": "gop_modulo",
            "shard_count": 2,
            "encode": True,
            "worker_failed": False,
            "frames_received": 4,
            "acks_sent": 4,
            "detach_copied": 4,
            "encode_enqueued": 4,
            "encode_skipped": 0,
            "encode_dropped": 0,
            "encode_queue_depth": 32,
            "encode_queue_high_water": 2,
            "frames_encoded": 4,
            "external_encode": {
                "frames_dropped": 0,
                "enqueue_age_p95_ms": 1.0,
            },
            "external_encode_shards": [
                {
                    "assigned_gpu_id": 5,
                    "assigned_shard_id": 0,
                    "frames_encoded": 2,
                    "frames_dropped": 0,
                    "worker_failed": False,
                    "mp4": str(shard0),
                    "mp4_retention": {
                        "status": "deleted_after_merged_finalization",
                        "retained": False,
                        "removed_after_merge": True,
                    },
                },
                {
                    "assigned_gpu_id": 6,
                    "assigned_shard_id": 1,
                    "frames_encoded": 2,
                    "frames_dropped": 0,
                    "worker_failed": False,
                    "mp4": str(shard1),
                    "mp4_retention": {
                        "status": "already_absent_after_merged_finalization",
                        "retained": False,
                        "removed_after_merge": False,
                    },
                },
            ],
            "merged_output": {
                "enabled": True,
                "failed": False,
                "packets_written": 4,
                "pending_gops": 0,
                "mp4": str(mp4_path),
            },
            "storage_preflight": storage_preflight_payload(),
            "ipc_protocol": ipc_protocol_payload(),
            "video_metadata": video_metadata_payload(serial),
            "outputs": {
                "mp4": str(mp4_path),
            },
        }
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        stream = {
            "stream_id": serial,
            "summary_json": str(summary_path),
            "mp4": str(mp4_path),
            "routing_policy": "gop_modulo",
            "expected_shard_gpu_ids": [5, 6],
        }
        contract = {
            "require_gop_routing": False,
            "require_merged_mp4": True,
            "require_video_sanity": False,
            "preserve_shard_mp4s": False,
        }
        original_ffprobe = verifier.ffprobe_video
        verifier.ffprobe_video = lambda path, ffprobe: {
            "tags": video_metadata_payload(serial)["mp4_tags_expected"],
            "packet_count": 4,
        }
        try:
            result = verifier.verify_summary(
                root,
                serial,
                stream,
                contract,
                "ffprobe",
                True,
                None,
                None,
                None,
                False,
                False,
                False,
                False,
            )
            require(
                result["shard_mp4_retention"] == "deleted_after_merged_finalization",
                "retention summary should report deleted shard MP4s",
            )

            shard0.write_bytes(b"leftover-shard")
            try:
                verifier.verify_summary(
                    root,
                    serial,
                    stream,
                    contract,
                    "ffprobe",
                    True,
                    None,
                    None,
                    None,
                    False,
                    False,
                    False,
                    False,
                )
            except verifier.VerificationError as exc:
                require(
                    "should have been removed" in str(exc),
                    f"unexpected retained shard failure: {exc}",
                )
            else:
                raise AssertionError("expected retained shard MP4 to fail")
        finally:
            verifier.ffprobe_video = original_ffprobe


def main() -> int:
    tests = [
        test_queue_thresholds_pass_and_summarize,
        test_single_clip_frame_metadata_is_required_and_complete,
        test_single_clip_frame_metadata_identity_contract,
        test_single_clip_frame_metadata_permits_intentional_rate_cap_gaps,
        test_single_clip_packet_parity_is_required,
        test_queue_threshold_failures,
        test_video_metadata_comment_mismatch_fails,
        test_queue_high_water_falls_back_to_detach_csv,
        test_mp4_queue_overflow_failures,
        test_storage_preflight_failures,
        test_rolling_output_uses_summary_recording_control,
        test_rolling_output_requires_keyframe_zero,
        test_rolling_full_frame_metadata_requires_canonical_leading_columns,
        test_status_sidecar_passes_and_summarizes,
        test_stream_kind_and_output_kind_match_contract,
        test_crop_external_mp4_requires_all_packet_key_samples,
        test_status_sidecar_checks_rolling_progress,
        test_status_sidecar_failures,
        test_runtime_status_is_checked_when_required,
        test_runtime_status_checks_rolling_progress,
        test_multi_shard_shard_mp4_retention_contract,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("verify_external_recorder_session_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
