#!/usr/bin/env python3
"""Read-only acceptance verifier for one completed spatial-ROI recording.

The verifier deliberately does not use pathlib.open/read_bytes for recording
artifacts.  Contract paths are walked from an opened recording-root directory
descriptor with O_NOFOLLOW at every component, and the bytes are hashed from
the retained descriptor.  No file in the recording folder is created,
replaced, or otherwise modified.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import os
import posixpath
import shutil
import stat
import subprocess
import sys
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple


ARTIFACT_KINDS = (
    "video",
    "metadata",
    "keyframes",
    "perf",
    "summary",
    "status",
    "video_sanity",
    "finalization",
    "recorder_log",
    "transport_sidecar",
    "evidence",
    "evidence_manifest",
)
FINALIZE_ARTIFACT_KINDS = ARTIFACT_KINDS[:10]
COUNT_KEYS = (
    "detach_successes",
    "dispatch_admitted",
    "dispatch_rejected",
    "ack_attempted",
    "ack_sent",
    "ack_accepted",
    "release_attempted",
    "release_sent",
    "encoded_frames",
    "failed_frames",
    "packet_count",
    "encoded_bytes",
    "keyframes",
    "ack_write_failures",
    "release_write_failures",
    "lifecycle_failures",
)
RECEIPT_KEYS = (
    "logical_stream_id",
    "identity",
    "counts",
    "ranges",
    "finalized_receipt_digest",
    "artifacts",
)
SESSION_KEYS = (
    "schema_id",
    "schema_version",
    "status",
    "recording_id",
    "session_id",
    "recording_identity_token",
    "producer_generation",
    "spatial_roi_plan_sha256",
    "product_kind",
    "stream_count",
    "stream_order",
    "identity",
    "camera",
    "camera_id",
    "camera_serial",
    "native_raster",
    "authorities",
    "gpu_mapping",
    "artifacts",
    "rois",
    "finalized_session_receipt",
    "recorder_process_status",
    "producer_status",
)
FULL_FRAME_KIND = "full"
ARTIFACT_ROOT = "external_spatial_roi_recorder"
STORAGE_PREFLIGHT_SCHEMA_ID = "orange.spatial_roi_recording.storage_preflight"
STORAGE_PREFLIGHT_SCHEMA_VERSION = 1
STORAGE_PREFLIGHT_POLICY_SCHEMA_ID = "orange.spatial_roi_recorder_storage_preflight_policy"
STORAGE_PREFLIGHT_POLICY_SCHEMA_VERSION = 1
MAX_JSON_BYTES = 16 * 1024 * 1024
MEDIA_POLICY_SCHEMA_ID = "orange.spatial_roi_recording.media_policy"
MEDIA_POLICY_SCHEMA_VERSION = 1
FIXED_ROIS_WITH_REGISTERED_CONTEXT = "fixed_rois_with_registered_context"
REGISTERED_CONTEXT_DESCRIPTOR_SCHEMA_ID = "orange.recording.registered_scene_context"
REGISTERED_CONTEXT_DESCRIPTOR_SCHEMA_VERSION = 1
REGISTERED_CONTEXT_CANONICALIZATION = "canonical_json_utf8_sort_keys_compact_v1"
REGISTERED_CONTEXT_CAPTURE_ROLE = "registered_scene_context"
REGISTERED_CONTEXT_PIXEL_FORMAT = "Mono8"
REGISTERED_CONTEXT_DESCRIPTOR_PATH = "registered_scene_context.json"
REGISTERED_CONTEXT_ARTIFACT_PATH = "registered_scene_context.mono8"
REGISTERED_CONTEXT_MAX_BYTES = 64 * 1024 * 1024
MEDIA_POLICY_KEYS = (
    "schema_id", "schema_version", "media_policy", "retained_products",
    "sink_backend",
)
REGISTERED_CONTEXT_RUNTIME_KEYS = (
    "schema_id", "schema_version", "required", "status", "capture_role",
    "artifact_relative_path", "descriptor_relative_path", "request_id",
    "capture_latency_ns", "failure_reason", "descriptor_receipt", "artifact",
    "registration_authority_status", "daily_registration_accepted", "source_frame",
)
REGISTERED_CONTEXT_DECLARATION_KEYS = (
    "schema_id", "schema_version", "registration_authority_status",
    "subject_presence", "dish_setup_complete", "nir_illumination_fixed",
    "camera_configuration_fixed", "rig_fixed",
)
EVIDENCE_MANIFEST_KEYS = (
    "schema_id",
    "schema_version",
    "canonicalization",
    "stream_kind",
    "binding",
    "evidence",
    "artifacts",
    "counts",
    "ranges",
    "terminal",
    "encoder_terminal",
    "finalize_request_sha256",
    "finalized_receipt_digest",
)
CONTRACT_KEYS = (
    "schema_id", "schema_version", "contract_scope", "strict", "backend",
    "mode", "supervise_processes", "require_summary", "require_status",
    "require_video_sanity", "require_protocol_hello",
    "require_frame_identity_proof", "require_gop_routing",
    "require_storage_preflight", "storage_preflight_policy",
    "preserve_shard_mp4s", "recording_id", "session_id",
    "recording_identity_token", "producer_generation",
    "spatial_roi_plan_sha256", "recording_root", "artifact_root",
    "source_cadence", "source_pixel_format", "stream_count", "stream_order",
    "ipc_v2", "aggregate_bounds", "recording_control", "rollover",
    "gpu_mapping", "streams",
)
CONTRACT_STREAM_KEYS = (
    "stream_id", "logical_stream_id", "stream_kind", "output_kind",
    "camera_id", "camera_serial", "env_key", "socket_path",
    "analytics_gpu_id", "recorder_gpu_id", "source_gpu_id",
    "assigned_gpu_id", "roi_id", "region_id", "arena_group_id", "arena_id",
    "recording_id", "session_id", "recording_identity_token",
    "producer_generation", "spatial_roi_plan_sha256", "frame_identity",
    "identity", "geometry_identity", "encode_profile", "encode_fps", "codec",
    "tuning", "rate_control_mode", "quality_value", "gop",
    "encode_queue_depth", "detach_pool_frames", "max_detach_pool_bytes",
    "max_queue_bytes", "writer_queue_max_packets", "writer_queue_max_bytes",
    "operation_timeout_ms", "max_frames_per_stream",
    "max_media_bytes_per_stream", "max_evidence_bytes_per_stream",
    "routing_policy", "expected_shard_gpu_ids", "recording_control", "rollover",
    "mp4", "metadata_csv", "keyframe_json", "perf_csv", "summary_json",
    "status_json", "video_sanity_json", "finalization_json", "recorder_log",
    "transport_sidecar", "evidence_jsonl", "evidence_manifest_json",
    "expected_artifacts",
)
CONTRACT_IPC_FEATURES = (
    "cuda_ipc", "packed_mono8", "ack_release", "terminal_error",
)
CONTRACT_WRITER_QUEUE_MAX_PACKETS = 512
CONTRACT_WRITER_QUEUE_MAX_BYTES = 128 * 1024 * 1024
CONTRACT_OPERATION_TIMEOUT_MS = 2000
CONTRACT_MAX_QUEUE_FRAMES = 4096


def _supported_encode_profiles(frame_rate: int) -> Tuple[Dict[str, Any], ...]:
    """Return the immutable ROI profiles accepted by session schema v3.

    ``frame_rate`` is the only profile field that is supplied by the camera
    authority rather than selected by policy.  Keeping the complete profile
    objects here gives all downstream projections one source for the direct
    rate-control, quality, and GOP fields.
    """
    return (
        {
            "profile_id": "hevc_p7_lossless_cqp0_gop1_v1",
            "codec": "hevc", "preset": "p7", "tuning": "lossless",
            "lossless": True, "rate_control_mode": "cqp",
            "quality_value": 0, "gop_length": 1,
            "aq": False, "temporal_aq": False,
            "lookahead": False, "lookahead_depth": 0,
            "frame_rate": frame_rate, "input_format": "mono8",
            "encoded_format": "nv12", "no_resize": True,
            "luma_preserved_exactly": True, "neutral_chroma_value": 128,
        },
        {
            "profile_id": "hevc_p1_low_latency_vbr_q20_gop1_v1",
            "codec": "hevc", "preset": "p1", "tuning": "ll",
            "lossless": False, "rate_control_mode": "vbr",
            "quality_value": 20, "gop_length": 1,
            "aq": False, "temporal_aq": False,
            "lookahead": False, "lookahead_depth": 0,
            "frame_rate": frame_rate, "input_format": "mono8",
            "encoded_format": "nv12", "no_resize": True,
            "luma_preserved_exactly": False, "neutral_chroma_value": 128,
        },
        {
            "profile_id": "hevc_p1_low_latency_vbr_q20_gop25_v1",
            "codec": "hevc", "preset": "p1", "tuning": "ll",
            "lossless": False, "rate_control_mode": "vbr",
            "quality_value": 20, "gop_length": 25,
            "aq": False, "temporal_aq": False,
            "lookahead": False, "lookahead_depth": 0,
            "frame_rate": frame_rate, "input_format": "mono8",
            "encoded_format": "nv12", "no_resize": True,
            "luma_preserved_exactly": False, "neutral_chroma_value": 128,
        },
    )


def _expected_keyframe_count(frame_count: int, gop_length: int) -> int:
    """Return the number of IDRs at zero-based GOP boundaries."""
    if frame_count <= 0:
        return 0
    if gop_length <= 0:
        raise VerificationError("encode profile GOP length must be positive")
    return 1 + (frame_count - 1) // gop_length


class VerificationError(Exception):
    """A fail-closed acceptance failure."""


class DuplicateKeyError(ValueError):
    pass


def _reject_duplicate_keys(pairs: List[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError("duplicate JSON object key: %s" % key)
        result[key] = value
    return result


def parse_json_bytes(data: bytes, label: str) -> Any:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise VerificationError("%s is not UTF-8 JSON: %s" % (label, exc)) from exc
    try:
        return json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except (DuplicateKeyError, json.JSONDecodeError, ValueError) as exc:
        raise VerificationError("%s is invalid JSON: %s" % (label, exc)) from exc


def canonical_json_sha256(value: Any) -> str:
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _exact_keys(value: Any, expected: Iterable[str]) -> bool:
    return isinstance(value, dict) and set(value) == set(expected)


def _required_keys(value: Any, expected: Iterable[str], label: str) -> None:
    if not _exact_keys(value, expected):
        actual = sorted(value) if isinstance(value, dict) else type(value).__name__
        raise VerificationError(
            "%s must have exactly keys %s (got %s)"
            % (label, ",".join(expected), actual)
        )


def _string(value: Any, label: str, nonempty: bool = True) -> str:
    if not isinstance(value, str) or (nonempty and not value):
        raise VerificationError("%s must be a %s string" % (label, "non-empty" if nonempty else ""))
    return value


def _integer(value: Any, label: str, positive: bool = False) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise VerificationError("%s must be a non-negative integer" % label)
    if positive and value == 0:
        raise VerificationError("%s must be positive" % label)
    return value


def _bool(value: Any, label: str) -> bool:
    if not isinstance(value, bool):
        raise VerificationError("%s must be boolean" % label)
    return value


def _canonical_sha(value: Any, label: str) -> str:
    value = _string(value, label)
    if len(value) != 71 or not value.startswith("sha256:") or any(
        char not in "0123456789abcdef" for char in value[7:]
    ):
        raise VerificationError("%s is not a canonical sha256 digest" % label)
    return value


def safe_relative_path(value: Any, label: str) -> str:
    value = _string(value, label)
    if (
        "\x00" in value
        or "\\" in value
        or ":" in value
        or value.startswith("/")
        or value.endswith("/")
        or "//" in value
        or posixpath.normpath(value) != value
    ):
        raise VerificationError("%s is not a safe relative path" % label)
    parts = value.split("/")
    if not parts or any(part in ("", ".", "..") for part in parts):
        raise VerificationError("%s is not a safe relative path" % label)
    return value


def _safe_identifier(value: Any, label: str) -> str:
    """Match the context descriptor's conservative flat identifier grammar."""
    value = _string(value, label)
    if not value[0].isalnum() or any(
        not (char.isalnum() or char in "_-. ") for char in value[1:]
    ):
        # Spaces are intentionally not accepted; keep the expression above
        # readable while rejecting them explicitly for parity with C++.
        raise VerificationError("%s is not a safe identifier" % label)
    if " " in value:
        raise VerificationError("%s is not a safe identifier" % label)
    return value


def validate_media_policy_object(value: Any, label: str = "media_policy") -> str:
    """Validate the closed v1 media-policy envelope and return its name."""
    _required_keys(value, MEDIA_POLICY_KEYS, label)
    if value["schema_id"] != MEDIA_POLICY_SCHEMA_ID or value["schema_version"] != MEDIA_POLICY_SCHEMA_VERSION:
        raise VerificationError("%s has an unexpected schema" % label)
    _string(value["media_policy"], label + ".media_policy")
    retained = value["retained_products"]
    _required_keys(retained, ("full_frame", "fixed_rois", "registered_context"), label + ".retained_products")
    for key in ("full_frame", "fixed_rois", "registered_context"):
        _bool(retained[key], "%s.retained_products.%s" % (label, key))
    expected = {
        "full_frame_only": {"full_frame": True, "fixed_rois": False, "registered_context": False},
        "full_frame_and_fixed_rois": {"full_frame": True, "fixed_rois": True, "registered_context": False},
        FIXED_ROIS_WITH_REGISTERED_CONTEXT: {"full_frame": False, "fixed_rois": True, "registered_context": True},
    }
    name = value["media_policy"]
    if name not in expected or retained != expected[name]:
        raise VerificationError("%s retained_products do not match media_policy" % label)
    sink = value["sink_backend"]
    if sink is not None:
        sink = _string(sink, label + ".sink_backend")
        if len(sink) > 128 or any(ord(char) < 0x20 or ord(char) == 0x7f for char in sink):
            raise VerificationError("%s.sink_backend contains unsafe text" % label)
    return name


def validate_registered_context_declaration(value: Any, label: str) -> Dict[str, Any]:
    _required_keys(value, REGISTERED_CONTEXT_DECLARATION_KEYS, label)
    if value["schema_id"] != "orange.recording.registered_scene_context.capture_declaration" or value["schema_version"] != 1:
        raise VerificationError("%s schema is invalid" % label)
    status = value["registration_authority_status"]
    if status not in ("accepted_for_experiment", "diagnostic_not_physical_acceptance"):
        raise VerificationError("%s registration_authority_status is invalid" % label)
    if value["subject_presence"] not in ("absent", "present", "unknown"):
        raise VerificationError("%s subject_presence is invalid" % label)
    for key in ("dish_setup_complete", "nir_illumination_fixed", "camera_configuration_fixed", "rig_fixed"):
        if value[key] is not True:
            raise VerificationError("%s.%s must be true" % (label, key))
    return value


def _validate_context_receipt(value: Any, label: str, allow_empty: bool = False) -> Dict[str, Any]:
    _required_keys(value, ("relative_path", "size_bytes", "sha256"), label)
    if allow_empty and value == {"relative_path": "", "size_bytes": 0, "sha256": ""}:
        return value
    path = safe_relative_path(value["relative_path"], label + ".relative_path")
    _integer(value["size_bytes"], label + ".size_bytes", True)
    _canonical_sha(value["sha256"], label + ".sha256")
    return {"relative_path": path, "size_bytes": value["size_bytes"], "sha256": value["sha256"]}


def validate_registered_context_runtime(runtime: Any, session: Dict[str, Any]) -> Dict[str, Any]:
    """Validate the compact runtime projection that links snapshots to the descriptor."""
    _required_keys(runtime, REGISTERED_CONTEXT_RUNTIME_KEYS, "registered_scene_context runtime")
    if (
        runtime["schema_id"] != "orange.recording.registered_scene_context_runtime"
        or runtime["schema_version"] != 1
        or runtime["required"] is not True
        or runtime["status"] != "finalized"
        or runtime["capture_role"] != REGISTERED_CONTEXT_CAPTURE_ROLE
        or runtime["artifact_relative_path"] != REGISTERED_CONTEXT_ARTIFACT_PATH
        or runtime["descriptor_relative_path"] != REGISTERED_CONTEXT_DESCRIPTOR_PATH
        or runtime["failure_reason"] != ""
    ):
        raise VerificationError("registered scene context runtime is not finalized/canonical")
    _integer(runtime["request_id"], "registered scene context runtime.request_id", True)
    _integer(runtime["capture_latency_ns"], "registered scene context runtime.capture_latency_ns")
    descriptor_receipt = _validate_context_receipt(runtime["descriptor_receipt"], "registered scene context runtime.descriptor_receipt")
    artifact_receipt = _validate_context_receipt(runtime["artifact"], "registered scene context runtime.artifact")
    if descriptor_receipt["relative_path"] != REGISTERED_CONTEXT_DESCRIPTOR_PATH or artifact_receipt["relative_path"] != REGISTERED_CONTEXT_ARTIFACT_PATH:
        raise VerificationError("registered scene context runtime receipt paths are substituted")
    if runtime["registration_authority_status"] not in ("accepted_for_experiment", "diagnostic_not_physical_acceptance"):
        raise VerificationError("registered scene context runtime registration status is invalid")
    if runtime["daily_registration_accepted"] is not (runtime["registration_authority_status"] == "accepted_for_experiment"):
        raise VerificationError("registered scene context runtime registration status is inconsistent")
    _required_keys(runtime["source_frame"], ("source_frame_id", "local_frame_id", "camera_frame_id", "recording_frame_id", "camera_timestamp_ns", "timestamp_sys_ns"), "registered scene context runtime.source_frame")
    for key in ("source_frame_id", "local_frame_id", "camera_frame_id", "camera_timestamp_ns", "timestamp_sys_ns"):
        _integer(runtime["source_frame"][key], "registered scene context runtime.source_frame.%s" % key, True)
    _integer(runtime["source_frame"]["recording_frame_id"], "registered scene context runtime.source_frame.recording_frame_id")
    return runtime


