#!/usr/bin/env python3
"""Summarize a scripts/trace_record_start_memory.sh trace directory.

  summarize_memory_trace.py <trace dir> [--json <path>]

Reads <dir>/trace.txt (tracefs text, mono clock) and <dir>/clock.json, and
answers, for the traced window:
  - who stalled in direct reclaim / direct compaction: process, allocation
    order (2^order pages of 4 KB), and the kernel stacks that asked;
  - how compaction attempts ended;
  - who sent IPIs, from which kernel function, with which callback, and how
    many deliveries landed on isolated CPUs;
  - which isolated CPUs took remote TLB shootdowns, and what was running there;
  - a per-second timeline in local wall-clock time.
Writes <dir>/summary.json unless --json says otherwise.
"""
from __future__ import annotations

import argparse
import collections
import datetime
import json
import re
import sys
from pathlib import Path

LINE = re.compile(r"^\s*(?P<task>.+?)-(?P<pid>\d+)\s+\[(?P<cpu>\d+)\]\s+(?:\S+\s+)?"
                  r"(?P<ts>\d+\.\d+):\s+(?P<event>[^:]+):\s?(?P<fields>.*)$")
STACK_HEADER = re.compile(r"^\s*.+?-\d+\s+\[\d+\]\s+(?:\S+\s+)?\d+\.\d+:\s+<stack trace>\s*$")
STACK_FRAME = re.compile(r"^\s*=>\s+(?P<frame>\S+)")
FIELD = re.compile(r"(\w+)[=:]\s*(\S+)")
STACK_SKIP = ("stack_trace_save", "trace_event_", "event_triggers_call", "trace_",
              "__traceiter_", "perf_trace", "ftrace")
STACK_DEPTH = 10


def parse_cpus(text: str) -> set[int]:
    cpus: set[int] = set()
    for part in text.strip().split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            cpus.update(range(int(a), int(b) + 1))
        else:
            cpus.add(int(part))
    return cpus


def parse_cpumask(text: str) -> set[int]:
    """Kernel cpumask bitmap as tracefs prints it: comma-separated 32-bit hex
    words, most significant first (e.g. 0e000000,40180000)."""
    cpus: set[int] = set()
    words = [w for w in text.strip().split(",") if w]
    for i, word in enumerate(reversed(words)):
        value = int(word, 16)
        bit = 0
        while value:
            if value & 1:
                cpus.add(i * 32 + bit)
            value >>= 1
            bit += 1
    return cpus


def fields_of(text: str) -> dict[str, str]:
    return {k: v for k, v in FIELD.findall(text)}


def frame_name(frame: str) -> str:
    return frame.split("+", 1)[0]


