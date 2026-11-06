#!/usr/bin/env python3
"""Tests for the recording-window camera drop check in validate_gui_ptp_recording.py."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import validate_gui_ptp_recording as validator  # noqa: E402

HEADER = "recording_frame_id,stream_frame_id,camera_frame_id,timestamp,has_detection\n"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def ledger(folder: Path, serial: str, rows: list[tuple[int, int, int]]) -> None:
    body = "".join(f"{r},{s},{c},0,1\n" for r, s, c in rows)
    (folder / f"Cam{serial}_crop_meta.csv").write_text(HEADER + body)


def judge(window: dict | None, stream_total: int | None) -> validator.Reporter:
    reporter = validator.Reporter(verbose=False)
    validator.check_camera_drop_counter(reporter, "2010094", "PTP frame gaps", stream_total, window)
    return reporter


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        # 2026_11_06_14_17_17 Cam2010094: 15 drops before Record, ids wrapped six times since.
        ledger(folder, "2010094", [(1, 62622, 62637), (2, 62623, 62638), (362128, 424749, 31554)])
        # Three frames lost inside the recording, across a wrap.
        ledger(folder, "2010093", [(1, 100, 65530), (50, 149, 47)])
        # One row only: start and end are the same frame.
        ledger(folder, "2010095", [(1, 7, 9)])
        (folder / "Cam2010096_crop_meta.csv").write_text(HEADER)
        windows = validator.recording_window_camera_drops(folder, ["2010093", "2010094", "2010095", "2010096", "2010097"])

    require(windows["2010094"]["dropped_during_recording"] == 0, f"pre-record drops only: {windows['2010094']}")
    require(windows["2010093"]["dropped_during_recording"] == 3, f"drops across a wrap: {windows['2010093']}")
    require(windows["2010095"]["dropped_during_recording"] == 0, "a one-row ledger has no loss")
    require("2010096" not in windows and "2010097" not in windows, "an empty or missing ledger gives no window")

    reporter = judge(windows["2010094"], 15)
    require(not reporter.failures, f"drops before Record do not fail: {reporter.failures}")
    require(any("all before the recording" in w for w in reporter.warnings), f"they warn: {reporter.warnings}")

    reporter = judge({**windows["2010094"], "dropped_during_recording": 4}, 15)
    require(any("=4 during the recording (15 since stream start)" in f for f in reporter.failures),
            f"drops inside the recording fail: {reporter.failures}")

    reporter = judge(None, 15)
    require(any(f == "Cam2010094 PTP frame gaps=15" for f in reporter.failures), "without a ledger the stream total fails")
    reporter = judge(None, 0)
    require(not reporter.failures and reporter.passes == ["Cam2010094 PTP frame gaps=0"], "zero passes")
    reporter = judge(windows["2010094"], 0)
    require(not reporter.failures and not reporter.warnings, "zero passes with a ledger too")

    print("validate_camera_drop_window_tests: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}")
        sys.exit(1)
