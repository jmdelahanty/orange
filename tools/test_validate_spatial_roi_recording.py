#!/usr/bin/env python3
"""Deterministic host tests for validate_spatial_roi_recording.py."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import validate_spatial_roi_recording as verifier


KINDS = verifier.ARTIFACT_KINDS


def digest(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def make_fixture(folder: Path) -> None:
    serial = "fixture-camera"
    roi_ids = ["roi-%d" % index for index in range(1, 5)]
    streams = [serial + "_spatial_roi_" + roi_id for roi_id in roi_ids]
    token_subject = {
        "canonicalization": "canonical_json_utf8_sort_keys_compact_v1",
        "recording_id": "fixture-recording",
        "schema_id": "orange.shaman_v2.recording_identity",
        "schema_version": 1,
        "scope": "recording_session",
    }
    token = verifier.canonical_json_sha256(token_subject)
    native = {"width": 16, "height": 16}
    authorities = {
        "layout": {"id": "layout", "sha256": "sha256:" + "c" * 64},
        "materialization": {"id": "materialization", "sha256": "sha256:" + "d" * 64},
        "registration": {"id": "registration", "sha256": "sha256:" + "e" * 64},
    }
    recorder_gpus = {stream: index + 1 for index, stream in enumerate(streams)}
    media_budget = 10_000
    evidence_budget = 100_000
    reserve = 10
    profile = {
        "profile_id": "hevc_p1_low_latency_vbr_q20_gop25_v1",
        "codec": "hevc", "preset": "p1", "tuning": "ll",
        "lossless": False, "rate_control_mode": "vbr",
        "quality_value": 20,
        "gop_length": 25, "aq": False, "temporal_aq": False,
        "lookahead": False, "lookahead_depth": 0,
        "frame_rate": 1, "input_format": "mono8",
        "encoded_format": "nv12", "no_resize": True,
        "luma_preserved_exactly": False, "neutral_chroma_value": 128,
    }
    config_rois = []
    resolved_rois = []
    for index, (roi_id, stream) in enumerate(zip(roi_ids, streams), 1):
        stem = "Cam" + serial + "_spatial_roi_" + roi_id
        content_rect = {
            "x": (index - 1) * 4,
            "y": 0,
            "width": 4,
            "height": 4,
        }
        config_roi = {
            "roi_id": roi_id,
            "region_id": "region-%d" % index,
            "required": True,
            "content_rect": content_rect,
            "logical_stream_id": stream,
            "artifact_stem": stem,
        }
        config_rois.append(config_roi)
        resolved_roi = dict(config_roi)
        resolved_roi.update(
            {
                "encoded_raster": {"width": 4, "height": 4},
                "content_offset": {"x": 0, "y": 0},
                "encoded_content_rect": {
                    "x": 0,
                    "y": 0,
                    "width": 4,
                    "height": 4,
                },
                "padding": {"right": 0, "bottom": 0, "value_mono8": 0},
                "no_scaling": True,
                "socket_path": verifier._expected_socket_path(token, stream),
                "expected_artifacts": {
                    "video": "%s/%s.mp4" % (verifier.ARTIFACT_ROOT, stem),
                    "metadata": "%s/%s_meta.csv" % (verifier.ARTIFACT_ROOT, stem),
                    "keyframes": "%s/%s_keyframe.json" % (verifier.ARTIFACT_ROOT, stem),
                    "perf": "%s/%s_perf.csv" % (verifier.ARTIFACT_ROOT, stem),
                    "summary": "%s/%s_summary.json" % (verifier.ARTIFACT_ROOT, stem),
                    "finalization": "%s/%s.mp4.finalization.json" % (verifier.ARTIFACT_ROOT, stem),
                },
            }
        )
        resolved_rois.append(resolved_roi)
    configured_camera = {
        "camera_id": 0,
        "camera_serial": serial,
        "native_raster": native,
        "source_frame_rate": 1,
        "arena_group_id": "group",
        "layout": authorities["layout"],
        "materialization": authorities["materialization"],
        "registration": authorities["registration"],
        "allow_roi_overlap": False,
        "rois": config_rois,
    }
    resolved_camera = dict(configured_camera)
    resolved_camera["rois"] = resolved_rois
    normalized_config = {
        "schema_id": "orange.spatial_roi_recording.config",
        "schema_version": 3,
        "enabled": True,
        "strict": True,
        "backend": "independent_hevc_external_ipc",
        "source_cadence": "every_recording_frame",
        "encode_profile": {
            "name": profile["profile_id"],
            "codec": profile["codec"],
            "preset": profile["preset"],
            "tuning": profile["tuning"],
            "lossless": profile["lossless"],
            "rate_control_mode": profile["rate_control_mode"],
            "quality_value": profile["quality_value"],
            "gop_length": profile["gop_length"],
            "aq": profile["aq"],
            "temporal_aq": profile["temporal_aq"],
            "lookahead": profile["lookahead"],
            "lookahead_depth": profile["lookahead_depth"],
        },
        "pixel_contract": {
            "source_format": "mono8",
            "no_resize": True,
            "no_color_conversion": True,
            "output_alignment_px": 2,
            "padding_value_mono8": 0,
        },
        "buffering": {
            "pool_frames_per_stream": 2,
            "queue_frames_per_stream": 1,
        },
        "recording_limits": {
            "max_frames_per_stream": 100,
            "max_media_bytes_per_stream": media_budget // 4,
            "max_evidence_bytes_per_stream": evidence_budget // 4,
        },
        "admission": {
            "max_rois_per_camera": 4,
            "max_total_rois": 4,
            "max_total_pixel_rate": 64,
            "max_total_encoder_streams": 4,
            "max_total_pool_bytes": 128,
            "max_total_queue_frames": 4,
            "max_total_media_bytes": media_budget,
            "max_total_evidence_bytes": evidence_budget,
        },
        "cameras": {serial: configured_camera},
    }
    plan_payload = {
        "schema_id": "orange.spatial_roi_recording.plan",
        "schema_version": 3,
        "plan_scope": "detector_independent_camera_native_spatial_rois",
        "recording_id": "fixture-recording",
        "recording_identity_token": token,
        "generated_at_utc": "fixture",
        "producer_generation": "generation-1",
        "configuration": normalized_config,
        "admission_usage": {
            "camera_count": 1,
            "roi_count": 4,
            "encoder_stream_count": 4,
            "content_pixel_rate": 64,
            "encoded_pixel_rate": 64,
            "pool_bytes": 128,
            "queue_frames": 4,
            "media_bytes": media_budget,
            "evidence_bytes": evidence_budget,
        },
        "resolved_cameras": {serial: resolved_camera},
    }
    plan = verifier.canonical_json_sha256(plan_payload)
    verified_plan = {
        "schema_id": "orange.spatial_roi_recording.plan",
        "schema_version": 3,
        "canonicalization": "canonical_json_utf8_sort_keys_compact_v1",
        "plan_sha256": plan,
        "plan": plan_payload,
    }
    rois = []
    descriptors = {}
    receipt_streams = []
    for index, stream in enumerate(streams, 1):
        roi_id = roi_ids[index - 1]
        region_id = "region-%d" % index
        content_rect = {"x": (index - 1) * 4, "y": 0, "width": 4, "height": 4}
        encoded = {"width": 4, "height": 4}
        encoded_rect = {"x": 0, "y": 0, "width": 4, "height": 4}
        geometry = {
            "layout": authorities["layout"],
            "materialization": authorities["materialization"],
            "registration": authorities["registration"],
            "native_raster": native,
            "content_rect": content_rect,
            "encoded_raster": encoded,
            "encoded_content_rect": encoded_rect,
            "content_offset": {"x": 0, "y": 0},
            "padding": {"left": 0, "top": 0, "right": 0, "bottom": 0, "value_mono8": 0},
            "source_coordinate_space": "camera_native_full_frame_pixels",
            "video_coordinate_space": "spatial_roi_encoded_pixels",
        }
        roi = {
            "stream_id": stream, "logical_stream_id": stream, "roi_id": roi_id,
            "region_id": region_id, "arena_group_id": "group", "arena_id": None,
            "geometry": geometry,
            "source_geometry": {"native_raster": native, "content_rect": content_rect, "coordinate_space": geometry["source_coordinate_space"]},
            "encoded_geometry": {"raster": encoded, "content_rect": encoded_rect, "coordinate_space": geometry["video_coordinate_space"]},
            "encode_profile": profile, "encode_fps": 1, "codec": "hevc", "tuning": "ll",
            "analytics_gpu_id": 1, "source_gpu_id": 1, "recorder_gpu_id": index,
            "assigned_gpu_id": index, "expected_shard_gpu_ids": [index],
        }
        rois.append(roi)
        stem = "Cam" + serial + "_spatial_roi_" + roi_id
        leaves = {
            "video": stem + ".mp4",
            "metadata": stem + "_meta.csv",
            "keyframes": stem + "_keyframe.json",
            "perf": stem + "_perf.csv",
            "summary": stem + "_summary.json",
            "status": stem + "_status.json",
            "video_sanity": stem + "_video_sanity.json",
            "finalization": stem + ".mp4.finalization.json",
            "recorder_log": stem + "_recorder.log",
            "transport_sidecar": stem + "_transport.jsonl",
            "evidence": stem + "_evidence.jsonl",
            "evidence_manifest": stem + "_evidence_manifest.json",
        }
        paths = {
            kind: verifier.ARTIFACT_ROOT + "/" + leaves[kind]
            for kind in KINDS
        }
        receipt_payload = {"fixture_stream": stream, "frame_count": 4}
        receipt_digest = verifier.canonical_json_sha256(receipt_payload)
        evidence_manifest = dict(receipt_payload)
        evidence_manifest["finalized_receipt_digest"] = receipt_digest
        artifacts = []
        for kind in KINDS:
            if kind == "evidence_manifest":
                data = json.dumps(
                    evidence_manifest,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=False,
                ).encode("utf-8")
            else:
                data = ("%s:%s\n" % (stream, kind)).encode("ascii")
            path = folder / paths[kind]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            artifacts.append({"kind": kind, "relative_path": leaves[kind], "size_bytes": len(data), "sha256": digest(data)})
        identity = {
            "recording_id": "fixture-recording", "session_id": "fixture-recording",
            "recording_identity_token": token, "producer_generation": "generation-1",
            "spatial_roi_plan_sha256": plan, "camera_id": 0, "camera_serial": serial,
            "roi_id": roi_id, "region_id": region_id, "arena_group_id": "group",
            "logical_stream_id": stream, "assigned_gpu_id": index, "assigned_shard_id": 0,
        }
        counts = {key: 4 for key in verifier.COUNT_KEYS}
        counts["keyframes"] = verifier._expected_keyframe_count(
            4, profile["gop_length"]
        )
        for key in ("dispatch_rejected", "failed_frames", "ack_write_failures", "release_write_failures", "lifecycle_failures"):
            counts[key] = 0
        counts["encoded_bytes"] = len(("%s:video\n" % stream).encode("ascii"))
        receipt = {
            "logical_stream_id": stream, "identity": identity, "counts": counts,
            "ranges": {"recording_frame_id": {"first": 1, "last": 4}, "roi_stream_frame_index": {"first": 1, "last": 4}, "has_frames": True, "frame_count": 4},
            "finalized_receipt_digest": receipt_digest, "artifacts": artifacts,
        }
        receipt_streams.append(receipt)
        descriptor_identity = {
            "recording_id": "fixture-recording", "recording_identity_token": token,
            "producer_generation": "generation-1", "spatial_roi_plan_sha256": plan,
            "camera_id": 0, "camera_serial": serial, "arena_group_id": "group",
            "arena_id": None, "region_id": region_id, "roi_id": roi_id,
            "logical_stream_id": stream,
        }
        details = {
            "stream_id": stream,
            "stream_kind": "spatial_roi",
            "identity": descriptor_identity,
            "artifact_path_scope": "recording_root_relative",
            "artifact_root_relative": verifier.ARTIFACT_ROOT,
            "recording_id": "fixture-recording", "session_id": "fixture-recording",
            "recording_identity_token": token, "producer_generation": "generation-1",
            "spatial_roi_plan_sha256": plan, "camera_id": 0, "camera_serial": serial,
            "roi_id": roi_id, "region_id": region_id, "arena_group_id": "group",
            "arena_id": None, "logical_stream_id": stream, "artifacts": paths,
            "analytics_gpu_id": 1, "source_gpu_id": 1, "recorder_gpu_id": index,
            "assigned_gpu_id": index, "geometry": geometry,
            "geometry_identity": geometry, "encode_profile": profile,
            "encode_fps": 1, "gop": profile["gop_length"],
            "rate_control_mode": profile["rate_control_mode"],
            "quality_value": profile["quality_value"], "encode_queue_depth": 1,
            "expected_shard_gpu_ids": [index],
            "source_geometry": roi["source_geometry"],
            "encoded_geometry": {
                "raster": encoded, "content_rect": encoded_rect,
                "coordinate_space": geometry["video_coordinate_space"],
            },
            "finalized_receipt": receipt,
        }
        descriptors[stream] = {
            "schema_version": 3, "camera_serial": serial, "output_kind": "spatial_roi",
            "logical_stream_id": stream, "role": "runtime_derived_acquisition_input",
            "backend": "external_ipc", "status": "complete", "video": paths["video"],
            "metadata": paths["metadata"], "keyframes": paths["keyframes"], "perf": paths["perf"],
            "summary": paths["summary"], "codec": "hevc", "container": "mp4",
            "tuning": profile["tuning"], "pixel_source_format": "mono8",
            "encoded_format": "nv12", "coordinate_space": geometry["source_coordinate_space"],
            "video_pixel_coordinate_space": geometry["video_coordinate_space"],
            "source_geometry_coordinate_space": geometry["source_coordinate_space"],
            "width": 4, "height": 4, "frame_rate": 1, "frame_count": 4,
            "first_recording_frame_id": 1,
            "last_recording_frame_id": 4, "recording_frame_id_gaps": 0, "packet_count": 4,
            "packet_count_source": "spatial_roi_finalized_session_receipt", "details": details,
        }
    contract_streams = {}
    contract_artifact_root = str(folder / verifier.ARTIFACT_ROOT)
    for index, stream in enumerate(streams):
        roi = rois[index]
        descriptor = descriptors[stream]
        expected_artifacts = {
            kind: str(folder / descriptor["details"]["artifacts"][kind])
            for kind in KINDS
        }
        stream_identity = {
            "recording_id": "fixture-recording",
            "recording_identity_token": token,
            "producer_generation": "generation-1",
            "spatial_roi_plan_sha256": plan,
            "camera_id": 0,
            "camera_serial": serial,
            "arena_group_id": roi["arena_group_id"],
            "arena_id": roi["arena_id"],
            "region_id": roi["region_id"],
            "roi_id": roi["roi_id"],
            "logical_stream_id": stream,
        }
        contract_streams[stream] = {
            "stream_id": stream,
            "logical_stream_id": stream,
            "stream_kind": "spatial_roi",
            "output_kind": "spatial_roi",
            "camera_id": 0,
            "camera_serial": serial,
            "env_key": "spatial_roi_" + stream,
            "socket_path": verifier._expected_socket_path(token, stream),
            "roi_id": roi["roi_id"],
            "region_id": roi["region_id"],
            "arena_group_id": roi["arena_group_id"],
            "arena_id": roi["arena_id"],
            "recording_id": "fixture-recording",
            "session_id": "fixture-recording",
            "recording_identity_token": token,
            "producer_generation": "generation-1",
            "spatial_roi_plan_sha256": plan,
            "identity": stream_identity,
            "analytics_gpu_id": roi["analytics_gpu_id"],
            "source_gpu_id": roi["source_gpu_id"],
            "recorder_gpu_id": roi["recorder_gpu_id"],
            "assigned_gpu_id": roi["assigned_gpu_id"],
            "geometry_identity": roi["geometry"],
            "encode_profile": roi["encode_profile"],
            "encode_fps": roi["encode_fps"],
            "codec": roi["codec"],
            "tuning": roi["tuning"],
            "rate_control_mode": profile["rate_control_mode"],
            "quality_value": profile["quality_value"],
            "gop": profile["gop_length"],
            "encode_queue_depth": 1,
            "detach_pool_frames": 1,
            "max_detach_pool_bytes": 40,
            "max_queue_bytes": 24,
            "writer_queue_max_packets": verifier.CONTRACT_WRITER_QUEUE_MAX_PACKETS,
            "writer_queue_max_bytes": verifier.CONTRACT_WRITER_QUEUE_MAX_BYTES,
            "operation_timeout_ms": verifier.CONTRACT_OPERATION_TIMEOUT_MS,
            "routing_policy": "single_shard",
            "max_frames_per_stream": 100,
            "max_media_bytes_per_stream": media_budget // 4,
            "max_evidence_bytes_per_stream": evidence_budget // 4,
            "expected_shard_gpu_ids": roi["expected_shard_gpu_ids"],
            "recording_control": {"record_for_seconds": 0, "clip_seconds": 0},
            "rollover": {
                "requested": False,
                "status": "not_requested",
                "implementation": "none",
            },
            "frame_identity": {
                "key_fields": [
                    "recording_identity_token",
                    "producer_generation",
                    "logical_stream_id",
                    "recording_frame_id",
                    "roi_stream_frame_index",
                ],
                "roi_stream_frame_index": "dense_one_based",
                "recording_frame_id_source": "parent_camera_recording",
            },
            "mp4": expected_artifacts["video"],
            "metadata_csv": expected_artifacts["metadata"],
            "keyframe_json": expected_artifacts["keyframes"],
            "perf_csv": expected_artifacts["perf"],
            "summary_json": expected_artifacts["summary"],
            "status_json": expected_artifacts["status"],
            "video_sanity_json": expected_artifacts["video_sanity"],
            "finalization_json": expected_artifacts["finalization"],
            "recorder_log": expected_artifacts["recorder_log"],
            "transport_sidecar": expected_artifacts["transport_sidecar"],
            "evidence_jsonl": expected_artifacts["evidence"],
            "evidence_manifest_json": expected_artifacts["evidence_manifest"],
            "expected_artifacts": expected_artifacts,
        }
    recorder_contract = {
        "schema_id": "orange.spatial_roi_recording.external_recorder_contract",
        "schema_version": 5,
        "contract_scope": "strict_spatial_roi_external_recorder_v5",
        "backend": "independent_hevc_external_ipc",
        "mode": "spatial_roi_external_recorder_v5",
        "strict": True,
        "supervise_processes": True,
        "require_summary": True,
        "require_status": True,
        "require_video_sanity": True,
        "require_protocol_hello": True,
        "require_frame_identity_proof": True,
        "require_gop_routing": False,
        "require_storage_preflight": True,
        "preserve_shard_mp4s": False,
        "recording_id": "fixture-recording",
        "session_id": "fixture-recording",
        "recording_identity_token": token,
        "producer_generation": "generation-1",
        "spatial_roi_plan_sha256": plan,
        "recording_root": str(folder),
        "artifact_root": contract_artifact_root,
        "source_cadence": "every_recording_frame",
        "source_pixel_format": "mono8",
        "stream_count": 4,
        "stream_order": streams,
        "ipc_v2": verifier.expected_contract_ipc_v2(1, 4),
        "gpu_mapping": {
            "analytics_gpu_by_camera_serial": {serial: 1},
            "recorder_gpu_by_logical_stream_id": recorder_gpus,
        },
        "storage_preflight_policy": {
            "schema_id": verifier.STORAGE_PREFLIGHT_POLICY_SCHEMA_ID,
            "schema_version": verifier.STORAGE_PREFLIGHT_POLICY_SCHEMA_VERSION,
            "required": True,
            "reserved_free_bytes": reserve,
        },
        "aggregate_bounds": {
            "max_detach_pool_bytes_total": 160,
            "max_queue_bytes_total": 96,
            "writer_queue_max_packets_total": (
                verifier.CONTRACT_WRITER_QUEUE_MAX_PACKETS * 4
            ),
            "writer_queue_max_bytes_total": (
                verifier.CONTRACT_WRITER_QUEUE_MAX_BYTES * 4
            ),
            "operation_timeout_ms_per_stream": verifier.CONTRACT_OPERATION_TIMEOUT_MS,
            "max_media_bytes_total": media_budget,
            "max_evidence_bytes_total": evidence_budget,
        },
        "recording_control": {"record_for_seconds": 0, "clip_seconds": 0},
        "rollover": {
            "requested": False,
            "status": "not_requested",
            "implementation": "none",
        },
        "streams": contract_streams,
    }
    binding_session = {
        "recording_id": "fixture-recording",
        "session_id": "fixture-recording",
        "recording_identity_token": token,
        "producer_generation": "generation-1",
        "spatial_roi_plan_sha256": plan,
        "camera_id": 0,
        "camera_serial": serial,
    }
    recording_limits = normalized_config["recording_limits"]
    for index, stream in enumerate(streams):
        roi = rois[index]
        receipt = receipt_streams[index]
        artifacts = {item["kind"]: item for item in receipt["artifacts"]}
        encoder_counts = {
            "enqueue_attempted": 4,
            "enqueued": 4,
            "dequeued": 4,
            "rejected": 0,
            "queue_overflows": 0,
            "copy_completed": 4,
            "source_releases": 4,
            "encoded_frames": 4,
            "encoded_packets": 4,
            "encoded_bytes": receipt["counts"]["encoded_bytes"],
            "copy_failures": 0,
            "encode_failures": 0,
            "writer_failures": 0,
            "writer_queue_overflows": 0,
            "frame_results_emitted": 4,
            "encoded_results": 4,
            "failed_results": 0,
            "result_callback_failures": 0,
            "source_quarantines": 0,
            "destination_quarantines": 0,
            "peak_queue_depth": 1,
            "finalize_calls": 1,
            "finalized": True,
            "failed": False,
            "source_release_safe": True,
            "metadata_flushed": True,
            "media_finalization_validated": True,
            "artifacts_sealed": True,
        }
        encoder_writer = {
            "observed": True,
            "failure_latched": False,
            "packet_write_error_latched": False,
            "writer_thread_failure_latched": False,
            "queue_overflow_latched": False,
            "close_finalization_validated": True,
            "close_finalization_failure_latched": False,
            "packet_allocation_failures": 0,
            "packet_enqueue_failures": 0,
            "packet_write_failures": 0,
            "muxer_flush_failures": 0,
            "sidecar_write_failures": 0,
            "video_size_limit_failures": 0,
            "thread_failures": 0,
            "total_failures": 0,
            "queue_overflow_events": 0,
            "last_error_code": 0,
            "first_failure_reason": "",
            "close_finalization_failure_reason": "",
            "snapshot_complete": True,
        }
        encoder_terminal = {
            "terminal": True,
            "successful": True,
            "drain_completed": True,
            "metadata_flushed": True,
            "media_finalization_validated": True,
            "artifacts_sealed": True,
            "all_admitted_results_emitted": True,
            "all_enqueue_attempts_accounted": True,
            "nonempty_stream": True,
            "source_release_safe": True,
            "source_quarantined": False,
            "destination_quarantined": False,
            "terminal_reason": "complete",
            "counts": encoder_counts,
            "writer": encoder_writer,
            "snapshot_schema": "spatial_roi_lossless_terminal_v2",
        }
        finalize_references = {
            kind: {
                key: artifacts[kind][key]
                for key in ("relative_path", "size_bytes", "sha256")
            }
            for kind in verifier.FINALIZE_ARTIFACT_KINDS
        }
        finalize_payload = {
            "terminal_state": "complete",
            "terminal_reason": "complete",
            "artifacts": {
                kind: finalize_references[kind]["relative_path"]
                for kind in verifier.FINALIZE_ARTIFACT_KINDS
            },
            "encoder_terminal": encoder_terminal,
        }
        evidence_reference = {
            key: artifacts["evidence"][key]
            for key in ("relative_path", "size_bytes", "sha256")
        }
        evidence_manifest = {
            "schema_id": "orange.spatial_roi_recorder.finalized_manifest",
            "schema_version": 2,
            "canonicalization": "canonical_json_utf8_sort_keys_compact_v1",
            "stream_kind": "fixed_region",
            "binding": verifier._expected_evidence_binding(
                recorder_contract,
                verified_plan,
                binding_session,
                roi,
                recording_limits,
            ),
            "evidence": evidence_reference,
            "artifacts": finalize_references,
            "counts": receipt["counts"],
            "ranges": receipt["ranges"],
            "terminal": {"state": "complete", "reason": "complete"},
            "encoder_terminal": encoder_terminal,
            "finalize_request_sha256": verifier.canonical_json_sha256(
                finalize_payload
            ),
        }
        receipt_digest = verifier.canonical_json_sha256(evidence_manifest)
        evidence_manifest["finalized_receipt_digest"] = receipt_digest
        data = json.dumps(
            evidence_manifest,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
        manifest_item = artifacts["evidence_manifest"]
        (folder / descriptors[stream]["details"]["artifacts"]["evidence_manifest"]).write_bytes(data)
        manifest_item["size_bytes"] = len(data)
        manifest_item["sha256"] = digest(data)
        receipt["finalized_receipt_digest"] = receipt_digest
    authority_documents = {
        "normalized_config": ("config.json", normalized_config),
        "verified_plan": ("plan.json", verified_plan),
        "recorder_contract": ("contract.json", recorder_contract),
    }
    authority_references = {}
    for name, (relative, document) in authority_documents.items():
        data = json.dumps(
            document, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
        (folder / relative).write_bytes(data)
        authority_references[name] = {
            "relative_path": relative,
            "size_bytes": len(data),
            "sha256": digest(data),
        }
    session = {
        "schema_id": "orange.spatial_roi_recording.session_snapshot", "schema_version": 3,
        "status": "complete", "recording_id": "fixture-recording", "session_id": "fixture-recording",
        "recording_identity_token": token, "producer_generation": "generation-1", "spatial_roi_plan_sha256": plan,
        "product_kind": "fixed_region", "stream_count": 4, "stream_order": streams,
        "identity": {"recording_id": "fixture-recording", "session_id": "fixture-recording", "recording_identity_token": token, "producer_generation": "generation-1", "spatial_roi_plan_sha256": plan},
        "camera": {"camera_id": 0, "camera_serial": serial, "native_raster": native}, "camera_id": 0, "camera_serial": serial, "native_raster": native,
        "authorities": authorities, "gpu_mapping": {"analytics_gpu_by_camera_serial": {serial: 1}, "recorder_gpu_by_logical_stream_id": recorder_gpus},
        "artifacts": authority_references,
        "rois": rois, "finalized_session_receipt": None, "recorder_process_status": {"state": "complete"}, "producer_status": {"state": "complete"},
    }
    receipt = {
        "schema_id": "orange.spatial_roi_recording.finalized_session_receipt", "schema_version": 1,
        "canonicalization": "canonical_json_utf8_sort_keys_compact_v1", "stream_kind": "fixed_region", "status": "complete", "stream_count": 4, "stream_order": streams,
        "identity": {"recording_id": "fixture-recording", "session_id": "fixture-recording", "recording_identity_token": token, "producer_generation": "generation-1", "spatial_roi_plan_sha256": plan, "camera_id": 0, "camera_serial": serial, "stream_count": 4, "stream_order": streams},
        "root_authority": {"artifact_root_relative": verifier.ARTIFACT_ROOT, "recording_root_identity": {"device": folder.stat().st_dev, "inode": folder.stat().st_ino}, "artifact_root_identity": {"device": (folder / verifier.ARTIFACT_ROOT).stat().st_dev, "inode": (folder / verifier.ARTIFACT_ROOT).stat().st_ino}, "root_continuity": {"proven": ["opened"], "not_proven": ["historical"]}},
        "streams": receipt_streams,
    }
    session["finalized_session_receipt"] = receipt
    preflight = {
        "schema_id": verifier.STORAGE_PREFLIGHT_SCHEMA_ID,
        "schema_version": verifier.STORAGE_PREFLIGHT_SCHEMA_VERSION,
        "checked": True,
        "passed": True,
        "status": "passed",
        "error": "",
        "policy": {
            "schema_id": verifier.STORAGE_PREFLIGHT_POLICY_SCHEMA_ID,
            "schema_version": verifier.STORAGE_PREFLIGHT_POLICY_SCHEMA_VERSION,
            "required": True,
            "reserved_free_bytes": reserve,
        },
        "artifact_root": receipt["root_authority"]["artifact_root_identity"],
        "filesystem": {
            "block_size_bytes": 1,
            "total_blocks": 200_000,
            "available_blocks": 200_000,
            "capacity_bytes": 200_000,
            "available_bytes": 200_000,
        },
        "budgets": {
            "max_media_bytes_total": media_budget,
            "max_evidence_bytes_total": evidence_budget,
            "reserved_free_bytes": reserve,
            "required_bytes": media_budget + evidence_budget + reserve,
        },
    }
    ready_child = {
        "event": "ready",
        "status": "ready",
        "state": "ready",
        "ready": True,
        "clean_eof": False,
        "completed": False,
        "failed": False,
        "first_failure_stream_id": "",
        "first_failure": "",
        "error": "",
        "payload": {"storage_preflight": preflight},
    }
    terminal_child = {
        "event": "terminal",
        "status": "complete",
        "state": "completed",
        "ready": True,
        "clean_eof": True,
        "completed": True,
        "failed": False,
        "first_failure_stream_id": "",
        "first_failure": "",
        "error": "",
        "payload": {"storage_preflight": preflight},
    }
    starting_child = {
        "event": "starting",
        "status": "starting",
        "state": "starting",
        "ready": False,
        "clean_eof": False,
        "completed": False,
        "failed": False,
        "first_failure_stream_id": "",
        "first_failure": "",
        "error": "",
        "payload": {
            "event": "starting",
            "status": "starting",
            "state": "starting",
            "pid": 123,
        },
    }
    heartbeat_child = {
        "event": "",
        "status": "",
        "state": "",
        "ready": False,
        "clean_eof": False,
        "completed": False,
        "failed": False,
        "first_failure_stream_id": "",
        "first_failure": "",
        "error": "",
        "payload": {},
    }
    session["recorder_process_status"] = {
        "schema_id": "orange.spatial_roi_recording.headless_process_status",
        "schema_version": 1,
        "session_state": "finished",
        "process_state": "exited",
        "pid": 123,
        "started": True,
        "sockets_bound": True,
        "ready": True,
        "terminal_seen": True,
        "exited": True,
        "reaped": True,
        "exit_code": 0,
        "term_signal": 0,
        "stdout_bytes_read": 1,
        "cleanup_complete": True,
        "first_failure": "",
        "error": "",
        "starting": starting_child,
        "ready_snapshot": ready_child,
        "heartbeat": heartbeat_child,
        "terminal": terminal_child,
        "last": terminal_child,
    }
    session["producer_status"] = {
        "schema_id": "orange.spatial_roi_recording.headless_producer_status",
        "schema_version": 1,
        "state": "stopped",
        "recording_id": "fixture-recording",
        "session_id": "fixture-recording",
        "recording_identity_token": token,
        "producer_generation": "generation-1",
        "spatial_roi_plan_sha256": plan,
        "camera_id": 0,
        "camera_serial": serial,
        "stream_count": 4,
        "submit_attempted": 4,
        "submitted": 4,
        "incomplete": 0,
        "rejected": 0,
        "acquisition_armed": False,
        "first_failure": "",
    }
    full = {
        "schema_version": 1,
        "camera_serial": serial,
        "output_kind": "full",
        "role": "ingest_authoritative",
        "backend": "in_process",
        "status": "finalized",
        "container": "mp4",
        "coordinate_space": "full_frame_pixels",
        "video": "Cam%s.mp4" % serial,
        "metadata": "Cam%s_meta.csv" % serial,
        "keyframes": "Cam%s_keyframe.json" % serial,
        "frame_count": 4,
        "first_recording_frame_id": 1,
        "last_recording_frame_id": 4,
        "recording_frame_id_gaps": 0,
        "packet_count": 4,
        "packet_count_source": "ffprobe_nb_read_packets",
    }
    outputs = {
        "schema_id": "orange.recording_outputs",
        "schema_version": 3,
        "cameras": {serial: {"full": full, "spatial_roi": descriptors}},
    }
    legacy_outputs = {serial: {"full": full}}
    camera_artifact = {
        "video": full["video"],
        "metadata": full["metadata"],
        "keyframes": full["keyframes"],
        "frame_count": 4,
        "first_recording_frame_id": 1,
        "last_recording_frame_id": 4,
        "recording_frame_id_gaps": 0,
        "packet_count": 4,
        "packet_count_source": "ffprobe_nb_read_packets",
    }
    (folder / ("Cam%s.mp4" % serial)).write_bytes(b"full-frame-fixture\n")
    (folder / ("Cam%s_meta.csv" % serial)).write_text(
        "frame_id,timestamp,timestamp_sys\n"
        "1,1000,2000\n2,1001,2001\n3,1002,2002\n4,1003,2003\n",
        encoding="utf-8",
    )
    (folder / ("Cam%s_keyframe.json" % serial)).write_text(
        "{}\n", encoding="utf-8"
    )
    clip_artifacts = {
        "videos": {serial: full["video"]},
        "metadata": {serial: full["metadata"]},
        "keyframes": {serial: full["keyframes"]},
    }
    manifest = {
        "schema_id": "orange.recording_session",
        "schema_version": 1,
        "producer": "fixture",
        "session_id": "fixture-recording",
        "created_at_utc": "fixture",
        "updated_at_utc": "fixture",
        "recording_folder": str(folder),
        "mode": "single_clip",
        "status": "completed",
        "cameras": [serial],
        "camera_artifacts": {serial: camera_artifact},
        "recording": {"started": True, "drain_completed": True},
        "recording_backend": {
            "mode": "in_process",
            "full_frame": {"status": "finalized", "first_class": True},
            "spatial_roi_recording": session,
        },
        "recording_outputs": legacy_outputs,
        "recording_outputs_v3": outputs,
        "clips": [
            {
                "recording_folder": str(folder),
                "directory": ".",
                "artifacts": clip_artifacts,
                "recording_outputs": legacy_outputs,
                "recording_outputs_v3": outputs,
            }
        ],
    }
    snapshot = {
        "recording_outputs": legacy_outputs,
        "recording_outputs_v3": outputs,
        "encoders": {serial: {"outputs": {"full": full}}},
        "session": {
            "recording_mode": "single_clip",
            "recording_session_manifest_path": str(
                folder / "recording_session.json"
            ),
            "spatial_roi_recording": session,
            "recording_backend": {
                "mode": "in_process",
                "full_frame": {"status": "finalized", "first_class": True},
                "spatial_roi_recording": session,
            },
        },
    }
    (folder / "recording_session.json").write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
    (folder / "recording_snapshot.json").write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")


class VerifierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="spatial-roi-verifier-")
        self.folder = Path(self.temp.name)
        make_fixture(self.folder)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def verify_without_probe(self):
        with mock.patch.object(verifier.shutil, "which", return_value=None):
            return verifier.verify(str(self.folder))

    def authenticated_inputs(self):
        manifest = json.loads(
            (self.folder / "recording_session.json").read_text(encoding="utf-8")
        )
        session = manifest["recording_backend"]["spatial_roi_recording"]
        documents = {
            name: json.loads((self.folder / path).read_text(encoding="utf-8"))
            for name, path in (
                ("normalized_config", "config.json"),
                ("verified_plan", "plan.json"),
                ("recorder_contract", "contract.json"),
            )
        }
        preflight = session["recorder_process_status"]["terminal"]["payload"][
            "storage_preflight"
        ]
        descriptors = manifest["recording_outputs_v3"]["cameras"][
            "fixture-camera"
        ]["spatial_roi"]
        return manifest, session, documents, preflight, descriptors

    def sync_manifest_output_compatibility(self, manifest):
        serial = "fixture-camera"
        versioned = manifest["recording_outputs_v3"]
        full = versioned["cameras"][serial]["full"]
        legacy = {serial: {"full": full}}
        manifest["recording_outputs"] = legacy
        manifest["camera_artifacts"] = {
            serial: {
                "video": full["video"],
                "metadata": full["metadata"],
                "keyframes": full["keyframes"],
                "frame_count": full["frame_count"],
                "first_recording_frame_id": full["first_recording_frame_id"],
                "last_recording_frame_id": full["last_recording_frame_id"],
                "recording_frame_id_gaps": full["recording_frame_id_gaps"],
                "packet_count": full["packet_count"],
                "packet_count_source": full["packet_count_source"],
            }
        }
        clip = manifest["clips"][0]
        clip["recording_outputs"] = legacy
        clip["recording_outputs_v3"] = versioned
        clip["artifacts"] = {
            "videos": {serial: full["video"]},
            "metadata": {serial: full["metadata"]},
            "keyframes": {serial: full["keyframes"]},
        }

    def sync_snapshot_output_compatibility(self, snapshot, manifest):
        serial = "fixture-camera"
        versioned = manifest["recording_outputs_v3"]
        full = versioned["cameras"][serial]["full"]
        snapshot["recording_outputs_v3"] = versioned
        snapshot["recording_outputs"] = {serial: {"full": full}}
        snapshot.setdefault("encoders", {}).setdefault(serial, {}).setdefault(
            "outputs", {}
        )["full"] = full

    def convert_fixture_to_external_combined(self):
        serial = "fixture-camera"
        manifest_path = self.folder / "recording_session.json"
        snapshot_path = self.folder / "recording_snapshot.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        full = manifest["recording_outputs_v3"]["cameras"][serial]["full"]
        external_root = self.folder / "external_recorder"
        external_root.mkdir()
        for key in ("video", "metadata", "keyframes"):
            source = self.folder / full[key]
            destination = external_root / source.name
            source.rename(destination)
            full[key] = "external_recorder/" + destination.name
        full["backend"] = "external_ipc"
        full["packet_count_source"] = (
            "external_recorder_summary.packets_written"
        )
        backend = {
            "mode": "external_ipc",
            "full_frame": {"status": "finalized", "first_class": True},
            "spatial_roi_recording": manifest["recording_backend"][
                "spatial_roi_recording"
            ],
            "artifact_root": str(external_root),
            "combined_storage_preflight": "combined_storage_preflight.json",
        }
        manifest["recording_backend"] = backend
        self.sync_manifest_output_compatibility(manifest)
        snapshot["session"]["recording_backend"] = backend
        self.sync_snapshot_output_compatibility(snapshot, manifest)

        contract = json.loads((self.folder / "contract.json").read_text())
        aggregate = contract["aggregate_bounds"]
        roi_total = (
            aggregate["max_media_bytes_total"]
            + aggregate["max_evidence_bytes_total"]
        )
        filesystem_key = "st_dev:%d" % self.folder.stat().st_dev
        full_stream = {
            "plan_artifact_root": str(external_root),
            "stream_id": serial,
            "camera_serial": serial,
            "stream_kind": "full_frame",
            "output_kind": "full",
            "output_path": str(external_root / ("Cam%s.mp4" % serial)),
            "filesystem_key": filesystem_key,
            "rate_basis": "configured_max_bitrate_bps",
            "duration_seconds": 4,
            "duration_hours": 4 / 3600,
            "encode_fps": 1,
            "conservative_rate_bps": 2000,
            "video_bytes": 1000,
            "metadata_bytes": 1000,
            "retained_copy_multiplier": 1,
            "peak_copy_multiplier": 1,
            "estimated_retained_bytes": 2000,
            "estimated_peak_bytes": 2000,
            "bounded": True,
            "error": "",
        }
        roi_stream = {
            "plan_artifact_root": str(self.folder),
            "stream_id": "spatial_roi_aggregate_bound",
            "camera_serial": serial,
            "stream_kind": "spatial_roi",
            "output_kind": "spatial_roi",
            "output_path": str(
                self.folder
                / verifier.ARTIFACT_ROOT
                / "combined_storage_preflight_probe.mp4"
            ),
            "filesystem_key": filesystem_key,
            "rate_basis": "configured_max_bitrate_bps",
            "duration_seconds": 1,
            "duration_hours": 1 / 3600,
            "encode_fps": 1,
            "conservative_rate_bps": roi_total * 8,
            "video_bytes": roi_total,
            "metadata_bytes": 0,
            "retained_copy_multiplier": 1,
            "peak_copy_multiplier": 1,
            "estimated_retained_bytes": roi_total,
            "estimated_peak_bytes": roi_total,
            "bounded": True,
            "error": "",
        }
        retained = roi_total + 2000
        peak = retained
        reserve = contract["storage_preflight_policy"]["reserved_free_bytes"]
        safety = (peak + 9) // 10
        required = peak + safety + reserve
        available = required + 100000
        evidence = {
            "schema_id": "orange.combined_storage_preflight",
            "schema_version": 1,
            "checked": True,
            "ok": True,
            "hard_guarantee": True,
            "status": "pass",
            "error": "",
            "product": "full_frame_plus_spatial_roi",
            "recording_root": str(self.folder),
            "full_frame_artifact_root": str(external_root),
            "spatial_roi_bounds": {
                "max_media_bytes_total": aggregate["max_media_bytes_total"],
                "max_evidence_bytes_total": aggregate[
                    "max_evidence_bytes_total"
                ],
                "reserved_free_bytes": reserve,
            },
            "capacity_binding": {
                "filesystem_key": filesystem_key,
                "full_frame_and_roi_summed": True,
                "single_shared_filesystem": True,
            },
            "summary": {
                "requested_duration_seconds": 4,
                "requested_duration_hours": 4 / 3600,
                "camera_count": 1,
                "stream_count": 2,
                "full_frame_stream_count": 1,
                "crop_stream_count": 0,
                "spatial_roi_aggregate_stream_count": 1,
                "filesystem_count": 1,
                "aggregate_estimated_retained_bytes": retained,
                "aggregate_estimated_peak_bytes": peak,
                "aggregate_required_available_bytes": required,
            },
            "policy": {
                "enabled": True,
                "safety_headroom_ratio": 0.1,
                "reserved_free_bytes": reserve,
                "metadata_bytes_per_frame": 1024,
                "raw_nv12_expansion_ratio": 1.1,
                "require_finite_duration": False,
                "planned_duration_seconds": 4,
            },
            "streams": [full_stream, roi_stream],
            "filesystems": [
                {
                    "filesystem_key": filesystem_key,
                    "probe_path": str(external_root),
                    "capacity_bytes": available + 100000,
                    "available_bytes": available,
                    "estimated_retained_bytes": retained,
                    "estimated_peak_bytes": peak,
                    "safety_headroom_bytes": safety,
                    "reserved_free_bytes": reserve,
                    "configured_min_free_bytes": 0,
                    "required_available_bytes": required,
                    "projected_available_after_bytes": available - peak,
                    "ok": True,
                    "error": "",
                }
            ],
        }
        (self.folder / "combined_storage_preflight.json").write_text(
            json.dumps(evidence, sort_keys=True), encoding="utf-8"
        )
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        return evidence

    def convert_fixture_to_registered_context(self):
        """Build the policy's no-full-frame compatibility and context artifacts."""
        serial = "fixture-camera"
        manifest_path = self.folder / "recording_session.json"
        snapshot_path = self.folder / "recording_snapshot.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        session = manifest["recording_backend"]["spatial_roi_recording"]
        policy = {
            "schema_id": verifier.MEDIA_POLICY_SCHEMA_ID,
            "schema_version": verifier.MEDIA_POLICY_SCHEMA_VERSION,
            "media_policy": verifier.FIXED_ROIS_WITH_REGISTERED_CONTEXT,
            "retained_products": {
                "full_frame": False,
                "fixed_rois": True,
                "registered_context": True,
            },
            "sink_backend": "external_ipc",
        }
        declaration = {
            "schema_id": "orange.recording.registered_scene_context.capture_declaration",
            "schema_version": 1,
            "registration_authority_status": "diagnostic_not_physical_acceptance",
            "subject_presence": "unknown",
            "dish_setup_complete": True,
            "nir_illumination_fixed": True,
            "camera_configuration_fixed": True,
            "rig_fixed": True,
        }
        context_bytes = bytes(range(256))
        artifact_path = self.folder / verifier.REGISTERED_CONTEXT_ARTIFACT_PATH
        artifact_path.write_bytes(context_bytes)
        descriptor = {
            "schema_id": verifier.REGISTERED_CONTEXT_DESCRIPTOR_SCHEMA_ID,
            "schema_version": verifier.REGISTERED_CONTEXT_DESCRIPTOR_SCHEMA_VERSION,
            "canonicalization": verifier.REGISTERED_CONTEXT_CANONICALIZATION,
            "capture_role": verifier.REGISTERED_CONTEXT_CAPTURE_ROLE,
            "status": "complete",
            "failure_reason": "",
            "recording": {
                "recording_id": session["recording_id"],
                "session_id": session["session_id"],
                "recording_identity_token": session["recording_identity_token"],
                "producer_generation": session["producer_generation"],
            },
            "camera": {
                "camera_id": session["camera_id"],
                "camera_serial": serial,
                "source_camera_stream_id": "camera_" + serial,
                "stream_epoch_id": session["producer_generation"],
                "camera_configuration_sha256": digest(b"camera-config"),
            },
            "source_frame": {
                "source_frame_id": 100,
                "local_frame_id": 100,
                "camera_frame_id": 200,
                "recording_frame_id": 0,
                "camera_timestamp_ns": 300,
                "timestamp_sys_ns": 400,
            },
            "native_raster": {"width": 16, "height": 16, "stride_bytes": 16},
            "coordinate_space": "camera_native_px",
            "pixel_format": "Mono8",
            "geometry_binding": {
                "layout": session["authorities"]["layout"],
                "materialization": session["authorities"]["materialization"],
                "registration": session["authorities"]["registration"],
            },
            "capture_invariants": {
                "daily_registration_accepted": False,
                "registration_authority_status": declaration["registration_authority_status"],
                "dish_setup_complete": True,
                "subject_presence": "unknown",
                "nir_illumination_fixed": True,
                "camera_configuration_fixed": True,
                "rig_fixed": True,
            },
            "artifact": {
                "relative_path": verifier.REGISTERED_CONTEXT_ARTIFACT_PATH,
                "size_bytes": len(context_bytes),
                "sha256": digest(context_bytes),
            },
        }
        descriptor_bytes = json.dumps(
            descriptor, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
        (self.folder / verifier.REGISTERED_CONTEXT_DESCRIPTOR_PATH).write_bytes(
            descriptor_bytes
        )
        runtime = {
            "schema_id": "orange.recording.registered_scene_context_runtime",
            "schema_version": 1,
            "required": True,
            "status": "finalized",
            "capture_role": verifier.REGISTERED_CONTEXT_CAPTURE_ROLE,
            "artifact_relative_path": verifier.REGISTERED_CONTEXT_ARTIFACT_PATH,
            "descriptor_relative_path": verifier.REGISTERED_CONTEXT_DESCRIPTOR_PATH,
            "request_id": 1,
            "capture_latency_ns": 2,
            "failure_reason": "",
            "descriptor_receipt": {
                "relative_path": verifier.REGISTERED_CONTEXT_DESCRIPTOR_PATH,
                "size_bytes": len(descriptor_bytes),
                "sha256": digest(descriptor_bytes),
            },
            "artifact": descriptor["artifact"],
            "registration_authority_status": descriptor["capture_invariants"]["registration_authority_status"],
            "daily_registration_accepted": False,
            "source_frame": descriptor["source_frame"],
        }
        backend = {
            "mode": "fixed_roi_external_ipc",
            "media_policy": policy,
            "full_frame": {
                "status": "omitted_by_policy",
                "required": False,
                "continuous": False,
                "first_class": True,
            },
            "registered_scene_context": runtime,
            "spatial_roi_recording": session,
        }
        manifest["recording_backend"] = backend
        manifest["recording_outputs"] = {}
        manifest["camera_artifacts"] = {}
        camera_outputs = manifest["recording_outputs_v3"]["cameras"][serial]
        camera_outputs.pop("full")
        manifest["clips"][0]["recording_outputs"] = {}
        manifest["clips"][0]["artifacts"] = {
            "videos": {}, "metadata": {}, "keyframes": {}
        }
        manifest["clips"][0]["recording_outputs_v3"] = manifest["recording_outputs_v3"]
        snapshot["recording_backend"] = backend
        snapshot["session"]["recording_backend"] = backend
        snapshot["session"]["spatial_roi_media_policy"] = policy
        snapshot["session"]["registered_scene_context_capture_declaration"] = declaration
        snapshot["session"]["registered_scene_context"] = runtime
        snapshot["recording_outputs"] = {serial: {}}
        snapshot["encoders"][serial]["outputs"].pop("full")
        snapshot["recording_outputs_v3"] = manifest["recording_outputs_v3"]
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        return descriptor, runtime

    def rewrite_context_descriptor(self, mutate):
        descriptor_path = self.folder / verifier.REGISTERED_CONTEXT_DESCRIPTOR_PATH
        descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
        mutate(descriptor)
        descriptor_bytes = json.dumps(
            descriptor, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
        descriptor_path.write_bytes(descriptor_bytes)
        manifest_path = self.folder / "recording_session.json"
        snapshot_path = self.folder / "recording_snapshot.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        runtime = manifest["recording_backend"]["registered_scene_context"]
        runtime["descriptor_receipt"] = {
            "relative_path": verifier.REGISTERED_CONTEXT_DESCRIPTOR_PATH,
            "size_bytes": len(descriptor_bytes),
            "sha256": digest(descriptor_bytes),
        }
        manifest["recording_backend"]["registered_scene_context"] = runtime
        snapshot["session"]["recording_backend"]["registered_scene_context"] = runtime
        snapshot["session"]["registered_scene_context"] = runtime
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")

    def rewrite_coupled(self, mutate):
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        session = manifest["recording_backend"]["spatial_roi_recording"]
        descriptor = manifest["recording_outputs_v3"]["cameras"]["fixture-camera"]["spatial_roi"]["fixture-camera_spatial_roi_roi-1"]
        mutate(session, descriptor)
        self.sync_manifest_output_compatibility(manifest)
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["spatial_roi_recording"] = session
        snapshot["session"]["recording_backend"]["spatial_roi_recording"] = session
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")

    def test_accepts_complete_fixture_and_does_not_mutate(self):
        before = {path: path.read_bytes() for path in self.folder.rglob("*") if path.is_file()}
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "pass", result)
        after = {path: path.read_bytes() for path in self.folder.rglob("*") if path.is_file()}
        self.assertEqual(before, after)

    def test_accepts_gop25_profile_and_boundary_keyframe_count(self):
        _, session, _, _, _ = self.authenticated_inputs()
        for roi, receipt in zip(
            session["rois"], session["finalized_session_receipt"]["streams"]
        ):
            self.assertEqual(
                roi["encode_profile"]["profile_id"],
                "hevc_p1_low_latency_vbr_q20_gop25_v1",
            )
            self.assertEqual(
                receipt["counts"]["keyframes"],
                verifier._expected_keyframe_count(
                    receipt["ranges"]["frame_count"],
                    roi["encode_profile"]["gop_length"],
                ),
            )
        self.assertEqual(self.verify_without_probe()["status"], "pass")

    def test_rejects_gop25_keyframe_count_mutation(self):
        def mutate(session, descriptor):
            receipt = session["finalized_session_receipt"]["streams"][0]
            receipt["counts"]["keyframes"] += 1
            descriptor["details"]["finalized_receipt"] = receipt

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("count keyframes", result["errors"][0])

    def test_accepts_current_in_process_three_column_full_metadata(self):
        header = (self.folder / "Camfixture-camera_meta.csv").read_text(
            encoding="utf-8"
        ).splitlines()[0]
        self.assertEqual(header, "frame_id,timestamp,timestamp_sys")
        self.assertEqual(self.verify_without_probe()["status"], "pass")

    def test_accepts_external_combined_storage_admission(self):
        self.convert_fixture_to_external_combined()
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "pass", result)
        self.assertIn("combined_full_roi_storage_preflight", result["checks"])

    def test_accepts_fixed_roi_registered_context_without_full_frame(self):
        self.convert_fixture_to_registered_context()
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "pass", result)
        self.assertIn("full_frame_omitted_by_media_policy", result["checks"])
        self.assertNotIn("full_frame_first_class", result["checks"])

    def test_rejects_registered_context_descriptor_extra_key(self):
        self.convert_fixture_to_registered_context()
        self.rewrite_context_descriptor(lambda descriptor: descriptor.update({"unexpected": True}))
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("exactly keys", result["errors"][0])

    def test_rejects_registered_context_mono8_size_or_digest_substitution(self):
        descriptor, _ = self.convert_fixture_to_registered_context()
        del descriptor
        context_path = self.folder / verifier.REGISTERED_CONTEXT_ARTIFACT_PATH
        context_path.write_bytes(b"substituted")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("bytes do not match receipt", result["errors"][0])

    def test_rejects_registered_context_registration_status_substitution(self):
        self.convert_fixture_to_registered_context()
        self.rewrite_context_descriptor(
            lambda descriptor: descriptor["capture_invariants"].update(
                {"registration_authority_status": "accepted_for_experiment"}
            )
        )
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("registration status", result["errors"][0])

    def test_rejects_registered_context_recording_camera_substitution(self):
        self.convert_fixture_to_registered_context()
        self.rewrite_context_descriptor(
            lambda descriptor: descriptor["camera"].update(
                {"camera_serial": "other-camera"}
            )
        )
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("camera.camera_serial", result["errors"][0])

    def test_rejects_roi_receipt_camera_identity_substitution_under_registered_context_policy(self):
        self.convert_fixture_to_registered_context()
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        session = manifest["recording_backend"]["spatial_roi_recording"]
        session["finalized_session_receipt"]["streams"][0]["identity"]["camera_serial"] = "other-camera"
        manifest["recording_backend"]["spatial_roi_recording"] = session
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["spatial_roi_recording"] = session
        snapshot["session"]["recording_backend"]["spatial_roi_recording"] = session
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertTrue(
            "receipt" in result["errors"][0]
            or "identity" in result["errors"][0],
            result,
        )

    def test_rejects_substituted_combined_storage_roi_bound(self):
        evidence = self.convert_fixture_to_external_combined()
        evidence["spatial_roi_bounds"]["max_media_bytes_total"] -= 1
        (self.folder / "combined_storage_preflight.json").write_text(
            json.dumps(evidence, sort_keys=True), encoding="utf-8"
        )
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("spatial ROI bounds", result["errors"][0])

    def test_rejects_missing_combined_storage_preflight(self):
        self.convert_fixture_to_external_combined()
        (self.folder / "combined_storage_preflight.json").unlink()
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("securely open", result["errors"][0])

    def test_rejects_roi_video_replaced_during_decode_verification(self):
        calls = 0

        def replacing_probe(*args, **kwargs):
            nonlocal calls
            del args, kwargs
            calls += 1
            if calls == 2:
                video = (
                    self.folder
                    / verifier.ARTIFACT_ROOT
                    / "Camfixture-camera_spatial_roi_roi-1.mp4"
                )
                replacement = video.with_name(video.name + ".replacement")
                replacement.write_bytes(b"substituted-video-bytes")
                os.replace(replacement, video)
            return {"decoded_frames": 2}

        with mock.patch.object(verifier, "ffprobe_video", side_effect=replacing_probe):
            result = verifier.verify(str(self.folder), ffprobe="/fake/ffprobe")
        self.assertEqual(result["status"], "fail")
        self.assertTrue(
            any(
                "changed during decode verification" in error
                or "unlinked or replaced during verification" in error
                for error in result["errors"]
            ),
            result,
        )

    def test_rejects_duplicate_manifest_key(self):
        path = self.folder / "recording_session.json"
        path.write_text('{"schema_id":"orange.recording_session","schema_id":"substituted"}', encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("duplicate JSON object key", result["errors"][0])

    def test_rejects_receipt_path_scope_substitution(self):
        path = self.folder / "recording_session.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        session = manifest["recording_backend"]["spatial_roi_recording"]
        session["finalized_session_receipt"]["streams"][0]["artifacts"][0]["relative_path"] = verifier.ARTIFACT_ROOT + "/Camfixture-camera_spatial_roi_roi-1.mp4"
        descriptor = manifest["recording_outputs_v3"]["cameras"]["fixture-camera"]["spatial_roi"]["fixture-camera_spatial_roi_roi-1"]
        descriptor["details"]["finalized_receipt"] = session["finalized_session_receipt"]["streams"][0]
        self.sync_manifest_output_compatibility(manifest)
        path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["spatial_roi_recording"] = session
        snapshot["session"]["recording_backend"]["spatial_roi_recording"] = session
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("receipt path must be relative", result["errors"][0])

    def test_rejects_session_geometry_substitution(self):
        def mutate(session, descriptor):
            del descriptor
            session["rois"][0]["geometry"]["padding"]["right"] = 1

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("padding", result["errors"][0])

    def test_rejects_session_native_raster_substitution(self):
        def mutate(session, descriptor):
            del descriptor
            session["rois"][0]["geometry"]["native_raster"]["width"] = 15

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("native_raster", result["errors"][0])

    def test_rejects_session_coordinate_space_substitution(self):
        def mutate(session, descriptor):
            del descriptor
            session["rois"][0]["geometry"]["video_coordinate_space"] = "camera_native_full_frame_pixels"

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("coordinate space", result["errors"][0])

    def test_rejects_session_authority_substitution(self):
        def mutate(session, descriptor):
            del descriptor
            session["rois"][0]["geometry"]["layout"]["id"] = "substituted-layout"

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("geometry.layout", result["errors"][0])

    def test_rejects_coherent_geometry_substitution_against_contract(self):
        def mutate(session, descriptor):
            roi = session["rois"][0]
            roi["geometry"]["content_rect"]["x"] = 1
            roi["source_geometry"]["content_rect"]["x"] = 1
            descriptor["details"]["geometry"] = roi["geometry"]
            descriptor["details"]["geometry_identity"] = roi["geometry"]
            descriptor["details"]["source_geometry"] = roi["source_geometry"]

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("geometry_identity", result["errors"][0])

    def test_rejects_session_profile_substitution(self):
        def mutate(session, descriptor):
            del descriptor
            session["rois"][0]["encode_profile"]["codec"] = "h264"

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertTrue(any("codec/profile" in error or "supported policies" in error for error in result["errors"]))

    def test_rejects_session_gpu_mapping_substitution(self):
        def mutate(session, descriptor):
            del descriptor
            session["rois"][0]["source_gpu_id"] = 99

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("source GPU", result["errors"][0])

    def test_rejects_descriptor_raster_substitution(self):
        def mutate(session, descriptor):
            del session
            descriptor["width"] = 5

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("descriptor[0].width", result["errors"][0])

    def test_rejects_descriptor_profile_gpu_substitution(self):
        def mutate(session, descriptor):
            del session
            descriptor["details"]["assigned_gpu_id"] = 99

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("assigned_gpu_id", result["errors"][0])

    def test_rejects_descriptor_profile_substitution(self):
        def mutate(session, descriptor):
            del session
            descriptor["details"]["encode_profile"]["encoded_format"] = "yuv420p"

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("encode profile", result["errors"][0])

    def test_rejects_hard_linked_artifact(self):
        source = self.folder / verifier.ARTIFACT_ROOT / "Camfixture-camera_spatial_roi_roi-1.mp4"
        alias = self.folder / verifier.ARTIFACT_ROOT / "Camfixture-camera_spatial_roi_roi-1.mp4-hardlink"
        os.link(source, alias)
        manifest = json.loads((self.folder / "recording_session.json").read_text(encoding="utf-8"))
        stream = manifest["recording_backend"]["spatial_roi_recording"]["finalized_session_receipt"]["streams"][0]
        stream["artifacts"][0]["relative_path"] = "Camfixture-camera_spatial_roi_roi-1.mp4-hardlink"
        session = manifest["recording_backend"]["spatial_roi_recording"]
        session["finalized_session_receipt"]["streams"][0]["artifacts"][0] = stream["artifacts"][0]
        descriptor = manifest["recording_outputs_v3"]["cameras"]["fixture-camera"]["spatial_roi"]["fixture-camera_spatial_roi_roi-1"]
        descriptor["details"]["artifacts"]["video"] = verifier.ARTIFACT_ROOT + "/Camfixture-camera_spatial_roi_roi-1.mp4-hardlink"
        descriptor["video"] = descriptor["details"]["artifacts"]["video"]
        descriptor["details"]["finalized_receipt"] = session["finalized_session_receipt"]["streams"][0]
        self.sync_manifest_output_compatibility(manifest)
        (self.folder / "recording_session.json").write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot = json.loads((self.folder / "recording_snapshot.json").read_text(encoding="utf-8"))
        snapshot["session"]["spatial_roi_recording"] = session
        snapshot["session"]["recording_backend"]["spatial_roi_recording"] = session
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        (self.folder / "recording_snapshot.json").write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertTrue(any(
            "path coupling" in error
            or "hard link" in error
            or "authenticated recorder contract artifact" in error
            for error in result["errors"]
        ))

    def test_rejects_symlinked_artifact(self):
        source = self.folder / verifier.ARTIFACT_ROOT / "Camfixture-camera_spatial_roi_roi-1.mp4"
        alias = self.folder / verifier.ARTIFACT_ROOT / "Camfixture-camera_spatial_roi_roi-1.mp4-symlink"
        alias.symlink_to(source.name)
        manifest = json.loads((self.folder / "recording_session.json").read_text(encoding="utf-8"))
        session = manifest["recording_backend"]["spatial_roi_recording"]
        stream = session["finalized_session_receipt"]["streams"][0]
        stream["artifacts"][0]["relative_path"] = alias.name
        descriptor = manifest["recording_outputs_v3"]["cameras"]["fixture-camera"]["spatial_roi"]["fixture-camera_spatial_roi_roi-1"]
        descriptor["details"]["artifacts"]["video"] = verifier.ARTIFACT_ROOT + "/" + alias.name
        descriptor["video"] = descriptor["details"]["artifacts"]["video"]
        descriptor["details"]["finalized_receipt"] = stream
        self.sync_manifest_output_compatibility(manifest)
        (self.folder / "recording_session.json").write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["spatial_roi_recording"] = session
        snapshot["session"]["recording_backend"]["spatial_roi_recording"] = session
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertTrue(any(
            "securely open" in error
            or "authenticated recorder contract artifact" in error
            for error in result["errors"]
        ))

    def test_storage_preflight_is_required_and_checked(self):
        path = self.folder / "recording_session.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        session = manifest["recording_backend"]["spatial_roi_recording"]
        for event in ("ready_snapshot", "terminal"):
            preflight = session["recorder_process_status"][event]["payload"]["storage_preflight"]
            preflight["filesystem"]["available_blocks"] = 150_000
            preflight["filesystem"]["available_bytes"] = 150_000
        session["recorder_process_status"]["last"] = session[
            "recorder_process_status"
        ]["terminal"]
        path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        # Keep snapshot coupled to the same session payload.
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        coupled_session = json.loads((self.folder / "recording_session.json").read_text(encoding="utf-8"))["recording_backend"]["spatial_roi_recording"]
        snapshot["session"]["spatial_roi_recording"] = coupled_session
        snapshot["session"]["recording_backend"]["spatial_roi_recording"] = coupled_session
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "pass", result)
        self.assertIn("storage_preflight", result["checks"])

    def test_rejects_missing_required_storage_preflight(self):
        def mutate(session, descriptor):
            del descriptor
            session["recorder_process_status"] = {"state": "complete"}

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("ready_snapshot", result["errors"][0])

    def test_rejects_missing_authority_artifact(self):
        (self.folder / "contract.json").unlink()
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("securely open", result["errors"][0])

    def test_rejects_substituted_authority_bytes(self):
        (self.folder / "plan.json").write_bytes(b"{}")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("do not match receipt", result["errors"][0])

    def test_rejects_unbound_finalized_receipt_digest(self):
        def mutate(session, descriptor):
            receipt = session["finalized_session_receipt"]["streams"][0]
            receipt["finalized_receipt_digest"] = "sha256:" + "0" * 64
            descriptor["details"]["finalized_receipt"] = receipt

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("finalized receipt digest", result["errors"][0])

    def test_rejects_partitioned_roi_frame_coverage(self):
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        session = manifest["recording_backend"]["spatial_roi_recording"]
        receipt = session["finalized_session_receipt"]["streams"][1]
        receipt["ranges"]["recording_frame_id"] = {"first": 2, "last": 5}
        descriptor = manifest["recording_outputs_v3"]["cameras"]["fixture-camera"]["spatial_roi"]["fixture-camera_spatial_roi_roi-2"]
        descriptor["first_recording_frame_id"] = 2
        descriptor["last_recording_frame_id"] = 5
        descriptor["details"]["finalized_receipt"] = receipt
        self.sync_manifest_output_compatibility(manifest)
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["spatial_roi_recording"] = session
        snapshot["session"]["recording_backend"]["spatial_roi_recording"] = session
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertTrue(
            "source-frame coverage" in result["errors"][0]
            or "evidence manifest ranges" in result["errors"][0]
        )

    def test_rejects_full_frame_roi_coverage_disagreement(self):
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        full = manifest["recording_outputs_v3"]["cameras"]["fixture-camera"]["full"]
        full["frame_count"] = 3
        full["last_recording_frame_id"] = 3
        self.sync_manifest_output_compatibility(manifest)
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("exact frame coverage", result["errors"][0])

    def test_rejects_missing_full_frame_metadata(self):
        (self.folder / "Camfixture-camera_meta.csv").unlink()
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("securely open", result["errors"][0])

    def test_rejects_missing_full_frame_keyframes(self):
        (self.folder / "Camfixture-camera_keyframe.json").unlink()
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("securely open", result["errors"][0])

    def test_rejects_contradictory_full_frame_metadata(self):
        (self.folder / "Camfixture-camera_meta.csv").write_text(
            "frame_id,recording_frame_id\n1,1\n2,2\n3,3\n4,5\n",
            encoding="utf-8",
        )
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("aliases disagree", result["errors"][0])

    def test_rejects_noncanonical_full_frame_identity_lexemes(self):
        path = self.folder / "Camfixture-camera_meta.csv"
        for substituted in ("+1", " 1", '"1"'):
            path.write_text(
                "frame_id,timestamp,timestamp_sys\n"
                + substituted
                + ",1000,2000\n2,1001,2001\n3,1002,2002\n4,1003,2003\n",
                encoding="utf-8",
            )
            result = self.verify_without_probe()
            self.assertEqual(result["status"], "fail", substituted)
            self.assertIn("non-canonical frame identity", result["errors"][0])

    def test_rejects_failed_producer_completion(self):
        def mutate(session, descriptor):
            del descriptor
            session["producer_status"]["rejected"] = 1

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("producer status.rejected", result["errors"][0])

    def test_rejects_failed_recorder_process_completion(self):
        def mutate(session, descriptor):
            del descriptor
            session["recorder_process_status"]["terminal"]["failed"] = True

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("terminal snapshot", result["errors"][0])

    def test_rejects_failed_latest_recorder_process_event(self):
        def mutate(session, descriptor):
            del descriptor
            session["recorder_process_status"]["last"]["status"] = "failed"
            session["recorder_process_status"]["last"]["state"] = "failed"
            session["recorder_process_status"]["last"]["completed"] = False
            session["recorder_process_status"]["last"]["failed"] = True
            session["recorder_process_status"]["last"]["error"] = "late failure"

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("last/terminal", result["errors"][0])

    def test_rejects_receipt_artifacts_over_authenticated_budget(self):
        def mutate(session, descriptor):
            receipt = session["finalized_session_receipt"]["streams"][0]
            receipt["artifacts"][0]["size_bytes"] = 2_501
            descriptor["details"]["finalized_receipt"] = receipt

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("media artifacts exceed", result["errors"][0])

    def test_rejects_receipt_frames_over_authenticated_budget(self):
        with self.assertRaisesRegex(
            verifier.VerificationError, "frame count exceeds"
        ):
            verifier.validate_per_stream_receipt_budget(
                [],
                {"ranges": {"frame_count": 4}},
                {
                    "max_frames_per_stream": 3,
                    "max_media_bytes_per_stream": 1,
                    "max_evidence_bytes_per_stream": 1,
                },
            )

    def test_rejects_session_arena_group_disagreement(self):
        _, session, documents, _, _ = self.authenticated_inputs()
        session["rois"][0]["arena_group_id"] = "substituted-group"
        with self.assertRaisesRegex(verifier.VerificationError, "arena_group_id"):
            verifier.validate_resolved_plan_authority(
                documents["verified_plan"]["plan"],
                documents["normalized_config"],
                session,
                documents["recorder_contract"]["streams"],
            )

    def test_rejects_overlapping_rois_when_overlap_is_disabled(self):
        _, session, documents, _, _ = self.authenticated_inputs()
        config = documents["normalized_config"]
        plan_payload = documents["verified_plan"]["plan"]
        second_config = config["cameras"]["fixture-camera"]["rois"][1]
        second_resolved = plan_payload["resolved_cameras"]["fixture-camera"][
            "rois"
        ][1]
        second_config["content_rect"]["x"] = 3
        second_resolved["content_rect"]["x"] = 3
        session["rois"][1]["geometry"]["content_rect"]["x"] = 3
        with self.assertRaisesRegex(verifier.VerificationError, "overlap"):
            verifier.validate_resolved_plan_authority(
                plan_payload,
                config,
                session,
                documents["recorder_contract"]["streams"],
            )

    def test_rejects_substituted_admission_usage(self):
        _, session, documents, _, _ = self.authenticated_inputs()
        plan_payload = documents["verified_plan"]["plan"]
        plan_payload["admission_usage"]["encoded_pixel_rate"] += 1
        with self.assertRaisesRegex(
            verifier.VerificationError, "admission_usage.encoded_pixel_rate"
        ):
            verifier.validate_plan_admission_usage(
                documents["normalized_config"], plan_payload, session
            )

    def test_rejects_admission_ceiling_below_computed_usage(self):
        _, session, documents, _, _ = self.authenticated_inputs()
        config = documents["normalized_config"]
        config["admission"]["max_total_pixel_rate"] = 63
        with self.assertRaisesRegex(
            verifier.VerificationError, "max_total_pixel_rate"
        ):
            verifier.validate_plan_admission_usage(
                config, documents["verified_plan"]["plan"], session
            )

    def test_rejects_substituted_closed_contract_flag(self):
        _, session, documents, preflight, descriptors = self.authenticated_inputs()
        documents["recorder_contract"]["require_summary"] = False
        with self.assertRaisesRegex(verifier.VerificationError, "strict schema v5"):
            verifier.validate_authenticated_authorities(
                documents, session, preflight, descriptors
            )

    def test_rejects_substituted_contract_ipc_semantics(self):
        _, session, documents, preflight, descriptors = self.authenticated_inputs()
        documents["recorder_contract"]["ipc_v2"]["release"][
            "source_safe_after_release"
        ] = False
        with self.assertRaisesRegex(verifier.VerificationError, "IPC-v2"):
            verifier.validate_authenticated_authorities(
                documents, session, preflight, descriptors
            )

    def test_rejects_full_frame_semantic_substitution(self):
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["recording_outputs_v3"]["cameras"]["fixture-camera"]["full"][
            "coordinate_space"
        ] = "substituted_pixels"
        self.sync_manifest_output_compatibility(manifest)
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("first-class product", result["errors"][0])

    def test_rejects_recorder_wrapper_payload_contradiction(self):
        def mutate(session, descriptor):
            del descriptor
            terminal = session["recorder_process_status"]["terminal"]
            terminal["payload"]["status"] = "failed"
            session["recorder_process_status"]["last"] = terminal

        self.rewrite_coupled(mutate)
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("wrapper/payload.status", result["errors"][0])

    def test_rejects_substituted_resolved_camera_authority(self):
        manifest = json.loads(
            (self.folder / "recording_session.json").read_text(encoding="utf-8")
        )
        session = manifest["recording_backend"]["spatial_roi_recording"]
        plan = json.loads((self.folder / "plan.json").read_text(encoding="utf-8"))
        contract = json.loads(
            (self.folder / "contract.json").read_text(encoding="utf-8")
        )
        plan["plan"]["resolved_cameras"]["fixture-camera"]["rois"][0][
            "socket_path"
        ] = "/tmp/substituted.sock"
        with self.assertRaisesRegex(
            verifier.VerificationError, "resolved camera authority"
        ):
            verifier.validate_resolved_plan_authority(
                plan["plan"],
                plan["plan"]["configuration"],
                session,
                contract["streams"],
            )

    def test_rejects_contract_per_stream_limit_substitution(self):
        manifest = json.loads(
            (self.folder / "recording_session.json").read_text(encoding="utf-8")
        )
        session = manifest["recording_backend"]["spatial_roi_recording"]
        documents = {
            "normalized_config": json.loads(
                (self.folder / "config.json").read_text(encoding="utf-8")
            ),
            "verified_plan": json.loads(
                (self.folder / "plan.json").read_text(encoding="utf-8")
            ),
            "recorder_contract": json.loads(
                (self.folder / "contract.json").read_text(encoding="utf-8")
            ),
        }
        stream_id = session["stream_order"][0]
        documents["recorder_contract"]["streams"][stream_id][
            "max_media_bytes_per_stream"
        ] += 1
        preflight = session["recorder_process_status"]["terminal"]["payload"][
            "storage_preflight"
        ]
        descriptors = manifest["recording_outputs_v3"]["cameras"][
            "fixture-camera"
        ]["spatial_roi"]
        with self.assertRaisesRegex(
            verifier.VerificationError, "max_media_bytes_per_stream"
        ):
            verifier.validate_authenticated_authorities(
                documents, session, preflight, descriptors
            )

    def test_rejects_self_digesting_nonproduction_evidence_manifest(self):
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        session = manifest["recording_backend"]["spatial_roi_recording"]
        receipt = session["finalized_session_receipt"]["streams"][0]
        descriptor = manifest["recording_outputs_v3"]["cameras"][
            "fixture-camera"
        ]["spatial_roi"][session["stream_order"][0]]
        document = {"self_signed_fixture": True}
        supplied = verifier.canonical_json_sha256(document)
        document["finalized_receipt_digest"] = supplied
        data = json.dumps(
            document, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        relative = descriptor["details"]["artifacts"]["evidence_manifest"]
        (self.folder / relative).write_bytes(data)
        artifact = next(
            item
            for item in receipt["artifacts"]
            if item["kind"] == "evidence_manifest"
        )
        artifact["size_bytes"] = len(data)
        artifact["sha256"] = digest(data)
        receipt["finalized_receipt_digest"] = supplied
        descriptor["details"]["finalized_receipt"] = receipt
        self.sync_manifest_output_compatibility(manifest)
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["spatial_roi_recording"] = session
        snapshot["session"]["recording_backend"]["spatial_roi_recording"] = session
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("exactly keys", result["errors"][0])

    def test_rejects_full_frame_path_aliasing_roi_artifact(self):
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        camera = manifest["recording_outputs_v3"]["cameras"]["fixture-camera"]
        first_roi = camera["spatial_roi"][next(iter(camera["spatial_roi"]))]
        camera["full"]["video"] = first_roi["video"]
        self.sync_manifest_output_compatibility(manifest)
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        self.sync_snapshot_output_compatibility(snapshot, manifest)
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("aliases", result["errors"][0])

    def test_rejects_stale_snapshot_backend_duplicate(self):
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["recording_backend"]["spatial_roi_recording"][
            "status"
        ] = "pending"
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("duplicated spatial ROI", result["errors"][0])

    def test_rejects_stale_snapshot_full_frame_backend_projection(self):
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["recording_backend"]["mode"] = "external_ipc"
        snapshot["session"]["recording_backend"]["full_frame"] = {
            "status": "failed",
            "first_class": False,
        }
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("recording_backend.mode", result["errors"][0])

    def test_rejects_stale_snapshot_manifest_pointer(self):
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["session"]["recording_mode"] = "rolling_clips"
        snapshot["session"]["recording_session_manifest_path"] = (
            "/substituted/recording_session.json"
        )
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("mode/manifest pointer", result["errors"][0])

    def test_rejects_substituted_manifest_and_clip_root(self):
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["recording_folder"] = "/substituted/root"
        manifest["clips"][0]["recording_folder"] = "/substituted/root"
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("identity/root/mode", result["errors"][0])

    def test_rejects_stale_manifest_full_frame_compatibility_projection(self):
        manifest_path = self.folder / "recording_session.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["camera_artifacts"]["fixture-camera"]["video"] = "stale.mp4"
        manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("camera_artifacts", result["errors"][0])

    def test_rejects_stale_snapshot_full_frame_compatibility_projection(self):
        snapshot_path = self.folder / "recording_snapshot.json"
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
        snapshot["recording_outputs"]["fixture-camera"]["full"][
            "status"
        ] = "pending"
        snapshot_path.write_text(json.dumps(snapshot, sort_keys=True), encoding="utf-8")
        result = self.verify_without_probe()
        self.assertEqual(result["status"], "fail")
        self.assertIn("snapshot schema-2", result["errors"][0])


if __name__ == "__main__":
    unittest.main()
