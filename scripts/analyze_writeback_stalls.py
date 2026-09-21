#!/usr/bin/env python3
"""Correlate YOLO-thread stalls with host writeback bursts.

usage: analyze_writeback_stalls.py <run folder> <host_writeback_sampler.csv>

Stall = a frame with yolo_queue_wait_ms > 1 in Cam*_yolo_perf.csv (frames after
200); events are clustered across cameras within 0.5 s and matched to the
sampler (10 Hz) by timestamp_sys. Writeback burst = Writeback > 50 MB.
Needs pandas (juicebox env)."""
import glob
import sys

import numpy as np
import pandas as pd


def main():
    d, samp = sys.argv[1], sys.argv[2]
    p = pd.read_csv(samp)
    p["t"] = p.t_ns / 1e9
    p["wr_mb_s"] = np.gradient(p.nvme_wr_sectors * 512 / 1e6, p.t)
    t0 = p.t.iloc[0]
    stalls = []
    for f in sorted(glob.glob(d + "/run_*/Cam*_yolo_perf.csv")):
        cam = f.split("Cam")[-1][:7]
        x = pd.read_csv(f, usecols=["recording_frame_id", "timestamp_sys", "yolo_queue_wait_ms", "acquisition_to_detect_done_ms"])
        x = x[x.recording_frame_id > 200]
        print("%s a2d p95 %.3f p99 %.3f max %.2f" % (cam, np.percentile(x.acquisition_to_detect_done_ms, 95), np.percentile(x.acquisition_to_detect_done_ms, 99), x.acquisition_to_detect_done_ms.max()))
        for _, r in x[x.yolo_queue_wait_ms > 1.0].iterrows():
            stalls.append((r.timestamp_sys / 1e9, cam, int(r.recording_frame_id), r.yolo_queue_wait_ms))
    stalls.sort()
    ev = []
    for t, cam, fr, q in stalls:
        if ev and t - ev[-1]["t"] < 0.5:
            ev[-1]["cams"].add(cam)
            ev[-1]["q"] = max(ev[-1]["q"], q)
        else:
            ev.append({"t": t, "cams": {cam}, "q": q, "f": fr})
    print("dirty MB: max %.0f, p50 %.0f | nvme write MB/s: max %.0f p50 %.0f" % (p.dirty_kb.max() / 1024, p.dirty_kb.median() / 1024, p.wr_mb_s.max(), p.wr_mb_s.median()))
    wb = p[p.writeback_kb > 50 * 1024]
    bt = []
    for t in wb.t.values:
        if not bt or t - bt[-1][-1] > 1.0:
            bt.append([t, t])
        else:
            bt[-1][-1] = t
    print("writeback bursts (>50 MB in flight): %d at %s" % (len(bt), [round(a - t0, 1) for a, _ in bt]))
    print("stall events: %d" % len(ev))
    for e in ev:
        w = p[(p.t > e["t"] - 1.0) & (p.t < e["t"] + 0.5)]
        print("  t=%7.2f frame %6d cams %d qwait %.1f ms | writeback max %4.0f MB | nvme write max %5.0f MB/s" % (e["t"] - t0, e["f"], len(e["cams"]), e["q"], w.writeback_kb.max() / 1024 if len(w) else -1, w.wr_mb_s.max() if len(w) else -1))


if __name__ == "__main__":
    main()