def validate_registered_context_descriptor(
    descriptor: Any,
    session: Dict[str, Any],
    declaration: Dict[str, Any],
    runtime: Dict[str, Any],
) -> Dict[str, Any]:
    """Validate the exact v1 descriptor schema and bind it to session authorities."""
    descriptor_keys = (
        "schema_id", "schema_version", "canonicalization", "capture_role", "status",
        "failure_reason", "recording", "camera", "source_frame", "native_raster",
        "coordinate_space", "pixel_format", "geometry_binding", "capture_invariants", "artifact",
    )
    _required_keys(descriptor, descriptor_keys, "registered_scene_context")
    if (
        descriptor["schema_id"] != REGISTERED_CONTEXT_DESCRIPTOR_SCHEMA_ID
        or descriptor["schema_version"] != REGISTERED_CONTEXT_DESCRIPTOR_SCHEMA_VERSION
        or descriptor["canonicalization"] != REGISTERED_CONTEXT_CANONICALIZATION
        or descriptor["capture_role"] != REGISTERED_CONTEXT_CAPTURE_ROLE
        or descriptor["status"] != "complete"
        or descriptor["failure_reason"] != ""
    ):
        raise VerificationError("registered scene context descriptor schema/status is invalid")
    recording = descriptor["recording"]
    _required_keys(recording, ("recording_id", "session_id", "recording_identity_token", "producer_generation"), "registered_scene_context.recording")
    for key, expected in (("recording_id", session["recording_id"]), ("session_id", session["session_id"]), ("recording_identity_token", session["recording_identity_token"]), ("producer_generation", session["producer_generation"])):
        if key == "recording_identity_token":
            _canonical_sha(recording[key], "registered_scene_context.recording.%s" % key)
        else:
            _string(recording[key], "registered_scene_context.recording.%s" % key)
        _same(recording[key], expected, "registered scene context recording.%s" % key)
    camera = descriptor["camera"]
    _required_keys(camera, ("camera_id", "camera_serial", "source_camera_stream_id", "stream_epoch_id", "camera_configuration_sha256"), "registered_scene_context.camera")
    _integer(camera["camera_id"], "registered scene context camera_id")
    _safe_identifier(camera["camera_serial"], "registered scene context camera_serial")
    _safe_identifier(camera["source_camera_stream_id"], "registered scene context source camera stream")
    _safe_identifier(camera["stream_epoch_id"], "registered scene context stream epoch")
    _canonical_sha(camera["camera_configuration_sha256"], "registered scene context camera configuration")
    for key, expected in (("camera_id", session["camera_id"]), ("camera_serial", session["camera_serial"]), ("source_camera_stream_id", "camera_" + session["camera_serial"]), ("stream_epoch_id", session["producer_generation"])):
        _same(camera[key], expected, "registered scene context camera.%s" % key)
    source = descriptor["source_frame"]
    _required_keys(source, ("source_frame_id", "local_frame_id", "camera_frame_id", "recording_frame_id", "camera_timestamp_ns", "timestamp_sys_ns"), "registered_scene_context.source_frame")
    for key in ("source_frame_id", "local_frame_id", "camera_frame_id", "camera_timestamp_ns", "timestamp_sys_ns"):
        _integer(source[key], "registered scene context source_frame.%s" % key, True)
    _integer(source["recording_frame_id"], "registered scene context source_frame.recording_frame_id")
    _same(source, runtime["source_frame"], "registered scene context source frame/runtime")
    raster = descriptor["native_raster"]
    _required_keys(raster, ("width", "height", "stride_bytes"), "registered_scene_context.native_raster")
    for key in ("width", "height", "stride_bytes"):
        _integer(raster[key], "registered scene context native_raster.%s" % key, True)
    _same({"width": raster["width"], "height": raster["height"]}, session["native_raster"], "registered scene context native raster")
    if raster["stride_bytes"] < raster["width"] or raster["stride_bytes"] * raster["height"] > REGISTERED_CONTEXT_MAX_BYTES:
        raise VerificationError("registered scene context raster exceeds Mono8 bounds")
    if descriptor["coordinate_space"] != "camera_native_px" or descriptor["pixel_format"] != REGISTERED_CONTEXT_PIXEL_FORMAT:
        raise VerificationError("registered scene context coordinate/pixel contract is invalid")
    geometry = descriptor["geometry_binding"]
    _required_keys(geometry, ("layout", "materialization", "registration"), "registered_scene_context.geometry_binding")
    for key in ("layout", "materialization", "registration"):
        _required_keys(geometry[key], ("id", "sha256"), "registered_scene_context.geometry_binding.%s" % key)
        _safe_identifier(geometry[key]["id"], "registered scene context %s id" % key)
        _canonical_sha(geometry[key]["sha256"], "registered scene context %s sha256" % key)
        _same(geometry[key], session["authorities"][key], "registered scene context geometry.%s" % key)
    invariants = descriptor["capture_invariants"]
    _required_keys(invariants, ("daily_registration_accepted", "registration_authority_status", "dish_setup_complete", "subject_presence", "nir_illumination_fixed", "camera_configuration_fixed", "rig_fixed"), "registered_scene_context.capture_invariants")
    if invariants["registration_authority_status"] not in ("accepted_for_experiment", "diagnostic_not_physical_acceptance"):
        raise VerificationError("registered scene context registration status is invalid")
    if invariants["daily_registration_accepted"] is not (invariants["registration_authority_status"] == "accepted_for_experiment"):
        raise VerificationError("registered scene context registration status is inconsistent")
    if invariants["subject_presence"] not in ("absent", "present", "unknown"):
        raise VerificationError("registered scene context subject_presence is invalid")
    for key in ("dish_setup_complete", "nir_illumination_fixed", "camera_configuration_fixed", "rig_fixed"):
        if invariants[key] is not True:
            raise VerificationError("registered scene context invariant %s is false" % key)
    for key in REGISTERED_CONTEXT_DECLARATION_KEYS[2:]:
        _same(invariants[key], declaration[key], "registered scene context declaration.%s" % key)
    _same(invariants["daily_registration_accepted"], invariants["registration_authority_status"] == "accepted_for_experiment", "registered scene context daily registration")
    _same(invariants["registration_authority_status"], runtime["registration_authority_status"], "registered scene context runtime registration status")
    _same(invariants["daily_registration_accepted"], runtime["daily_registration_accepted"], "registered scene context runtime registration acceptance")
    artifact = _validate_context_receipt(descriptor["artifact"], "registered_scene_context.artifact")
    if artifact["relative_path"] != REGISTERED_CONTEXT_ARTIFACT_PATH or artifact != runtime["artifact"]:
        raise VerificationError("registered scene context artifact receipt/runtime is substituted")
    expected_bytes = raster["stride_bytes"] * raster["height"]
    if artifact["size_bytes"] != expected_bytes:
        raise VerificationError("registered scene context artifact size does not match native raster")
    return descriptor


def secure_verify_registered_context(
    root_fd: int,
    backend: Dict[str, Any],
    snapshot_session: Dict[str, Any],
    session: Dict[str, Any],
    seen_paths: Set[str],
    seen_inodes: Set[Tuple[int, int]],
) -> Dict[str, Any]:
    runtime = validate_registered_context_runtime(backend.get("registered_scene_context"), session)
    if snapshot_session.get("registered_scene_context") != runtime:
        raise VerificationError("recording_snapshot registered context runtime differs from manifest")
    declaration = validate_registered_context_declaration(
        snapshot_session.get("registered_scene_context_capture_declaration"),
        "recording_snapshot registered_scene_context_capture_declaration",
    )
    descriptor_path = REGISTERED_CONTEXT_DESCRIPTOR_PATH
    descriptor_receipt = runtime["descriptor_receipt"]
    if descriptor_path in seen_paths:
        raise VerificationError("registered scene context descriptor path aliases another authority")
    descriptor_fd = open_relative(root_fd, descriptor_path)
    try:
        before = os.fstat(descriptor_fd)
        inode = (before.st_dev, before.st_ino)
        if inode in seen_inodes:
            raise VerificationError("registered scene context descriptor aliases another authority inode")
        hash_open_fd(descriptor_fd, descriptor_path, descriptor_receipt["size_bytes"], descriptor_receipt["sha256"])
        descriptor = read_open_json_fd(descriptor_fd, descriptor_path)
    finally:
        _close_quietly(descriptor_fd)
    seen_paths.add(descriptor_path)
    seen_inodes.add(inode)
    validate_registered_context_descriptor(descriptor, session, declaration, runtime)
    artifact = descriptor["artifact"]
    artifact_path = artifact["relative_path"]
    if artifact_path in seen_paths:
        raise VerificationError("registered scene context artifact path aliases another authority")
    image_fd = open_relative(root_fd, artifact_path)
    try:
        image_stat = os.fstat(image_fd)
        image_inode = (image_stat.st_dev, image_stat.st_ino)
        if image_inode in seen_inodes:
            raise VerificationError("registered scene context artifact aliases another authority inode")
        hash_open_fd(image_fd, artifact_path, artifact["size_bytes"], artifact["sha256"])
    finally:
        _close_quietly(image_fd)
    seen_paths.add(artifact_path)
    seen_inodes.add(image_inode)
    return descriptor


def _close_quietly(fd: Optional[int]) -> None:
    if fd is not None:
        try:
            os.close(fd)
        except OSError:
            pass


