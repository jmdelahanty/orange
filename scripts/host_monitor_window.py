#!/usr/bin/env python3
"""Host stall monitor record for one recording.

host_stall_monitor.py runs once per GUI session (from the sudo wrapper) and
its own summary covers the whole session, which may hold several recordings.
This cuts the monitor's per-second files down to one recording's window
(from the session's start, recording_startup_audit.jsonl session_realtime_ns,
to recording_session.json recording.stop_requested_at_utc) and builds the same summary fields for that window only.

  host_monitor_window.py --prefix <monitor prefix> --recording-folder <folder>
      prints the window summary as JSON
  ... --write
      also writes host_monitor_summary.json and the windowed
      host_monitor_{heartbeat,counters,irq_top,procs}.csv, plus a copy of the
      session's host_monitor_kernel_tuning.txt, into the recording folder

The counters are sampled once per second and the recording times have 1 s
resolution, so deltas are taken from the last sample at or before the start
to the first sample at or after the stop. foreign_cpu_seconds_by_process and
compilers_seen come from the three busiest foreign processes the monitor
keeps per sample, not every process (basis recorded in the summary).
Heartbeat gap rows are flushed as they happen, so an empty heartbeat file
means no gaps so far.
"""
import argparse
import csv
import datetime
import json
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_stall_monitor import COMPILER_NAMES  # noqa: E402

DELTA_KEYS = ("allocstall_normal", "allocstall_movable", "compact_stall",
              "thp_fault_fallback", "pgmajfault", "isolated_core_irqs")
IRQ_BURST_PER_S = 5000  # as host_stall_monitor.py
FOREIGN_BUSY_PCT = 50.0  # as host_stall_monitor.py


def utc_to_ns(text):
    stamp = datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
    return int(stamp.timestamp()) * 1_000_000_000


def session_begin_ns(folder):
    """Realtime ns at which the recording session began (folder created,
    before recorders start), from the startup audit's session record."""
    try:
        with open(Path(folder) / "recording_startup_audit.jsonl") as f:
            first = json.loads(f.readline())
    except (OSError, ValueError):
        return None
    value = first.get("session_realtime_ns") if first.get("record") == "session" else None
    return int(value) if value else None


def recording_window(folder):
    """(start_ns, end_ns, source) of a recording; end_ns is None while it records.
    The window starts when the recording session begins, so the recorder start-up
    (where start-up frame loss happens) is inside it."""
    session_path = Path(folder) / "recording_session.json"
    if not session_path.is_file():
        raise ValueError(f"{folder} has no recording_session.json (start aborted, or still starting)")
    session = json.loads(session_path.read_text())
    recording = session.get("recording", {})
    started = recording.get("started_at_utc")
    if not started:
        raise ValueError(f"{folder}/recording_session.json has no recording.started_at_utc")
    stopped = recording.get("stop_requested_at_utc")
    start_ns = utc_to_ns(started)
    begin_ns = session_begin_ns(folder)
    if begin_ns is not None and begin_ns < start_ns:
        start_ns = begin_ns
        source = ("recording_startup_audit.jsonl session_realtime_ns.."
                  "recording_session.json recording.stop_requested_at_utc")
    else:
        source = "recording_session.json recording.started_at_utc..stop_requested_at_utc"
    return start_ns, (utc_to_ns(stopped) if stopped else None), source


def read_rows(path):
    try:
        with open(path, newline="") as f:
            return list(csv.DictReader(f))
    except FileNotFoundError:
        return None


