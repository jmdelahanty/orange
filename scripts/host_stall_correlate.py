#!/usr/bin/env python3
"""Correlate pipeline stall events in a run folder with host_stall_monitor output.

usage: host_stall_correlate.py <run folder> <monitor prefix> [--window-ms 20]

Events come from every Cam*_owner_push.csv (pull fallbacks and pushes slower
than 7 ms, after recording frame 100) and Cam*_yolo_perf.csv (acquisition-to-
detect above 5 ms). Events within 50 ms are one cluster. For each cluster the
report says which cameras took part, whether the heartbeat process saw a late
wake-up in the same window (host-wide) or not (inside Orange), and what the
1 Hz counters did in that second. Needs pandas (juicebox env).
"""
import glob
import json
import sys

import pandas as pd


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    run, prefix = sys.argv[1], sys.argv[2]
    window_ms = float(sys.argv[sys.argv.index("--window-ms") + 1]) if "--window-ms" in sys.argv else 20.0
    ev = []
    for f in sorted(glob.glob(run + "/Cam*_owner_push.csv")):
        cam = f.split("Cam")[-1][:7]
        op = pd.read_csv(f)
        op["dur"] = (op.push_done_steady_ns - op.push_start_steady_ns) / 1e6
        st = op[op.recording_frame_id > 100]
        for _, r in st[(st.kind != "push") | (st.dur > 7)].iterrows():
            ev.append((int(r.capture_sys_ns), cam, "push:" + str(r.kind) + (" %.1fms" % r.dur if r.kind == "push" else " age %.1fms" % r.age_at_push_ms)))
    for f in sorted(glob.glob(run + "/Cam*_yolo_perf.csv")):
        cam = f.split("Cam")[-1][:7]
        yp = pd.read_csv(f, usecols=lambda c: c in ("frame_id", "timestamp_sys", "acquisition_to_detect_done_ms"))
        yp = yp[yp.frame_id > yp.frame_id.min() + 100]
        for _, r in yp[yp.acquisition_to_detect_done_ms > 5].iterrows():
            ev.append((int(r.timestamp_sys), cam, "detect %.1fms" % r.acquisition_to_detect_done_ms))
    ev.sort()
    clusters = []
    for t, cam, what in ev:
        if clusters and t - clusters[-1]["t_end"] < 50e6:
            c = clusters[-1]
            c["t_end"] = t
            c["cams"].add(cam)
            c["items"].append(what)
        else:
            clusters.append({"t0": t, "t_end": t, "cams": {cam}, "items": [what]})
    hb = pd.read_csv(prefix + "_heartbeat.csv")
    ct = pd.read_csv(prefix + "_counters.csv")
    summ = json.load(open(prefix + "_summary.json"))
    t_run0 = ct.t_ns.iloc[0]
    print("monitor: %.0f s, heartbeat gaps=%d (max %.1f ms), smi_delta=%s, allocstall=%d, compact_stall=%d" % (
        summ["duration_s"], summ["heartbeat"]["gaps"], summ["heartbeat"]["max_gap_ms"], summ["smi_delta"],
        summ["deltas"]["allocstall_normal"], summ["deltas"]["compact_stall"]))
    print("%d stall events in %d clusters" % (len(ev), len(clusters)))
    hostwide = 0
    for c in clusters:
        w = window_ms * 1e6
        gaps = hb[(hb.t_ns >= c["t0"] - w) & (hb.t_ns <= c["t_end"] + w)]
        sec = ct[(ct.t_ns >= c["t0"] - 1.5e9) & (ct.t_ns <= c["t_end"] + 1.5e9)]
        verdict = "HOST-WIDE (heartbeat late %.1f ms)" % gaps.late_ms.max() if len(gaps) else "inside Orange (heartbeat on time)"
        if len(gaps):
            hostwide += 1
        detail = ""
        if len(sec) >= 2:
            d = sec.iloc[-1]
            d0 = sec.iloc[0]
            detail = " | dirty max %.0f MB, writeback max %.0f MB, nvme %.0f MB/s, allocstall +%d, compact +%d, smi +%s, iso irqs +%d" % (
                sec.dirty_kb.max() / 1024, sec.writeback_kb.max() / 1024,
                (d.nvme_wr_sectors - d0.nvme_wr_sectors) * 512 / 1e6 / max(1e-9, (d.t_ns - d0.t_ns) / 1e9),
                d.allocstall_normal - d0.allocstall_normal, d.compact_stall - d0.compact_stall,
                (d.smi_count - d0.smi_count) if d.smi_count >= 0 else "n/a", d.isolated_core_irqs - d0.isolated_core_irqs)
        print("t=%7.1fs cams=%s n=%d %s%s" % ((c["t0"] - t_run0) / 1e9, "/".join(sorted(x[-2:] for x in c["cams"])), len(c["items"]), verdict, detail))
    print("verdict: %d/%d clusters coincide with a host-wide heartbeat gap" % (hostwide, len(clusters)))


if __name__ == "__main__":
    main()
