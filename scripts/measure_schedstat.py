#!/usr/bin/env python3
import argparse
import os
import subprocess
import time


def parse_args():
    parser = argparse.ArgumentParser(
        description="Measure per-thread off-CPU time via /proc/<pid>/task/<tid>/schedstat."
    )
    parser.add_argument(
        "--pid",
        type=int,
        default=0,
        help="Target process ID (default: newest 'orange' process).",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=20.0,
        help="Capture duration in seconds (default: %(default)s).",
    )
    parser.add_argument(
        "--filter",
        default="YOLO",
        help="Only include threads whose comm contains this string (default: %(default)s).",
    )
    parser.add_argument(
        "--out",
        default="",
        help="Optional output CSV path.",
    )
    return parser.parse_args()


def find_pid():
    try:
        out = subprocess.check_output(["pgrep", "-n", "orange"], text=True).strip()
    except subprocess.CalledProcessError:
        return 0
    try:
        return int(out)
    except ValueError:
        return 0


def list_threads(pid, filt):
    out = subprocess.check_output(
        ["ps", "-L", "-p", str(pid), "-o", "tid,comm"], text=True
    ).splitlines()
    threads = []
    for line in out[1:]:
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            continue
        tid_str, comm = parts
        if filt and filt not in comm:
            continue
        try:
            tid = int(tid_str)
        except ValueError:
            continue
        threads.append((tid, comm))
    return threads


def read_schedstat(pid, tid):
    path = f"/proc/{pid}/task/{tid}/schedstat"
    with open(path, "r", encoding="utf-8") as f:
        parts = f.read().strip().split()
    if len(parts) < 2:
        raise RuntimeError(f"unexpected schedstat format: {path}")
    run_ns = int(parts[0])
    wait_ns = int(parts[1])
    return run_ns, wait_ns


def snapshot(pid, filt):
    data = {}
    for tid, comm in list_threads(pid, filt):
        run_ns, wait_ns = read_schedstat(pid, tid)
        data[tid] = (comm, run_ns, wait_ns)
    return data


def main():
    args = parse_args()
    pid = args.pid or find_pid()
    if not pid:
        print("No PID found. Use --pid or ensure orange is running.")
        return 1

    before = snapshot(pid, args.filter)
    if not before:
        print("No matching threads found.")
        return 1

    time.sleep(args.duration)
    after = snapshot(pid, args.filter)

    lines = ["tid,comm,run_ms,wait_ms,wait_pct"]
    for tid, (comm, run_a, wait_a) in before.items():
        if tid not in after:
            continue
        comm2, run_b, wait_b = after[tid]
        run_ms = (run_b - run_a) / 1e6
        wait_ms = (wait_b - wait_a) / 1e6
        total = run_ms + wait_ms
        wait_pct = 100.0 * wait_ms / total if total > 0.0 else 0.0
        lines.append(f"{tid},{comm2},{run_ms:.3f},{wait_ms:.3f},{wait_pct:.1f}")

    output = "\n".join(lines)
    print(output)
    if args.out:
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(output + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
