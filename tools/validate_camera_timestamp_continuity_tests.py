#!/usr/bin/env python3
"""Tests for the camera timestamp continuity and recording-window GetFrame error checks
in validate_gui_ptp_recording.py."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import validate_gui_ptp_recording as validator  # noqa: E402

META_HEADER = "recording_frame_id,timestamp,timestamp_sys,stream_frame_id\n"
PERF_HEADER = "timestamp_utc,stream_frame_id,recording_frame_id,camera_dropped_frames,get_frame_errors\n"
START_NS = 1794349349914896172


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def camera_timestamps(count: int, lost_after: set[int] = frozenset()) -> list[int]:
    """4 ms frames with a few ns of jitter, like the rig's (deltas 4,000,004 /
    3,999,989 / 4,000,007: mean exactly 4 ms, median 4 ns long, which over
    800,000 frames adds up to most of a period), skipping one frame after each
    index in lost_after."""
    jitter = [1, 5, -6]
    stamps = []
    frame = 0
    while len(stamps) < count:
        stamps.append(START_NS + frame * 4_000_000 + jitter[frame % 3])
        frame += 2 if len(stamps) in lost_after else 1
    return stamps


def write_meta(path: Path, stamps: list[int], first_id: int = 1) -> None:
    rows = "".join(f"{first_id + i},{ts},0,{i}\n" for i, ts in enumerate(stamps))
    path.write_text(META_HEADER + rows)


def run(folder: Path, metas: dict[str, list[Path]], window_drops: dict | None = None) -> tuple:
    clips = []
    for serial, paths in metas.items():
        for index, path in enumerate(paths):
            while len(clips) <= index:
                clips.append({"artifacts": {"metadata": {}}})
            clips[index]["artifacts"]["metadata"][serial] = str(path)
    (folder / "recording_session.json").write_text(json.dumps({"clips": clips}))
    reporter = validator.Reporter(verbose=False)
    results = validator.check_camera_timestamp_continuity(reporter, folder, list(metas), window_drops)
    return reporter, results


def test_timestamps() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        # Long enough that a 1 ns period error would add up to a whole frame.
        clean = camera_timestamps(800_000)
        write_meta(folder / "clean.csv", clean)
        reporter, results = run(folder, {"2010095": [folder / "clean.csv"]})
        require(results["2010095"]["missing_frames"] == 0, f"jitter is not loss: {results['2010095']}")
        require(not reporter.failures, f"clean recording passes: {reporter.failures}")

        # 2026_11_10_17_21_50 Cam2010096: one frame lost after recorded row 553288.
        lossy = camera_timestamps(797_137, {553_288})
        write_meta(folder / "lossy.csv", lossy)
        window = {"2010096": {"dropped_during_recording": 1, "source": "Cam2010096_crop_meta.csv"}}
        reporter, results = run(folder, {"2010096": [folder / "lossy.csv"]}, window)
        require(results["2010096"]["missing_frames"] == 1, f"one lost frame: {results['2010096']}")
        require(results["2010096"]["gaps"][0]["after_recorded_row"] == 553_288, "gap position")
        require(any("1 camera frame(s) missing" in f for f in reporter.failures), f"loss fails: {reporter.failures}")
        require(not any("disagree" in f for f in reporter.failures), "counter and timestamps agree")

        # The counter says 0 but a frame is missing (a counter blind spot).
        window = {"2010096": {"dropped_during_recording": 0, "source": "Cam2010096_crop_meta.csv"}}
        reporter, _ = run(folder, {"2010096": [folder / "lossy.csv"]}, window)
        require(any("disagree with the timestamp check (1)" in f for f in reporter.failures),
                f"counter disagreement fails: {reporter.failures}")

        # Two rolling clips: the step across the clip boundary is checked too.
        stamps = camera_timestamps(6_000, {3_000})
        write_meta(folder / "clip0.csv", stamps[:3_000])
        write_meta(folder / "clip1.csv", stamps[3_000:], first_id=3_001)
        reporter, results = run(folder, {"2010093": [folder / "clip0.csv", folder / "clip1.csv"]})
        require(results["2010093"]["missing_frames"] == 1, f"loss at the clip boundary: {results['2010093']}")

        # A repeated timestamp is not a frame period.
        stamps = camera_timestamps(5_000)
        stamps.insert(2_500, stamps[2_499])
        write_meta(folder / "repeat.csv", stamps)
        reporter, results = run(folder, {"2010094": [folder / "repeat.csv"]})
        require(results["2010094"]["non_advancing_steps"] == 1, f"repeat counted: {results['2010094']}")
        require(any("shorter than half a period" in f for f in reporter.failures), "repeat fails")

        # No metadata: warn, do not pass.
        reporter, results = run(folder, {"2010096": [folder / "missing.csv"]})
        require("2010096" not in results and reporter.warnings and not reporter.passes, "missing metadata warns")


def write_perf(folder: Path, serial: str, rows: list[tuple[int, int]]) -> None:
    body = "".join(f"t,{i},{rec},0,{errors}\n" for i, (rec, errors) in enumerate(rows))
    (folder / f"Cam{serial}_pipeline_perf.csv").write_text(PERF_HEADER + body)


def judge_errors(window: dict | None, total: int | None) -> validator.Reporter:
    reporter = validator.Reporter(verbose=False)
    validator.check_get_frame_errors(reporter, "2010096", total, window)
    return reporter


def test_get_frame_errors() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        write_perf(folder, "2010093", [(0, 0), (0, 1), (21, 1), (500, 1), (900, 1)])
        write_perf(folder, "2010096", [(0, 1), (21, 1), (85451, 2), (737632, 3)])
        write_perf(folder, "2010094", [(5, 2), (900, 2)])
        windows = validator.recording_window_get_frame_errors(folder, ["2010093", "2010094", "2010096", "2010095"])

    require(windows["2010093"]["errors_during_recording"] == 0, f"before Record only: {windows['2010093']}")
    require(windows["2010096"]["errors_during_recording"] == 2, f"two during: {windows['2010096']}")
    require(windows["2010094"]["baseline"] == "stream_start"
            and windows["2010094"]["errors_during_recording"] == 2,
            f"no pre-record sample counts from stream start: {windows['2010094']}")
    require("2010095" not in windows, "missing perf CSV gives no window")

    reporter = judge_errors(windows["2010093"], 1)
    require(not reporter.failures and any("none during the recording" in w for w in reporter.warnings),
            f"errors before Record warn: {reporter.failures} {reporter.warnings}")
    reporter = judge_errors(windows["2010096"], 3)
    require(any("GetFrame errors=2 during the recording (3 since stream start" in f for f in reporter.failures),
            f"errors during the recording fail: {reporter.failures}")
    reporter = judge_errors(None, 3)
    require(reporter.failures == ["Cam2010096 GetFrame errors=3"], "without a window the stream total fails")
    reporter = judge_errors(None, 0)
    require(not reporter.failures, "zero passes")


def main() -> int:
    test_timestamps()
    test_get_frame_errors()
    print("validate_camera_timestamp_continuity_tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
