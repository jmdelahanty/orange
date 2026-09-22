#!/usr/bin/env python3
"""Low-overhead host stall monitor to run beside a GUI/headless run.

Answers one question per stall: was the whole host paused, or only Orange?

Three probes, all cheap:
  1. heartbeat  - a thread sleeps 1 ms in a loop, pinned to a housekeeping
                  core; every wake-up later than --gap-ms is logged with the
                  wall-clock time (ns since epoch, comparable to capture_sys_ns
                  in Cam*_owner_push.csv and timestamp_sys in Cam*_yolo_perf.csv).
  2. counters   - once per second: /proc/vmstat (dirty, writeback, allocation
                  stalls, compaction, THP), /proc/stat (procs_blocked, ctxt),
                  /proc/diskstats (nvme sectors written), /proc/interrupts
                  (total IRQs on the isolated cores), Dirty/Writeback from
                  /proc/meminfo.
  3. SMI count  - MSR 0x34 on cpu0 (Intel only; needs root and the msr
                  module). On AMD hosts, or when it cannot be read, the
                  column is -1 and firmware stalls stay unknown for that run.

Outputs <prefix>_heartbeat.csv, <prefix>_counters.csv and <prefix>_summary.json.
Stop with SIGINT/SIGTERM or by creating <prefix>.stop.

usage: host_stall_monitor.py --prefix /tmp/stall_run1 [--gap-ms 3] [--core 3]
Run under sudo to get the SMI counter; unprivileged otherwise.
"""
import argparse
import csv
import json
import os
import signal
import struct
import sys
import threading
import time

VMSTAT_KEYS = ("nr_dirty", "nr_writeback", "allocstall_normal", "allocstall_movable",
               "compact_stall", "compact_fail", "thp_fault_alloc", "thp_fault_fallback",
               "thp_collapse_alloc", "pgmajfault")


def read_isolated():
    try:
        s = open("/sys/devices/system/cpu/isolated").read().strip()
    except OSError:
        return set()
    out = set()
    for part in s.split(","):
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        else:
            out.add(int(part))
    return out


def cpu_is_amd():
    try:
        return "AuthenticAMD" in open("/proc/cpuinfo").read(4096)
    except OSError:
        return False


def read_smi():
    # MSR 0x34 (MSR_SMI_COUNT) exists on Intel only; AMD exposes no SMI
    # counter from Linux, so the column stays -1 there.
    if cpu_is_amd():
        return -1
    try:
        with open("/dev/cpu/0/msr", "rb") as f:
            f.seek(0x34)
            return struct.unpack("<Q", f.read(8))[0]
    except OSError:
        return -1


def read_counters(isolated):
    row = {"t_ns": time.time_ns(), "mono_ns": time.monotonic_ns()}
    vm = {}
    for line in open("/proc/vmstat"):
        k, v = line.split()
        if k in VMSTAT_KEYS:
            vm[k] = int(v)
    for k in VMSTAT_KEYS:
        row[k] = vm.get(k, -1)
    for line in open("/proc/meminfo"):
        if line.startswith("Dirty:"):
            row["dirty_kb"] = int(line.split()[1])
        elif line.startswith("Writeback:"):
            row["writeback_kb"] = int(line.split()[1])
    for line in open("/proc/stat"):
        if line.startswith("procs_blocked"):
            row["procs_blocked"] = int(line.split()[1])
        elif line.startswith("ctxt"):
            row["ctxt"] = int(line.split()[1])
    wr = 0
    for line in open("/proc/diskstats"):
        f = line.split()
        if len(f) > 9 and f[2].startswith("nvme") and "p" not in f[2][4:]:
            wr += int(f[9])
    row["nvme_wr_sectors"] = wr
    iso_irq = 0
    with open("/proc/interrupts") as f:
        header = f.readline().split()
        cols = [i for i, c in enumerate(header) if c.startswith("CPU") and int(c[3:]) in isolated]
        for line in f:
            parts = line.split()
            for i in cols:
                if i + 1 < len(parts) and parts[i + 1].isdigit():
                    iso_irq += int(parts[i + 1])
    row["isolated_core_irqs"] = iso_irq
    row["smi_count"] = read_smi()
    return row


