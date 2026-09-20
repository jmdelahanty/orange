#!/usr/bin/env python3
"""Run the eight-case CUDA 12 producer to CUDA 13 recorder correctness matrix."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCHEMA_ID = "orange.nvenc_native.ipc_replay_matrix"
FORCED_ENV_NAMES = (
    "ORANGE_EXTERNAL_RECORDER_NATIVE_LOCAL_INPUT",
    "ORANGE_EXTERNAL_RECORDER_NATIVE_KERNEL_PTX",
    "ORANGE_EXTERNAL_RECORDER_SPLIT_SUBMIT",
    "ORANGE_EXTERNAL_RECORDER_REGISTERED_SOURCE",
    "ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT",
    "ORANGE_EXTERNAL_RECORDER_DEFERRED_RELEASE",
    "ORANGE_EXTERNAL_RECORDER_EARLY_PEER_STAGE",
    "ORANGE_EXTERNAL_RECORDER_EARLY_STAGE_PUSH",
    "ORANGE_EXTERNAL_RECORDER_PEER_ACCESS",
    "ORANGE_EXTERNAL_RECORDER_EXTRA_OUTPUT_DELAY",
)


@dataclass(frozen=True)
class Case:
    topology: str
    native: bool
    split_submit: bool

    @property
    def name(self) -> str:
        return (
            f"{self.topology}_native{int(self.native)}_"
            f"split_submit{int(self.split_submit)}"
        )


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def read_json(path: Path) -> dict[str, Any]:
    require(path.is_file(), f"missing JSON artifact: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid JSON artifact: {path}: {exc}") from exc
    require(isinstance(value, dict), f"expected JSON object: {path}")
    return value


def read_csv(path: Path) -> list[dict[str, str]]:
    require(path.is_file(), f"missing CSV artifact: {path}")
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    require(bool(rows), f"CSV has no data rows: {path}")
    return rows


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def native_update_for_case(case: Case, args: argparse.Namespace) -> str | None:
    if not case.native:
        return None
    return "kernel" if args.native_kernel_ptx is not None else "copy"


def native_kernel_provenance(case: Case, args: argparse.Namespace) -> dict[str, Any]:
    return {
        "configured": args.native_kernel_ptx is not None,
        "active_for_case": case.native and args.native_kernel_ptx is not None,
        "path": str(args.native_kernel_ptx) if args.native_kernel_ptx is not None else None,
        "sha256": args.native_kernel_ptx_sha256,
    }


def percentile(values: list[float], fraction: float) -> float:
    require(bool(values), "cannot calculate percentile of an empty sequence")
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def expected_route(frame_id: int, gop: int, gpu_ids: list[int]) -> tuple[int, int]:
    gop_index = (frame_id - 1) // gop
    shard = gop_index % len(gpu_ids)
    return gpu_ids[shard], shard


def expected_shard_frames(frames: int, gop: int, shard_count: int) -> list[int]:
    counts = [0] * shard_count
    for frame_id in range(1, frames + 1):
        counts[((frame_id - 1) // gop) % shard_count] += 1
    return counts


def validate_replay_csv(
    path: Path,
    frames: int,
    source_frames: int,
    pool: int,
    gop: int,
    gpu_ids: list[int],
    fps: int,
) -> dict[str, Any]:
    rows = read_csv(path)
    require(len(rows) == frames, f"producer CSV rows={len(rows)}, expected {frames}")
    prior_release_by_slot: dict[int, int] = {}
    prior_generation_by_slot: dict[int, int] = {}
    sent: list[int] = []
    targets: list[int] = []
    ack_latencies_ms: list[float] = []
    release_latencies_ms: list[float] = []
    for index, row in enumerate(rows):
        frame_id = int(row["recording_frame_id"])
        require(frame_id == index + 1, f"producer frame sequence mismatch at row {index}")
        require(int(row["local_frame_id"]) == frame_id, f"local frame mismatch at {frame_id}")
        require(int(row["source_index"]) == index % source_frames, f"source map mismatch at {frame_id}")
        slot = int(row["slot_index"])
        generation = int(row["slot_generation"])
        require(0 <= slot < pool, f"slot {slot} outside pool at frame {frame_id}")
        require(generation == prior_generation_by_slot.get(slot, 0) + 1, f"slot generation gap at frame {frame_id}")
        prior_generation_by_slot[slot] = generation
        target_ns = int(row["target_steady_ns"])
        sent_ns = int(row["sent_steady_ns"])
        ack_ns = int(row["ack_steady_ns"])
        release_ns = int(row["release_steady_ns"])
        require(target_ns > 0 and sent_ns > 0 and ack_ns > 0 and release_ns > 0, f"zero protocol timestamp at frame {frame_id}")
        require(target_ns <= sent_ns <= ack_ns <= release_ns, f"protocol timestamp order violation at frame {frame_id}")
        if slot in prior_release_by_slot:
            require(prior_release_by_slot[slot] <= sent_ns, f"slot {slot} reused before RELEASE at frame {frame_id}")
        prior_release_by_slot[slot] = release_ns
        require(int(row["ack_count"]) == 1, f"ACK count mismatch at frame {frame_id}")
        require(int(row["release_count"]) == 1, f"RELEASE count mismatch at frame {frame_id}")
        require(int(row["deferred_ack"]) == 1, f"ACK was not deferred-release at frame {frame_id}")
        expected_gpu, expected_shard = expected_route(frame_id, gop, gpu_ids)
        for prefix in ("expected", "ack", "release"):
            require(int(row[f"{prefix}_gpu_id"]) == expected_gpu, f"{prefix} GPU mismatch at frame {frame_id}")
            require(int(row[f"{prefix}_shard_id"]) == expected_shard, f"{prefix} shard mismatch at frame {frame_id}")
        sent.append(sent_ns)
        targets.append(target_ns)
        ack_latencies_ms.append((ack_ns - sent_ns) / 1_000_000.0)
        release_latencies_ms.append((release_ns - ack_ns) / 1_000_000.0)

    expected_period_ns = 1_000_000_000 // fps
    target_deltas = [targets[i] - targets[i - 1] for i in range(1, len(targets))]
    require(all(delta == expected_period_ns for delta in target_deltas), "producer target cadence is not exact")
    send_deltas_ms = [(sent[i] - sent[i - 1]) / 1_000_000.0 for i in range(1, len(sent))]
    lateness_ms = [(actual - target) / 1_000_000.0 for actual, target in zip(sent, targets)]
    require(all(delta >= 0 for delta in send_deltas_ms), "producer send timestamps are not monotonic")
    elapsed_s = (sent[-1] - sent[0]) / 1_000_000_000.0 if len(sent) > 1 else 0.0
    achieved_fps = (len(sent) - 1) / elapsed_s if elapsed_s > 0 else None
    return {
        "target_fps": fps,
        "target_period_ms": expected_period_ns / 1_000_000.0,
        "achieved_send_fps": achieved_fps,
        "send_interval_ms": {
            "p50": percentile(send_deltas_ms, 0.50) if send_deltas_ms else None,
            "p95": percentile(send_deltas_ms, 0.95) if send_deltas_ms else None,
            "max": max(send_deltas_ms) if send_deltas_ms else None,
        },
        "schedule_lateness_ms": {
            "p50": percentile(lateness_ms, 0.50),
            "p95": percentile(lateness_ms, 0.95),
            "max": max(lateness_ms),
        },
        "send_to_ack_ms": {
            "p50": percentile(ack_latencies_ms, 0.50),
            "p95": percentile(ack_latencies_ms, 0.95),
            "max": max(ack_latencies_ms),
        },
        "ack_to_release_ms": {
            "p50": percentile(release_latencies_ms, 0.50),
            "p95": percentile(release_latencies_ms, 0.95),
            "max": max(release_latencies_ms),
        },
        "slots_used": len(prior_generation_by_slot),
        "max_slot_generation": max(prior_generation_by_slot.values()),
    }


def validate_sequential_csv(path: Path, frames: int, label: str) -> list[dict[str, str]]:
    rows = read_csv(path)
    require(len(rows) == frames, f"{label} rows={len(rows)}, expected {frames}")
    for index, row in enumerate(rows):
        require(int(row["recording_frame_id"]) == index + 1, f"{label} frame gap at row {index}")
    return rows


def validate_summary(
    summary_path: Path,
    case: Case,
    args: argparse.Namespace,
    case_dir: Path,
    session_id: str,
    gpu_ids: list[int],
) -> dict[str, Any]:
    summary = read_json(summary_path)
    frames = args.frames
    require(summary.get("schema_id") == "orange.external_recorder.summary", "unexpected recorder summary schema")
    require(summary.get("schema_version") == 2, "unexpected recorder summary version")
    require(summary.get("tool") == "external_recorder_ipc_probe", "unexpected recorder tool")
    require(summary.get("session_id") == session_id, "summary session mismatch")
    require(summary.get("stream_id") == session_id, "summary stream mismatch")
    require(summary.get("stream_kind") == "full_frame", "summary stream kind mismatch")
    require(summary.get("output_kind") == "full", "summary output kind mismatch")
    require(summary.get("gpu_id") == args.source_gpu, "summary primary GPU mismatch")
    require(summary.get("routing_policy") == ("single_shard" if len(gpu_ids) == 1 else "gop_modulo"), "routing policy mismatch")
    require(summary.get("shard_count") == len(gpu_ids), "summary shard count mismatch")
    require(summary.get("encode") is True, "encoding not enabled")
    require(summary.get("direct_input_source") is True, "direct input not enabled")
    require(summary.get("deferred_source_release") is True, "deferred release not enabled")
    require(summary.get("registered_source") is True, "registered source not enabled")
    require(summary.get("split_submit") is case.split_submit, "split-submit resolution mismatch")
    require(summary.get("native_local_requested") is case.native, "native request resolution mismatch")
    expected_native_update = native_update_for_case(case, args)
    require(
        summary.get("native_update_requested") == expected_native_update,
        "native update request resolution mismatch",
    )
    require(summary.get("native_update") == expected_native_update, "native update resolution mismatch")
    expected_release_boundary = (
        f"{expected_native_update}_complete" if expected_native_update is not None else None
    )
    require(
        summary.get("native_source_release_boundary") == expected_release_boundary,
        "native source release boundary mismatch",
    )
    require(summary.get("fps") == args.fps, "summary FPS mismatch")
    require(summary.get("resolved_gop_length") == args.gop, "summary GOP mismatch")
    require(summary.get("encode_max_fps") == 0, "encode cap unexpectedly enabled")
    require(summary.get("encode_queue_depth") == args.pool, "encode queue depth mismatch")
    for field in ("frames_received", "acks_sent", "encode_enqueued", "frames_encoded"):
        require(summary.get(field) == frames, f"summary {field}={summary.get(field)}, expected {frames}")
    for field in ("encode_skipped", "encode_dropped"):
        require(summary.get(field) == 0, f"summary {field} is nonzero")
    require(summary.get("worker_failed") is False, "recorder worker failed")
    require(0 <= int(summary.get("encode_queue_high_water", -1)) <= args.pool, "encode queue high water exceeds pool")

    protocol = summary.get("ipc_protocol", {})
    require(protocol.get("name") == "orange.external_recorder.ipc", "protocol name mismatch")
    require(protocol.get("version") == 1, "protocol version mismatch")
    require(protocol.get("recorder_hello_sent") is True, "recorder hello missing")
    require(protocol.get("client_hello_received") is True, "client hello missing")
    require(protocol.get("recorder_status_send_failures") == 0, "recorder status send failure")
    require(protocol.get("client_control_messages_received") == 2, "control message count mismatch")
    require(protocol.get("client_drain_messages_received") == 1, "drain count mismatch")
    require(protocol.get("client_finalize_messages_received") == 1, "finalize count mismatch")
    require(protocol.get("client_drain_received") is True, "drain missing")
    require(protocol.get("client_finalize_received") is True, "finalize missing")
    require(protocol.get("client_drain_first_frame_count") == frames, "drain frame count mismatch")
    require(protocol.get("client_finalize_frame_count") == frames, "finalize frame count mismatch")
    require(protocol.get("descriptor_intake_end_reason") == "client_finalize", "unclean intake end reason")
    require(protocol.get("descriptor_intake_completed_cleanly") is True, "descriptor intake not clean")
    require(protocol.get("duration_safety_ceiling_exceeded") is False, "duration safety ceiling exceeded")

    metadata = summary.get("frame_metadata", {})
    require(metadata.get("rows_written") == frames, "metadata row count mismatch")
    require(metadata.get("first_recording_frame_id") == 1, "metadata first frame mismatch")
    require(metadata.get("last_recording_frame_id") == frames, "metadata last frame mismatch")
    require(metadata.get("recording_frame_id_gaps") == 0, "metadata frame gaps")
    require(metadata.get("zero_camera_timestamp_rows") == 0, "zero camera timestamps")
    require(metadata.get("zero_system_timestamp_rows") == 0, "zero system timestamps")

    proof = summary.get("frame_identity_proof", {})
    binding = proof.get("video_binding", {})
    require(proof.get("status") == "passed", "frame identity proof failed")
    require(proof.get("source_frames_dropped") == 0, "identity proof reports drops")
    require(proof.get("source_frames_skipped_by_policy") == 0, "identity proof reports skips")
    require(binding.get("verified") is True, "video binding is not verified")
    exact_binding_fields = (
        "encoded_video_frames",
        "metadata_rows",
        "packet_submissions_accepted",
        "packet_write_attempts",
        "packets_written",
        "returned_identity_matches",
        "submitted_frame_identities",
    )
    for field in exact_binding_fields:
        require(binding.get(field) == frames, f"video binding {field} mismatch")
    for field in (
        "identity_mismatches",
        "outstanding_submitted_identities",
        "packet_submissions_rejected",
        "packet_write_failures",
    ):
        require(binding.get(field) == 0, f"video binding {field} is nonzero")

    external = summary.get("external_encode", {})
    require(external.get("frames_dropped") == 0, "external encode dropped frames")
    require(external.get("source_releases_sent") == frames, "source RELEASE total mismatch")
    require(external.get("source_release_failures") == 0, "source RELEASE failures")
    require(external.get("returned_packets", 0) + external.get("flush_packets", 0) == frames, "NVENC packet return count mismatch")
    require(external.get("mp4_queue_overflowed") is False, "MP4 queue overflowed")
    require(external.get("mp4_queue_overflow_events") == 0, "MP4 queue overflow events")

    expected_counts = expected_shard_frames(frames, args.gop, len(gpu_ids))
    shards = summary.get("external_encode_shards", [])
    require(len(shards) == len(gpu_ids), "external shard summary count mismatch")
    expected_native_frames = 0
    for index, shard in enumerate(shards):
        require(shard.get("assigned_gpu_id") == gpu_ids[index], f"shard {index} GPU mismatch")
        require(shard.get("assigned_shard_id") == index, f"shard {index} id mismatch")
        require(shard.get("frames_encoded") == expected_counts[index], f"shard {index} frame count mismatch")
        require(shard.get("frames_dropped") == 0, f"shard {index} dropped frames")
        require(shard.get("source_releases_sent") == expected_counts[index], f"shard {index} RELEASE count mismatch")
        require(shard.get("source_release_failures") == 0, f"shard {index} RELEASE failure")
        require(shard.get("worker_failed") is False, f"shard {index} worker failed")
        require(shard.get("mp4_queue_overflowed") is False, f"shard {index} MP4 overflow")
        require(shard.get("native_local_requested") is case.native, f"shard {index} native request mismatch")
        should_be_native = case.native and gpu_ids[index] == args.source_gpu
        expected_backend = "native_nv12_array" if should_be_native else "linear"
        expected_shard_update = expected_native_update if should_be_native else None
        expected_shard_release_boundary = (
            f"{expected_shard_update}_complete" if expected_shard_update is not None else None
        )
        require(shard.get("input_backend") == expected_backend, f"shard {index} backend mismatch")
        require(shard.get("native_update") == expected_shard_update, f"shard {index} native update mismatch")
        require(shard.get("native_frames") == (expected_counts[index] if should_be_native else 0), f"shard {index} native frame count mismatch")
        require(shard.get("native_source_release_boundary") == expected_shard_release_boundary, f"shard {index} native release boundary mismatch")
        if should_be_native:
            expected_native_frames += expected_counts[index]
    require(summary.get("native_frames") == expected_native_frames, "aggregate native frame count mismatch")

    merged = summary.get("merged_output", {})
    require(merged.get("coordinator_enabled") is True, "merged coordinator disabled")
    require(merged.get("enabled") is True and merged.get("failed") is False, "merged output failed")
    for field in (
        "packets_written",
        "packet_submissions_accepted",
        "packet_write_attempts",
        "frame_identities_submitted",
        "frame_identities_returned",
        "metadata_rows",
    ):
        require(merged.get(field) == frames, f"merged output {field} mismatch")
    for field in (
        "packet_submissions_rejected",
        "packet_write_failures",
        "frame_identity_mismatches",
        "outstanding_frame_identities",
        "pending_gops",
        "pending_bytes",
        "metadata_recording_frame_id_gaps",
        "mp4_queue_overflow_events",
    ):
        require(merged.get(field) == 0, f"merged output {field} is nonzero")
    require(merged.get("mp4_queue_overflowed") is False, "merged MP4 queue overflowed")
    require(merged.get("error_message") == "", "merged output carries an error")

    output = case_dir / "output.mp4"
    require(Path(merged.get("mp4", "")) == output, "merged MP4 path mismatch")
    require(output.is_file() and output.stat().st_size > 0, "merged MP4 missing or empty")
    sizes = summary.get("output_file_sizes", {})
    require(sizes.get("mp4_bytes") == output.stat().st_size, "summary MP4 size mismatch")
    require(sizes.get("metadata_bytes", 0) > 0, "summary metadata size is zero")

    finalization = read_json(Path(str(output) + ".finalization.json"))
    require(finalization.get("status") == "complete", "MP4 finalization status is not complete")
    require(finalization.get("terminal") is True, "MP4 finalization is not terminal")
    container = finalization.get("container", {})
    packet_writes = finalization.get("packet_writes", {})
    require(container.get("finalized") is True, "MP4 container not finalized")
    require(container.get("trailer_written") is True, "MP4 trailer missing")
    require(container.get("output_closed") is True, "MP4 output not closed")
    require(container.get("file_size_bytes") == output.stat().st_size, "finalization MP4 size mismatch")
    require(packet_writes.get("complete") is True, "packet writes incomplete")
    require(packet_writes.get("packets_written") == frames, "finalization packet count mismatch")
    require(packet_writes.get("submissions_accepted") == frames, "finalization submission count mismatch")
    require(packet_writes.get("submissions_rejected") == 0, "finalization rejected packets")
    require(packet_writes.get("write_failures") == 0, "finalization write failures")

    keyframes = read_json(case_dir / "output_keyframes.json")
    require(keyframes.get("total_frames") == frames, "keyframe sidecar frame count mismatch")
    require(keyframes.get("keyframe_frames") == list(range(0, frames, args.gop)), "keyframe GOP boundaries mismatch")

    metadata_rows = validate_sequential_csv(case_dir / "output_meta.csv", frames, "metadata CSV")
    intake_rows = validate_sequential_csv(case_dir / "intake.csv", frames, "intake CSV")
    routing_rows = validate_sequential_csv(case_dir / "routing.csv", frames, "routing CSV")
    for row_set, label in ((metadata_rows, "metadata"), (intake_rows, "intake"), (routing_rows, "routing")):
        for index, row in enumerate(row_set):
            frame_id = index + 1
            gpu, shard = expected_route(frame_id, args.gop, gpu_ids)
            require(int(row["assigned_gpu_id"]) == gpu, f"{label} GPU route mismatch at frame {frame_id}")
            require(int(row["assigned_shard_id"]) == shard, f"{label} shard route mismatch at frame {frame_id}")
    encoded_frame_ids: list[int] = []
    for shard_index, shard in enumerate(shards):
        encode_path = Path(shard.get("encode_csv", ""))
        require(encode_path.is_file(), f"missing shard {shard_index} encode CSV: {encode_path}")
        encode_rows = read_csv(encode_path)
        require(len(encode_rows) == expected_counts[shard_index], f"shard {shard_index} encode CSV row count mismatch")
        for row in encode_rows:
            frame_id = int(row["recording_frame_id"])
            gpu, route_shard = expected_route(frame_id, args.gop, gpu_ids)
            require(route_shard == shard_index, f"frame {frame_id} is in the wrong shard encode CSV")
            require(int(row["assigned_gpu_id"]) == gpu, f"encode GPU route mismatch at frame {frame_id}")
            require(int(row["assigned_shard_id"]) == route_shard, f"encode shard route mismatch at frame {frame_id}")
            encoded_frame_ids.append(frame_id)
    require(sorted(encoded_frame_ids) == list(range(1, frames + 1)), "shard encode CSVs do not cover each frame exactly once")
    return summary


def terminate_process(process: subprocess.Popen[Any] | None) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)


def wait_for_socket(path: Path, process: subprocess.Popen[Any], timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"recorder exited before listening, return code {process.returncode}")
        try:
            if stat.S_ISSOCK(path.stat().st_mode):
                return
        except FileNotFoundError:
            pass
        time.sleep(0.02)
    raise RuntimeError(f"timed out waiting for recorder socket: {path}")


def child_env(case: Case, args: argparse.Namespace) -> tuple[dict[str, str], dict[str, str]]:
    overrides = {
        "ORANGE_EXTERNAL_RECORDER_NATIVE_LOCAL_INPUT": "1" if case.native else "0",
        "ORANGE_EXTERNAL_RECORDER_NATIVE_KERNEL_PTX": (
            str(args.native_kernel_ptx)
            if case.native and args.native_kernel_ptx is not None
            else ""
        ),
        "ORANGE_EXTERNAL_RECORDER_SPLIT_SUBMIT": "1" if case.split_submit else "0",
        "ORANGE_EXTERNAL_RECORDER_REGISTERED_SOURCE": "1",
        "ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT": "1",
        "ORANGE_EXTERNAL_RECORDER_DEFERRED_RELEASE": "1",
        "ORANGE_EXTERNAL_RECORDER_EARLY_PEER_STAGE": "1",
        "ORANGE_EXTERNAL_RECORDER_EARLY_STAGE_PUSH": "0",
        "ORANGE_EXTERNAL_RECORDER_PEER_ACCESS": "0",
        "ORANGE_EXTERNAL_RECORDER_EXTRA_OUTPUT_DELAY": "3",
    }
    require(set(overrides) == set(FORCED_ENV_NAMES), "internal forced environment mismatch")
    environment = os.environ.copy()
    environment.update(overrides)
    return environment, overrides


def commands_for_case(
    case: Case,
    index: int,
    args: argparse.Namespace,
    case_dir: Path,
) -> tuple[list[str], list[str], Path, str, list[int]]:
    socket_path = Path(f"/tmp/orange_ipc_replay_matrix_{os.getpid()}_{index}.sock")
    session_id = case.name
    gpu_ids = [args.source_gpu] if case.topology == "same_gpu" else [args.source_gpu, args.peer_gpu]
    recorder = [
        str(args.recorder),
        "--socket", str(socket_path),
        "--gpu-id", str(args.source_gpu),
        "--session-id", session_id,
        "--stream-id", session_id,
        "--encode",
        "--encode-max-fps", "0",
        "--encode-queue-depth", str(args.pool),
        "--fps", str(args.fps),
        "--gop", str(args.gop),
        "--codec", "hevc",
        "--preset", "p1",
        "--tuning", "ll",
        "--extra-output-delay", "3",
        "--mp4-out", str(case_dir / "output.mp4"),
        "--mp4-keyframe", str(case_dir / "output_keyframes.json"),
        "--metadata-csv", str(case_dir / "output_meta.csv"),
        "--encode-csv", str(case_dir / "encode.csv"),
        "--csv", str(case_dir / "intake.csv"),
        "--gop-routing-csv", str(case_dir / "routing.csv"),
        "--summary-json", str(case_dir / "summary.json"),
        "--status-json", str(case_dir / "status.json"),
    ]
    if len(gpu_ids) > 1:
        recorder.extend(["--shard-gpu-ids", ",".join(str(gpu) for gpu in gpu_ids)])
    producer = [
        str(args.producer),
        "--socket", str(socket_path),
        "--session-id", session_id,
        "--stream-id", session_id,
        "--camera-serial", "ipc_replay_camera",
        "--source-gpu", str(args.source_gpu),
        "--route-gpu-ids", ",".join(str(gpu) for gpu in gpu_ids),
        "--frames", str(args.frames),
        "--width", str(args.width),
        "--height", str(args.height),
        "--fps", str(args.fps),
        "--gop", str(args.gop),
        "--pool", str(args.pool),
        "--raw-file", str(args.raw_file),
        "--raw-pitch", str(args.raw_pitch),
        "--raw-frame-bytes", str(args.raw_frame_bytes),
        "--source-frames", str(args.source_frames),
        "--csv", str(case_dir / "producer.csv"),
        "--timeout-ms", str(round(args.child_timeout_s * 1000)),
    ]
    return recorder, producer, socket_path, session_id, gpu_ids


def run_content_validator(
    args: argparse.Namespace,
    case_dir: Path,
) -> tuple[list[str], dict[str, Any], str]:
    command = [
        str(args.numpy_python),
        str(args.content_validator),
        "--mp4", str(case_dir / "output.mp4"),
        "--replay-csv", str(case_dir / "producer.csv"),
        "--raw-file", str(args.raw_file),
        "--raw-pitch", str(args.raw_pitch),
        "--raw-frame-bytes", str(args.raw_frame_bytes),
        "--source-frames", str(args.source_frames),
        "--frames", str(args.frames),
        "--width", str(args.width),
        "--height", str(args.height),
        "--samples", args.samples,
        "--ffmpeg", args.ffmpeg,
        "--ffprobe", args.ffprobe,
        "--timeout-s", str(args.decode_timeout_s),
        "--output", str(case_dir / "content_validation.json"),
    ]
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=args.decode_timeout_s + 30.0,
    )
    log = completed.stdout
    (case_dir / "content_validation.log").write_text(log, encoding="utf-8")
    require(completed.returncode == 0, f"content validator exited {completed.returncode}: {log[-2000:]}")
    report = read_json(case_dir / "content_validation.json")
    require(report.get("pass") is True, "content validator did not pass")
    return command, report, log


def run_case(case: Case, index: int, args: argparse.Namespace) -> dict[str, Any]:
    case_dir = args.root / case.name
    case_dir.mkdir(parents=True)
    recorder_command, producer_command, socket_path, session_id, gpu_ids = commands_for_case(
        case, index, args, case_dir
    )
    environment, overrides = child_env(case, args)
    commands_manifest: dict[str, Any] = {
        "case": case.name,
        "started_at_utc": utc_now(),
        "environment_overrides": overrides,
        "native_kernel_ptx": native_kernel_provenance(case, args),
        "recorder": {"argv": recorder_command, "shell": shlex.join(recorder_command)},
        "producer": {"argv": producer_command, "shell": shlex.join(producer_command)},
    }
    (case_dir / "commands.json").write_text(
        json.dumps(commands_manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    recorder_process: subprocess.Popen[Any] | None = None
    producer_process: subprocess.Popen[Any] | None = None
    result: dict[str, Any] = {
        "name": case.name,
        "topology": case.topology,
        "native": case.native,
        "split_submit": case.split_submit,
        "gpu_ids": gpu_ids,
        "pass": False,
        "started_at_utc": commands_manifest["started_at_utc"],
    }
    try:
        try:
            socket_path.unlink()
        except FileNotFoundError:
            pass
        with (case_dir / "recorder.log").open("wb") as recorder_log:
            recorder_process = subprocess.Popen(
                recorder_command,
                stdout=recorder_log,
                stderr=subprocess.STDOUT,
                env=environment,
                start_new_session=True,
            )
            wait_for_socket(socket_path, recorder_process, args.listen_timeout_s)
            with (case_dir / "producer.log").open("wb") as producer_log:
                producer_process = subprocess.Popen(
                    producer_command,
                    stdout=producer_log,
                    stderr=subprocess.STDOUT,
                    env=environment,
                    start_new_session=True,
                )
                try:
                    producer_returncode = producer_process.wait(timeout=args.child_timeout_s)
                except subprocess.TimeoutExpired as exc:
                    raise RuntimeError("producer timed out") from exc
            result["producer_returncode"] = producer_returncode
            if producer_returncode != 0:
                terminate_process(recorder_process)
                raise RuntimeError(f"producer exited {producer_returncode}")
            try:
                recorder_returncode = recorder_process.wait(timeout=args.child_timeout_s)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("recorder timed out after producer completion") from exc
            result["recorder_returncode"] = recorder_returncode
        require(recorder_returncode == 0, f"recorder exited {recorder_returncode}")
        producer_log_text = (case_dir / "producer.log").read_text(encoding="utf-8", errors="replace")
        require("ipc_replay_probe: PASS" in producer_log_text, "producer log lacks PASS summary")

        summary = validate_summary(
            case_dir / "summary.json", case, args, case_dir, session_id, gpu_ids
        )
        cadence = validate_replay_csv(
            case_dir / "producer.csv",
            args.frames,
            args.source_frames,
            args.pool,
            args.gop,
            gpu_ids,
            args.fps,
        )
        content_command, content, _ = run_content_validator(args, case_dir)
        commands_manifest["content_validator"] = {
            "argv": content_command,
            "shell": shlex.join(content_command),
        }
        result.update(
            {
                "pass": True,
                "cadence": cadence,
                "frames_received": summary["frames_received"],
                "frames_encoded": summary["frames_encoded"],
                "packets_written": summary["merged_output"]["packets_written"],
                "native_frames": summary["native_frames"],
                "native_update": summary["native_update"],
                "encode_queue_high_water": summary["encode_queue_high_water"],
                "content_samples": content["sample_metrics"],
            }
        )
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        result["error"] = str(exc)
    finally:
        terminate_process(producer_process)
        terminate_process(recorder_process)
        try:
            socket_path.unlink()
        except FileNotFoundError:
            pass
        result["finished_at_utc"] = utc_now()
        commands_manifest["finished_at_utc"] = result["finished_at_utc"]
        commands_manifest["result"] = result
        (case_dir / "commands.json").write_text(
            json.dumps(commands_manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        (case_dir / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    return result


def executable_path(value: str) -> Path:
    resolved = shutil.which(value) if os.sep not in value else value
    if not resolved:
        raise argparse.ArgumentTypeError(f"executable not found: {value}")
    path = Path(resolved).resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise argparse.ArgumentTypeError(f"not executable: {path}")
    return path


def preflight(args: argparse.Namespace) -> None:
    require(not args.root.exists(), f"artifact root already exists: {args.root}")
    require(args.raw_file.is_file(), f"raw source does not exist: {args.raw_file}")
    require(args.content_validator.is_file(), f"content validator does not exist: {args.content_validator}")
    if args.native_kernel_ptx is not None:
        require(args.native_kernel_ptx.is_file(), f"native kernel PTX does not exist: {args.native_kernel_ptx}")
        args.native_kernel_ptx_sha256 = sha256_file(args.native_kernel_ptx)
    else:
        args.native_kernel_ptx_sha256 = None
    require(args.frames > 0 and args.source_frames > 0, "frame counts must be positive")
    require(args.pool > 0 and args.fps > 0 and args.gop > 0, "pool, FPS, and GOP must be positive")
    require(args.width > 0 and args.height > 0 and args.width % 2 == 0 and args.height % 2 == 0, "NV12 dimensions must be positive and even")
    require(args.raw_pitch >= args.width, "raw pitch is smaller than width")
    require(args.raw_frame_bytes >= args.raw_pitch * args.height, "raw record stride is too small")
    required_raw_bytes = args.raw_frame_bytes * args.source_frames
    require(args.raw_file.stat().st_size >= required_raw_bytes, f"raw source has {args.raw_file.stat().st_size} bytes, needs {required_raw_bytes}")
    require(args.source_gpu >= 0 and args.peer_gpu >= 0 and args.source_gpu != args.peer_gpu, "source and peer GPUs must be distinct nonnegative ids")
    for value, label in ((args.listen_timeout_s, "listen timeout"), (args.child_timeout_s, "child timeout"), (args.decode_timeout_s, "decode timeout")):
        require(value > 0, f"{label} must be positive")

    numpy_check = subprocess.run(
        [str(args.numpy_python), "-c", "import numpy"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=15,
    )
    require(numpy_check.returncode == 0, f"NumPy preflight failed for {args.numpy_python}: {numpy_check.stderr.strip()}")
    for program in (args.ffmpeg, args.ffprobe):
        resolved = shutil.which(program) if os.sep not in program else program
        require(bool(resolved) and Path(str(resolved)).is_file(), f"required decoder tool not found: {program}")


def parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    default_numpy = Path("/home/jeremy/miniforge3/bin/python")
    if not default_numpy.is_file():
        default_numpy = Path(sys.executable)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="New, unique artifact root")
    parser.add_argument("--recorder", type=executable_path, required=True)
    parser.add_argument("--producer", type=executable_path, required=True)
    parser.add_argument("--raw-file", type=Path, required=True)
    parser.add_argument("--raw-pitch", type=int, default=4512)
    parser.add_argument("--raw-frame-bytes", type=int, default=0)
    parser.add_argument("--source-frames", type=int, default=8)
    parser.add_argument("--frames", type=int, default=200)
    parser.add_argument("--width", type=int, default=4512)
    parser.add_argument("--height", type=int, default=4512)
    parser.add_argument("--fps", type=int, default=100)
    parser.add_argument("--gop", type=int, default=25)
    parser.add_argument("--pool", type=int, default=32)
    parser.add_argument("--source-gpu", type=int, default=1)
    parser.add_argument("--peer-gpu", type=int, default=2)
    parser.add_argument("--numpy-python", type=executable_path, default=default_numpy)
    parser.add_argument("--content-validator", type=Path, default=script_dir / "validate_ipc_replay_content.py")
    parser.add_argument(
        "--native-kernel-ptx",
        type=Path,
        help="Use this native surface-update PTX for native cases; omitted keeps the copy backend",
    )
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--samples", default="0,1,24,25,49,50,last")
    parser.add_argument("--listen-timeout-s", type=float, default=30.0)
    parser.add_argument("--child-timeout-s", type=float, default=180.0)
    parser.add_argument("--decode-timeout-s", type=float, default=300.0)
    parser.add_argument("--fail-fast", action="store_true")
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.raw_file = args.raw_file.resolve()
    args.content_validator = args.content_validator.resolve()
    if args.native_kernel_ptx is not None:
        args.native_kernel_ptx = args.native_kernel_ptx.resolve()
    if args.raw_frame_bytes == 0:
        args.raw_frame_bytes = args.raw_pitch * args.height
    return args


def main() -> int:
    args = parse_args()
    try:
        preflight(args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"run_ipc_replay_validation: preflight FAIL: {exc}", file=sys.stderr)
        return 2

    args.root.mkdir(parents=True)
    cases = [
        Case(topology, native, split_submit)
        for topology in ("same_gpu", "split_gpu")
        for native in (False, True)
        for split_submit in (False, True)
    ]
    matrix: dict[str, Any] = {
        "schema_id": SCHEMA_ID,
        "schema_version": 1,
        "started_at_utc": utc_now(),
        "pass": False,
        "configuration": {
            "recorder": str(args.recorder),
            "producer": str(args.producer),
            "raw_file": str(args.raw_file),
            "raw_pitch": args.raw_pitch,
            "raw_frame_bytes": args.raw_frame_bytes,
            "source_frames": args.source_frames,
            "frames": args.frames,
            "width": args.width,
            "height": args.height,
            "fps": args.fps,
            "gop": args.gop,
            "pool": args.pool,
            "source_gpu": args.source_gpu,
            "peer_gpu": args.peer_gpu,
            "numpy_python": str(args.numpy_python),
            "content_validator": str(args.content_validator),
            "native_kernel_ptx": {
                "path": str(args.native_kernel_ptx) if args.native_kernel_ptx is not None else None,
                "sha256": args.native_kernel_ptx_sha256,
            },
            "samples": args.samples,
            "forced_environment_names": list(FORCED_ENV_NAMES),
        },
        "cases": [],
    }
    manifest_path = args.root / "matrix_summary.json"
    manifest_path.write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    for index, case in enumerate(cases):
        print(f"[{index + 1}/{len(cases)}] {case.name}", flush=True)
        result = run_case(case, index, args)
        matrix["cases"].append(result)
        manifest_path.write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if result["pass"]:
            cadence = result["cadence"]
            print(
                f"PASS {case.name}: encoded={result['frames_encoded']} "
                f"packets={result['packets_written']} native={result['native_frames']} "
                f"send_fps={cadence['achieved_send_fps']:.3f}",
                flush=True,
            )
        else:
            print(f"FAIL {case.name}: {result.get('error', 'unknown error')}", file=sys.stderr, flush=True)
            if args.fail_fast:
                break

    matrix["finished_at_utc"] = utc_now()
    matrix["completed_cases"] = len(matrix["cases"])
    matrix["passed_cases"] = sum(1 for result in matrix["cases"] if result["pass"])
    matrix["pass"] = len(matrix["cases"]) == len(cases) and matrix["passed_cases"] == len(cases)
    manifest_path.write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "pass": matrix["pass"],
                "passed_cases": matrix["passed_cases"],
                "completed_cases": matrix["completed_cases"],
                "expected_cases": len(cases),
                "summary": str(manifest_path),
            },
            sort_keys=True,
        )
    )
    return 0 if matrix["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
