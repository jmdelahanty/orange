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


COMPILER_NAMES = ("cc1plus", "cc1", "nvcc", "cicc", "ptxas", "ld", "ld.gold", "ld.lld", "lld", "gmake", "make",
                  "ninja", "cmake", "rustc", "cargo", "clang", "clang++", "g++", "gcc", "as")
ORANGE_NAMES = ("orange", "orange_client", "external_record", "external_recorder_ipc_probe", "host_stall_monitor")


def read_process_cpu():
    """Returns {pid: (comm, utime+stime ticks)} for every process, cheaply."""
    out = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/stat") as f:
                stat = f.read()
        except OSError:
            continue
        lp = stat.rfind(")")
        comm = stat[stat.find("(") + 1:lp]
        fields = stat[lp + 2:].split()
        try:
            out[int(pid)] = (comm, int(fields[11]) + int(fields[12]))
        except (IndexError, ValueError):
            continue
    return out


def read_loadavg():
    try:
        return float(open("/proc/loadavg").read().split()[0])
    except (OSError, ValueError):
        return -1.0


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
    per_irq = {}
    with open("/proc/interrupts") as f:
        header = f.readline().split()
        cols = [i for i, c in enumerate(header) if c.startswith("CPU") and int(c[3:]) in isolated]
        for line in f:
            parts = line.split()
            if not parts or not parts[0].endswith(":"):
                continue
            total = 0
            for i in cols:
                if i + 1 < len(parts) and parts[i + 1].isdigit():
                    total += int(parts[i + 1])
            iso_irq += total
            name = parts[0] + " " + " ".join(parts[len(header) + 1:])[:48] if len(parts) > len(header) + 1 else parts[0]
            per_irq[name] = total
    row["isolated_core_irqs"] = iso_irq
    row["_per_irq"] = per_irq  # not written to the counters row; see top-IRQ log
    row["smi_count"] = read_smi()
    row["loadavg_1m"] = read_loadavg()
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
    per_irq_prev = first.pop("_per_irq")
    cw = csv.DictWriter(counters_out, fieldnames=list(first.keys()))
    cw.writeheader()
    cw.writerow(first)
    counters_out.flush()
    # Top interrupt sources on the isolated cores per sample (name, delta),
    # so an interrupt burst can be attributed to a device or IPI kind.
    irq_out = open(a.prefix + "_irq_top.csv", "w", newline="")
    iw = csv.writer(irq_out)
    iw.writerow(["t_ns", "isolated_irq_delta", "top1", "top1_delta", "top2", "top2_delta", "top3", "top3_delta"])
    # Busy processes other than Orange and its recorders, per sample: who
    # else used the CPU while the run was going (a concurrent build shows
    # up here as cc1plus/ld/gmake with large CPU deltas).
    procs_out = open(a.prefix + "_procs.csv", "w", newline="")
    pw = csv.writer(procs_out)
    pw.writerow(["t_ns", "loadavg_1m", "foreign_cpu_pct", "top1", "top1_cpu_pct", "top2", "top2_cpu_pct", "top3", "top3_cpu_pct"])
    proc_prev = read_process_cpu()
    proc_prev_t = time.monotonic()
    ticks_per_s = os.sysconf("SC_CLK_TCK")
    foreign_totals = {}          # comm -> cpu seconds by non-Orange processes
    compilers_seen = {}          # comm -> cpu seconds
    foreign_busy_samples = 0     # samples where foreign processes used >= 50% of one core
    loadavg_max = 0.0
    stopping = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stopping.set())
    signal.signal(signal.SIGTERM, lambda *_: stopping.set())
    print("host_stall_monitor: heartbeat core=%d gap>%.1f ms, smi=%s, isolated=%s" % (
        core, a.gap_ms, "readable" if first["smi_count"] >= 0 else ("not supported on AMD" if cpu_is_amd() else "unavailable (run as root)"),
        ",".join(map(str, sorted(isolated))) or "none"), flush=True)
    last = first
    prev_iso = first["isolated_core_irqs"]
    while not stopping.is_set() and not os.path.exists(stop_file):
        time.sleep(a.interval_s)
        last = read_counters(isolated)
        per_irq = last.pop("_per_irq")
        cw.writerow(last)
        counters_out.flush()
        deltas = sorted(((per_irq[k] - per_irq_prev.get(k, 0), k) for k in per_irq), reverse=True)[:3]
        iw.writerow([last["t_ns"], last["isolated_core_irqs"] - prev_iso] + [x for d, k in deltas for x in (k, d)])
        irq_out.flush()
        per_irq_prev = per_irq
        prev_iso = last["isolated_core_irqs"]
        # busy processes
        proc_now = read_process_cpu()
        now_t = time.monotonic()
        span = max(1e-3, now_t - proc_prev_t)
        by_comm = {}
        for pid, (comm, ticks) in proc_now.items():
            prev = proc_prev.get(pid)
            if not prev or prev[0] != comm:
                continue
            d = (ticks - prev[1]) / ticks_per_s / span * 100.0  # percent of one core
            if d <= 0:
                continue
            if any(comm.startswith(n) for n in ORANGE_NAMES):
                continue
            by_comm[comm] = by_comm.get(comm, 0.0) + d
        foreign = sum(by_comm.values())
        if foreign >= 50.0:
            foreign_busy_samples += 1
        for comm, pct in by_comm.items():
            foreign_totals[comm] = foreign_totals.get(comm, 0.0) + pct * span / 100.0
            if comm in COMPILER_NAMES:
                compilers_seen[comm] = compilers_seen.get(comm, 0.0) + pct * span / 100.0
        top = sorted(by_comm.items(), key=lambda kv: kv[1], reverse=True)[:3]
        loadavg_max = max(loadavg_max, last["loadavg_1m"])
        pw.writerow([last["t_ns"], last["loadavg_1m"], round(foreign, 1)] + [x for c, pct in top for x in (c, round(pct, 1))])
        procs_out.flush()
        proc_prev, proc_prev_t = proc_now, now_t
    hb.stop.set()
    hb.join(timeout=2)
    hb_out.close()
    counters_out.close()
    irq_out.close()
    procs_out.close()
    # interrupt bursts on the isolated cores, with their top sources
    burst_seconds = 0
    burst_sources = {}
    try:
        with open(a.prefix + "_irq_top.csv") as f:
            rd = csv.DictReader(f)
            for r in rd:
                if int(r["isolated_irq_delta"]) > 5000:
                    burst_seconds += 1
                    burst_sources[r["top1"]] = burst_sources.get(r["top1"], 0) + 1
    except (OSError, ValueError, KeyError):
        pass
    summary = {
        "started_t_ns": first["t_ns"], "ended_t_ns": last["t_ns"],
        "duration_s": (last["t_ns"] - first["t_ns"]) / 1e9,
        "heartbeat": {"core": core, "gap_threshold_ms": a.gap_ms, "iterations": hb.iterations,
                      "gaps": hb.gaps, "max_gap_ms": round(hb.max_gap_ms, 3)},
        "deltas": {k: last[k] - first[k] for k in ("allocstall_normal", "allocstall_movable", "compact_stall",
                                                    "thp_fault_fallback", "pgmajfault", "isolated_core_irqs")},
        "smi_delta": (last["smi_count"] - first["smi_count"]) if first["smi_count"] >= 0 else None,
        "nvme_written_mb": (last["nvme_wr_sectors"] - first["nvme_wr_sectors"]) * 512 / 1e6,
        "isolated_irq_burst_seconds": burst_seconds,
        "isolated_irq_burst_sources": burst_sources,
        "loadavg_1m_start": first["loadavg_1m"],
        "loadavg_1m_max": loadavg_max,
        "foreign_busy_samples": foreign_busy_samples,
        "foreign_cpu_seconds_by_process": dict(sorted(foreign_totals.items(), key=lambda kv: kv[1], reverse=True)[:12]),
        "compilers_seen": compilers_seen,
    }
    json.dump(summary, open(a.prefix + "_summary.json", "w"), indent=2)
    print(json.dumps(summary, indent=2))
    try:
        os.remove(stop_file)
    except OSError:
        pass


if __name__ == "__main__":
    main()
