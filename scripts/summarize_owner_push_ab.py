#!/usr/bin/env python3
"""Summarize paired owner-push / pull runs: gates, inference tails, push timing.

Usage: summarize_owner_push_ab.py <rounds.txt> where each run is two lines:
  <spec-suffix> <run folder name>
     NIC: {...} | clean
"""
import csv, glob, json, os, sys
import numpy as np

EXP = "/home/jeremy/orange_data/exp/unsorted"
REC = "/home/jeremy/orange_data/external_recorder"

def pct(v, q): return float(np.percentile(v, q)) if len(v) else float("nan")

def run_metrics(kind, name):
    run = glob.glob(os.path.join(EXP, name, "run_0001*"))[0]
    r = json.load(open(os.path.join(EXP, name, "runs.json")))["runs"][0]
    gaps = [c.get("camera_frame_id_gaps") for c in r["camera_results"]]
    rec_drops = []
    for f in sorted(glob.glob(os.path.join(REC, name, "Cam*_external_summary.json"))):
        d = json.load(open(f)); rec_drops.append((d.get("encode_dropped", 0), d.get("frames_received", 0)))
    out = {"kind": kind, "pass": r["pass_fail"], "gaps": gaps, "recorder(drops,received)": rec_drops, "cams": {}}
    for f in sorted(glob.glob(os.path.join(run, "Cam*_yolo_perf.csv"))):
        cam = os.path.basename(f)[3:10]; rows = list(csv.DictReader(open(f)))[200:]
        a2d = np.array([float(x["acquisition_to_detect_done_ms"]) for x in rows])
        c = out["cams"].setdefault(cam, {})
        c["a2d p95/p99/max"] = (round(pct(a2d, 95), 3), round(pct(a2d, 99), 3), round(float(a2d.max()), 2))
    for f in sorted(glob.glob(os.path.join(run, "Cam*_pose_events.jsonl"))):
        cam = os.path.basename(f)[3:10]; v = []
        for line in open(f):
            try: e = json.loads(line)
            except: continue
            x = (e.get("latency_ms") or {}).get("capture_to_pose_done")
            if x is not None: v.append(x)
        v = np.array(v[200:])
        if len(v): out["cams"].setdefault(cam, {})["c2p p95/p99/max"] = (round(pct(v, 95), 3), round(pct(v, 99), 3), round(float(v.max()), 2))
    for f in sorted(glob.glob(os.path.join(run, "Cam*_owner_push.csv"))):
        cam = os.path.basename(f)[3:10]; rows = list(csv.DictReader(open(f)))
        pushes = [x for x in rows if x["kind"] == "push"]
        if not pushes: continue
        # done after capture: push done (steady) vs capture (realtime) are different clocks; use wait + age at push for the tail
        done_after = np.array([float(x["age_at_push_ms"]) for x in pushes])  # age measured at completion in the CSV writer
        q = np.array([int(x["queue_in"]) for x in pushes])
        c = out["cams"].setdefault(cam, {})
        c["push done-after-capture mean/max, >10ms"] = (round(float(done_after.mean()), 2), round(float(done_after.max()), 2), int((done_after > 10.0).sum()))
        c["pushes / too_old / no_slot"] = (len(pushes), sum(1 for x in rows if x["kind"].startswith("too_old")), sum(1 for x in rows if x["kind"] == "no_slot"))
        c["queue_in max"] = int(q.max())
    return out

def main(path):
    lines = [l.rstrip("\n") for l in open(path) if l.strip()]
    i = 0
    while i < len(lines):
        kind, name = lines[i].split()[:2]; nic = lines[i + 1].strip() if i + 1 < len(lines) and lines[i + 1].strip().startswith("NIC") else ""
        m = run_metrics(kind, name)
        print(f"== {kind:22s} {name[-15:]}  pass={m['pass']}  gaps={m['gaps']}  {nic}")
        print(f"   recorder (drops, received) per camera: {m['recorder(drops,received)']}")
        for cam, c in sorted(m["cams"].items()):
            print(f"   {cam}: " + " | ".join(f"{k} {v}" for k, v in c.items()))
        i += 2 if nic else 1

if __name__ == "__main__":
    main(sys.argv[1])
