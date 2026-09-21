#!/usr/bin/env python3
"""Align camera frame-id gaps with ring buffer hold times and owner pushes.

Inputs (per camera, in a run folder): Cam<serial>_ring_release.csv (acquisition
loop, ORANGE_ACQ_RING_RELEASE_LOG=1), Cam<serial>_owner_push.csv (external
handoff worker, owner push on), Cam<serial>_acquisition_cadence_probe.csv
(ORANGE_ACQ_CADENCE_PROBE_ALL=1). All host times are steady-clock ns.

For each camera: hold-time percentiles for frames whose hold window overlapped a
push versus not, pending-buffer high-water, and for every camera frame-id gap
the hold times of the preceding frames and whether a push was in flight.
"""
import csv, glob, os, sys
import numpy as np

def load(path):
    if not os.path.exists(path):
        return []
    return list(csv.DictReader(open(path)))

def main(run_dir):
    for rr in sorted(glob.glob(os.path.join(run_dir, "Cam*_ring_release.csv"))):
        serial = os.path.basename(rr)[3:10]
        rel = load(rr)
        pushes = [r for r in load(os.path.join(run_dir, f"Cam{serial}_owner_push.csv")) if r["kind"] == "push"]
        rel.sort(key=lambda r: int(r["local_frame_id"]))
        holds = np.array([int(r["hold_ns"]) for r in rel]) / 1e6
        pend = np.array([int(r["pending_after"]) for r in rel])
        recv = np.array([int(r["receive_host_ns"]) for r in rel])
        req = np.array([int(r["requeue_host_ns"]) for r in rel])
        camid = np.array([int(r["camera_frame_id"]) for r in rel])
        print(f"== {serial}: {len(rel)} releases, hold ms p50 {np.percentile(holds,50):.2f} p95 {np.percentile(holds,95):.2f} p99 {np.percentile(holds,99):.2f} max {holds.max():.2f}, pending after release max {pend.max()}, forced {sum(int(r['forced']) for r in rel)}")
        if pushes:
            ps = np.array([int(p["push_start_steady_ns"]) for p in pushes]); pe = np.array([int(p["push_done_steady_ns"]) for p in pushes])
            # a frame's hold window [recv, req] overlaps a push if any push interval intersects it
            idx = np.searchsorted(ps, req)  # pushes starting before requeue
            overl = np.zeros(len(rel), dtype=bool)
            for i in range(len(rel)):
                j = idx[i] - 1
                while j >= 0 and pe[j] > recv[i]:
                    if ps[j] < req[i]:
                        overl[i] = True; break
                    j -= 1
            for name, m in (("hold overlapping a push", overl), ("hold with no push", ~overl)):
                if m.sum():
                    h = holds[m]; print(f"   {name:24s} n={m.sum():5d} p50 {np.percentile(h,50):.2f} p95 {np.percentile(h,95):.2f} p99 {np.percentile(h,99):.2f} max {h.max():.2f} ms")
        # gaps: camera_frame_id jumps (16-bit wrap tolerated)
        gaps = [i for i in range(1, len(rel)) if (camid[i] - camid[i-1]) % 65536 not in (1,)]
        print(f"   camera frame-id gaps in this file: {len(gaps)}")
        for i in gaps[:12]:
            lo = max(0, i - 4)
            ctx = ", ".join(f"{holds[k]:.1f}" for k in range(lo, i + 1))
            inflight = ""
            if pushes:
                t = recv[i]
                k = np.searchsorted(ps, t) - 1
                inflight = " push in flight at arrival" if k >= 0 and pe[k] > t else " no push at arrival"
            print(f"   gap before local {rel[i]['local_frame_id']} (cam id {camid[i-1]}->{camid[i]}): holds of prior frames [{ctx}] ms, pending {pend[lo:i+1].tolist()}{inflight}")

if __name__ == "__main__":
    main(sys.argv[1])