def window_summary(prefix, start_ns, end_ns, source="explicit"):
    """Summary of the monitor files under `prefix` between start_ns and end_ns
    (None = up to the latest sample). Returns (summary, windowed rows by kind)."""
    counters = read_rows(prefix + "_counters.csv")
    if not counters:
        raise ValueError(f"no monitor counters at {prefix}_counters.csv")
    t = [int(r["t_ns"]) for r in counters]
    last_sample_ns = t[-1]
    end = end_ns if end_ns is not None else last_sample_ns
    base_i = max([i for i, v in enumerate(t) if v <= start_ns], default=0)
    end_i = min([i for i, v in enumerate(t) if v >= end], default=len(t) - 1)
    base, last = counters[base_i], counters[end_i]
    coverage = "full"
    if t[0] > start_ns or (end_ns is not None and last_sample_ns < end_ns):
        coverage = "partial"

    def in_window(rows):
        return [r for r in (rows or []) if start_ns <= int(r["t_ns"]) <= end]

    heartbeat = in_window(read_rows(prefix + "_heartbeat.csv"))
    irq_top = in_window(read_rows(prefix + "_irq_top.csv"))
    procs = in_window(read_rows(prefix + "_procs.csv"))

    burst_seconds = 0
    burst_sources = {}
    for r in irq_top:
        if int(r["isolated_irq_delta"]) > IRQ_BURST_PER_S:
            burst_seconds += 1
            burst_sources[r["top1"]] = burst_sources.get(r["top1"], 0) + 1
    foreign = {}
    compilers = {}
    foreign_busy = 0
    loadavg_max = float(base["loadavg_1m"])
    for r in procs:
        loadavg_max = max(loadavg_max, float(r["loadavg_1m"]))
        if float(r["foreign_cpu_pct"]) >= FOREIGN_BUSY_PCT:
            foreign_busy += 1
        for n in (1, 2, 3):
            comm, pct = r.get(f"top{n}") or "", r.get(f"top{n}_cpu_pct") or ""
            if not comm or not pct:
                continue
            seconds = float(pct) / 100.0  # one-second samples
            foreign[comm] = foreign.get(comm, 0.0) + seconds
            if comm in COMPILER_NAMES:
                compilers[comm] = compilers.get(comm, 0.0) + seconds
    smi_base = int(base.get("smi_count", -1))
    summary = {
        "basis": "recording_window",
        "window": {
            "start_t_ns": start_ns,
            "end_t_ns": end_ns,
            "source": source,
            "coverage": coverage,
            "first_sample_t_ns": int(base["t_ns"]),
            "last_sample_t_ns": int(last["t_ns"]),
        },
        "session_monitor_prefix": prefix,
        "started_t_ns": int(base["t_ns"]),
        "ended_t_ns": int(last["t_ns"]),
        "duration_s": (int(last["t_ns"]) - int(base["t_ns"])) / 1e9,
        "heartbeat": {
            "gaps": len(heartbeat),
            "max_gap_ms": round(max((float(r["late_ms"]) for r in heartbeat), default=0.0), 3),
            "iterations": None,
        },
        "deltas": {k: int(last[k]) - int(base[k]) for k in DELTA_KEYS},
        "smi_delta": (int(last["smi_count"]) - smi_base) if smi_base >= 0 else None,
        "nvme_written_mb": (int(last["nvme_wr_sectors"]) - int(base["nvme_wr_sectors"])) * 512 / 1e6,
        "isolated_irq_burst_seconds": burst_seconds,
        "isolated_irq_burst_sources": burst_sources,
        "loadavg_1m_start": float(base["loadavg_1m"]),
        "loadavg_1m_max": loadavg_max,
        "foreign_busy_samples": foreign_busy,
        "foreign_cpu_seconds_by_process": dict(sorted(foreign.items(), key=lambda kv: kv[1], reverse=True)[:12]),
        "foreign_cpu_basis": "top3_processes_per_sample",
        "compilers_seen": compilers,
    }
    rows = {
        "counters": counters[base_i:end_i + 1],
        "heartbeat": heartbeat,
        "irq_top": irq_top,
        "procs": procs,
    }
    return summary, rows


def recording_summary(prefix, folder):
    start_ns, end_ns, source = recording_window(folder)
    return window_summary(prefix, start_ns, end_ns, source)


def write_recording_copy(prefix, folder):
    summary, rows = recording_summary(prefix, folder)
    folder = Path(folder)
    for kind, kind_rows in rows.items():
        header_source = prefix + f"_{kind}.csv"
        try:
            with open(header_source, newline="") as f:
                header = next(csv.reader(f), None)
        except FileNotFoundError:
            continue
        if not header:
            continue
        with open(folder / f"host_monitor_{kind}.csv", "w", newline="") as out:
            writer = csv.DictWriter(out, fieldnames=header)
            writer.writeheader()
            writer.writerows(kind_rows)
    tuning = prefix + "_kernel_tuning.txt"
    if os.path.isfile(tuning):
        shutil.copyfile(tuning, folder / "host_monitor_kernel_tuning.txt")
    (folder / "host_monitor_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prefix", required=True, help="host_stall_monitor prefix (no suffix)")
    ap.add_argument("--recording-folder", required=True)
    ap.add_argument("--write", action="store_true", help="write host_monitor_* files into the recording folder")
    a = ap.parse_args()
    try:
        if a.write:
            summary = write_recording_copy(a.prefix, a.recording_folder)
        else:
            summary, _ = recording_summary(a.prefix, a.recording_folder)
    except (OSError, ValueError, KeyError) as exc:
        print(f"host_monitor_window: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
