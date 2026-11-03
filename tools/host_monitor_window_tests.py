#!/usr/bin/env python3
"""No-hardware tests for scripts/host_monitor_window.py: one GUI session's
host stall monitor files cut down to each recording's window."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "host_monitor_window.py"
sys.path.insert(0, str(REPO_ROOT / "scripts"))
from host_monitor_window import recording_summary, write_recording_copy  # noqa: E402

S = 1_000_000_000
T0 = 1_793_667_000 * S  # whole seconds, so UTC strings round-trip


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def utc(t_ns: int) -> str:
    import datetime
    return datetime.datetime.fromtimestamp(t_ns // S, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def write_csv(path: Path, header: list[str], rows: list[list[object]]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def make_session(directory: Path) -> str:
    """Twenty one-second samples; allocstall +1 per second from second 10."""
    prefix = str(directory / "session")
    counter_header = ["t_ns", "mono_ns", "allocstall_normal", "allocstall_movable", "compact_stall",
                      "thp_fault_fallback", "pgmajfault", "isolated_core_irqs", "nvme_wr_sectors",
                      "smi_count", "loadavg_1m", "dirty_kb"]
    counters = []
    for i in range(20):
        counters.append([T0 + i * S, i, max(0, i - 10), 0, 2 * i, 0, 0, 1000 * i, 2048 * i, -1, 1.0 + i / 10, 0])
    write_csv(Path(prefix + "_counters.csv"), counter_header, counters)
    write_csv(Path(prefix + "_heartbeat.csv"), ["t_ns", "mono_ns", "late_ms"],
              [[T0 + 3 * S + 5, 0, "4.000"], [T0 + 12 * S + 5, 0, "7.500"]])
    write_csv(Path(prefix + "_irq_top.csv"),
              ["t_ns", "isolated_irq_delta", "top1", "top1_delta", "top2", "top2_delta", "top3", "top3_delta"],
              [[T0 + i * S, 9000 if i in (4, 13) else 100, "TLB: TLB shootdowns", 50, "", "", "", ""]
               for i in range(1, 20)])
    write_csv(Path(prefix + "_procs.csv"),
              ["t_ns", "loadavg_1m", "foreign_cpu_pct", "top1", "top1_cpu_pct", "top2", "top2_cpu_pct",
               "top3", "top3_cpu_pct"],
              [[T0 + i * S, 1.0 + i / 10, 120.0 if i >= 10 else 10.0,
                "cc1plus" if i >= 10 else "citrus", 100.0, "chrome", 20.0, "", ""] for i in range(1, 20)])
    Path(prefix + "_kernel_tuning.txt").write_text("kernel tuning: PASS\n")
    return prefix


def make_recording(directory: Path, name: str, begin: int, start: int, stop: int | None) -> Path:
    folder = directory / name
    folder.mkdir()
    recording = {"started_at_utc": utc(T0 + start * S)}
    if stop is not None:
        recording["stop_requested_at_utc"] = utc(T0 + stop * S)
    (folder / "recording_session.json").write_text(json.dumps({"recording": recording}))
    audit = {"record": "session", "session_realtime_ns": T0 + begin * S}
    (folder / "recording_startup_audit.jsonl").write_text(json.dumps(audit) + "\n")
    return folder


def test_each_recording_gets_its_own_window() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        prefix = make_session(tmp_path)
        first = make_recording(tmp_path, "first", begin=2, start=3, stop=7)
        second = make_recording(tmp_path, "second", begin=11, start=12, stop=16)

        a, _ = recording_summary(prefix, str(first))
        require(a["basis"] == "recording_window", "basis")
        require(a["window"]["start_t_ns"] == T0 + 2 * S, "window starts at session begin, not started_at")
        require(a["window"]["coverage"] == "full", "coverage")
        require(a["heartbeat"]["gaps"] == 1 and a["heartbeat"]["max_gap_ms"] == 4.0, f"first gaps {a['heartbeat']}")
        require(a["deltas"]["allocstall_normal"] == 0, "no stalls before second 10")
        require(a["deltas"]["compact_stall"] == 10, f"compact delta 2..7 s {a['deltas']}")
        require(a["isolated_irq_burst_seconds"] == 1, "burst at second 4 only")
        require(a["compilers_seen"] == {}, "no compiler in the first window")
        require(abs(a["nvme_written_mb"] - 5 * 2048 * 512 / 1e6) < 1e-9, "nvme delta")

        b, _ = recording_summary(prefix, str(second))
        require(b["heartbeat"]["gaps"] == 1 and b["heartbeat"]["max_gap_ms"] == 7.5, f"second gaps {b['heartbeat']}")
        require(b["deltas"]["allocstall_normal"] == 5, f"allocstall 11..16 s {b['deltas']}")
        require(b["isolated_irq_burst_seconds"] == 1, "burst at second 13 only")
        require(b["foreign_busy_samples"] == 6, f"busy samples {b['foreign_busy_samples']}")
        require(abs(b["compilers_seen"].get("cc1plus", 0) - 6.0) < 1e-9, f"compilers {b['compilers_seen']}")


def test_open_recording_and_partial_coverage() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        prefix = make_session(tmp_path)
        running = make_recording(tmp_path, "running", begin=15, start=15, stop=None)
        s, _ = recording_summary(prefix, str(running))
        require(s["window"]["end_t_ns"] is None, "no stop yet")
        require(s["ended_t_ns"] == T0 + 19 * S, "runs to the latest sample")
        early = make_recording(tmp_path, "early", begin=-5, start=-4, stop=3)
        e, _ = recording_summary(prefix, str(early))
        require(e["window"]["coverage"] == "partial", "monitor started after the recording")


def test_write_and_cli() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        prefix = make_session(tmp_path)
        folder = make_recording(tmp_path, "rec", begin=2, start=3, stop=7)
        write_recording_copy(prefix, str(folder))
        for name in ("summary.json", "counters.csv", "heartbeat.csv", "irq_top.csv", "procs.csv",
                     "kernel_tuning.txt"):
            require((folder / f"host_monitor_{name}").is_file(), f"host_monitor_{name} written")
        with (folder / "host_monitor_counters.csv").open() as f:
            rows = list(csv.DictReader(f))
        require(int(rows[0]["t_ns"]) == T0 + 2 * S and int(rows[-1]["t_ns"]) == T0 + 7 * S, "windowed counters")

        aborted = tmp_path / "aborted"
        aborted.mkdir()
        result = subprocess.run([sys.executable, str(SCRIPT), "--prefix", prefix, "--recording-folder", str(aborted)],
                                capture_output=True, text=True)
        require(result.returncode == 1, "aborted start fails")
        require("no recording_session.json" in result.stderr and "Traceback" not in result.stderr,
                f"clean error: {result.stderr}")


def main() -> int:
    test_each_recording_gets_its_own_window()
    test_open_recording_and_partial_coverage()
    test_write_and_cli()
    print("host_monitor_window_tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
