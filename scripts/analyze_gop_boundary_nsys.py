#!/usr/bin/env python3
"""Recorder copy phase vs detect graph, from an nsys sqlite export (--trace=cuda).

usage: analyze_gop_boundary_nsys.py <trace.sqlite> [analytics device ids...]

For each analytics die: graph span percentiles, every large copy (>5 MB) that
overlaps a graph span, and the recorder's local native-array upload (DtoA,
copyKind 7) start phase after the preceding graph start by position within the
25-frame local GOP. Written for the 2026-09-20 GOP-boundary spike investigation:
with extra_output_delay 3 (4 encoder buffers) the local shard's submit thread
blocks on WaitForNextInputFrameAvailable from about GOP position 20, the upload
drifts ~1.2 ms/frame later and lands inside the next detect graph (+1.7 ms) on
the last local frame, stretching that graph by ~0.7 ms.
Export first: nsys export --type sqlite <trace.nsys-rep>
"""
import sqlite3
import sys
import numpy as np

KINDS = {1: "HtoD", 2: "DtoH", 7: "DtoA", 8: "DtoD", 10: "PtoP"}


def main():
    db = sqlite3.connect(sys.argv[1])
    c = db.cursor()
    devices = [int(x) for x in sys.argv[2:]] or [1, 3, 5, 7]
    pids = {r[0]: (r[1] or "").split("/")[-1] for r in c.execute("select globalPid, name from PROCESSES")}
    g = c.execute("select start, end, deviceId from CUPTI_ACTIVITY_KIND_GRAPH_TRACE order by start").fetchall()
    mem = c.execute("select start, end, deviceId, globalPid, bytes, copyKind from CUPTI_ACTIVITY_KIND_MEMCPY order by start").fetchall()
    for dev in devices:
        rows = [r for r in g if r[2] == dev]
        if not rows:
            print("device %d: no graph launches" % dev)
            continue
        starts = np.array([r[0] for r in rows])
        ends = np.array([r[1] for r in rows])
        span = (ends - starts) / 1e6
        print("device %d: %d graphs; span p50 %.3f p95 %.3f p99 %.3f max %.3f ms" % (
            dev, len(rows), np.percentile(span, 50), np.percentile(span, 95), np.percentile(span, 99), span.max()))
        big = [m for m in mem if m[2] == dev and (m[4] or 0) > 5e6]
        for m in big:
            i = np.searchsorted(starts, m[0]) - 1
            if i >= 0 and m[0] < ends[i]:
                print("   graph %d (span %.3f): %s %s %.1f MB at %+.3f..%+.3f ms" % (
                    i, span[i], pids.get(m[3], str(m[3]))[:16], KINDS.get(m[5], str(m[5])), m[4] / 1e6,
                    (m[0] - starts[i]) / 1e6, (m[1] - starts[i]) / 1e6))
        loc = [m for m in big if m[5] == 7]
        groups, cur = [], []
        for m in loc:
            if cur and (m[0] - cur[-1][0]) / 1e6 > 15:
                groups.append(cur)
                cur = []
            cur.append(m)
        if cur:
            groups.append(cur)
        full = [gr for gr in groups if len(gr) == 25]
        if not full:
            continue
        ph = np.zeros((len(full), 25))
        for a, gr in enumerate(full):
            for k, m in enumerate(gr):
                i = np.searchsorted(starts, m[0]) - 1
                ph[a, k] = (m[0] - starts[i]) / 1e6
        print("   local DtoA upload start after preceding graph start, median by GOP position 0..24:")
        print("   " + " ".join("%.1f" % v for v in np.median(ph, axis=0)))


if __name__ == "__main__":
    main()