def summarize(directory: Path) -> dict:
    clock = json.loads((directory / "clock.json").read_text())
    offset = float(clock["realtime_minus_mono_s"])
    isolated = parse_cpus(clock.get("isolated", ""))

    reclaim = collections.Counter()          # (task, order)
    compact = collections.Counter()          # (task, order)
    compact_status = collections.Counter()   # status
    extfrag = collections.Counter()          # (task, alloc_order, fragmenting)
    stacks = collections.Counter()           # (event, task, frames)
    ipi = collections.Counter()              # (task, callsite, callback) -> sends
    ipi_iso = collections.Counter()          # same key -> deliveries to isolated cpus
    tlb_iso = collections.Counter()          # (cpu, task on that cpu)
    timeline = collections.defaultdict(collections.Counter)
    first_ts = last_ts = None

    pending = None  # (event, task) waiting for its stack
    last_event = None
    frames: list[str] = []

    def flush_stack():
        nonlocal pending, frames
        if pending and frames:
            kept = [frame_name(f) for f in frames if not frame_name(f).startswith(STACK_SKIP)]
            stacks[(pending[0], pending[1], tuple(kept[:STACK_DEPTH]))] += 1
        pending, frames = None, []

    with (directory / "trace.txt").open(errors="replace") as f:
        for raw in f:
            if raw.startswith("#"):
                continue
            m_frame = STACK_FRAME.match(raw)
            if m_frame:
                if pending:
                    frames.append(m_frame.group("frame"))
                continue
            if STACK_HEADER.match(raw):
                flush_stack()
                if last_event and last_event[0] in ("mm_compaction_try_to_compact_pages",
                                                    "mm_vmscan_direct_reclaim_begin"):
                    pending = last_event
                continue
            m = LINE.match(raw)
            if not m:
                continue
            event = m.group("event").strip()
            task = m.group("task").strip()
            cpu = int(m.group("cpu"))
            ts = float(m.group("ts"))
            second = int(ts + offset)
            first_ts = ts if first_ts is None else min(first_ts, ts)
            last_ts = ts if last_ts is None else max(last_ts, ts)
            flush_stack()
            fl = fields_of(m.group("fields"))
            last_event = (event, task)
            if event == "mm_vmscan_direct_reclaim_begin":
                reclaim[(task, int(fl.get("order", -1)))] += 1
                timeline[second]["direct_reclaim"] += 1
            elif event == "mm_compaction_try_to_compact_pages":
                compact[(task, int(fl.get("order", -1)))] += 1
                timeline[second]["direct_compaction"] += 1
            elif event == "mm_compaction_end":
                compact_status[fl.get("status", "?")] += 1
            elif event == "mm_page_alloc_extfrag":
                extfrag[(task, int(fl.get("alloc_order", -1)), fl.get("fragmenting", "?"))] += 1
            elif event in ("ipi_send_cpumask", "ipi_send_cpu"):
                targets = parse_cpumask(fl.get("cpumask", "")) if event == "ipi_send_cpumask" \
                    else {int(fl.get("cpu", -1))}
                key = (task, frame_name(fl.get("callsite", "?")), frame_name(fl.get("callback", "?")))
                ipi[key] += 1
                hits = len(targets & isolated)
                if hits:
                    ipi_iso[key] += hits
                    timeline[second]["ipi_deliveries_to_isolated"] += hits
            elif event == "tlb_flush":
                if cpu in isolated:
                    tlb_iso[(cpu, task)] += 1
                    timeline[second]["tlb_shootdowns_on_isolated"] += 1
        flush_stack()

    def local(sec: int) -> str:
        return datetime.datetime.fromtimestamp(sec).strftime("%H:%M:%S")

    def top(counter, n=15):
        return counter.most_common(n)

    summary = {
        "trace_dir": str(directory),
        "isolated_cpus": sorted(isolated),
        "window_local": [local(int(first_ts + offset)), local(int(last_ts + offset))] if first_ts else None,
        "direct_reclaim": {"total": sum(reclaim.values()),
                           "by_task_order": [{"task": t, "order": o, "count": c} for (t, o), c in top(reclaim)]},
        "direct_compaction": {"total": sum(compact.values()),
                              "by_task_order": [{"task": t, "order": o, "count": c} for (t, o), c in top(compact)],
                              "end_status": dict(compact_status)},
        "extfrag_fallbacks": [{"task": t, "alloc_order": o, "fragmenting": fr, "count": c}
                              for (t, o, fr), c in top(extfrag, 10)],
        "stacks": [{"event": e, "task": t, "count": c, "frames": list(fr)}
                   for (e, t, fr), c in top(stacks, 12)],
        "ipi_senders": [{"task": t, "callsite": cs, "callback": cb, "sends": c,
                         "deliveries_to_isolated": ipi_iso.get((t, cs, cb), 0)}
                        for (t, cs, cb), c in sorted(ipi.items(), key=lambda kv: -ipi_iso.get(kv[0], 0))[:15]],
        "ipi_deliveries_to_isolated_total": sum(ipi_iso.values()),
        "tlb_shootdowns_on_isolated": [{"cpu": cpu, "task_running": t, "count": c}
                                       for (cpu, t), c in top(tlb_iso, 20)],
        "timeline": [{"time": local(sec), **dict(counts)} for sec, counts in sorted(timeline.items())],
    }
    return summary


def render(s: dict) -> str:
    out = [f"memory trace {s['trace_dir']}", f"window (local): {s['window_local']}",
           f"isolated CPUs: {s['isolated_cpus']}", ""]
    out.append(f"direct reclaim: {s['direct_reclaim']['total']}")
    for r in s["direct_reclaim"]["by_task_order"]:
        out.append(f"  {r['count']:>6}  order {r['order']:>2} ({4 << r['order'] if r['order'] >= 0 else '?'} KB)  {r['task']}")
    out.append(f"direct compaction: {s['direct_compaction']['total']}  end status {s['direct_compaction']['end_status']}")
    for r in s["direct_compaction"]["by_task_order"]:
        out.append(f"  {r['count']:>6}  order {r['order']:>2} ({4 << r['order'] if r['order'] >= 0 else '?'} KB)  {r['task']}")
    out.append("")
    out.append("kernel stacks that asked (most common first):")
    for st in s["stacks"]:
        out.append(f"  {st['count']:>6}  {st['event']}  {st['task']}")
        for fr in st["frames"]:
            out.append(f"            {fr}")
    out.append("")
    out.append(f"IPI deliveries to isolated CPUs: {s['ipi_deliveries_to_isolated_total']}")
    for r in s["ipi_senders"]:
        out.append(f"  {r['deliveries_to_isolated']:>7} to isolated / {r['sends']:>7} sends  "
                   f"{r['task']}  {r['callsite']} -> {r['callback']}")
    out.append("remote TLB shootdowns taken on isolated CPUs (cpu, task running there):")
    for r in s["tlb_shootdowns_on_isolated"]:
        out.append(f"  {r['count']:>7}  cpu {r['cpu']:>2}  {r['task_running']}")
    out.append("")
    out.append("per second: direct_reclaim direct_compaction ipi_to_isolated tlb_on_isolated")
    for r in s["timeline"]:
        out.append(f"  {r['time']}  {r.get('direct_reclaim', 0):>6} {r.get('direct_compaction', 0):>6} "
                   f"{r.get('ipi_deliveries_to_isolated', 0):>8} {r.get('tlb_shootdowns_on_isolated', 0):>8}")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace_dir", type=Path)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    s = summarize(a.trace_dir)
    (a.json or a.trace_dir / "summary.json").write_text(json.dumps(s, indent=2) + "\n")
    print(render(s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