def open_root(folder: str) -> int:
    try:
        fd = os.open(
            folder,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
    except OSError as exc:
        raise VerificationError("recording folder cannot be securely opened: %s" % exc) from exc
    try:
        mode = os.fstat(fd)
        if not stat.S_ISDIR(mode.st_mode):
            raise VerificationError("recording folder is not a directory")
        return fd
    except Exception:
        _close_quietly(fd)
        raise


def open_relative(root_fd: int, relative: str, want_directory: bool = False) -> int:
    """Open a path below root_fd, applying O_NOFOLLOW to every component."""
    safe_relative_path(relative, "contract path")
    current: Optional[int] = os.dup(root_fd)
    try:
        parts = relative.split("/")
        for index, component in enumerate(parts):
            final = index == len(parts) - 1
            flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
            if not final or want_directory:
                flags |= os.O_DIRECTORY
            try:
                child = os.open(component, flags, dir_fd=current)
            except OSError as exc:
                raise VerificationError(
                    "cannot securely open recording component %s: %s" % (relative, exc)
                ) from exc
            _close_quietly(current)
            current = child
        assert current is not None
        return current
    except Exception:
        _close_quietly(current)
        raise


def secure_json(root_fd: int, relative: str) -> Any:
    fd = open_relative(root_fd, relative)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1:
            raise VerificationError("%s is not a unique regular JSON file" % relative)
        if st.st_size <= 0 or st.st_size > MAX_JSON_BYTES:
            raise VerificationError(
                "%s JSON size is outside the 1..%d byte bound"
                % (relative, MAX_JSON_BYTES)
            )
        chunks: List[bytes] = []
        offset = 0
        while offset < st.st_size:
            chunk = os.pread(fd, min(1024 * 1024, st.st_size - offset), offset)
            if not chunk:
                raise VerificationError("%s changed or could not be read" % relative)
            chunks.append(chunk)
            offset += len(chunk)
        after = os.fstat(fd)
        if (st.st_dev, st.st_ino, st.st_size) != (after.st_dev, after.st_ino, after.st_size):
            raise VerificationError("%s changed during read" % relative)
        return parse_json_bytes(b"".join(chunks), relative)
    finally:
        _close_quietly(fd)


def hash_open_fd(fd: int, relative: str, expected_size: int, expected_sha: str) -> Dict[str, Any]:
    before = os.fstat(fd)
    if not stat.S_ISREG(before.st_mode):
        raise VerificationError("%s is not a regular file" % relative)
    if before.st_nlink != 1:
        if before.st_nlink == 0:
            raise VerificationError(
                "%s was unlinked or replaced during verification" % relative
            )
        raise VerificationError("%s is a hard link (nlink=%d)" % (relative, before.st_nlink))
    if before.st_size <= 0:
        raise VerificationError("%s is empty" % relative)
    digest = hashlib.sha256()
    offset = 0
    while offset < before.st_size:
        chunk = os.pread(fd, min(1024 * 1024, before.st_size - offset), offset)
        if not chunk:
            raise VerificationError("%s changed or could not be read" % relative)
        digest.update(chunk)
        offset += len(chunk)
    after = os.fstat(fd)
    if (before.st_dev, before.st_ino, before.st_size) != (
        after.st_dev,
        after.st_ino,
        after.st_size,
    ):
        raise VerificationError("%s changed during hashing" % relative)
    actual_sha = "sha256:" + digest.hexdigest()
    if before.st_size != expected_size or actual_sha != expected_sha:
        raise VerificationError(
            "%s bytes do not match receipt (size %d/%d sha %s/%s)"
            % (relative, before.st_size, expected_size, actual_sha, expected_sha)
        )
    return {"size_bytes": before.st_size, "sha256": actual_sha, "inode": [before.st_dev, before.st_ino]}


def read_open_json_fd(fd: int, relative: str) -> Any:
    before = os.fstat(fd)
    if (
        not stat.S_ISREG(before.st_mode)
        or before.st_nlink != 1
        or before.st_size <= 0
        or before.st_size > MAX_JSON_BYTES
    ):
        raise VerificationError(
            "%s is not a unique bounded non-empty JSON file" % relative
        )
    chunks: List[bytes] = []
    offset = 0
    while offset < before.st_size:
        chunk = os.pread(fd, min(1024 * 1024, before.st_size - offset), offset)
        if not chunk:
            raise VerificationError("%s changed or could not be read" % relative)
        chunks.append(chunk)
        offset += len(chunk)
    after = os.fstat(fd)
    if (before.st_dev, before.st_ino, before.st_size) != (
        after.st_dev,
        after.st_ino,
        after.st_size,
    ):
        raise VerificationError("%s changed during JSON read" % relative)
    return parse_json_bytes(b"".join(chunks), relative)


def read_stat_secure(root_fd: int, relative: str) -> Tuple[int, os.stat_result]:
    fd = open_relative(root_fd, relative)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            raise VerificationError("%s is not a regular file" % relative)
        if st.st_nlink != 1:
            raise VerificationError("%s is a hard link (nlink=%d)" % (relative, st.st_nlink))
        return fd, st
    except Exception:
        _close_quietly(fd)
        raise


def _same(a: Any, b: Any, label: str) -> None:
    if a != b:
        raise VerificationError("%s does not match its coupled authority" % label)


def _identity_from_session(session: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "recording_id": session["recording_id"],
        "session_id": session["session_id"],
        "recording_identity_token": session["recording_identity_token"],
        "producer_generation": session["producer_generation"],
        "spatial_roi_plan_sha256": session["spatial_roi_plan_sha256"],
    }


def validate_session_shape(session: Any) -> Dict[str, Any]:
    _required_keys(session, SESSION_KEYS, "spatial_roi_recording")
    if session["schema_id"] != "orange.spatial_roi_recording.session_snapshot" or session["schema_version"] != 3:
        raise VerificationError("spatial_roi_recording is not schema v3")
    if session["status"] != "complete" or session["product_kind"] != "fixed_region":
        raise VerificationError("spatial_roi_recording must be complete fixed_region")
    _string(session["recording_id"], "session.recording_id")
    _same(session["session_id"], session["recording_id"], "session.session_id")
    _canonical_sha(session["recording_identity_token"], "session.recording_identity_token")
    token_subject = {
        "canonicalization": "canonical_json_utf8_sort_keys_compact_v1",
        "recording_id": session["recording_id"],
        "schema_id": "orange.shaman_v2.recording_identity",
        "schema_version": 1,
        "scope": "recording_session",
    }
    _same(
        session["recording_identity_token"],
        canonical_json_sha256(token_subject),
        "session recording identity token",
    )
    _string(session["producer_generation"], "session.producer_generation")
    _canonical_sha(session["spatial_roi_plan_sha256"], "session.spatial_roi_plan_sha256")
    if session["stream_count"] != 4 or not isinstance(session["stream_order"], list) or len(session["stream_order"]) != 4:
        raise VerificationError("spatial_roi_recording must contain exactly four streams")
    if len(set(session["stream_order"])) != 4 or any(not isinstance(v, str) or not v for v in session["stream_order"]):
        raise VerificationError("spatial_roi_recording stream order is not unique")
    expected_identity = _identity_from_session(session)
    _required_keys(session["identity"], expected_identity.keys(), "spatial_roi_recording.identity")
    _same(session["identity"], expected_identity, "spatial_roi_recording.identity")
    camera = session["camera"]
    _required_keys(camera, ("camera_id", "camera_serial", "native_raster"), "session.camera")
    _integer(camera["camera_id"], "session.camera.camera_id")
    _string(camera["camera_serial"], "session.camera.camera_serial")
    _required_keys(session["native_raster"], ("width", "height"), "session.native_raster")
    _integer(session["native_raster"]["width"], "session.native_raster.width", True)
    _integer(session["native_raster"]["height"], "session.native_raster.height", True)
    _same(camera["camera_id"], session["camera_id"], "session camera_id")
    _same(camera["camera_serial"], session["camera_serial"], "session camera_serial")
    _same(camera["native_raster"], session["native_raster"], "session native_raster")
    _required_keys(session["authorities"], ("layout", "materialization", "registration"), "session.authorities")
    for name, authority in session["authorities"].items():
        _required_keys(authority, ("id", "sha256"), "session.authorities.%s" % name)
        _string(authority["id"], "session.authorities.%s.id" % name)
        _canonical_sha(authority["sha256"], "session.authorities.%s.sha256" % name)
    mapping = session["gpu_mapping"]
    _required_keys(mapping, ("analytics_gpu_by_camera_serial", "recorder_gpu_by_logical_stream_id"), "session.gpu_mapping")
    analytics = mapping["analytics_gpu_by_camera_serial"]
    if not isinstance(analytics, dict) or set(analytics) != {session["camera_serial"]}:
        raise VerificationError("session analytics GPU mapping is not one-camera")
    _integer(analytics[session["camera_serial"]], "session analytics GPU")
    recorders = mapping["recorder_gpu_by_logical_stream_id"]
    if not isinstance(recorders, dict) or set(recorders) != set(session["stream_order"]):
        raise VerificationError("session recorder GPU mapping is not exactly plan keyed")
    for stream_id in session["stream_order"]:
        _integer(recorders[stream_id], "session recorder GPU %s" % stream_id)
    artifacts = session["artifacts"]
    _required_keys(artifacts, ("normalized_config", "verified_plan", "recorder_contract"), "session.artifacts")
    seen: Set[str] = set()
    for name, ref in artifacts.items():
        _required_keys(ref, ("relative_path", "size_bytes", "sha256"), "session.artifacts.%s" % name)
        path = safe_relative_path(ref["relative_path"], "session.artifacts.%s.relative_path" % name)
        _integer(ref["size_bytes"], "session.artifacts.%s.size_bytes" % name)
        _canonical_sha(ref["sha256"], "session.artifacts.%s.sha256" % name)
        if path in seen:
            raise VerificationError("session authority artifact paths collide")
        seen.add(path)
    rois = session["rois"]
    if not isinstance(rois, list) or len(rois) != 4:
        raise VerificationError("spatial_roi_recording must contain four ordered ROI descriptors")
    roi_ids: Set[str] = set()
    region_ids: Set[str] = set()
    for index, roi in enumerate(rois):
        _required_keys(
            roi,
            (
                "stream_id", "logical_stream_id", "roi_id", "region_id", "arena_group_id",
                "arena_id", "geometry", "source_geometry", "encoded_geometry", "encode_profile",
                "encode_fps", "codec", "tuning", "analytics_gpu_id", "source_gpu_id",
                "recorder_gpu_id", "assigned_gpu_id", "expected_shard_gpu_ids",
            ),
            "session.rois[%d]" % index,
        )
        stream_id = session["stream_order"][index]
        _same(roi["stream_id"], stream_id, "session ROI stream order")
        _same(roi["logical_stream_id"], stream_id, "session ROI logical stream order")
        _string(roi["roi_id"], "session ROI id")
        _string(roi["region_id"], "session ROI region id")
        _string(roi["arena_group_id"], "session ROI arena group")
        if roi["roi_id"] in roi_ids or roi["region_id"] in region_ids:
            raise VerificationError("session ROI or region identities are duplicated")
        roi_ids.add(roi["roi_id"])
        region_ids.add(roi["region_id"])
        if roi["arena_id"] is not None:
            _string(roi["arena_id"], "session ROI arena_id")
        for key in ("encode_fps", "analytics_gpu_id", "source_gpu_id", "recorder_gpu_id", "assigned_gpu_id"):
            _integer(roi[key], "session ROI %s" % key, key == "encode_fps")
        if not isinstance(roi["expected_shard_gpu_ids"], list) or len(roi["expected_shard_gpu_ids"]) != 1:
            raise VerificationError("session ROI shard mapping is not exactly one GPU")
        _integer(roi["expected_shard_gpu_ids"][0], "session ROI shard GPU")
        _validate_roi_geometry(
            roi,
            "session.rois[%d]" % index,
            session["authorities"],
            session["native_raster"],
        )
        profile = roi["encode_profile"]
        _required_keys(profile, ("profile_id", "codec", "preset", "tuning", "lossless", "rate_control_mode", "quality_value", "gop_length", "aq", "temporal_aq", "lookahead", "lookahead_depth", "frame_rate", "input_format", "encoded_format", "no_resize", "luma_preserved_exactly", "neutral_chroma_value"), "session ROI encode_profile")
        for key in ("profile_id", "codec", "preset", "tuning", "rate_control_mode", "input_format", "encoded_format"):
            _string(profile[key], "session ROI profile.%s" % key)
        for key in ("lossless", "no_resize", "luma_preserved_exactly", "aq", "temporal_aq", "lookahead"):
            _bool(profile[key], "session ROI profile.%s" % key)
        for key in ("quality_value", "gop_length", "lookahead_depth", "frame_rate", "neutral_chroma_value"):
            _integer(profile[key], "session ROI profile.%s" % key)
        _same(roi["codec"], profile["codec"], "session ROI codec/profile")
        _same(roi["tuning"], profile["tuning"], "session ROI tuning/profile")
        _same(roi["encode_fps"], profile["frame_rate"], "session ROI frame-rate/profile")
        expected_analytics_gpu = analytics[session["camera_serial"]]
        _same(roi["analytics_gpu_id"], expected_analytics_gpu, "session ROI analytics GPU")
        _same(roi["source_gpu_id"], roi["analytics_gpu_id"], "session ROI source GPU")
        _same(roi["recorder_gpu_id"], recorders[stream_id], "session ROI recorder GPU mapping")
        _same(roi["assigned_gpu_id"], roi["recorder_gpu_id"], "session ROI assigned GPU")
        _same(roi["expected_shard_gpu_ids"], [roi["recorder_gpu_id"]], "session ROI shard GPU mapping")
        supported_profiles = _supported_encode_profiles(roi["encode_fps"])
        if profile not in supported_profiles:
            raise VerificationError("session ROI encode profile is not one of the exact supported policies")
    if not isinstance(session["finalized_session_receipt"], dict):
        raise VerificationError("complete spatial_roi_recording requires a receipt object")
    return session


def validate_storage_preflight(preflight: Any, label: str) -> None:
    """Validate the recorder's required closed storage-preflight result."""
    _required_keys(
        preflight,
        ("schema_id", "schema_version", "checked", "passed", "status", "error", "policy", "artifact_root", "filesystem", "budgets"),
        label,
    )
    if preflight["schema_id"] != STORAGE_PREFLIGHT_SCHEMA_ID or preflight["schema_version"] != STORAGE_PREFLIGHT_SCHEMA_VERSION:
        raise VerificationError("%s has an unexpected storage-preflight schema" % label)
    if preflight["checked"] is not True or preflight["passed"] is not True or preflight["status"] != "passed" or preflight["error"] != "":
        raise VerificationError("%s is not a checked/passed preflight" % label)
    policy = preflight["policy"]
    _required_keys(policy, ("schema_id", "schema_version", "required", "reserved_free_bytes"), label + ".policy")
    if policy["schema_id"] != STORAGE_PREFLIGHT_POLICY_SCHEMA_ID or policy["schema_version"] != STORAGE_PREFLIGHT_POLICY_SCHEMA_VERSION:
        raise VerificationError("%s.policy has an unexpected storage-preflight policy schema" % label)
    if policy["required"] is not True:
        raise VerificationError("%s.policy.required is false" % label)
    _integer(policy["reserved_free_bytes"], label + ".policy.reserved_free_bytes", True)
    for name in ("artifact_root", "filesystem"):
        if not isinstance(preflight[name], dict):
            raise VerificationError("%s.%s is not an object" % (label, name))
    _required_keys(preflight["artifact_root"], ("device", "inode"), label + ".artifact_root")
    _integer(preflight["artifact_root"]["device"], label + ".artifact_root.device")
    _integer(preflight["artifact_root"]["inode"], label + ".artifact_root.inode", True)
    _required_keys(preflight["filesystem"], ("block_size_bytes", "total_blocks", "available_blocks", "capacity_bytes", "available_bytes"), label + ".filesystem")
    for key in ("block_size_bytes", "total_blocks", "available_blocks", "capacity_bytes", "available_bytes"):
        _integer(preflight["filesystem"][key], "%s.filesystem.%s" % (label, key))
    filesystem = preflight["filesystem"]
    if filesystem["block_size_bytes"] <= 0 or filesystem["available_blocks"] > filesystem["total_blocks"]:
        raise VerificationError("%s filesystem block evidence is invalid" % label)
    if filesystem["capacity_bytes"] != filesystem["block_size_bytes"] * filesystem["total_blocks"]:
        raise VerificationError("%s filesystem capacity arithmetic is inconsistent" % label)
    if filesystem["available_bytes"] != filesystem["block_size_bytes"] * filesystem["available_blocks"]:
        raise VerificationError("%s filesystem available-byte arithmetic is inconsistent" % label)
    budgets = preflight["budgets"]
    _required_keys(budgets, ("max_media_bytes_total", "max_evidence_bytes_total", "reserved_free_bytes", "required_bytes"), label + ".budgets")
    for key in ("max_media_bytes_total", "max_evidence_bytes_total", "reserved_free_bytes", "required_bytes"):
        _integer(budgets[key], "%s.budgets.%s" % (label, key))
    if budgets["reserved_free_bytes"] != policy["reserved_free_bytes"] or budgets["reserved_free_bytes"] <= 0:
        raise VerificationError("%s reserve is missing or inconsistent" % label)
    if budgets["required_bytes"] != (
        budgets["max_media_bytes_total"]
        + budgets["max_evidence_bytes_total"]
        + budgets["reserved_free_bytes"]
    ):
        raise VerificationError("%s required-byte arithmetic is inconsistent" % label)
    if budgets["required_bytes"] > preflight["filesystem"]["available_bytes"]:
        raise VerificationError("%s required bytes exceed available bytes" % label)


def validate_complete_process_wrapper(process: Any) -> None:
    _required_keys(
        process,
        (
            "schema_id", "schema_version", "session_state", "process_state",
            "pid", "started", "sockets_bound", "ready", "terminal_seen",
            "exited", "reaped", "exit_code", "term_signal",
            "stdout_bytes_read", "cleanup_complete", "first_failure", "error",
            "starting", "ready_snapshot", "heartbeat", "terminal", "last",
        ),
        "session.recorder_process_status",
    )
    if (
        process["schema_id"]
        != "orange.spatial_roi_recording.headless_process_status"
        or process["schema_version"] != 1
        or process["session_state"] != "finished"
        or process["process_state"] != "exited"
        or process["started"] is not True
        or process["sockets_bound"] is not True
        or process["ready"] is not True
        or process["terminal_seen"] is not True
        or process["exited"] is not True
        or process["reaped"] is not True
        or process["cleanup_complete"] is not True
        or process["exit_code"] != 0
        or process["term_signal"] != 0
        or process["first_failure"] != ""
        or process["error"] != ""
    ):
        raise VerificationError("complete recorder process lifecycle is not cleanly finished")
    _integer(process["pid"], "complete recorder process pid", True)
    _integer(
        process["stdout_bytes_read"],
        "complete recorder process stdout bytes",
        True,
    )
    child_keys = (
        "event", "status", "state", "ready", "clean_eof", "completed",
        "failed", "first_failure_stream_id", "first_failure", "error",
        "payload",
    )
    def validate_child(name: str) -> Dict[str, Any]:
        child = process[name]
        _required_keys(child, child_keys, "recorder %s snapshot" % name)
        for key in (
            "event", "status", "state", "first_failure_stream_id",
            "first_failure", "error",
        ):
            _string(
                child[key],
                "recorder %s snapshot.%s" % (name, key),
                False,
            )
        for key in ("ready", "clean_eof", "completed", "failed"):
            _bool(child[key], "recorder %s snapshot.%s" % (name, key))
        if not isinstance(child["payload"], dict):
            raise VerificationError("recorder %s snapshot.payload is not an object" % name)
        # The supervisor extracts these fields from the raw child line. If a
        # field remains in the retained payload, the wrapper must be its exact
        # projection rather than a contradictory second lifecycle authority.
        for key in (
            "event", "status", "state", "ready", "clean_eof", "completed",
            "failed", "first_failure_stream_id", "first_failure", "error",
        ):
            if key in child["payload"]:
                _same(
                    child[key],
                    child["payload"][key],
                    "recorder %s wrapper/payload.%s" % (name, key),
                )
        return child

    for child_name in (
        "starting", "ready_snapshot", "heartbeat", "terminal", "last"
    ):
        validate_child(child_name)
    ready = process["ready_snapshot"]
    terminal = process["terminal"]
    if (
        ready["event"] != "ready"
        or ready["status"] != "ready"
        or ready["state"] != "ready"
        or ready["ready"] is not True
        or ready["clean_eof"] is not False
        or ready["completed"] is not False
        or ready["failed"] is not False
        or ready["first_failure_stream_id"] != ""
        or ready["first_failure"] != ""
        or ready["error"] != ""
    ):
        raise VerificationError("complete recorder ready snapshot is invalid")
    if (
        terminal["event"] != "terminal"
        or terminal["status"] != "complete"
        or terminal["state"] != "completed"
        or terminal["ready"] is not True
        or terminal["clean_eof"] is not True
        or terminal["completed"] is not True
        or terminal["failed"] is not False
        or terminal["first_failure_stream_id"] != ""
        or terminal["first_failure"] != ""
        or terminal["error"] != ""
    ):
        raise VerificationError("complete recorder terminal snapshot is invalid")
    _same(process["last"], terminal, "complete recorder last/terminal event")


def validate_required_process_preflight(session: Dict[str, Any]) -> Dict[str, Any]:
    process = session.get("recorder_process_status")
    if not isinstance(process, dict):
        raise VerificationError("complete session lacks recorder_process_status")
    validate_complete_process_wrapper(process)
    observed: Dict[str, Dict[str, Any]] = {}
    for name in ("ready_snapshot", "terminal"):
        payload = process.get(name)
        if not isinstance(payload, dict):
            raise VerificationError(
                "complete session lacks recorder_process_status.%s" % name
            )
        payload = payload.get("payload")
        if not isinstance(payload, dict) or "storage_preflight" not in payload:
            raise VerificationError(
                "storage_preflight must be present in recorder_process_status.%s.payload"
                % name
            )
        validate_storage_preflight(payload["storage_preflight"], "session.recorder_process_status.%s.payload.storage_preflight" % name)
        observed[name] = payload["storage_preflight"]
    _same(observed["ready_snapshot"], observed["terminal"],
          "ready/terminal storage_preflight")
    receipt_root = session["finalized_session_receipt"]["root_authority"][
        "artifact_root_identity"
    ]
    _same(observed["terminal"]["artifact_root"], receipt_root,
          "storage_preflight artifact-root identity")
    return observed["terminal"]


def secure_verify_session_authorities(
    root_fd: int,
    session: Dict[str, Any],
    seen_paths: Set[str],
    seen_inodes: Set[Tuple[int, int]],
) -> Dict[str, Any]:
    documents: Dict[str, Any] = {}
    for name in ("normalized_config", "verified_plan", "recorder_contract"):
        reference = session["artifacts"][name]
        relative = safe_relative_path(
            reference["relative_path"], "session.artifacts.%s.relative_path" % name
        )
        if relative in seen_paths:
            raise VerificationError("session authority artifact paths collide")
        seen_paths.add(relative)
        fd = open_relative(root_fd, relative)
        try:
            checked = hash_open_fd(
                fd,
                relative,
                reference["size_bytes"],
                reference["sha256"],
            )
            inode = tuple(checked["inode"])
            if inode in seen_inodes:
                raise VerificationError(
                    "session authority artifacts resolve to duplicate inodes"
                )
            seen_inodes.add(inode)
            document = read_open_json_fd(fd, relative)
            if not isinstance(document, dict):
                raise VerificationError(
                    "session authority artifact %s is not a JSON object" % name
                )
            documents[name] = document
        finally:
            _close_quietly(fd)
    return documents


def _expected_socket_path(recording_identity_token: str, stream_id: str) -> str:
    digest = hashlib.sha256(stream_id.encode("utf-8")).hexdigest()
    return (
        "/tmp/orange_spatial_roi_"
        + recording_identity_token[7:31]
        + "/"
        + digest[:24]
        + ".sock"
    )


def _round_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def _rectangles_overlap(a: Dict[str, Any], b: Dict[str, Any]) -> bool:
    return (
        a["x"] < b["x"] + b["width"]
        and b["x"] < a["x"] + a["width"]
        and a["y"] < b["y"] + b["height"]
        and b["y"] < a["y"] + a["height"]
    )


def validate_resolved_plan_authority(
    plan_payload: Dict[str, Any],
    config: Dict[str, Any],
    session: Dict[str, Any],
    contract_streams: Dict[str, Any],
) -> None:
    """Bind the authenticated plan's resolved camera/ROI map to the session."""
    config_keys = (
        "schema_id", "schema_version", "enabled", "backend", "strict",
        "source_cadence", "pixel_contract", "buffering",
        "recording_limits", "admission", "cameras",
    )
    if config.get("schema_version") == 3:
        config_keys += ("encode_profile",)
    _required_keys(config, config_keys, "normalized config")
    pixel = config["pixel_contract"]
    _required_keys(
        pixel,
        (
            "source_format", "no_resize", "no_color_conversion",
            "output_alignment_px", "padding_value_mono8",
        ),
        "normalized config.pixel_contract",
    )
    if (
        pixel["source_format"] != "mono8"
        or pixel["no_resize"] is not True
        or pixel["no_color_conversion"] is not True
        or pixel["padding_value_mono8"] != 0
    ):
        raise VerificationError("normalized config pixel contract is not exact")
    alignment = _integer(
        pixel["output_alignment_px"],
        "normalized config pixel alignment",
        True,
    )
    if alignment not in (1, 2, 4, 8, 16):
        raise VerificationError("normalized config pixel alignment is unsupported")

    cameras = config["cameras"]
    serial = session["camera_serial"]
    if not isinstance(cameras, dict) or set(cameras) != {serial}:
        raise VerificationError("normalized config is not exactly one-camera")
    configured = cameras[serial]
    _required_keys(
        configured,
        (
            "camera_id", "camera_serial", "native_raster", "source_frame_rate",
            "arena_group_id", "layout", "materialization", "registration",
            "allow_roi_overlap", "rois",
        ),
        "normalized config camera",
    )
    for key, expected in (
        ("camera_id", session["camera_id"]),
        ("camera_serial", serial),
        ("native_raster", session["native_raster"]),
        ("layout", session["authorities"]["layout"]),
        ("materialization", session["authorities"]["materialization"]),
        ("registration", session["authorities"]["registration"]),
    ):
        _same(configured[key], expected, "normalized config camera.%s" % key)
    source_fps = _integer(
        configured["source_frame_rate"],
        "normalized config camera.source_frame_rate",
        True,
    )
    configured_arena_group = _string(
        configured["arena_group_id"], "normalized config arena group"
    )
    _bool(configured["allow_roi_overlap"], "normalized config ROI overlap policy")
    configured_rois = configured["rois"]
    if not isinstance(configured_rois, list) or len(configured_rois) != 4:
        raise VerificationError("normalized config camera must contain four ROIs")

    resolved_cameras = plan_payload.get("resolved_cameras")
    if not isinstance(resolved_cameras, dict) or set(resolved_cameras) != {serial}:
        raise VerificationError("verified plan resolved_cameras is not exactly one-camera")
    resolved = resolved_cameras[serial]
    resolved_rois: List[Dict[str, Any]] = []
    for index, configured_roi in enumerate(configured_rois):
        if not isinstance(configured_roi, dict):
            raise VerificationError("normalized config ROI is not an object")
        required = {
            "roi_id", "region_id", "required", "content_rect",
            "logical_stream_id", "artifact_stem",
        }
        optional = {"arena_id", "region_mask"}
        if not required.issubset(configured_roi) or not set(configured_roi).issubset(
            required | optional
        ):
            raise VerificationError("normalized config ROI has a non-closed schema")
        roi = session["rois"][index]
        stream_id = session["stream_order"][index]
        _same(
            roi["arena_group_id"],
            configured_arena_group,
            "normalized config/session arena_group_id",
        )
        for key, expected in (
            ("roi_id", roi["roi_id"]),
            ("region_id", roi["region_id"]),
            ("required", True),
            ("content_rect", roi["geometry"]["content_rect"]),
            ("logical_stream_id", stream_id),
        ):
            _same(configured_roi.get(key), expected, "normalized config ROI.%s" % key)
        if configured_roi.get("arena_id") != roi["arena_id"]:
            if not ("arena_id" not in configured_roi and roi["arena_id"] is None):
                raise VerificationError("normalized config ROI arena_id is substituted")
        expected_stream_id = serial + "_spatial_roi_" + roi["roi_id"]
        expected_stem = "Cam" + serial + "_spatial_roi_" + roi["roi_id"]
        _same(stream_id, expected_stream_id, "normalized config logical stream convention")
        _same(
            configured_roi["artifact_stem"],
            expected_stem,
            "normalized config artifact stem convention",
        )
        rect = configured_roi["content_rect"]
        width = _round_up(
            _integer(rect["width"], "normalized config ROI width", True), alignment
        )
        height = _round_up(
            _integer(rect["height"], "normalized config ROI height", True), alignment
        )
        plan_artifacts = {
            "video": ARTIFACT_ROOT + "/" + expected_stem + ".mp4",
            "metadata": ARTIFACT_ROOT + "/" + expected_stem + "_meta.csv",
            "keyframes": ARTIFACT_ROOT + "/" + expected_stem + "_keyframe.json",
            "perf": ARTIFACT_ROOT + "/" + expected_stem + "_perf.csv",
            "summary": ARTIFACT_ROOT + "/" + expected_stem + "_summary.json",
            "finalization": ARTIFACT_ROOT + "/" + expected_stem + ".mp4.finalization.json",
        }
        expected_resolved_roi = dict(configured_roi)
        expected_resolved_roi.update(
            {
                "encoded_raster": {"width": width, "height": height},
                "content_offset": {"x": 0, "y": 0},
                "encoded_content_rect": {
                    "x": 0,
                    "y": 0,
                    "width": rect["width"],
                    "height": rect["height"],
                },
                "padding": {
                    "right": width - rect["width"],
                    "bottom": height - rect["height"],
                    "value_mono8": 0,
                },
                "no_scaling": True,
                "socket_path": _expected_socket_path(
                    session["recording_identity_token"], stream_id
                ),
                "expected_artifacts": plan_artifacts,
            }
        )
        resolved_rois.append(expected_resolved_roi)
        _same(
            roi["encode_fps"], source_fps, "resolved plan ROI/source frame rate"
        )
        _same(
            roi["geometry"]["encoded_raster"],
            expected_resolved_roi["encoded_raster"],
            "resolved plan/session encoded raster",
        )
        _same(
            roi["geometry"]["encoded_content_rect"],
            expected_resolved_roi["encoded_content_rect"],
            "resolved plan/session encoded content rect",
        )
        _same(
            roi["geometry"]["padding"]["right"],
            expected_resolved_roi["padding"]["right"],
            "resolved plan/session right padding",
        )
        _same(
            roi["geometry"]["padding"]["bottom"],
            expected_resolved_roi["padding"]["bottom"],
            "resolved plan/session bottom padding",
        )
        stream = contract_streams[stream_id]
        _same(
            stream.get("socket_path"),
            expected_resolved_roi["socket_path"],
            "resolved plan/contract socket path",
        )
        for kind, path in plan_artifacts.items():
            _same(
                posixpath.relpath(stream["expected_artifacts"][kind],
                                  posixpath.dirname(stream["expected_artifacts"][kind])),
                posixpath.basename(path),
                "resolved plan/contract artifact %s" % kind,
            )
    if configured["allow_roi_overlap"] is False:
        for index, roi in enumerate(configured_rois):
            for prior in configured_rois[:index]:
                if _rectangles_overlap(prior["content_rect"], roi["content_rect"]):
                    raise VerificationError(
                        "normalized config ROIs overlap while allow_roi_overlap is false"
                    )
    expected_resolved = dict(configured)
    expected_resolved["rois"] = resolved_rois
    _same(resolved, expected_resolved, "verified plan resolved camera authority")


def validate_plan_admission_usage(
    config: Dict[str, Any], plan_payload: Dict[str, Any], session: Dict[str, Any]
) -> Dict[str, int]:
    buffering = config.get("buffering")
    _required_keys(
        buffering,
        ("pool_frames_per_stream", "queue_frames_per_stream"),
        "normalized config.buffering",
    )
    pool_frames = _integer(
        buffering["pool_frames_per_stream"],
        "normalized config buffering.pool_frames_per_stream",
        True,
    )
    queue_frames = _integer(
        buffering["queue_frames_per_stream"],
        "normalized config buffering.queue_frames_per_stream",
        True,
    )

    limits = config["recording_limits"]
    per_stream_media = _integer(
        limits["max_media_bytes_per_stream"],
        "normalized config recording_limits.max_media_bytes_per_stream",
        True,
    )
    per_stream_evidence = _integer(
        limits["max_evidence_bytes_per_stream"],
        "normalized config recording_limits.max_evidence_bytes_per_stream",
        True,
    )
    configured = config["cameras"][session["camera_serial"]]
    source_fps = _integer(
        configured["source_frame_rate"],
        "normalized config camera.source_frame_rate",
        True,
    )
    content_rate = 0
    encoded_rate = 0
    pool_bytes = 0
    for roi in session["rois"]:
        content = roi["geometry"]["content_rect"]
        encoded = roi["geometry"]["encoded_raster"]
        content_rate += content["width"] * content["height"] * source_fps
        encoded_rate += encoded["width"] * encoded["height"] * source_fps
        # The configuration admission pool accounts for packed Mono8 source
        # frames. Recorder-side Mono8+NV12 detach pools are separately bound
        # by the authenticated recorder contract.
        pool_bytes += encoded["width"] * encoded["height"] * pool_frames
    expected = {
        "camera_count": 1,
        "roi_count": session["stream_count"],
        "encoder_stream_count": session["stream_count"],
        "content_pixel_rate": content_rate,
        "encoded_pixel_rate": encoded_rate,
        "pool_bytes": pool_bytes,
        "queue_frames": queue_frames * session["stream_count"],
        "media_bytes": per_stream_media * session["stream_count"],
        "evidence_bytes": per_stream_evidence * session["stream_count"],
    }
    admission_usage = plan_payload.get("admission_usage")
    _required_keys(admission_usage, expected.keys(), "verified plan.admission_usage")
    for key, expected_value in expected.items():
        _same(
            _integer(admission_usage[key], "verified plan admission_usage.%s" % key, True),
            expected_value,
            "verified plan admission_usage.%s" % key,
        )

    admission = config.get("admission")
    admission_keys = (
        "max_rois_per_camera", "max_total_rois", "max_total_pixel_rate",
        "max_total_encoder_streams", "max_total_pool_bytes",
        "max_total_queue_frames", "max_total_media_bytes",
        "max_total_evidence_bytes",
    )
    _required_keys(admission, admission_keys, "normalized config.admission")
    ceilings = {
        key: _integer(
            admission[key], "normalized config admission.%s" % key, True
        )
        for key in admission_keys
    }
    if len(configured["rois"]) > ceilings["max_rois_per_camera"]:
        raise VerificationError("configured ROI count exceeds max_rois_per_camera")
    comparisons = (
        ("roi_count", "max_total_rois"),
        ("encoded_pixel_rate", "max_total_pixel_rate"),
        ("encoder_stream_count", "max_total_encoder_streams"),
        ("pool_bytes", "max_total_pool_bytes"),
        ("queue_frames", "max_total_queue_frames"),
        ("media_bytes", "max_total_media_bytes"),
        ("evidence_bytes", "max_total_evidence_bytes"),
    )
    for usage_key, ceiling_key in comparisons:
        if expected[usage_key] > ceilings[ceiling_key]:
            raise VerificationError(
                "verified plan %s exceeds normalized config %s"
                % (usage_key, ceiling_key)
            )
    return expected


def expected_contract_ipc_v2(
    queue_frames_per_stream: int, stream_count: int
) -> Dict[str, Any]:
    total = queue_frames_per_stream * stream_count
    return {
        "protocol": "orange.spatial_roi.external_recorder_ipc",
        "version": 2,
        "features": list(CONTRACT_IPC_FEATURES),
        "source_lifetime_mode": "deferred_release",
        "ack": {
            "message_kind": "ACK",
            "accepted_true": {
                "means": "recorder_accepted_frame_and_retains_source_access",
                "source_safe_after_ack": False,
                "release_required": True,
            },
            "accepted_false": {
                "means": "recorder_rejected_frame_but_source_access_is_not_yet_released",
                "source_safe_after_ack": False,
                "release_required": True,
            },
        },
        "release": {
            "message_kind": "RELEASE",
            "means": "recorder_finished_with_source_allocation",
            "source_safe_after_release": True,
            "required_after_accepted_ack": True,
            "required_after_rejected_ack": True,
            "does_not_mean_encode_or_disk_complete": True,
        },
        "drain_finalize": {
            "status": "defined_not_negotiated",
            "operational": False,
            "message_order": [
                "DRAIN_REQUEST", "DRAIN_STATUS", "FINALIZE_REQUEST",
                "FINALIZE_STATUS",
            ],
            "drain_request": {
                "message_kind": "DRAIN_REQUEST",
                "sender": "producer",
                "receiver": "recorder",
                "correlation": "stream_identity_and_drain_sequence",
                "reason_required": True,
            },
            "drain_status": {
                "message_kind": "DRAIN_STATUS",
                "sender": "recorder",
                "receiver": "producer",
                "states": ["draining", "drained", "failed"],
                "correlation": "stream_identity_and_drain_sequence",
                "reason_required": True,
                "finalize_request_allowed_only_when": "state=drained",
            },
            "finalize_request": {
                "message_kind": "FINALIZE_REQUEST",
                "sender": "producer",
                "receiver": "recorder",
                "requires": "matching_drained_status",
                "correlation": "stream_identity_and_drain_sequence",
                "nonce": "fresh_16_byte_lower_hex",
                "reason_required": True,
            },
            "finalize_status": {
                "message_kind": "FINALIZE_STATUS",
                "sender": "recorder",
                "receiver": "producer",
                "states": ["finalized", "failed"],
                "correlation": "stream_identity_drain_sequence_and_finalize_nonce",
                "nonce": "must_equal_request",
                "reason_required": True,
                "session_finalized_only_when": "state=finalized",
            },
        },
        "bounds": {
            "queue_capacity_frames_per_stream": queue_frames_per_stream,
            "max_outstanding_frames_per_stream": queue_frames_per_stream,
            "max_queue_capacity_frames_per_stream": CONTRACT_MAX_QUEUE_FRAMES,
            "queue_capacity_frames_total": total,
            "max_outstanding_frames_total": total,
            "overflow_action": "reject_frame_without_releasing_prior_frames",
            "producer_backpressure": "nonblocking_fail_closed",
        },
    }


def validate_authenticated_authorities(
    documents: Dict[str, Any],
    session: Dict[str, Any],
    preflight: Dict[str, Any],
    descriptors: Dict[str, Any],
) -> Dict[str, int]:
    config = documents["normalized_config"]
    config_version = config.get("schema_version")
    if config_version not in (2, 3):
        raise VerificationError("authenticated normalized config has an unsupported schema version")
    expected_config_backend = (
        "independent_lossless_external_ipc"
        if config_version == 2
        else "independent_hevc_external_ipc"
    )
    if (
        config.get("schema_id") != "orange.spatial_roi_recording.config"
        or config.get("schema_version") != config_version
        or config.get("enabled") is not True
        or config.get("strict") is not True
        or config.get("backend") != expected_config_backend
        or config.get("source_cadence") != "every_recording_frame"
    ):
        raise VerificationError(
            "authenticated normalized config is not enabled strict schema v%d"
            % config_version
        )
    if config_version == 3:
        config_profile = config.get("encode_profile")
        _required_keys(
            config_profile,
            (
                "name", "codec", "preset", "tuning", "lossless",
                "rate_control_mode", "quality_value", "gop_length", "aq",
                "temporal_aq", "lookahead", "lookahead_depth",
            ),
            "normalized config.encode_profile",
        )
        for key in ("name", "codec", "preset", "tuning", "rate_control_mode"):
            _string(config_profile[key], "normalized config.encode_profile.%s" % key)
        for key in ("lossless", "aq", "temporal_aq", "lookahead"):
            _bool(config_profile[key], "normalized config.encode_profile.%s" % key)
        for key in ("quality_value", "gop_length", "lookahead_depth"):
            _integer(config_profile[key], "normalized config.encode_profile.%s" % key)

    plan_document = documents["verified_plan"]
    _required_keys(
        plan_document,
        ("schema_id", "schema_version", "canonicalization", "plan_sha256", "plan"),
        "authenticated verified plan envelope",
    )
    expected_plan_version = config_version
    if (
        plan_document.get("schema_id") != "orange.spatial_roi_recording.plan"
        or plan_document.get("schema_version") != expected_plan_version
        or plan_document.get("canonicalization")
        != "canonical_json_utf8_sort_keys_compact_v1"
        or not isinstance(plan_document.get("plan"), dict)
    ):
        raise VerificationError("authenticated verified plan envelope is invalid")
    plan_payload = plan_document["plan"]
    _required_keys(
        plan_payload,
        (
            "schema_id", "schema_version", "plan_scope", "recording_id",
            "recording_identity_token", "generated_at_utc", "producer_generation",
            "configuration", "admission_usage", "resolved_cameras",
        ),
        "authenticated verified plan payload",
    )
    expected_plan_digest = canonical_json_sha256(plan_payload)
    _same(plan_document.get("plan_sha256"), expected_plan_digest,
          "verified plan canonical digest")
    _same(plan_document.get("plan_sha256"), session["spatial_roi_plan_sha256"],
          "verified plan/session digest")
    for key, expected in (
        ("recording_id", session["recording_id"]),
        ("recording_identity_token", session["recording_identity_token"]),
        ("producer_generation", session["producer_generation"]),
    ):
        _same(plan_payload.get(key), expected, "verified plan.%s" % key)
    _same(plan_payload.get("configuration"), config,
          "verified plan embedded normalized config")

    contract = documents["recorder_contract"]
    _required_keys(contract, CONTRACT_KEYS, "authenticated recorder contract")
    contract_version = 4 if config_version == 2 else 5
    expected_contract_scope = (
        "strict_spatial_roi_external_recorder_v4"
        if contract_version == 4
        else "strict_spatial_roi_external_recorder_v5"
    )
    expected_contract_mode = (
        "spatial_roi_external_recorder_v4"
        if contract_version == 4
        else "spatial_roi_external_recorder_v5"
    )
    if (
        contract.get("schema_id")
        != "orange.spatial_roi_recording.external_recorder_contract"
        or contract.get("schema_version") != contract_version
        or contract.get("contract_scope") != expected_contract_scope
        or contract.get("mode") != expected_contract_mode
        or contract.get("strict") is not True
        or contract.get("backend") != expected_config_backend
        or contract.get("supervise_processes") is not True
        or contract.get("require_summary") is not True
        or contract.get("require_status") is not True
        or contract.get("require_video_sanity") is not True
        or contract.get("require_protocol_hello") is not True
        or contract.get("require_frame_identity_proof") is not True
        or contract.get("require_gop_routing") is not False
        or contract.get("require_storage_preflight") is not True
        or contract.get("preserve_shard_mp4s") is not False
        or contract.get("source_cadence") != "every_recording_frame"
        or contract.get("source_pixel_format") != "mono8"
    ):
        raise VerificationError(
            "authenticated recorder contract is not strict schema v%d"
            % contract_version
        )
    _same(
        contract.get("recording_control"),
        {"record_for_seconds": 0, "clip_seconds": 0},
        "recorder contract non-rolling recording_control",
    )
    _same(
        contract.get("rollover"),
        {"requested": False, "status": "not_requested", "implementation": "none"},
        "recorder contract non-rolling rollover",
    )
    for key, expected in (
        ("recording_id", session["recording_id"]),
        ("session_id", session["session_id"]),
        ("recording_identity_token", session["recording_identity_token"]),
        ("producer_generation", session["producer_generation"]),
        ("spatial_roi_plan_sha256", session["spatial_roi_plan_sha256"]),
        ("stream_count", session["stream_count"]),
        ("stream_order", session["stream_order"]),
        ("gpu_mapping", session["gpu_mapping"]),
    ):
        _same(contract.get(key), expected, "recorder contract.%s" % key)
    contract_streams = contract.get("streams")
    if not isinstance(contract_streams, dict) or set(contract_streams) != set(
        session["stream_order"]
    ):
        raise VerificationError("authenticated recorder contract stream set is substituted")
    selected_profile = session["rois"][0]["encode_profile"]
    for roi in session["rois"][1:]:
        _same(
            roi["encode_profile"],
            selected_profile,
            "session ROI encode profile consistency",
        )
    if config_version == 2:
        _same(
            selected_profile,
            _supported_encode_profiles(selected_profile["frame_rate"])[0],
            "legacy normalized config/session encode profile",
        )
    else:
        expected_config_profile = {
            "name": selected_profile["profile_id"],
            "codec": selected_profile["codec"],
            "preset": selected_profile["preset"],
            "tuning": selected_profile["tuning"],
            "lossless": selected_profile["lossless"],
            "rate_control_mode": selected_profile["rate_control_mode"],
            "quality_value": selected_profile["quality_value"],
            "gop_length": selected_profile["gop_length"],
            "aq": selected_profile["aq"],
            "temporal_aq": selected_profile["temporal_aq"],
            "lookahead": selected_profile["lookahead"],
            "lookahead_depth": selected_profile["lookahead_depth"],
        }
        _same(
            config["encode_profile"],
            expected_config_profile,
            "normalized config/session encode profile",
        )
    limits = config.get("recording_limits")
    _required_keys(
        limits,
        (
            "max_frames_per_stream",
            "max_media_bytes_per_stream",
            "max_evidence_bytes_per_stream",
        ),
        "normalized config.recording_limits",
    )
    authenticated_limits = {
        key: _integer(limits[key], "normalized config recording limit %s" % key, True)
        for key in (
            "max_frames_per_stream",
            "max_media_bytes_per_stream",
            "max_evidence_bytes_per_stream",
        )
    }
    queue_depth = _integer(
        config["buffering"]["queue_frames_per_stream"],
        "normalized config buffering.queue_frames_per_stream",
        True,
    )
    if queue_depth > CONTRACT_MAX_QUEUE_FRAMES:
        raise VerificationError("authenticated recorder queue depth exceeds IPC-v2 limit")
    _same(
        contract.get("ipc_v2"),
        expected_contract_ipc_v2(queue_depth, session["stream_count"]),
        "recorder contract IPC-v2 protocol",
    )
    aggregate_expected = {
        "max_detach_pool_bytes_total": 0,
        "max_queue_bytes_total": 0,
        "writer_queue_max_packets_total": 0,
        "writer_queue_max_bytes_total": 0,
        "operation_timeout_ms_per_stream": CONTRACT_OPERATION_TIMEOUT_MS,
        "max_media_bytes_total": 0,
        "max_evidence_bytes_total": 0,
    }
    for index, stream_id in enumerate(session["stream_order"]):
        stream = contract_streams[stream_id]
        roi = session["rois"][index]
        descriptor = descriptors[stream_id]
        if not isinstance(stream, dict):
            raise VerificationError("authenticated recorder contract stream is not an object")
        _required_keys(
            stream,
            CONTRACT_STREAM_KEYS,
            "authenticated recorder contract stream %s" % stream_id,
        )
        pixels = (
            roi["geometry"]["encoded_raster"]["width"]
            * roi["geometry"]["encoded_raster"]["height"]
        )
        if (
            roi["geometry"]["encoded_raster"]["width"] % 2 != 0
            or roi["geometry"]["encoded_raster"]["height"] % 2 != 0
        ):
            raise VerificationError("recorder contract encoded raster is not NV12-compatible")
        max_queue_bytes = (pixels + pixels // 2) * queue_depth
        max_detach_pool_bytes = (pixels + pixels + pixels // 2) * queue_depth
        expected_identity = {
            "recording_id": session["recording_id"],
            "recording_identity_token": session["recording_identity_token"],
            "producer_generation": session["producer_generation"],
            "spatial_roi_plan_sha256": session["spatial_roi_plan_sha256"],
            "camera_id": session["camera_id"],
            "camera_serial": session["camera_serial"],
            "arena_group_id": roi["arena_group_id"],
            "arena_id": roi["arena_id"],
            "region_id": roi["region_id"],
            "roi_id": roi["roi_id"],
            "logical_stream_id": stream_id,
        }
        for key, expected in (
            ("stream_id", stream_id),
            ("logical_stream_id", stream_id),
            ("stream_kind", "spatial_roi"),
            ("output_kind", "spatial_roi"),
            ("camera_id", session["camera_id"]),
            ("camera_serial", session["camera_serial"]),
            ("env_key", "spatial_roi_" + stream_id),
            ("roi_id", roi["roi_id"]),
            ("region_id", roi["region_id"]),
            ("arena_group_id", roi["arena_group_id"]),
            ("arena_id", roi["arena_id"]),
            ("recording_id", session["recording_id"]),
            ("session_id", session["session_id"]),
            ("recording_identity_token", session["recording_identity_token"]),
            ("producer_generation", session["producer_generation"]),
            ("spatial_roi_plan_sha256", session["spatial_roi_plan_sha256"]),
            ("analytics_gpu_id", roi["analytics_gpu_id"]),
            ("source_gpu_id", roi["source_gpu_id"]),
            ("recorder_gpu_id", roi["recorder_gpu_id"]),
            ("assigned_gpu_id", roi["assigned_gpu_id"]),
            ("geometry_identity", roi["geometry"]),
            ("encode_profile", roi["encode_profile"]),
            ("encode_fps", roi["encode_fps"]),
            ("codec", roi["codec"]),
            ("tuning", roi["tuning"]),
            ("rate_control_mode", roi["encode_profile"]["rate_control_mode"]),
            ("quality_value", roi["encode_profile"]["quality_value"]),
            ("gop", roi["encode_profile"]["gop_length"]),
            ("encode_queue_depth", queue_depth),
            ("detach_pool_frames", queue_depth),
            ("max_detach_pool_bytes", max_detach_pool_bytes),
            ("max_queue_bytes", max_queue_bytes),
            ("writer_queue_max_packets", CONTRACT_WRITER_QUEUE_MAX_PACKETS),
            ("writer_queue_max_bytes", CONTRACT_WRITER_QUEUE_MAX_BYTES),
            ("operation_timeout_ms", CONTRACT_OPERATION_TIMEOUT_MS),
            ("routing_policy", "single_shard"),
            ("recording_control", {"record_for_seconds": 0, "clip_seconds": 0}),
            ("rollover", {"requested": False, "status": "not_requested", "implementation": "none"}),
            ("identity", expected_identity),
            ("expected_shard_gpu_ids", roi["expected_shard_gpu_ids"]),
            ("max_frames_per_stream", authenticated_limits["max_frames_per_stream"]),
            ("max_media_bytes_per_stream", authenticated_limits["max_media_bytes_per_stream"]),
            ("max_evidence_bytes_per_stream", authenticated_limits["max_evidence_bytes_per_stream"]),
        ):
            _same(stream.get(key), expected,
                  "recorder contract stream %s.%s" % (stream_id, key))
        expected_frame_identity = {
            "key_fields": [
                "recording_identity_token",
                "producer_generation",
                "logical_stream_id",
                "recording_frame_id",
                "roi_stream_frame_index",
            ],
            "roi_stream_frame_index": "dense_one_based",
            "recording_frame_id_source": "parent_camera_recording",
        }
        _same(stream.get("frame_identity"), expected_frame_identity,
              "recorder contract stream %s.frame_identity" % stream_id)
        expected_artifacts = stream.get("expected_artifacts")
        descriptor_artifacts = descriptor.get("details", {}).get("artifacts")
        if (
            not isinstance(expected_artifacts, dict)
            or set(expected_artifacts) != set(ARTIFACT_KINDS)
            or not isinstance(descriptor_artifacts, dict)
        ):
            raise VerificationError(
                "authenticated recorder contract stream artifact set is incomplete"
            )
        contract_artifact_root = contract.get("artifact_root")
        if not isinstance(contract_artifact_root, str) or not contract_artifact_root:
            raise VerificationError("authenticated recorder contract artifact root is invalid")
        for kind in ARTIFACT_KINDS:
            contract_path = expected_artifacts[kind]
            if (
                not isinstance(contract_path, str)
                or posixpath.dirname(contract_path) != contract_artifact_root
                or descriptor_artifacts.get(kind)
                != ARTIFACT_ROOT + "/" + posixpath.basename(contract_path)
            ):
                raise VerificationError(
                    "authenticated recorder contract artifact %s is not coupled to descriptor %s"
                    % (kind, stream_id)
                )
        direct_artifacts = {
            "mp4": "video",
            "metadata_csv": "metadata",
            "keyframe_json": "keyframes",
            "perf_csv": "perf",
            "summary_json": "summary",
            "status_json": "status",
            "video_sanity_json": "video_sanity",
            "finalization_json": "finalization",
            "recorder_log": "recorder_log",
            "transport_sidecar": "transport_sidecar",
            "evidence_jsonl": "evidence",
            "evidence_manifest_json": "evidence_manifest",
        }
        for direct, kind in direct_artifacts.items():
            _same(
                stream[direct],
                expected_artifacts[kind],
                "recorder contract stream %s.%s" % (stream_id, direct),
            )
        aggregate_expected["max_detach_pool_bytes_total"] += max_detach_pool_bytes
        aggregate_expected["max_queue_bytes_total"] += max_queue_bytes
        aggregate_expected["writer_queue_max_packets_total"] += (
            CONTRACT_WRITER_QUEUE_MAX_PACKETS
        )
        aggregate_expected["writer_queue_max_bytes_total"] += (
            CONTRACT_WRITER_QUEUE_MAX_BYTES
        )
        aggregate_expected["max_media_bytes_total"] += authenticated_limits[
            "max_media_bytes_per_stream"
        ]
        aggregate_expected["max_evidence_bytes_total"] += authenticated_limits[
            "max_evidence_bytes_per_stream"
        ]

    validate_resolved_plan_authority(
        plan_payload, config, session, contract_streams
    )

    bounds = contract.get("aggregate_bounds")
    _same(bounds, aggregate_expected, "recorder contract aggregate bounds")
    media_bound = _integer(
        bounds.get("max_media_bytes_total"),
        "recorder contract aggregate media bound",
        True,
    )
    evidence_bound = _integer(
        bounds.get("max_evidence_bytes_total"),
        "recorder contract aggregate evidence bound",
        True,
    )
    _same(
        media_bound,
        authenticated_limits["max_media_bytes_per_stream"] * session["stream_count"],
        "recorder contract aggregate/per-stream media bounds",
    )
    _same(
        evidence_bound,
        authenticated_limits["max_evidence_bytes_per_stream"] * session["stream_count"],
        "recorder contract aggregate/per-stream evidence bounds",
    )
    _same(
        preflight["budgets"]["max_media_bytes_total"],
        media_bound,
        "preflight/authenticated contract media budget",
    )
    _same(
        preflight["budgets"]["max_evidence_bytes_total"],
        evidence_bound,
        "preflight/authenticated contract evidence budget",
    )
    _same(
        contract.get("storage_preflight_policy"),
        preflight["policy"],
        "preflight/authenticated contract policy",
    )
    admission = validate_plan_admission_usage(config, plan_payload, session)
    _same(admission["media_bytes"], media_bound,
          "verified plan/contract media admission")
    _same(admission["evidence_bytes"], evidence_bound,
          "verified plan/contract evidence admission")
    return authenticated_limits


def validate_complete_producer_status(
    session: Dict[str, Any], shared_frame_count: int
) -> None:
    producer = session.get("producer_status")
    _required_keys(
        producer,
        (
            "schema_id", "schema_version", "state", "recording_id",
            "session_id", "recording_identity_token", "producer_generation",
            "spatial_roi_plan_sha256", "camera_id", "camera_serial",
            "stream_count", "submit_attempted", "submitted", "incomplete",
            "rejected", "acquisition_armed", "first_failure",
        ),
        "session.producer_status",
    )
    if (
        producer["schema_id"]
        != "orange.spatial_roi_recording.headless_producer_status"
        or producer["schema_version"] != 1
        or producer["state"] != "stopped"
        or producer["acquisition_armed"] is not False
        or producer["first_failure"] != ""
    ):
        raise VerificationError("complete session producer status is not stopped cleanly")
    for key, expected in (
        ("recording_id", session["recording_id"]),
        ("session_id", session["session_id"]),
        ("recording_identity_token", session["recording_identity_token"]),
        ("producer_generation", session["producer_generation"]),
        ("spatial_roi_plan_sha256", session["spatial_roi_plan_sha256"]),
        ("camera_id", session["camera_id"]),
        ("camera_serial", session["camera_serial"]),
        ("stream_count", session["stream_count"]),
        ("submit_attempted", shared_frame_count),
        ("submitted", shared_frame_count),
        ("incomplete", 0),
        ("rejected", 0),
    ):
        _same(producer[key], expected, "complete producer status.%s" % key)


def _validate_roi_geometry(
    roi: Dict[str, Any],
    label: str,
    authorities: Optional[Dict[str, Any]] = None,
    expected_native_raster: Optional[Dict[str, Any]] = None,
) -> None:
    geometry = roi["geometry"]
    _required_keys(geometry, ("layout", "materialization", "registration", "native_raster", "content_rect", "encoded_raster", "encoded_content_rect", "content_offset", "padding", "source_coordinate_space", "video_coordinate_space"), label + ".geometry")
    for key in ("native_raster", "encoded_raster"):
        _required_keys(geometry[key], ("width", "height"), label + ".geometry." + key)
        _integer(geometry[key]["width"], label + ".geometry." + key + ".width", True)
        _integer(geometry[key]["height"], label + ".geometry." + key + ".height", True)
    for key in ("content_rect", "encoded_content_rect"):
        _required_keys(geometry[key], ("x", "y", "width", "height"), label + ".geometry." + key)
        for field in ("x", "y", "width", "height"):
            _integer(geometry[key][field], label + ".geometry.%s.%s" % (key, field), field in ("width", "height"))
    _required_keys(geometry["content_offset"], ("x", "y"), label + ".geometry.content_offset")
    _required_keys(geometry["padding"], ("left", "top", "right", "bottom", "value_mono8"), label + ".geometry.padding")
    for key in ("x", "y"):
        _integer(geometry["content_offset"][key], label + ".geometry.content_offset." + key)
    for key in ("left", "top", "right", "bottom", "value_mono8"):
        _integer(geometry["padding"][key], label + ".geometry.padding." + key)
    for key in ("source_coordinate_space", "video_coordinate_space"):
        _string(geometry[key], label + ".geometry." + key)
    for key in ("layout", "materialization", "registration"):
        if authorities is not None:
            _same(geometry[key], authorities[key], label + ".geometry." + key)
    if expected_native_raster is not None:
        _same(geometry["native_raster"], expected_native_raster, label + ".native_raster")
    if geometry["source_coordinate_space"] != "camera_native_full_frame_pixels":
        raise VerificationError("%s source coordinate space is not exact" % label)
    if geometry["video_coordinate_space"] != "spatial_roi_encoded_pixels":
        raise VerificationError("%s video coordinate space is not exact" % label)
    native = geometry["native_raster"]
    encoded = geometry["encoded_raster"]
    for key, raster, rect in (
        ("content_rect", native, geometry["content_rect"]),
        ("encoded_content_rect", encoded, geometry["encoded_content_rect"]),
    ):
        if rect["x"] > raster["width"] - rect["width"] or rect["y"] > raster["height"] - rect["height"]:
            raise VerificationError("%s.%s does not fit its raster" % (label, key))
    if geometry["content_rect"]["width"] != geometry["encoded_content_rect"]["width"] or geometry["content_rect"]["height"] != geometry["encoded_content_rect"]["height"]:
        raise VerificationError("%s content and encoded rectangles differ in size" % label)
    if geometry["encoded_content_rect"]["x"] != 0 or geometry["encoded_content_rect"]["y"] != 0:
        raise VerificationError("%s encoded content rectangle is not origin anchored" % label)
    if geometry["content_offset"] != {"x": 0, "y": 0}:
        raise VerificationError("%s content offset is not zero" % label)
    expected_padding = {
        "left": 0,
        "top": 0,
        "right": encoded["width"] - geometry["encoded_content_rect"]["width"],
        "bottom": encoded["height"] - geometry["encoded_content_rect"]["height"],
        "value_mono8": 0,
    }
    if geometry["padding"] != expected_padding:
        raise VerificationError("%s padding is not the exact encoded-content relation" % label)
    _same(geometry["native_raster"], roi["source_geometry"]["native_raster"], label + ".source_geometry.native_raster")
    _same(geometry["content_rect"], roi["source_geometry"]["content_rect"], label + ".source_geometry.content_rect")
    _same(geometry["source_coordinate_space"], roi["source_geometry"]["coordinate_space"], label + ".source_geometry.coordinate_space")
    _same(geometry["encoded_raster"], roi["encoded_geometry"]["raster"], label + ".encoded_geometry.raster")
    _same(geometry["encoded_content_rect"], roi["encoded_geometry"]["content_rect"], label + ".encoded_geometry.content_rect")
    _same(geometry["video_coordinate_space"], roi["encoded_geometry"]["coordinate_space"], label + ".encoded_geometry.coordinate_space")


def validate_manifest_and_descriptors(
    manifest: Dict[str, Any],
    snapshot: Dict[str, Any],
    session: Dict[str, Any],
    recording_root: str,
) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    if manifest.get("schema_id") != "orange.recording_session" or manifest.get("schema_version") != 1:
        raise VerificationError("recording_session.json is not schema v1")
    if manifest.get("status") not in ("completed", "complete"):
        raise VerificationError("recording_session.json is not complete")
    if (
        manifest.get("session_id") != session["session_id"]
        or manifest.get("recording_folder") != recording_root
        or manifest.get("mode") != "single_clip"
    ):
        raise VerificationError(
            "recording_session.json identity/root/mode is not coupled to the spatial session"
        )
    if manifest.get("cameras") != [session["camera_serial"]]:
        raise VerificationError("recording_session.json must select exactly one camera")
    if not isinstance(manifest.get("recording"), dict) or manifest["recording"].get("started") is not True or manifest["recording"].get("drain_completed") is not True:
        raise VerificationError("recording_session.json recording lifecycle is not complete")
    backend = manifest.get("recording_backend")
    if not isinstance(backend, dict) or not isinstance(backend.get("spatial_roi_recording"), dict):
        raise VerificationError("recording_session.json lacks recording_backend.spatial_roi_recording")
    media_policy = backend.get("media_policy")
    roi_only = media_policy is not None and validate_media_policy_object(media_policy) == FIXED_ROIS_WITH_REGISTERED_CONTEXT
    if roi_only:
        if backend.get("mode") != "fixed_roi_external_ipc" or media_policy.get("sink_backend") != "external_ipc":
            raise VerificationError("registered-context media policy requires fixed_roi_external_ipc")
        full_backend = backend.get("full_frame")
        if not isinstance(full_backend, dict) or set(full_backend) != {"status", "required", "continuous", "first_class"} or full_backend != {
            "status": "omitted_by_policy", "required": False, "continuous": False, "first_class": True
        }:
            raise VerificationError("full-frame product must be explicitly omitted by media policy")
        snapshot_session = snapshot.get("session")
        if not isinstance(snapshot_session, dict):
            raise VerificationError("recording_snapshot.json lacks session")
        if snapshot_session.get("spatial_roi_media_policy") != media_policy:
            raise VerificationError("recording_snapshot media policy differs from manifest")
        snapshot_backend = snapshot_session.get("recording_backend")
        if not isinstance(snapshot_backend, dict):
            raise VerificationError("recording_snapshot.json lacks session.recording_backend")
        if snapshot_backend.get("media_policy") != media_policy or snapshot_backend.get("full_frame") != full_backend:
            raise VerificationError("recording_snapshot media policy/full-frame omission differs from manifest")
        if "combined_storage_preflight" in backend:
            raise VerificationError("registered-context media policy must not claim combined storage preflight")
    _same(backend["spatial_roi_recording"], session, "recording_session spatial ROI session")
    snapshot_session = snapshot.get("session")
    if not isinstance(snapshot_session, dict) or not isinstance(snapshot_session.get("spatial_roi_recording"), dict):
        raise VerificationError("recording_snapshot.json lacks session.spatial_roi_recording")
    if (
        snapshot_session.get("recording_mode") != "single_clip"
        or snapshot_session.get("recording_session_manifest_path")
        != os.path.join(recording_root, "recording_session.json")
    ):
        raise VerificationError(
            "recording_snapshot session mode/manifest pointer is substituted"
        )
    _same(snapshot_session["spatial_roi_recording"], session, "recording_snapshot spatial ROI session")
    snapshot_backend = snapshot_session.get("recording_backend")
    if (
        not isinstance(snapshot_backend, dict)
        or not isinstance(snapshot_backend.get("spatial_roi_recording"), dict)
    ):
        raise VerificationError(
            "recording_snapshot.json lacks session.recording_backend.spatial_roi_recording"
        )
    _same(
        snapshot_backend["spatial_roi_recording"],
        session,
        "recording_snapshot duplicated spatial ROI session",
    )
    backend_projection_keys = ["mode", "full_frame"]
    if roi_only:
        backend_projection_keys.extend(["media_policy", "registered_scene_context"])
    if backend.get("mode") == "external_ipc":
        backend_projection_keys.extend(
            ["artifact_root", "combined_storage_preflight"]
        )
    for key in backend_projection_keys:
        _same(
            snapshot_backend.get(key),
            backend.get(key),
            "recording_snapshot recording_backend.%s" % key,
        )
    manifest_v3 = manifest.get("recording_outputs_v3")
    snapshot_v3 = snapshot.get("recording_outputs_v3")
    if not isinstance(manifest_v3, dict) or not isinstance(snapshot_v3, dict) or manifest_v3 != snapshot_v3:
        raise VerificationError("recording outputs v3 is missing or differs between session and snapshot")
    if manifest_v3.get("schema_id") != "orange.recording_outputs" or manifest_v3.get("schema_version") != 3:
        raise VerificationError("recording_outputs_v3 is not schema v3")
    cameras = manifest_v3.get("cameras")
    if not isinstance(cameras, dict) or set(cameras) != {session["camera_serial"]}:
        raise VerificationError("recording_outputs_v3 must contain exactly one camera")
    camera_outputs = cameras[session["camera_serial"]]
    if roi_only:
        if not isinstance(camera_outputs, dict) or set(camera_outputs) != {"spatial_roi"}:
            raise VerificationError("recording_outputs_v3 must omit full-frame output by media policy")
    elif not isinstance(camera_outputs, dict) or set(camera_outputs) != {"full", "spatial_roi"}:
        raise VerificationError("recording_outputs_v3 must retain first-class full and spatial ROI products")
    descriptors = camera_outputs["spatial_roi"]
    if not isinstance(descriptors, dict) or set(descriptors) != set(session["stream_order"]):
        raise VerificationError("spatial ROI descriptors are not exactly four plan-ordered entries")
    full = None if roi_only else camera_outputs["full"]
    if roi_only:
        if manifest.get("recording_outputs") != {} or manifest.get("camera_artifacts") != {}:
            raise VerificationError("manifest compatibility views must omit full-frame artifacts by policy")
        clips = manifest.get("clips")
        if not isinstance(clips, list) or len(clips) != 1 or not isinstance(clips[0], dict):
            raise VerificationError("single-clip manifest must contain one compatibility clip")
        clip = clips[0]
        if clip.get("recording_outputs") != {} or clip.get("artifacts") != {"videos": {}, "metadata": {}, "keyframes": {}}:
            raise VerificationError("clip compatibility views must omit full-frame artifacts by policy")
        snapshot_legacy = snapshot.get("recording_outputs")
        if isinstance(snapshot_legacy, dict):
            for value in snapshot_legacy.values():
                if isinstance(value, dict) and "full" in value:
                    raise VerificationError("snapshot full-frame compatibility output was not omitted by policy")
        snapshot_encoders = snapshot.get("encoders")
        if isinstance(snapshot_encoders, dict):
            for value in snapshot_encoders.values():
                if isinstance(value, dict) and isinstance(value.get("outputs"), dict) and "full" in value["outputs"]:
                    raise VerificationError("snapshot encoder full-frame output was not omitted by policy")
        return descriptors, full
    full_backend_mode = backend.get("mode")
    if full_backend_mode not in ("in_process", "external_ipc"):
        raise VerificationError("recording_backend.mode is not a supported full-frame backend")
    expected_packet_source = (
        "ffprobe_nb_read_packets"
        if full_backend_mode == "in_process"
        else "external_recorder_summary.packets_written"
    )
    if (
        not isinstance(full, dict)
        or full.get("schema_version") != 1
        or full.get("camera_serial") != session["camera_serial"]
        or full.get("output_kind") != FULL_FRAME_KIND
        or full.get("status") != "finalized"
        or full.get("role") != "ingest_authoritative"
        or full.get("backend") != full_backend_mode
        or full.get("container") != "mp4"
        or full.get("coordinate_space") != "full_frame_pixels"
        or full.get("packet_count_source") != expected_packet_source
    ):
        raise VerificationError("full-frame output is not present as a finalized first-class product")
    _integer(full.get("packet_count"), "full-frame packet_count", True)
    legacy_outputs = manifest.get("recording_outputs")
    if (
        not isinstance(legacy_outputs, dict)
        or not isinstance(legacy_outputs.get(session["camera_serial"]), dict)
        or legacy_outputs[session["camera_serial"]].get("full") != full
    ):
        raise VerificationError("manifest schema-2 full output differs from v3")
    camera_artifact = manifest.get("camera_artifacts", {}).get(
        session["camera_serial"]
    )
    expected_camera_artifact = {
        "video": full.get("video"),
        "metadata": full.get("metadata"),
        "keyframes": full.get("keyframes"),
        "frame_count": full.get("frame_count"),
        "first_recording_frame_id": full.get("first_recording_frame_id"),
        "last_recording_frame_id": full.get("last_recording_frame_id"),
        "recording_frame_id_gaps": full.get("recording_frame_id_gaps"),
        "packet_count": full.get("packet_count"),
        "packet_count_source": full.get("packet_count_source"),
    }
    if camera_artifact != expected_camera_artifact:
        raise VerificationError("manifest camera_artifacts full projection differs from v3")
    clips = manifest.get("clips")
    if not isinstance(clips, list) or len(clips) != 1 or not isinstance(clips[0], dict):
        raise VerificationError("single-clip manifest must contain one compatibility clip")
    clip = clips[0]
    if (
        clip.get("recording_folder") != recording_root
        or clip.get("directory") != "."
    ):
        raise VerificationError("single compatibility clip root/directory is substituted")
    if clip.get("recording_outputs") != legacy_outputs or clip.get("recording_outputs_v3") != manifest_v3:
        raise VerificationError("clip recording-output projections differ from terminal v3")
    expected_clip_artifacts = {
        "videos": {session["camera_serial"]: full.get("video")},
        "metadata": {session["camera_serial"]: full.get("metadata")},
        "keyframes": {session["camera_serial"]: full.get("keyframes")},
    }
    if clip.get("artifacts") != expected_clip_artifacts:
        raise VerificationError("clip full-frame artifact projection differs from v3")
    snapshot_legacy = snapshot.get("recording_outputs")
    if (
        not isinstance(snapshot_legacy, dict)
        or snapshot_legacy.get(session["camera_serial"], {}).get("full") != full
    ):
        raise VerificationError("snapshot schema-2 full output differs from v3")
    snapshot_encoders = snapshot.get("encoders")
    if (
        not isinstance(snapshot_encoders, dict)
        or snapshot_encoders.get(session["camera_serial"], {})
        .get("outputs", {})
        .get("full")
        != full
    ):
        raise VerificationError("snapshot encoder full output differs from v3")
    full_backend = backend.get("full_frame")
    if not isinstance(full_backend, dict) or full_backend.get("first_class") is not True or full_backend.get("status") != "finalized":
        raise VerificationError("recording_backend full_frame is not finalized first-class evidence")
    return descriptors, full


def validate_descriptor(descriptor: Dict[str, Any], roi: Dict[str, Any], session: Dict[str, Any], index: int) -> Dict[str, str]:
    label = "descriptor[%d]" % index
    for key, expected in (("schema_version", 3), ("camera_serial", session["camera_serial"]), ("output_kind", "spatial_roi"), ("logical_stream_id", roi["logical_stream_id"]), ("role", "runtime_derived_acquisition_input"), ("backend", "external_ipc"), ("status", "complete")):
        if descriptor.get(key) != expected:
            raise VerificationError("%s.%s is not coupled to the complete session" % (label, key))
    details = descriptor.get("details")
    if not isinstance(details, dict):
        raise VerificationError("%s.details is missing" % label)
    geometry = roi["geometry"]
    profile = roi["encode_profile"]
    expected_descriptor_fields = {
        "codec": profile["codec"],
        "container": "mp4",
        "tuning": profile["tuning"],
        "pixel_source_format": profile["input_format"],
        "encoded_format": profile["encoded_format"],
        "coordinate_space": geometry["source_coordinate_space"],
        "video_pixel_coordinate_space": geometry["video_coordinate_space"],
        "source_geometry_coordinate_space": geometry["source_coordinate_space"],
        "width": geometry["encoded_raster"]["width"],
        "height": geometry["encoded_raster"]["height"],
        "frame_rate": roi["encode_fps"],
    }
    for key, expected in expected_descriptor_fields.items():
        if descriptor.get(key) != expected:
            raise VerificationError("%s.%s is not coupled to the session ROI" % (label, key))
    shared = _identity_from_session(session)
    expected_outer = dict(shared)
    expected_outer.update({
        "camera_id": session["camera_id"],
        "camera_serial": session["camera_serial"],
        "logical_stream_id": roi["logical_stream_id"],
        "roi_id": roi["roi_id"],
        "region_id": roi["region_id"],
        "arena_group_id": roi["arena_group_id"],
        "arena_id": roi["arena_id"],
        "analytics_gpu_id": roi["analytics_gpu_id"],
        "source_gpu_id": roi["source_gpu_id"],
        "recorder_gpu_id": roi["recorder_gpu_id"],
        "assigned_gpu_id": roi["assigned_gpu_id"],
    })
    for key, expected in expected_outer.items():
        if details.get(key) != expected:
            raise VerificationError("%s.details.%s identity mismatch" % (label, key))
    if details.get("stream_id") != roi["logical_stream_id"] or details.get("stream_kind") != "spatial_roi":
        raise VerificationError("%s.details stream identity/kind mismatch" % label)
    if details.get("geometry") != roi["geometry"] or details.get("source_geometry") != roi["source_geometry"]:
        raise VerificationError("%s.details geometry is not coupled to the session ROI" % label)
    if details.get("geometry_identity") != roi["geometry"]:
        raise VerificationError("%s.details.geometry_identity is not coupled to the session ROI" % label)
    encoded_details = details.get("encoded_geometry")
    if not isinstance(encoded_details, dict) or any(
        encoded_details.get(key) != roi["encoded_geometry"].get(key)
        for key in ("raster", "content_rect", "coordinate_space")
    ):
        raise VerificationError("%s.details encoded geometry is not coupled to the session ROI" % label)
    for key, expected in (
        ("encoded_raster", geometry["encoded_raster"]),
        ("encoded_content_rect", geometry["encoded_content_rect"]),
    ):
        if key in encoded_details and encoded_details[key] != expected:
            raise VerificationError("%s.details.encoded_geometry.%s is not coupled to the session ROI" % (label, key))
    if details.get("encode_profile") != profile or details.get("encode_fps") != roi["encode_fps"]:
        raise VerificationError("%s.details encode profile/fps is not coupled to the session ROI" % label)
    for key, expected in (
        ("gop", profile["gop_length"]),
        ("rate_control_mode", profile["rate_control_mode"]),
        ("quality_value", profile["quality_value"]),
    ):
        if details.get(key) != expected:
            raise VerificationError(
                "%s.details.%s is not coupled to the selected exact encode profile"
                % (label, key)
            )
    _integer(details.get("encode_queue_depth"), "%s.details.encode_queue_depth" % label, True)
    if details.get("expected_shard_gpu_ids") != roi["expected_shard_gpu_ids"]:
        raise VerificationError("%s.details.expected_shard_gpu_ids is not coupled to the session ROI" % label)
    identity = details.get("identity")
    if not isinstance(identity, dict):
        raise VerificationError("%s.details.identity is missing" % label)
    expected_identity = {"recording_id": session["recording_id"], "recording_identity_token": session["recording_identity_token"], "producer_generation": session["producer_generation"], "spatial_roi_plan_sha256": session["spatial_roi_plan_sha256"], "camera_id": session["camera_id"], "camera_serial": session["camera_serial"], "arena_group_id": roi["arena_group_id"], "arena_id": roi["arena_id"], "region_id": roi["region_id"], "roi_id": roi["roi_id"], "logical_stream_id": roi["logical_stream_id"]}
    _same(identity, expected_identity, "%s.details.identity" % label)
    if details.get("artifact_path_scope") != "recording_root_relative" or details.get("artifact_root_relative") != ARTIFACT_ROOT:
        raise VerificationError("%s artifact path scope is not recording-root-relative" % label)
    artifacts = details.get("artifacts")
    if not isinstance(artifacts, dict) or set(artifacts) != set(ARTIFACT_KINDS):
        raise VerificationError("%s.details.artifacts is not the exact twelve-artifact set" % label)
    paths: Dict[str, str] = {}
    for kind in ARTIFACT_KINDS:
        paths[kind] = safe_relative_path(artifacts[kind], "%s artifact %s" % (label, kind))
        if not paths[kind].startswith(ARTIFACT_ROOT + "/"):
            raise VerificationError("%s artifact %s does not lie below external recorder root" % (label, kind))
    for kind in ("video", "metadata", "keyframes", "perf", "summary"):
        if descriptor.get(kind) != paths[kind]:
            raise VerificationError("%s.%s does not match details.artifacts" % (label, kind))
    if not isinstance(details.get("finalized_receipt"), dict):
        raise VerificationError("%s lacks its finalized receipt" % label)
    if descriptor.get("frame_count", 0) <= 0 or descriptor.get("packet_count", 0) <= 0:
        raise VerificationError("%s has no positive frame/packet cardinality" % label)
    for key in ("frame_count", "first_recording_frame_id", "last_recording_frame_id", "recording_frame_id_gaps", "packet_count"):
        _integer(descriptor.get(key), "%s.%s" % (label, key), key in ("frame_count", "packet_count"))
    return paths


def validate_receipt(receipt: Dict[str, Any], descriptor: Dict[str, Any], roi: Dict[str, Any], session: Dict[str, Any], index: int, all_paths: Set[str]) -> List[Dict[str, Any]]:
    label = "receipt[%d]" % index
    _required_keys(receipt, RECEIPT_KEYS, label)
    _same(receipt["logical_stream_id"], roi["logical_stream_id"], label + ".logical_stream_id")
    _canonical_sha(receipt["finalized_receipt_digest"], label + ".finalized_receipt_digest")
    expected_identity = {"recording_id": session["recording_id"], "session_id": session["session_id"], "recording_identity_token": session["recording_identity_token"], "producer_generation": session["producer_generation"], "spatial_roi_plan_sha256": session["spatial_roi_plan_sha256"], "camera_id": session["camera_id"], "camera_serial": session["camera_serial"], "roi_id": roi["roi_id"], "region_id": roi["region_id"], "arena_group_id": roi["arena_group_id"], "logical_stream_id": roi["logical_stream_id"], "assigned_gpu_id": roi["assigned_gpu_id"], "assigned_shard_id": 0}
    _same(receipt["identity"], expected_identity, label + ".identity")
    counts = receipt["counts"]
    _required_keys(counts, COUNT_KEYS, label + ".counts")
    for key in COUNT_KEYS:
        _integer(counts[key], "%s.counts.%s" % (label, key))
    ranges = receipt["ranges"]
    _required_keys(ranges, ("recording_frame_id", "roi_stream_frame_index", "has_frames", "frame_count"), label + ".ranges")
    _bool(ranges["has_frames"], label + ".ranges.has_frames")
    frame_count = _integer(ranges["frame_count"], label + ".ranges.frame_count")
    for key in ("recording_frame_id", "roi_stream_frame_index"):
        _required_keys(ranges[key], ("first", "last"), "%s.ranges.%s" % (label, key))
        _integer(ranges[key]["first"], "%s.%s.first" % (label, key))
        _integer(ranges[key]["last"], "%s.%s.last" % (label, key))
        if ranges[key]["last"] < ranges[key]["first"]:
            raise VerificationError("%s range is reversed" % label)
    expected_keyframes = _expected_keyframe_count(
        frame_count, roi["encode_profile"]["gop_length"]
    )
    if not ranges["has_frames"] or frame_count <= 0:
        raise VerificationError("%s must report a non-empty complete stream" % label)
    if ranges["roi_stream_frame_index"] != {"first": 1, "last": frame_count}:
        raise VerificationError("%s ROI stream index is not dense one-based" % label)
    if ranges["recording_frame_id"]["last"] - ranges["recording_frame_id"]["first"] + 1 != frame_count:
        raise VerificationError("%s recording-frame range is not dense" % label)
    accepted = ("detach_successes", "dispatch_admitted", "ack_attempted", "ack_sent", "ack_accepted", "release_attempted", "release_sent", "encoded_frames", "packet_count")
    for key in accepted:
        if counts[key] != frame_count:
            raise VerificationError("%s count %s does not equal frame_count" % (label, key))
    if counts["keyframes"] != expected_keyframes:
        raise VerificationError(
            "%s count keyframes does not match GOP cadence (expected %d)"
            % (label, expected_keyframes)
        )
    for key in ("dispatch_rejected", "failed_frames", "ack_write_failures", "release_write_failures", "lifecycle_failures"):
        if counts[key] != 0:
            raise VerificationError("%s count %s indicates failure" % (label, key))
    if counts["encoded_bytes"] <= 0:
        raise VerificationError("%s encoded_bytes must be positive" % label)
    if descriptor.get("frame_count") != frame_count or descriptor.get("packet_count") != counts["packet_count"] or descriptor.get("first_recording_frame_id") != ranges["recording_frame_id"]["first"] or descriptor.get("last_recording_frame_id") != ranges["recording_frame_id"]["last"]:
        raise VerificationError("%s descriptor cardinality/range differs from receipt" % label)
    artifacts = receipt["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != len(ARTIFACT_KINDS):
        raise VerificationError("%s must contain exactly twelve artifacts" % label)
    seen_local: Set[str] = set()
    checked: List[Dict[str, Any]] = []
    descriptor_artifacts = descriptor["details"]["artifacts"]
    for expected_kind, artifact in zip(ARTIFACT_KINDS, artifacts):
        _required_keys(artifact, ("kind", "relative_path", "size_bytes", "sha256"), "%s artifact" % label)
        _same(artifact["kind"], expected_kind, "%s artifact kind/order" % label)
        receipt_path = safe_relative_path(artifact["relative_path"], "%s artifact path" % label)
        if receipt_path.startswith(ARTIFACT_ROOT + "/"):
            raise VerificationError("%s receipt path must be relative to external recorder child" % label)
        descriptor_path = ARTIFACT_ROOT + "/" + receipt_path
        _same(descriptor_artifacts[expected_kind], descriptor_path, "%s %s path coupling" % (label, expected_kind))
        _integer(artifact["size_bytes"], "%s %s size" % (label, expected_kind), True)
        _canonical_sha(artifact["sha256"], "%s %s hash" % (label, expected_kind))
        if receipt_path in seen_local or descriptor_path in all_paths:
            raise VerificationError("%s contains duplicate artifact paths" % label)
        seen_local.add(receipt_path)
        all_paths.add(descriptor_path)
        checked.append({"kind": expected_kind, "receipt_path": receipt_path, "descriptor_path": descriptor_path, "size_bytes": artifact["size_bytes"], "sha256": artifact["sha256"]})
    return checked


def validate_receipt_envelope(receipt: Dict[str, Any], session: Dict[str, Any]) -> None:
    _required_keys(receipt, ("schema_id", "schema_version", "canonicalization", "stream_kind", "status", "stream_count", "stream_order", "identity", "root_authority", "streams"), "finalized_session_receipt")
    if receipt["schema_id"] != "orange.spatial_roi_recording.finalized_session_receipt" or receipt["schema_version"] != 1 or receipt["canonicalization"] != "canonical_json_utf8_sort_keys_compact_v1" or receipt["stream_kind"] != "fixed_region" or receipt["status"] != "complete" or receipt["stream_count"] != 4:
        raise VerificationError("finalized session receipt schema/status is invalid")
    if receipt["stream_order"] != session["stream_order"] or not isinstance(receipt["streams"], list) or len(receipt["streams"]) != 4:
        raise VerificationError("finalized session receipt stream order/cardinality is invalid")
    expected = {
        "recording_id": session["recording_id"],
        "session_id": session["session_id"],
        "recording_identity_token": session["recording_identity_token"],
        "producer_generation": session["producer_generation"],
        "spatial_roi_plan_sha256": session["spatial_roi_plan_sha256"],
        "camera_id": session["camera_id"],
        "camera_serial": session["camera_serial"],
        "stream_count": 4,
        "stream_order": session["stream_order"],
    }
    _same(receipt["identity"], expected, "finalized session receipt.identity")


def secure_verify_artifacts(
    root_fd: int,
    receipt_artifacts: List[Dict[str, Any]],
    child_fd: int,
    seen_inodes: Set[Tuple[int, int]],
) -> Dict[str, Tuple[int, int, int, int, int]]:
    identities: Dict[str, Tuple[int, int, int, int, int]] = {}
    for item in receipt_artifacts:
        fd = open_relative(root_fd, item["descriptor_path"])
        receipt_fd = open_relative(child_fd, item["receipt_path"])
        try:
            first = os.fstat(fd)
            second = os.fstat(receipt_fd)
            if (first.st_dev, first.st_ino) != (second.st_dev, second.st_ino):
                raise VerificationError("descriptor/receipt path resolves to different files: %s" % item["descriptor_path"])
            inode = (first.st_dev, first.st_ino)
            if inode in seen_inodes:
                raise VerificationError("artifact files resolve to duplicate inodes: %s" % item["descriptor_path"])
            seen_inodes.add(inode)
            hash_open_fd(fd, item["descriptor_path"], item["size_bytes"], item["sha256"])
            after = os.fstat(fd)
            identities[item["descriptor_path"]] = (
                after.st_dev,
                after.st_ino,
                after.st_size,
                after.st_mtime_ns,
                after.st_ctime_ns,
            )
        finally:
            _close_quietly(fd)
            _close_quietly(receipt_fd)
    return identities


def _validate_complete_encoder_terminal(
    terminal: Any, receipt: Dict[str, Any]
) -> None:
    top_keys = (
        "terminal", "successful", "drain_completed", "metadata_flushed",
        "media_finalization_validated", "artifacts_sealed",
        "all_admitted_results_emitted", "all_enqueue_attempts_accounted",
        "nonempty_stream", "source_release_safe", "source_quarantined",
        "destination_quarantined", "terminal_reason", "counts", "writer",
        "snapshot_schema",
    )
    count_keys = (
        "enqueue_attempted", "enqueued", "dequeued", "rejected",
        "queue_overflows", "copy_completed", "source_releases",
        "encoded_frames", "encoded_packets", "encoded_bytes",
        "copy_failures", "encode_failures", "writer_failures",
        "writer_queue_overflows", "frame_results_emitted", "encoded_results",
        "failed_results", "result_callback_failures", "source_quarantines",
        "destination_quarantines", "peak_queue_depth", "finalize_calls",
        "finalized", "failed", "source_release_safe", "metadata_flushed",
        "media_finalization_validated", "artifacts_sealed",
    )
    writer_keys = (
        "observed", "failure_latched", "packet_write_error_latched",
        "writer_thread_failure_latched", "queue_overflow_latched",
        "close_finalization_validated", "close_finalization_failure_latched",
        "packet_allocation_failures", "packet_enqueue_failures",
        "packet_write_failures", "muxer_flush_failures",
        "sidecar_write_failures", "video_size_limit_failures",
        "thread_failures", "total_failures", "queue_overflow_events",
        "last_error_code", "first_failure_reason",
        "close_finalization_failure_reason", "snapshot_complete",
    )
    _required_keys(terminal, top_keys, "evidence manifest.encoder_terminal")
    counts = terminal["counts"]
    writer = terminal["writer"]
    _required_keys(counts, count_keys, "evidence manifest.encoder_terminal.counts")
    _required_keys(writer, writer_keys, "evidence manifest.encoder_terminal.writer")
    for key in (
        "terminal", "successful", "drain_completed", "metadata_flushed",
        "media_finalization_validated", "artifacts_sealed",
        "all_admitted_results_emitted", "all_enqueue_attempts_accounted",
        "nonempty_stream", "source_release_safe",
    ):
        if terminal[key] is not True:
            raise VerificationError("complete encoder terminal.%s is not true" % key)
    for key in ("source_quarantined", "destination_quarantined"):
        if terminal[key] is not False:
            raise VerificationError("complete encoder terminal.%s is not false" % key)
    if terminal["snapshot_schema"] != "spatial_roi_lossless_terminal_v2" or terminal["terminal_reason"] != "complete":
        raise VerificationError("complete encoder terminal schema/reason is invalid")
    frames = receipt["ranges"]["frame_count"]
    expected_count_values = {
        "enqueue_attempted": frames,
        "enqueued": frames,
        "dequeued": frames,
        "rejected": 0,
        "queue_overflows": 0,
        "copy_completed": frames,
        "source_releases": frames,
        "encoded_frames": frames,
        "encoded_packets": receipt["counts"]["packet_count"],
        "encoded_bytes": receipt["counts"]["encoded_bytes"],
        "copy_failures": 0,
        "encode_failures": 0,
        "writer_failures": 0,
        "writer_queue_overflows": 0,
        "frame_results_emitted": frames,
        "encoded_results": frames,
        "failed_results": 0,
        "result_callback_failures": 0,
        "source_quarantines": 0,
        "destination_quarantines": 0,
    }
    for key, expected in expected_count_values.items():
        _same(counts[key], expected, "complete encoder terminal.counts.%s" % key)
    _integer(counts["peak_queue_depth"], "complete encoder peak queue depth", True)
    _integer(counts["finalize_calls"], "complete encoder finalize calls", True)
    for key in (
        "finalized", "source_release_safe", "metadata_flushed",
        "media_finalization_validated", "artifacts_sealed",
    ):
        if counts[key] is not True:
            raise VerificationError("complete encoder count flag %s is not true" % key)
    if counts["failed"] is not False:
        raise VerificationError("complete encoder count flag failed is true")
    for key in ("observed", "close_finalization_validated", "snapshot_complete"):
        if writer[key] is not True:
            raise VerificationError("complete encoder writer.%s is not true" % key)
    for key in (
        "failure_latched", "packet_write_error_latched",
        "writer_thread_failure_latched", "queue_overflow_latched",
        "close_finalization_failure_latched",
    ):
        if writer[key] is not False:
            raise VerificationError("complete encoder writer.%s is not false" % key)
    for key in (
        "packet_allocation_failures", "packet_enqueue_failures",
        "packet_write_failures", "muxer_flush_failures",
        "sidecar_write_failures", "video_size_limit_failures",
        "thread_failures", "total_failures", "queue_overflow_events",
        "last_error_code",
    ):
        if writer[key] != 0:
            raise VerificationError("complete encoder writer.%s is nonzero" % key)
    if writer["first_failure_reason"] != "" or writer["close_finalization_failure_reason"] != "":
        raise VerificationError("complete encoder writer contains a failure reason")


def _expected_evidence_binding(
    contract: Dict[str, Any],
    verified_plan: Dict[str, Any],
    session: Dict[str, Any],
    roi: Dict[str, Any],
    limits: Dict[str, int],
) -> Dict[str, Any]:
    stream_id = roi["logical_stream_id"]
    stream = contract["streams"][stream_id]
    artifact_root = contract["artifact_root"].rstrip("/")
    relative_artifacts: Dict[str, str] = {}
    for kind in ARTIFACT_KINDS:
        absolute = stream["expected_artifacts"][kind]
        prefix = artifact_root + "/"
        if not isinstance(absolute, str) or not absolute.startswith(prefix):
            raise VerificationError("contract artifact is not below its artifact root")
        relative_artifacts[kind] = safe_relative_path(
            absolute[len(prefix):], "evidence binding expected artifact"
        )
    return {
        "contract": {
            "schema_id": contract["schema_id"],
            "schema_version": contract["schema_version"],
            "sha256": canonical_json_sha256(contract),
            "mode": contract["mode"],
        },
        "plan": {
            "schema_id": verified_plan["schema_id"],
            "schema_version": verified_plan["schema_version"],
            "sha256": verified_plan["plan_sha256"],
        },
        "recording": {
            "recording_id": session["recording_id"],
            "session_id": session["session_id"],
            "recording_identity_token": session["recording_identity_token"],
            "producer_generation": session["producer_generation"],
        },
        "camera": {
            "camera_id": session["camera_id"],
            "camera_serial": session["camera_serial"],
            "analytics_gpu_id": roi["analytics_gpu_id"],
            "source_gpu_id": roi["source_gpu_id"],
        },
        "stream": {
            "roi_id": roi["roi_id"],
            "region_id": roi["region_id"],
            "arena_group_id": roi["arena_group_id"],
            "arena_id": roi["arena_id"],
            "has_arena_id": roi["arena_id"] is not None,
            "logical_stream_id": stream_id,
            "routing_policy": stream["routing_policy"],
        },
        "geometry": roi["geometry"],
        "gpu": {
            "recorder_gpu_id": roi["recorder_gpu_id"],
            "assigned_gpu_id": roi["assigned_gpu_id"],
            "assigned_shard_id": 0,
            "routing_policy": stream["routing_policy"],
        },
        "encode_profile": roi["encode_profile"],
        "roots": {
            "recording_root": contract["recording_root"],
            "artifact_root": contract["artifact_root"],
        },
        "limits": limits,
        "expected_artifacts": relative_artifacts,
    }


def verify_evidence_manifest_receipt_digest(
    root_fd: int,
    receipt: Dict[str, Any],
    descriptor: Dict[str, Any],
    session: Dict[str, Any],
    roi: Dict[str, Any],
    contract: Dict[str, Any],
    verified_plan: Dict[str, Any],
    limits: Dict[str, int],
) -> None:
    artifact = next(
        (
            value
            for value in receipt["artifacts"]
            if value.get("kind") == "evidence_manifest"
        ),
        None,
    )
    if not isinstance(artifact, dict):
        raise VerificationError("receipt lacks its evidence_manifest artifact")
    relative = descriptor["details"]["artifacts"]["evidence_manifest"]
    fd = open_relative(root_fd, relative)
    try:
        hash_open_fd(
            fd, relative, artifact["size_bytes"], artifact["sha256"]
        )
        document = read_open_json_fd(fd, relative)
    finally:
        _close_quietly(fd)
    if not isinstance(document, dict):
        raise VerificationError("evidence manifest is not a JSON object")
    _required_keys(document, EVIDENCE_MANIFEST_KEYS, "evidence manifest")
    if (
        document["schema_id"] != "orange.spatial_roi_recorder.finalized_manifest"
        or document["schema_version"] != 2
        or document["canonicalization"] != "canonical_json_utf8_sort_keys_compact_v1"
        or document["stream_kind"] != "fixed_region"
    ):
        raise VerificationError("evidence manifest schema/kind is invalid")
    _same(
        document["binding"],
        _expected_evidence_binding(
            contract, verified_plan, session, roi, limits
        ),
        "evidence manifest authenticated binding",
    )
    receipt_by_kind = {value["kind"]: value for value in receipt["artifacts"]}
    expected_evidence = {
        key: receipt_by_kind["evidence"][key]
        for key in ("relative_path", "size_bytes", "sha256")
    }
    _same(document["evidence"], expected_evidence, "evidence manifest evidence reference")
    expected_finalize_artifacts = {
        kind: {
            key: receipt_by_kind[kind][key]
            for key in ("relative_path", "size_bytes", "sha256")
        }
        for kind in FINALIZE_ARTIFACT_KINDS
    }
    _same(
        document["artifacts"],
        expected_finalize_artifacts,
        "evidence manifest finalized artifact references",
    )
    _same(document["counts"], receipt["counts"], "evidence manifest counts")
    _same(document["ranges"], receipt["ranges"], "evidence manifest ranges")
    _same(
        document["terminal"],
        {"state": "complete", "reason": "complete"},
        "evidence manifest terminal state",
    )
    _validate_complete_encoder_terminal(document["encoder_terminal"], receipt)
    finalize_payload = {
        "terminal_state": "complete",
        "terminal_reason": "complete",
        "artifacts": {
            kind: expected_finalize_artifacts[kind]["relative_path"]
            for kind in FINALIZE_ARTIFACT_KINDS
        },
        "encoder_terminal": document["encoder_terminal"],
    }
    _same(
        document["finalize_request_sha256"],
        canonical_json_sha256(finalize_payload),
        "evidence manifest finalization request digest",
    )
    supplied = _canonical_sha(
        document.get("finalized_receipt_digest"),
        "evidence manifest finalized_receipt_digest",
    )
    _same(
        receipt["finalized_receipt_digest"],
        supplied,
        "session/evidence-manifest finalized receipt digest",
    )
    payload = dict(document)
    del payload["finalized_receipt_digest"]
    expected = canonical_json_sha256(payload)
    _same(supplied, expected, "evidence manifest canonical receipt digest")


def validate_complete_frame_coverage(
    session: Dict[str, Any], descriptors: Dict[str, Any], full: Optional[Dict[str, Any]]
) -> Tuple[int, int, int]:
    shared: Optional[Tuple[int, int, int]] = None
    for index, stream_id in enumerate(session["stream_order"]):
        receipt = session["finalized_session_receipt"]["streams"][index]
        ranges = receipt["ranges"]
        coverage = (
            ranges["frame_count"],
            ranges["recording_frame_id"]["first"],
            ranges["recording_frame_id"]["last"],
        )
        if shared is None:
            shared = coverage
        elif coverage != shared:
            raise VerificationError(
                "complete ROI streams do not share one source-frame coverage"
            )
        descriptor = descriptors[stream_id]
        if (
            descriptor["frame_count"],
            descriptor["first_recording_frame_id"],
            descriptor["last_recording_frame_id"],
        ) != coverage:
            raise VerificationError(
                "ROI descriptor coverage differs from its finalized receipt"
            )
    if shared is None:
        raise VerificationError("complete session contains no ROI coverage")
    if full is not None:
        full_coverage = (
            _integer(full.get("frame_count"), "full-frame frame_count", True),
            _integer(
                full.get("first_recording_frame_id"),
                "full-frame first recording frame",
                True,
            ),
            _integer(
                full.get("last_recording_frame_id"),
                "full-frame last recording frame",
                True,
            ),
        )
        if full.get("recording_frame_id_gaps") != 0 or full_coverage != shared:
            raise VerificationError(
                "first-class full-frame and four ROI products do not share exact frame coverage"
            )
    validate_complete_producer_status(session, shared[0])
    return shared


def secure_verify_full_metadata_coverage(
    root_fd: int, full: Dict[str, Any], expected: Tuple[int, int, int]
) -> None:
    relative = safe_relative_path(
        full.get("metadata"), "full-frame metadata path"
    )
    fd = open_relative(root_fd, relative)
    try:
        before = os.fstat(fd)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_nlink != 1
            or before.st_size <= 0
        ):
            raise VerificationError(
                "full-frame metadata is not a unique non-empty regular file"
            )
        text = io.TextIOWrapper(
            os.fdopen(os.dup(fd), "rb"), encoding="utf-8", newline=""
        )
        try:
            header_line = text.readline()
            if header_line == "":
                raise VerificationError("full-frame metadata has no header")
            header_line = header_line.rstrip("\n")
            if header_line.endswith("\r"):
                header_line = header_line[:-1]
            header = header_line.split(",")
            if (
                not header
                or any(column == "" for column in header)
                or len(header) != len(set(header))
            ):
                raise VerificationError(
                    "full-frame metadata has empty or duplicate columns"
                )
            frame_index = (
                header.index("frame_id") if "frame_id" in header else None
            )
            recording_index = (
                header.index("recording_frame_id")
                if "recording_frame_id" in header
                else None
            )
            if frame_index is None and recording_index is None:
                raise VerificationError(
                    "full-frame metadata lacks a frame identity column"
                )
            canonical_index = (
                recording_index if recording_index is not None else frame_index
            )
            count = 0
            first = 0
            previous = 0
            for line in text:
                line = line.rstrip("\n")
                if line.endswith("\r"):
                    line = line[:-1]
                row = line.split(",")
                if len(row) != len(header):
                    raise VerificationError(
                        "full-frame metadata contains a malformed row"
                    )
                canonical_text = row[canonical_index]
                frame_text = (
                    row[frame_index]
                    if frame_index is not None
                    else canonical_text
                )
                if (
                    not canonical_text
                    or not frame_text
                    or any(char < "0" or char > "9" for char in canonical_text)
                    or any(char < "0" or char > "9" for char in frame_text)
                ):
                    raise VerificationError(
                        "full-frame metadata contains a non-canonical frame identity"
                    )
                recording_frame_id = int(canonical_text, 10)
                frame_id = int(frame_text, 10)
                if (
                    frame_id <= 0
                    or recording_frame_id <= 0
                    or (
                        frame_index is not None
                        and recording_index is not None
                        and frame_id != recording_frame_id
                    )
                    or (count != 0 and recording_frame_id != previous + 1)
                ):
                    raise VerificationError(
                        "full-frame metadata canonical identities are not dense or aliases disagree"
                    )
                if count == 0:
                    first = recording_frame_id
                previous = recording_frame_id
                count += 1
        except UnicodeDecodeError as exc:
            raise VerificationError("full-frame metadata is not UTF-8") from exc
        finally:
            text.close()
        after = os.fstat(fd)
        if (before.st_dev, before.st_ino, before.st_size) != (
            after.st_dev,
            after.st_ino,
            after.st_size,
        ):
            raise VerificationError("full-frame metadata changed during read")
    finally:
        _close_quietly(fd)
    observed = (count, first, previous)
    if observed != expected:
        raise VerificationError(
            "full-frame metadata coverage does not match full/ROI descriptors"
        )


def validate_receipt_budget_coverage(
    artifacts: List[Dict[str, Any]], preflight: Dict[str, Any]
) -> None:
    media_bytes = sum(
        item["size_bytes"] for item in artifacts if item["kind"] == "video"
    )
    evidence_bytes = sum(
        item["size_bytes"] for item in artifacts if item["kind"] != "video"
    )
    if media_bytes > preflight["budgets"]["max_media_bytes_total"]:
        raise VerificationError(
            "finalized ROI media artifacts exceed authenticated preflight budget"
        )
    if evidence_bytes > preflight["budgets"]["max_evidence_bytes_total"]:
        raise VerificationError(
            "finalized ROI evidence artifacts exceed authenticated preflight budget"
        )


def validate_per_stream_receipt_budget(
    artifacts: List[Dict[str, Any]], receipt: Dict[str, Any], limits: Dict[str, int]
) -> None:
    media_bytes = sum(
        item["size_bytes"] for item in artifacts if item["kind"] == "video"
    )
    evidence_bytes = sum(
        item["size_bytes"] for item in artifacts if item["kind"] != "video"
    )
    if media_bytes > limits["max_media_bytes_per_stream"]:
        raise VerificationError(
            "finalized ROI media artifacts exceed authenticated per-stream budget"
        )
    if evidence_bytes > limits["max_evidence_bytes_per_stream"]:
        raise VerificationError(
            "finalized ROI evidence artifacts exceed authenticated per-stream budget"
        )
    if (
        _integer(
            receipt.get("ranges", {}).get("frame_count"),
            "finalized ROI receipt frame_count",
            True,
        )
        > limits["max_frames_per_stream"]
    ):
        raise VerificationError(
            "finalized ROI frame count exceeds authenticated per-stream budget"
        )


def validate_combined_storage_preflight(
    root_fd: int,
    backend: Dict[str, Any],
    contract: Dict[str, Any],
    recording_root: str,
    camera_serial: str,
    seen_paths: Set[str],
    seen_inodes: Set[Tuple[int, int]],
) -> None:
    if backend.get("mode") != "external_ipc":
        if "combined_storage_preflight" in backend:
            raise VerificationError(
                "in-process full-frame backend must not claim combined external storage admission"
            )
        return

    relative = safe_relative_path(
        backend.get("combined_storage_preflight"),
        "combined storage preflight path",
    )
    if relative != "combined_storage_preflight.json":
        raise VerificationError(
            "combined storage preflight is not the canonical recording-root artifact"
        )
    if relative in seen_paths:
        raise VerificationError("combined storage preflight path aliases another authority")
    fd = open_relative(root_fd, relative)
    try:
        before = os.fstat(fd)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_nlink != 1
            or before.st_size <= 0
        ):
            raise VerificationError(
                "combined storage preflight is not a unique non-empty regular file"
            )
        inode = (before.st_dev, before.st_ino)
        if inode in seen_inodes:
            raise VerificationError(
                "combined storage preflight aliases another authority inode"
            )
        evidence = read_open_json_fd(fd, relative)
    finally:
        _close_quietly(fd)
    seen_paths.add(relative)
    seen_inodes.add(inode)

    top_keys = (
        "schema_id", "schema_version", "checked", "ok", "hard_guarantee",
        "status", "error", "summary", "policy", "streams", "filesystems",
        "product", "recording_root", "full_frame_artifact_root",
        "spatial_roi_bounds", "capacity_binding",
    )
    _required_keys(evidence, top_keys, "combined storage preflight")
    if (
        evidence["schema_id"] != "orange.combined_storage_preflight"
        or evidence["schema_version"] != 1
        or evidence["checked"] is not True
        or evidence["ok"] is not True
        or evidence["hard_guarantee"] is not True
        or evidence["status"] != "pass"
        or evidence["error"] != ""
        or evidence["product"] != "full_frame_plus_spatial_roi"
        or evidence["recording_root"] != recording_root
    ):
        raise VerificationError(
            "combined storage preflight identity/status is not a passed hard admission"
        )
    expected_full_root = os.path.join(recording_root, "external_recorder")
    if (
        evidence["full_frame_artifact_root"] != expected_full_root
        or backend.get("artifact_root") != expected_full_root
    ):
        raise VerificationError(
            "combined storage preflight full-frame artifact root is substituted"
        )

    aggregate = contract.get("aggregate_bounds")
    storage_policy = contract.get("storage_preflight_policy")
    if not isinstance(aggregate, dict) or not isinstance(storage_policy, dict):
        raise VerificationError(
            "recorder contract lacks combined-storage source bounds"
        )
    expected_roi_bounds = {
        "max_media_bytes_total": aggregate.get("max_media_bytes_total"),
        "max_evidence_bytes_total": aggregate.get("max_evidence_bytes_total"),
        "reserved_free_bytes": storage_policy.get("reserved_free_bytes"),
    }
    _same(
        evidence["spatial_roi_bounds"],
        expected_roi_bounds,
        "combined storage preflight spatial ROI bounds",
    )
    roi_total = _integer(
        expected_roi_bounds["max_media_bytes_total"],
        "combined storage ROI media bound",
        True,
    ) + _integer(
        expected_roi_bounds["max_evidence_bytes_total"],
        "combined storage ROI evidence bound",
        True,
    )

    streams = evidence["streams"]
    stream_keys = (
        "plan_artifact_root", "stream_id", "camera_serial", "stream_kind",
        "output_kind", "output_path", "filesystem_key", "rate_basis",
        "duration_seconds", "duration_hours", "encode_fps",
        "conservative_rate_bps", "video_bytes", "metadata_bytes",
        "retained_copy_multiplier", "peak_copy_multiplier",
        "estimated_retained_bytes", "estimated_peak_bytes", "bounded", "error",
    )
    if not isinstance(streams, list) or len(streams) != 2:
        raise VerificationError(
            "combined storage preflight must contain one full and one aggregate ROI stream"
        )
    for index, stream in enumerate(streams):
        _required_keys(stream, stream_keys, "combined storage stream[%d]" % index)
        if (
            stream["camera_serial"] != camera_serial
            or stream["bounded"] is not True
            or stream["error"] != ""
            or not isinstance(stream["filesystem_key"], str)
            or not stream["filesystem_key"]
        ):
            raise VerificationError(
                "combined storage preflight contains an invalid/unbounded stream"
            )
        for key in (
            "duration_seconds", "encode_fps", "conservative_rate_bps",
            "video_bytes", "metadata_bytes", "retained_copy_multiplier",
            "peak_copy_multiplier", "estimated_retained_bytes",
            "estimated_peak_bytes",
        ):
            _integer(stream[key], "combined storage stream[%d].%s" % (index, key))
    roi_streams = [
        stream for stream in streams
        if stream["stream_id"] == "spatial_roi_aggregate_bound"
    ]
    if len(roi_streams) != 1:
        raise VerificationError(
            "combined storage preflight lacks its unique aggregate ROI bound"
        )
    roi_stream = roi_streams[0]
    if (
        roi_stream["plan_artifact_root"] != recording_root
        or roi_stream["stream_kind"] != "spatial_roi"
        or roi_stream["output_kind"] != "spatial_roi"
        or roi_stream["duration_seconds"] != 1
        or roi_stream["encode_fps"] != 1
        or roi_stream["rate_basis"] != "configured_max_bitrate_bps"
        or roi_stream["conservative_rate_bps"] != roi_total * 8
        or roi_stream["video_bytes"] != roi_total
        or roi_stream["metadata_bytes"] != 0
        or roi_stream["retained_copy_multiplier"] != 1
        or roi_stream["peak_copy_multiplier"] != 1
        or roi_stream["estimated_retained_bytes"] != roi_total
        or roi_stream["estimated_peak_bytes"] != roi_total
        or roi_stream["output_path"]
        != os.path.join(
            recording_root,
            ARTIFACT_ROOT,
            "combined_storage_preflight_probe.mp4",
        )
    ):
        raise VerificationError(
            "combined storage preflight aggregate ROI stream is not the authenticated exact bound"
        )
    full_stream = next(stream for stream in streams if stream is not roi_stream)
    if (
        full_stream["plan_artifact_root"] != expected_full_root
        or full_stream["stream_kind"] != "full_frame"
        or full_stream["output_kind"] != "full"
    ):
        raise VerificationError(
            "combined storage preflight full-frame stream identity is invalid"
        )

    filesystems = evidence["filesystems"]
    filesystem_keys = (
        "filesystem_key", "probe_path", "capacity_bytes", "available_bytes",
        "estimated_retained_bytes", "estimated_peak_bytes",
        "safety_headroom_bytes", "reserved_free_bytes",
        "configured_min_free_bytes", "required_available_bytes",
        "projected_available_after_bytes", "ok", "error",
    )
    if not isinstance(filesystems, list) or len(filesystems) != 1:
        raise VerificationError(
            "combined storage preflight must bind one shared filesystem"
        )
    filesystem = filesystems[0]
    _required_keys(filesystem, filesystem_keys, "combined storage filesystem")
    if (
        filesystem["ok"] is not True
        or filesystem["error"] != ""
        or filesystem["filesystem_key"] != roi_stream["filesystem_key"]
        or any(
            stream["filesystem_key"] != filesystem["filesystem_key"]
            for stream in streams
        )
    ):
        raise VerificationError(
            "combined storage preflight streams do not share one passed filesystem"
        )
    for key in filesystem_keys[2:-2]:
        _integer(filesystem[key], "combined storage filesystem.%s" % key)
    retained = sum(stream["estimated_retained_bytes"] for stream in streams)
    peak = sum(stream["estimated_peak_bytes"] for stream in streams)

    policy = evidence["policy"]
    policy_keys = (
        "enabled", "safety_headroom_ratio", "reserved_free_bytes",
        "metadata_bytes_per_frame", "raw_nv12_expansion_ratio",
        "require_finite_duration", "planned_duration_seconds",
    )
    _required_keys(policy, policy_keys, "combined storage policy")
    if (
        policy["enabled"] is not True
        or not isinstance(policy["safety_headroom_ratio"], (int, float))
        or isinstance(policy["safety_headroom_ratio"], bool)
        or policy["safety_headroom_ratio"] < 0
        or policy["reserved_free_bytes"]
        < expected_roi_bounds["reserved_free_bytes"]
    ):
        raise VerificationError("combined storage preflight policy is invalid")
    safety = math.ceil(peak * policy["safety_headroom_ratio"])
    required = max(
        peak + safety + policy["reserved_free_bytes"],
        filesystem["configured_min_free_bytes"],
    )
    if (
        filesystem["estimated_retained_bytes"] != retained
        or filesystem["estimated_peak_bytes"] != peak
        or filesystem["safety_headroom_bytes"] != safety
        or filesystem["reserved_free_bytes"] != policy["reserved_free_bytes"]
        or filesystem["required_available_bytes"] != required
        or filesystem["available_bytes"] < required
        or filesystem["projected_available_after_bytes"]
        != filesystem["available_bytes"] - peak
    ):
        raise VerificationError(
            "combined storage preflight filesystem arithmetic is inconsistent"
        )

    summary = evidence["summary"]
    summary_keys = (
        "requested_duration_seconds", "requested_duration_hours", "camera_count",
        "stream_count", "full_frame_stream_count", "crop_stream_count",
        "spatial_roi_aggregate_stream_count", "filesystem_count",
        "aggregate_estimated_retained_bytes", "aggregate_estimated_peak_bytes",
        "aggregate_required_available_bytes",
    )
    _required_keys(summary, summary_keys, "combined storage preflight summary")
    if (
        summary["camera_count"] != 1
        or summary["stream_count"] != 2
        or summary["full_frame_stream_count"] != 1
        or summary["crop_stream_count"] != 0
        or summary["spatial_roi_aggregate_stream_count"] != 1
        or summary["filesystem_count"] != 1
        or summary["aggregate_estimated_retained_bytes"] != retained
        or summary["aggregate_estimated_peak_bytes"] != peak
        or summary["aggregate_required_available_bytes"] != required
    ):
        raise VerificationError(
            "combined storage preflight summary is inconsistent"
        )
    binding = evidence["capacity_binding"]
    _required_keys(
        binding,
        ("filesystem_key", "full_frame_and_roi_summed", "single_shared_filesystem"),
        "combined storage preflight capacity binding",
    )
    if (
        binding["filesystem_key"] != filesystem["filesystem_key"]
        or binding["full_frame_and_roi_summed"] is not True
        or binding["single_shared_filesystem"] is not True
    ):
        raise VerificationError(
            "combined storage preflight capacity binding is invalid"
        )


def secure_register_full_frame_artifacts(
    root_fd: int,
    full: Dict[str, Any],
    seen_paths: Set[str],
    seen_inodes: Set[Tuple[int, int]],
) -> Dict[str, Tuple[int, int, int, int, int]]:
    identities: Dict[str, Tuple[int, int, int, int, int]] = {}
    for kind in ("video", "metadata", "keyframes"):
        relative = safe_relative_path(
            full.get(kind), "full-frame %s path" % kind
        )
        if relative in seen_paths:
            raise VerificationError(
                "full-frame %s path aliases an authority or ROI artifact" % kind
            )
        fd = open_relative(root_fd, relative)
        try:
            st = os.fstat(fd)
            inode = (st.st_dev, st.st_ino)
            if (
                not stat.S_ISREG(st.st_mode)
                or st.st_nlink != 1
                or st.st_size <= 0
            ):
                raise VerificationError(
                    "full-frame %s is not a unique non-empty regular file" % kind
                )
            if inode in seen_inodes:
                raise VerificationError(
                    "full-frame %s aliases an authority or ROI artifact inode" % kind
                )
            seen_paths.add(relative)
            seen_inodes.add(inode)
            identities[relative] = (
                st.st_dev,
                st.st_ino,
                st.st_size,
                st.st_mtime_ns,
                st.st_ctime_ns,
            )
        finally:
            _close_quietly(fd)
    return identities


def secure_verify_full_frame(
    root_fd: int,
    full: Dict[str, Any],
    session: Dict[str, Any],
    expected_identity: Tuple[int, int, int, int, int],
) -> Tuple[int, int]:
    path = safe_relative_path(full.get("video"), "full-frame video path")
    frame_count = _integer(full.get("frame_count"), "full-frame frame_count", True)
    if full.get("camera_serial") != session["camera_serial"] or full.get("status") != "finalized":
        raise VerificationError("full-frame descriptor identity/status is invalid")
    fd = open_relative(root_fd, path)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1 or st.st_size <= 0:
            raise VerificationError("full-frame video is not a unique non-empty regular file")
        observed_identity = (
            st.st_dev,
            st.st_ino,
            st.st_size,
            st.st_mtime_ns,
            st.st_ctime_ns,
        )
        if observed_identity != expected_identity:
            raise VerificationError(
                "full-frame video changed between registration and decode"
            )
        return frame_count, fd
    except Exception:
        _close_quietly(fd)
        raise


def ffprobe_video(ffprobe: str, fd: int, expected_width: int, expected_height: int, expected_frames: int, label: str) -> Dict[str, Any]:
    command = [ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=codec_type,width,height,nb_read_frames,nb_frames", "-of", "json", "/proc/self/fd/%d" % fd]
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=False, pass_fds=(fd,))
    except OSError as exc:
        raise VerificationError("%s ffprobe failed to start: %s" % (label, exc)) from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace")[-600:]
        raise VerificationError("%s ffprobe decode failed: %s" % (label, detail.strip()))
    parsed = parse_json_bytes(result.stdout, label + " ffprobe output")
    streams = parsed.get("streams") if isinstance(parsed, dict) else None
    if not isinstance(streams, list) or len(streams) != 1 or not isinstance(streams[0], dict):
        raise VerificationError("%s ffprobe did not report exactly one video stream" % label)
    stream = streams[0]
    if stream.get("codec_type") != "video" or stream.get("width") != expected_width or stream.get("height") != expected_height:
        raise VerificationError("%s decoded raster/stream does not match contract" % label)
    raw_frames = stream.get("nb_read_frames", stream.get("nb_frames"))
    try:
        decoded_frames = int(raw_frames)
    except (TypeError, ValueError) as exc:
        raise VerificationError("%s ffprobe did not report a decoded frame count" % label) from exc
    if decoded_frames != expected_frames:
        raise VerificationError("%s decoded frame count %d differs from expected %d" % (label, decoded_frames, expected_frames))
    return {"width": expected_width, "height": expected_height, "frames": decoded_frames}


def verify(folder: str, ffprobe: Optional[str] = None, require_ffprobe: bool = False) -> Dict[str, Any]:
    summary: Dict[str, Any] = {"status": "fail", "recording_folder": os.path.abspath(folder), "checks": [], "warnings": [], "errors": [], "streams": []}
    root_fd: Optional[int] = None
    try:
        root_fd = open_root(folder)
        manifest = secure_json(root_fd, "recording_session.json")
        snapshot = secure_json(root_fd, "recording_snapshot.json")
        if not isinstance(manifest, dict) or not isinstance(snapshot, dict):
            raise VerificationError("recording_session.json and recording_snapshot.json must be JSON objects")
        session = validate_session_shape(snapshot.get("session", {}).get("spatial_roi_recording") if isinstance(snapshot.get("session"), dict) else None)
        all_paths: Set[str] = set()
        all_inodes: Set[Tuple[int, int]] = set()
        authority_documents = secure_verify_session_authorities(
            root_fd, session, all_paths, all_inodes
        )
        descriptors, full = validate_manifest_and_descriptors(
            manifest, snapshot, session, os.path.abspath(folder)
        )
        backend_media_policy = (
            manifest["recording_backend"].get("media_policy")
            if isinstance(manifest.get("recording_backend"), dict)
            else None
        )
        roi_only = (
            backend_media_policy is not None
            and validate_media_policy_object(
                backend_media_policy, "recording_backend.media_policy"
            ) == FIXED_ROIS_WITH_REGISTERED_CONTEXT
        )
        summary["checks"].extend([
            "required_manifests", "session_schema_v3", "authority_artifact_hashes",
            "descriptor_session_coupling",
        ])
        if roi_only:
            secure_verify_registered_context(
                root_fd,
                manifest["recording_backend"],
                snapshot["session"],
                session,
                all_paths,
                all_inodes,
            )
            summary["checks"].extend([
                "fixed_roi_registered_context_media_policy",
                "full_frame_omitted_by_media_policy",
                "registered_scene_context_descriptor",
                "registered_scene_context_mono8_bytes",
            ])
        else:
            summary["checks"].append("full_frame_first_class")
        preflight = validate_required_process_preflight(session)
        contract_root = authority_documents["recorder_contract"].get(
            "recording_root"
        )
        _same(
            contract_root,
            os.path.abspath(folder),
            "recorder contract recording root",
        )
        _same(
            authority_documents["recorder_contract"].get("artifact_root"),
            os.path.join(contract_root, ARTIFACT_ROOT),
            "recorder contract artifact root",
        )
        recording_limits = validate_authenticated_authorities(
            authority_documents, session, preflight, descriptors
        )
        validate_combined_storage_preflight(
            root_fd,
            manifest["recording_backend"],
            authority_documents["recorder_contract"],
            os.path.abspath(folder),
            session["camera_serial"],
            all_paths,
            all_inodes,
        )
        summary["checks"].extend(
            ["storage_preflight", "authenticated_plan_contract_budgets"]
        )
        if manifest["recording_backend"].get("mode") == "external_ipc":
            summary["checks"].append("combined_full_roi_storage_preflight")
        validate_receipt_envelope(session["finalized_session_receipt"], session)
        flattened: List[Dict[str, Any]] = []
        for index, stream_id in enumerate(session["stream_order"]):
            roi = session["rois"][index]
            descriptor = descriptors[stream_id]
            paths = validate_descriptor(descriptor, roi, session, index)
            receipt = session["finalized_session_receipt"]["streams"][index]
            if session["finalized_session_receipt"].get("stream_order") != session["stream_order"]:
                raise VerificationError("finalized session receipt stream order is substituted")
            if descriptor["details"].get("finalized_receipt") != receipt:
                raise VerificationError("descriptor finalized receipt is not exact receipt object")
            stream_artifacts = validate_receipt(
                receipt, descriptor, roi, session, index, all_paths
            )
            validate_per_stream_receipt_budget(
                stream_artifacts, receipt, recording_limits
            )
            flattened.extend(stream_artifacts)
            verify_evidence_manifest_receipt_digest(
                root_fd,
                receipt,
                descriptor,
                session,
                roi,
                authority_documents["recorder_contract"],
                authority_documents["verified_plan"],
                recording_limits,
            )
            summary["streams"].append({"logical_stream_id": stream_id, "frame_count": receipt["ranges"]["frame_count"], "artifact_count": len(ARTIFACT_KINDS)})
        if len(flattened) != 4 * len(ARTIFACT_KINDS):
            raise VerificationError("accepted artifact set is not exactly 12 artifacts x 4 streams")
        shared_coverage = validate_complete_frame_coverage(
            session, descriptors, full
        )
        if full is not None:
            secure_verify_full_metadata_coverage(root_fd, full, shared_coverage)
        validate_receipt_budget_coverage(flattened, preflight)
        child_fd = open_relative(root_fd, ARTIFACT_ROOT, want_directory=True)
        try:
            child_stat = os.fstat(child_fd)
            root_stat = os.fstat(root_fd)
            root_authority = session["finalized_session_receipt"].get("root_authority")
            _required_keys(root_authority, ("artifact_root_relative", "recording_root_identity", "artifact_root_identity", "root_continuity"), "receipt.root_authority")
            if root_authority["artifact_root_relative"] != ARTIFACT_ROOT:
                raise VerificationError("receipt artifact root authority is substituted")
            for key, actual in (("recording_root_identity", root_stat), ("artifact_root_identity", child_stat)):
                identity = root_authority[key]
                _required_keys(identity, ("device", "inode"), "receipt.root_authority.%s" % key)
                if identity["device"] != actual.st_dev or identity["inode"] != actual.st_ino:
                    raise VerificationError("receipt root identity does not match opened directory")
            continuity = root_authority["root_continuity"]
            _required_keys(continuity, ("proven", "not_proven"), "receipt.root_authority.root_continuity")
            for key in ("proven", "not_proven"):
                if not isinstance(continuity[key], list) or any(
                    not isinstance(statement, str) or not statement
                    for statement in continuity[key]
                ):
                    raise VerificationError("receipt root continuity contains an invalid statement")
            roi_artifact_identities = secure_verify_artifacts(
                root_fd, flattened, child_fd, all_inodes
            )
        finally:
            _close_quietly(child_fd)
        full_artifact_identities: Dict[str, Tuple[int, int, int, int, int]] = {}
        full_fd: Optional[int] = None
        full_video_path: Optional[str] = None
        full_frame_count = 0
        if full is not None:
            full_artifact_identities = secure_register_full_frame_artifacts(
                root_fd, full, all_paths, all_inodes
            )
            full_video_path = safe_relative_path(
                full.get("video"), "full-frame video path"
            )
            full_frame_count, full_fd = secure_verify_full_frame(
                root_fd,
                full,
                session,
                full_artifact_identities[full_video_path],
            )
        summary_checks = [
            "receipt_identity_and_ranges", "cross_product_frame_coverage",
            "producer_completion", "receipt_digest_recomputation",
            "artifact_paths_and_hashes", "receipt_budget_coverage",
        ]
        if full is not None:
            summary_checks.append("full_frame_metadata_coverage")
        summary["checks"].extend(summary_checks)
        receipt_artifacts_by_path = {
            item["descriptor_path"]: item for item in flattened
        }
        roi_video_fds: List[Tuple[str, int, int, int, str]] = []
        try:
            for index, stream_id in enumerate(session["stream_order"]):
                descriptor = descriptors[stream_id]
                roi_video_path = descriptor["details"]["artifacts"]["video"]
                fd = open_relative(root_fd, roi_video_path)
                st = os.fstat(fd)
                observed_identity = (
                    st.st_dev,
                    st.st_ino,
                    st.st_size,
                    st.st_mtime_ns,
                    st.st_ctime_ns,
                )
                if (
                    not stat.S_ISREG(st.st_mode)
                    or st.st_nlink != 1
                    or st.st_size <= 0
                    or observed_identity != roi_artifact_identities[roi_video_path]
                ):
                    _close_quietly(fd)
                    raise VerificationError(
                        "ROI %s video changed between receipt hashing and decode"
                        % stream_id
                    )
                geometry = session["rois"][index]["geometry"]["encoded_raster"]
                roi_video_fds.append(
                    (
                        stream_id,
                        fd,
                        geometry["width"],
                        geometry["height"],
                        roi_video_path,
                    )
                )
            probe = ffprobe or shutil.which("ffprobe")
            if probe is None:
                if require_ffprobe:
                    raise VerificationError("ffprobe is required but unavailable")
                summary["warnings"].append("ffprobe unavailable; media decode checks skipped")
            else:
                if full is not None and full_fd is not None:
                    summary["full_frame_video"] = ffprobe_video(probe, full_fd, session["native_raster"]["width"], session["native_raster"]["height"], full_frame_count, "full-frame video")
                    summary["checks"].append("full_frame_ffprobe")
                for index, (stream_id, fd, width, height, _) in enumerate(roi_video_fds):
                    expected_frames = session["finalized_session_receipt"]["streams"][index]["ranges"]["frame_count"]
                    ffprobe_video(probe, fd, width, height, expected_frames, "ROI %s video" % stream_id)
                summary["checks"].append("roi_ffprobe_decode")
            if full is not None and full_fd is not None and full_video_path is not None:
                full_after = os.fstat(full_fd)
                if (
                    full_after.st_dev,
                    full_after.st_ino,
                    full_after.st_size,
                    full_after.st_mtime_ns,
                    full_after.st_ctime_ns,
                ) != full_artifact_identities[full_video_path]:
                    raise VerificationError("full-frame video changed during decode verification")
                full_path_fd = open_relative(root_fd, full_video_path)
                try:
                    full_path_after = os.fstat(full_path_fd)
                    if (
                        full_path_after.st_dev,
                        full_path_after.st_ino,
                        full_path_after.st_size,
                        full_path_after.st_mtime_ns,
                        full_path_after.st_ctime_ns,
                    ) != full_artifact_identities[full_video_path]:
                        raise VerificationError(
                            "full-frame video path changed during decode verification"
                        )
                finally:
                    _close_quietly(full_path_fd)
            for stream_id, fd, _, _, roi_video_path in roi_video_fds:
                receipt_artifact = receipt_artifacts_by_path[roi_video_path]
                hash_open_fd(
                    fd,
                    roi_video_path,
                    receipt_artifact["size_bytes"],
                    receipt_artifact["sha256"],
                )
                after = os.fstat(fd)
                if (
                    after.st_dev,
                    after.st_ino,
                    after.st_size,
                    after.st_mtime_ns,
                    after.st_ctime_ns,
                ) != roi_artifact_identities[roi_video_path]:
                    raise VerificationError(
                        "ROI %s video changed during decode verification" % stream_id
                    )
                roi_path_fd = open_relative(root_fd, roi_video_path)
                try:
                    path_after = os.fstat(roi_path_fd)
                    if (
                        path_after.st_dev,
                        path_after.st_ino,
                        path_after.st_size,
                        path_after.st_mtime_ns,
                        path_after.st_ctime_ns,
                    ) != roi_artifact_identities[roi_video_path]:
                        raise VerificationError(
                            "ROI %s video path changed during decode verification"
                            % stream_id
                        )
                finally:
                    _close_quietly(roi_path_fd)
        finally:
            for _, fd, _, _, _ in roi_video_fds:
                _close_quietly(fd)
            _close_quietly(full_fd)
        summary["status"] = "pass"
        return summary
    except VerificationError as exc:
        summary["errors"].append(str(exc))
        return summary
    except Exception as exc:  # fail closed even for unexpected malformed input
        summary["errors"].append("unexpected verifier failure: %s" % exc)
        return summary
    finally:
        _close_quietly(root_fd)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Read-only offline acceptance verification of one completed spatial-ROI recording folder."
    )
    parser.add_argument("recording_folder", help="one completed recording folder")
    parser.add_argument("--json", action="store_true", dest="as_json", help="emit a JSON summary")
    parser.add_argument("--ffprobe", metavar="PATH", help="ffprobe executable (default: PATH lookup)")
    parser.add_argument("--require-ffprobe", action="store_true", help="fail if ffprobe is unavailable or any media decode check fails")
    args = parser.parse_args(argv)
    result = verify(args.recording_folder, args.ffprobe, args.require_ffprobe)
    if args.as_json:
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    else:
        print("[spatial-roi-acceptance] status=%s folder=%s" % (result["status"], result["recording_folder"]))
        for check in result.get("checks", []):
            print("[spatial-roi-acceptance] check=%s pass" % check)
        for warning in result.get("warnings", []):
            print("[spatial-roi-acceptance] warning=%s" % warning)
        for error in result.get("errors", []):
            print("[spatial-roi-acceptance] error=%s" % error, file=sys.stderr)
        for stream in result.get("streams", []):
            print("[spatial-roi-acceptance] stream=%s frames=%s artifacts=%s" % (stream["logical_stream_id"], stream["frame_count"], stream["artifact_count"]))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
