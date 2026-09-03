#!/usr/bin/env python3
"""Synthetic checks for scripts/analyze_yolo_latency_phases.py."""

from __future__ import annotations

import csv
import json
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import analyze_yolo_latency_phases as analysis  # noqa: E402

PERF_HEADER = [
    "frame_id",
    "recording_frame_id",
    "timestamp_sys",
    "ok",
    "fps",
    "queue_depth_at_worker_start",
    "acquisition_to_worker_start_ms",
    "acquisition_to_yolo_enqueue_ms",
    "acquisition_to_ptp_done_ms",
    "yolo_queue_wait_ms",
    "acquisition_to_detect_done_ms",
    "worker_start_to_detect_done_ms",
    "total_ms",
    "cpu_pre_sync_ms",
    "cpu_post_sync_ms",
    "cpu_event_record_ms",
    "yolo_affinity_effective_cpus",
    "sync_mode",
]


def write_synthetic_camera(run_dir: Path, serial: str, detect_gpu: int, other_gpu: int, frames: int) -> None:
    rng = np.random.default_rng(int(serial))
    gop = 25
    (run_dir / "external_recorder").mkdir(exist_ok=True)
    with open(run_dir / f"Cam{serial}_yolo_perf.csv", "w", newline="") as perf, open(
        run_dir / "external_recorder" / f"Cam{serial}_external_gop_routing.csv", "w", newline=""
    ) as routing, open(
        run_dir / "external_recorder" / f"Cam{serial}_external_detach.csv", "w", newline=""
    ) as detach:
        pw, rw, dw = csv.writer(perf), csv.writer(routing), csv.writer(detach)
        pw.writerow(PERF_HEADER)
        rw.writerow(
            ["frame_index", "recording_frame_id", "gop_index", "source_gpu_id", "assigned_gpu_id", "assigned_shard_id"]
        )
        dw.writerow(["frame_index", "recording_frame_id", "assigned_gpu_id", "copy_ms"])
        t0 = 1_700_000_000_000_000_000
        for i in range(frames):
            fid = i + 1  # local frame id, 1-based like the acquisition loop
            rec = i + 1
            gop_index = (rec - 1) // gop
            shard = gop_index % 2
            assigned = detect_gpu if shard == 0 else other_gpu
            same_die = assigned == detect_gpu
            base = 3.1 if same_die else 2.5
            total = base + rng.normal(0.0, 0.05)
            pre, post = 0.05, 0.01
            # PTP latch on every 100th frame (frame_id % 100 == 0) adds ~1.7 ms
            # before the worker starts.
            latch = fid % 100 == 0
            a2ws = 0.05 + (1.7 if latch else 0.0)
            ptp_done = 1.66 if latch else 0.0
            # A 1 Hz stall lands at 0.52 s into each second.
            ts = t0 + i * 10_000_000
            stall = 2.7 if (i % 100) == 52 else 0.001
            pw.writerow(
                [
                    fid,
                    rec,
                    ts,
                    1,
                    100.0,
                    0,
                    f"{a2ws:.6f}",
                    f"{a2ws - 0.01:.6f}",
                    f"{ptp_done:.6f}",
                    0.011,
                    f"{a2ws + total:.6f}",
                    f"{total - 0.01:.6f}",
                    f"{total:.6f}",
                    pre,
                    post,
                    stall,
                    "6",
                    "poll",
                ]
            )
            rw.writerow([i, rec, gop_index, detect_gpu, assigned, shard])
            dw.writerow([i, rec, assigned, 2.0 if same_die else 7.0])


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        run_dir = Path(tmp)
        write_synthetic_camera(run_dir, "2010093", 3, 4, 1000)
        write_synthetic_camera(run_dir, "2010094", 1, 2, 1000)
        json_path = run_dir / "out.json"
        rc = analysis.main([str(run_dir), "--json", str(json_path), "--quiet", "--steady-after", "0"])
        assert rc == 0
        result = json.loads(json_path.read_text())
        assert result["schema_id"] == "orange.yolo_latency_phases"
        assert sorted(result["cameras"]) == ["2010093", "2010094"]

        cam = result["cameras"]["2010093"]
        assert cam["rows_steady"] == 1000
        assert cam["sync_mode"] == "poll"
        assert cam["cycle_frames"] == 50, cam["cycle_frames"]

        split = cam["encoder_die_split"]
        assert split["detect_gpu"] == 3 and split["other_gpus"] == [4]
        assert split["same_die"]["mean"] > split["other_die"]["mean"] + 0.4
        assert abs(split["same_die_fraction"] - 0.5) < 0.05

        # Same-die frames are recording ids 1..25 of each 50-frame cycle.
        means = cam["cycle_phase"]["mean"]
        assert np.nanmean(means[1:26]) > np.nanmean(means[26:50]) + 0.4

        ptp = cam["ptp_latch"]
        assert ptp["latch_frames"] == 10
        assert ptp["latch_frames_acq_to_worker_start_ms"]["mean"] > 1.5
        assert ptp["other_frames_acq_to_worker_start_ms"]["mean"] < 0.1
        assert ptp["by_phase_mean_ms"][0] > 1.5

        stalls = cam["event_record_stalls"]
        assert stalls["count"] == 10
        assert stalls["peak_bin_fraction"] == 1.0
        assert stalls["hidden_in_gpu_wait"] is True

        detach = cam["detach_copy_ms_by_gpu"]
        assert detach["3"]["p50"] == 2.0 and detach["4"]["p50"] == 7.0

        hist = cam["total_ms_histogram"]
        assert sum(hist["same_die"]) + sum(hist["other_die"]) == 1000

        gpu_wait = cam["gpu_wait_ms"]["mean"]
        assert abs(gpu_wait - (cam["worker_total_ms"]["mean"] - 0.06)) < 1e-6

        # Camera filter and steady-after trimming.
        rc = analysis.main(
            [str(run_dir), "--json", str(json_path), "--quiet", "--cameras", "2010094", "--steady-after", "100"]
        )
        assert rc == 0
        result = json.loads(json_path.read_text())
        assert list(result["cameras"]) == ["2010094"]
        assert result["cameras"]["2010094"]["rows_steady"] == 900

        # A run without the external recorder CSVs still analyses.
        for name in os.listdir(run_dir / "external_recorder"):
            os.remove(run_dir / "external_recorder" / name)
        rc = analysis.main([str(run_dir), "--json", str(json_path), "--quiet"])
        assert rc == 0
        result = json.loads(json_path.read_text())
        assert result["cameras"]["2010093"]["encoder_die_split"] is None
        assert result["cameras"]["2010093"]["cycle_frames"] == 50
        assert "all" in result["cameras"]["2010093"]["total_ms_histogram"]
    print("analyze_yolo_latency_phases_tests: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
