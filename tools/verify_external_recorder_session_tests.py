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


def write_summary(
    root: Path,
    serial: str,
    *,
    queue_depth: int = 64,
    queue_high_water: int | None = 12,
    enqueue_age_p95_ms: float = 2.5,
    detach_depths: list[int] | None = None,
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
        },
        "external_encode_shards": [
            {
                "assigned_gpu_id": 5,
                "frames_encoded": 3,
                "frames_dropped": 0,
                "worker_failed": False,
            }
        ],
        "merged_output": {},
        "outputs": {
            "detach_csv": str(detach_path),
            "mp4": str(mp4_path),
        },
    }
    if queue_high_water is not None:
        summary["encode_queue_high_water"] = queue_high_water
    summary_path = root / f"Cam{serial}_external_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary_path, mp4_path


def verify_one(
    root: Path,
    summary_path: Path,
    mp4_path: Path,
    *,
    expected_depth: int | None = None,
    max_high_water: int | None = None,
    max_enqueue_age: float | None = None,
) -> dict:
    serial = "2010096"
    stream = {
        "stream_id": serial,
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
    verifier.ffprobe_video = lambda path, ffprobe: None
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
        )
    finally:
        verifier.ffprobe_video = original_ffprobe


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


def main() -> int:
    tests = [
        test_queue_thresholds_pass_and_summarize,
        test_queue_threshold_failures,
        test_queue_high_water_falls_back_to_detach_csv,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("verify_external_recorder_session_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
