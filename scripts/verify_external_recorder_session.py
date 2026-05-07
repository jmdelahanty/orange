#!/usr/bin/env python3
"""Verify the diagnostic external-recorder session contract."""

from __future__ import annotations

import argparse
import csv
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


CONTRACT_SCHEMA_ID = "orange.external_recorder.contract"
SUMMARY_SCHEMA_ID = "orange.external_recorder.summary"
DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    default_ffprobe = (
        str(DEFAULT_FFPROBE)
        if DEFAULT_FFPROBE.exists()
        else shutil.which("ffprobe") or "ffprobe"
    )
    parser = argparse.ArgumentParser(
        description=(
            "Verify an external_recorder_ipc_probe artifact root against "
            "fixed.external_recorder_contract and analytics runs.json."
        )
    )
    parser.add_argument("artifact_root", help="External recorder artifact root.")
    parser.add_argument(
        "--analytics-root",
        help="Headless analytics experiment root containing experiment_spec.json and runs.json.",
    )
    parser.add_argument(
        "--spec",
        help="Experiment spec JSON. Defaults to <analytics-root>/experiment_spec.json when present.",
    )
    parser.add_argument(
        "--camera",
        action="append",
        help="Camera serial to verify. May be repeated. Defaults to contract streams or summaries found in artifact root.",
    )
    parser.add_argument(
        "--allow-missing-video-sanity",
        action="store_true",
        help="Allow legacy artifacts without Cam*_external_video_sanity.json.",
    )
    parser.add_argument(
        "--ffprobe",
        default=default_ffprobe,
        help="ffprobe executable path for a basic MP4 fallback check.",
    )
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except FileNotFoundError as exc:
        raise VerificationError(f"missing JSON file: {path}") from exc
    except json.JSONDecodeError as exc:
        raise VerificationError(f"invalid JSON file {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise VerificationError(f"expected object JSON in {path}")
    return payload


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def as_int(value: Any, field: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise VerificationError(f"invalid integer {field}={value!r}") from exc


def path_from(value: Any, base: Path) -> Path:
    path = Path(str(value))
    return path if path.is_absolute() else base / path


def load_spec(args: argparse.Namespace) -> dict[str, Any] | None:
    if args.spec:
        return read_json(Path(args.spec).expanduser())
    if args.analytics_root:
        spec_path = Path(args.analytics_root).expanduser() / "experiment_spec.json"
        if spec_path.exists():
            return read_json(spec_path)
    return None


def contract_from_spec(spec: dict[str, Any] | None) -> dict[str, Any] | None:
    if spec is None:
        return None
    fixed = spec.get("fixed")
    if not isinstance(fixed, dict):
        return None
    contract = fixed.get("external_recorder_contract")
    return contract if isinstance(contract, dict) else None


def synthesize_contract(artifact_root: Path, cameras: list[str] | None) -> dict[str, Any]:
    summaries = sorted(artifact_root.glob("Cam*_external_summary.json"))
    streams: dict[str, Any] = {}
    for summary_path in summaries:
        serial = summary_path.name.removeprefix("Cam").removesuffix("_external_summary.json")
        if cameras and serial not in cameras:
            continue
        streams[serial] = {
            "stream_id": serial,
            "summary_json": str(summary_path),
            "video_sanity_json": str(artifact_root / f"Cam{serial}_external_video_sanity.json"),
            "mp4": str(artifact_root / f"Cam{serial}_external.mp4"),
            "gop_routing_csv": str(artifact_root / f"Cam{serial}_external_gop_routing.csv"),
            "routing_policy": "gop_modulo",
        }
    return {
        "schema_id": CONTRACT_SCHEMA_ID,
        "schema_version": 1,
        "mode": "diagnostic_ipc_v1",
        "artifact_root": str(artifact_root),
        "require_summary": True,
        "require_video_sanity": False,
        "require_merged_mp4": True,
        "require_gop_routing": True,
        "streams": streams,
    }


def selected_streams(contract: dict[str, Any], cameras: list[str] | None) -> dict[str, dict[str, Any]]:
    streams = contract.get("streams")
    require(isinstance(streams, dict) and bool(streams), "external recorder contract has no streams")
    selected: dict[str, dict[str, Any]] = {}
    for serial, stream in streams.items():
        if cameras and serial not in cameras:
            continue
        require(isinstance(stream, dict), f"contract stream {serial} is not an object")
        selected[str(serial)] = stream
    require(bool(selected), "no external recorder streams selected for verification")
    return selected


def ffprobe_video(path: Path, ffprobe: str) -> None:
    command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,duration:format=size,duration",
        "-of",
        "json",
        str(path),
    ]
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except FileNotFoundError as exc:
        raise VerificationError(f"ffprobe executable not found: {ffprobe}") from exc
    except subprocess.TimeoutExpired as exc:
        raise VerificationError(f"ffprobe timed out for {path}") from exc
    if result.returncode != 0:
        raise VerificationError(f"ffprobe failed for {path}: {result.stderr.strip()}")
    payload = json.loads(result.stdout)
    streams = payload.get("streams")
    require(isinstance(streams, list) and bool(streams), f"ffprobe found no video stream in {path}")
    stream = streams[0]
    require(as_int(stream.get("width"), "ffprobe width") > 0, f"invalid MP4 width: {path}")
    require(as_int(stream.get("height"), "ffprobe height") > 0, f"invalid MP4 height: {path}")


def verify_video_sanity(path: Path, allow_missing: bool) -> str:
    if not path.exists():
        if allow_missing:
            return "missing_allowed"
        raise VerificationError(f"missing video sanity JSON: {path}")
    payload = read_json(path)
    require(payload.get("content_checked") is True, f"video sanity did not run: {path}")
    require(payload.get("content_valid") is True, f"video sanity failed: {path}")
    require(payload.get("status") == "pass", f"video sanity status is not pass: {path}")
    return "pass"


def count_csv_data_rows(path: Path) -> int:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            return sum(1 for _ in csv.DictReader(handle))
    except FileNotFoundError as exc:
        raise VerificationError(f"missing CSV file: {path}") from exc


def verify_summary(
    artifact_root: Path,
    serial: str,
    stream: dict[str, Any],
    contract: dict[str, Any],
    ffprobe: str,
    allow_missing_video_sanity: bool,
) -> dict[str, Any]:
    summary_path = path_from(
        stream.get("summary_json") or artifact_root / f"Cam{serial}_external_summary.json",
        artifact_root,
    )
    summary = read_json(summary_path)
    schema_id = summary.get("schema_id")
    require(
        schema_id in (None, SUMMARY_SCHEMA_ID),
        f"unexpected external summary schema_id={schema_id!r} in {summary_path}",
    )
    require(summary.get("schema_version") == 1, f"unexpected summary schema_version in {summary_path}")
    require(summary.get("tool") == "external_recorder_ipc_probe", f"unexpected recorder tool in {summary_path}")
    require(str(summary.get("stream_id")) == str(stream.get("stream_id", serial)), f"stream_id mismatch in {summary_path}")
    require(summary.get("encode") is True, f"summary encode=false in {summary_path}")
    require(summary.get("worker_failed") is False, f"recorder worker_failed=true in {summary_path}")

    frames_received = as_int(summary.get("frames_received"), "frames_received")
    acks_sent = as_int(summary.get("acks_sent"), "acks_sent")
    detach_copied = as_int(summary.get("detach_copied"), "detach_copied")
    encode_enqueued = as_int(summary.get("encode_enqueued"), "encode_enqueued")
    encode_skipped = as_int(summary.get("encode_skipped"), "encode_skipped")
    encode_dropped = as_int(summary.get("encode_dropped"), "encode_dropped")
    frames_encoded = as_int(summary.get("frames_encoded"), "frames_encoded")
    require(frames_received > 0, f"no frames received in {summary_path}")
    require(acks_sent == frames_received, f"acks_sent != frames_received in {summary_path}")
    require(
        encode_enqueued + encode_skipped + encode_dropped == frames_received,
        f"encode accounting does not sum to frames_received in {summary_path}",
    )
    require(detach_copied == encode_enqueued, f"detach_copied != encode_enqueued in {summary_path}")
    require(encode_dropped == 0, f"encode_dropped is nonzero in {summary_path}")
    require(frames_encoded == encode_enqueued, f"frames_encoded != encode_enqueued in {summary_path}")
    require(frames_encoded > 0, f"no frames encoded in {summary_path}")

    expected_routing_policy = stream.get("routing_policy")
    if expected_routing_policy:
        require(
            summary.get("routing_policy") == expected_routing_policy,
            f"routing_policy mismatch in {summary_path}",
        )

    shards = summary.get("external_encode_shards")
    require(isinstance(shards, list) and bool(shards), f"summary has no external_encode_shards in {summary_path}")
    expected_gpus = stream.get("expected_shard_gpu_ids")
    if expected_gpus is not None:
        actual_gpus = [as_int(shard.get("assigned_gpu_id"), "assigned_gpu_id") for shard in shards]
        require(
            actual_gpus == [int(value) for value in expected_gpus],
            f"shard GPU ids mismatch for {serial}: expected {expected_gpus}, got {actual_gpus}",
        )
    require(as_int(summary.get("shard_count"), "shard_count") == len(shards), f"shard_count mismatch in {summary_path}")
    for shard in shards:
        require(shard.get("worker_failed") is False, f"shard worker_failed=true for {serial}")
        require(as_int(shard.get("frames_dropped"), "shard frames_dropped") == 0, f"shard dropped frames for {serial}")
        require(as_int(shard.get("frames_encoded"), "shard frames_encoded") > 0, f"shard encoded no frames for {serial}")

    merged = summary.get("merged_output")
    require(isinstance(merged, dict), f"summary missing merged_output in {summary_path}")
    if bool(contract.get("require_merged_mp4", True)) and len(shards) > 1:
        require(merged.get("enabled") is True, f"merged output disabled for {serial}")
        require(merged.get("failed") is False, f"merged output failed for {serial}")
        require(as_int(merged.get("pending_gops"), "merged pending_gops") == 0, f"merged output has pending GOPs for {serial}")
        require(as_int(merged.get("packets_written"), "merged packets_written") > 0, f"merged output wrote no packets for {serial}")

    mp4_path = path_from(stream.get("mp4") or summary.get("outputs", {}).get("mp4"), artifact_root)
    require(mp4_path.exists() and mp4_path.stat().st_size > 0, f"missing or empty external MP4: {mp4_path}")
    ffprobe_video(mp4_path, ffprobe)

    if bool(contract.get("require_gop_routing", True)):
        routing_path = path_from(
            stream.get("gop_routing_csv") or summary.get("outputs", {}).get("gop_routing_csv"),
            artifact_root,
        )
        require(count_csv_data_rows(routing_path) == frames_received, f"GOP routing rows do not match frames_received for {serial}")

    video_sanity_path = path_from(
        stream.get("video_sanity_json") or artifact_root / f"Cam{serial}_external_video_sanity.json",
        artifact_root,
    )
    sanity_status = verify_video_sanity(
        video_sanity_path,
        allow_missing_video_sanity or not bool(contract.get("require_video_sanity", True)),
    )

    return {
        "serial": serial,
        "summary_path": str(summary_path),
        "mp4_path": str(mp4_path),
        "frames_received": frames_received,
        "frames_encoded": frames_encoded,
        "shard_count": len(shards),
        "routing_policy": summary.get("routing_policy"),
        "video_sanity": sanity_status,
    }


def verify_analytics_root(analytics_root: Path, serials: list[str]) -> None:
    runs_path = analytics_root / "runs.json"
    runs_json = read_json(runs_path)
    rows: list[dict[str, Any]] = []
    for run in runs_json.get("runs", []):
        for row in run.get("camera_results", []):
            if str(row.get("camera_serial")) in serials:
                rows.append(row)
    require(len(rows) == len(serials), f"runs.json did not contain one row per verified camera in {analytics_root}")
    for row in rows:
        serial = str(row.get("camera_serial"))
        require(row.get("recording_sink_mode") == "external_ipc", f"analytics row {serial} is not external_ipc")
        require(row.get("pass_fail") == "pass", f"analytics row {serial} pass_fail={row.get('pass_fail')!r}")
        require(as_int(row.get("external_ipc_failures_final", 0), "external_ipc_failures_final") == 0, f"external IPC failures for {serial}")
        require(as_int(row.get("external_ipc_ack_timeouts_final", 0), "external_ipc_ack_timeouts_final") == 0, f"external IPC ACK timeouts for {serial}")
        acked = as_int(row.get("external_ipc_frames_acked_final", 0), "external_ipc_frames_acked_final")
        submitted = as_int(row.get("submitted_frames_final", 0), "submitted_frames_final")
        require(submitted == 0 or acked >= submitted, f"external IPC ACKed fewer frames than submitted for {serial}")


def verify(args: argparse.Namespace) -> None:
    artifact_root = Path(args.artifact_root).expanduser()
    require(artifact_root.exists(), f"artifact root does not exist: {artifact_root}")
    analytics_root = Path(args.analytics_root).expanduser() if args.analytics_root else None
    requested_cameras = args.camera

    spec = load_spec(args)
    contract = contract_from_spec(spec)
    if contract is None:
        contract = synthesize_contract(artifact_root, requested_cameras)
    require(contract.get("schema_id") in (None, CONTRACT_SCHEMA_ID), "unexpected external recorder contract schema_id")
    require(contract.get("schema_version", 1) == 1, "unexpected external recorder contract schema_version")
    require(contract.get("mode") == "diagnostic_ipc_v1", "external recorder contract mode must be diagnostic_ipc_v1")

    streams = selected_streams(contract, requested_cameras)
    summaries = [
        verify_summary(
            artifact_root,
            serial,
            stream,
            contract,
            args.ffprobe,
            args.allow_missing_video_sanity,
        )
        for serial, stream in streams.items()
    ]

    if analytics_root is not None:
        verify_analytics_root(analytics_root, list(streams.keys()))

    total_frames = sum(item["frames_received"] for item in summaries)
    print("External recorder verification passed")
    print(f"  artifact_root: {artifact_root}")
    if analytics_root is not None:
        print(f"  analytics_root: {analytics_root}")
    print(f"  streams: {len(summaries)}")
    print(f"  frames_received: {total_frames}")
    for item in summaries:
        print(
            "  "
            f"camera={item['serial']} frames={item['frames_received']} "
            f"encoded={item['frames_encoded']} shards={item['shard_count']} "
            f"routing={item['routing_policy']} video_sanity={item['video_sanity']}"
        )


def main() -> int:
    args = parse_args()
    try:
        verify(args)
    except VerificationError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