class Heartbeat(threading.Thread):
    def __init__(self, gap_ms, core, out):
        super().__init__(daemon=True)
        self.gap_ns = int(gap_ms * 1e6)
        self.core = core
        self.out = out
        self.stop = threading.Event()
        self.gaps = 0
        self.max_gap_ms = 0.0
        self.iterations = 0

    def run(self):
        try:
            os.sched_setaffinity(0, {self.core})
        except OSError:
            pass
        try:
            os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(50))
        except (OSError, AttributeError):
            pass  # unprivileged: ordinary priority is still a usable probe
        w = csv.writer(self.out)
        w.writerow(["t_ns", "mono_ns", "late_ms"])
        period = 1_000_000
        nxt = time.monotonic_ns() + period
        while not self.stop.is_set():
            now = time.monotonic_ns()
            if now < nxt:
                time.sleep((nxt - now) / 1e9)
                now = time.monotonic_ns()
            late = now - nxt
            self.iterations += 1
            if late > self.gap_ns:
                self.gaps += 1
                late_ms = late / 1e6
                self.max_gap_ms = max(self.max_gap_ms, late_ms)
                w.writerow([time.time_ns(), now, "%.3f" % late_ms])
                self.out.flush()
            nxt = now + period


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--gap-ms", type=float, default=3.0)
    ap.add_argument("--core", type=int, default=None, help="housekeeping core for the heartbeat (default: lowest non-isolated core above 0)")
    ap.add_argument("--interval-s", type=float, default=1.0)
    a = ap.parse_args()
    isolated = read_isolated()
    core = a.core
    if core is None:
        core = next(c for c in range(1, os.cpu_count()) if c not in isolated)
    stop_file = a.prefix + ".stop"
    hb_out = open(a.prefix + "_heartbeat.csv", "w", newline="")
    hb = Heartbeat(a.gap_ms, core, hb_out)
    hb.start()
    counters_out = open(a.prefix + "_counters.csv", "w", newline="")
    first = read_counters(isolated)
    cw = csv.DictWriter(counters_out, fieldnames=list(first.keys()))
    cw.writeheader()
    cw.writerow(first)
    counters_out.flush()
    stopping = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stopping.set())
    signal.signal(signal.SIGTERM, lambda *_: stopping.set())
    print("host_stall_monitor: heartbeat core=%d gap>%.1f ms, smi=%s, isolated=%s" % (
        core, a.gap_ms, "readable" if first["smi_count"] >= 0 else ("not supported on AMD" if cpu_is_amd() else "unavailable (run as root)"),
        ",".join(map(str, sorted(isolated))) or "none"), flush=True)
    last = first
    while not stopping.is_set() and not os.path.exists(stop_file):
        time.sleep(a.interval_s)
        last = read_counters(isolated)
        cw.writerow(last)
        counters_out.flush()
    hb.stop.set()
    hb.join(timeout=2)
    hb_out.close()
    counters_out.close()
    summary = {
        "started_t_ns": first["t_ns"], "ended_t_ns": last["t_ns"],
        "duration_s": (last["t_ns"] - first["t_ns"]) / 1e9,
        "heartbeat": {"core": core, "gap_threshold_ms": a.gap_ms, "iterations": hb.iterations,
                      "gaps": hb.gaps, "max_gap_ms": round(hb.max_gap_ms, 3)},
        "deltas": {k: last[k] - first[k] for k in ("allocstall_normal", "allocstall_movable", "compact_stall",
                                                    "thp_fault_fallback", "pgmajfault", "isolated_core_irqs")},
        "smi_delta": (last["smi_count"] - first["smi_count"]) if first["smi_count"] >= 0 else None,
        "nvme_written_mb": (last["nvme_wr_sectors"] - first["nvme_wr_sectors"]) * 512 / 1e6,
    }
    json.dump(summary, open(a.prefix + "_summary.json", "w"), indent=2)
    print(json.dumps(summary, indent=2))
    try:
        os.remove(stop_file)
    except OSError:
        pass


if __name__ == "__main__":
    main()
