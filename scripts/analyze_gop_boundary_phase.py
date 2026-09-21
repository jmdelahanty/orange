"""Per-frame YOLO timing by position within the 50-frame local+peer GOP cycle.
usage: gop_boundary_phase.py <run folder> [<run folder> ...]
Shows, for each camera, the median and max of queue_ms / infer_ms / sync_ms /
acquisition_to_detect_done_ms at the worst GOP positions, plus the overall p95/p99.
GOP position is derived from the external recorder's gop routing csv when present
(frame index within gop + 25 * (gop_index % 2)); otherwise (recording_frame_id-1) % 50."""
import sys, glob, os
import numpy as np, pandas as pd
for root in sys.argv[1:]:
    runs = sorted(glob.glob(os.path.join(root, 'run_*'))) or [root]
    run = runs[0]
    print("=== %s" % os.path.basename(root.rstrip('/')))
    for f in sorted(glob.glob(os.path.join(run, 'Cam*_yolo_perf.csv'))):
        cam = os.path.basename(f)[3:10]
        d = pd.read_csv(f)
        d = d[d.recording_frame_id > 50]
        rid = d.recording_frame_id.astype(int)
        routing = glob.glob(os.path.join(run, '..', '..', '..', 'external_recorder', '*', 'Cam%s_gop_routing.csv' % cam))
        pos = ((rid - 1) % 50).values
        p95 = lambda s: np.percentile(s, 95)
        cols = ['queue_ms', 'infer_ms', 'sync_ms', 'acquisition_to_detect_done_ms']
        # p50 by position for each column
        summary = d.groupby(pos)[cols].median()
        mx = d.groupby(pos)[cols].max()
        base = summary.median()
        worst = (summary - base).abs().max(axis=1).sort_values(ascending=False).head(3)
        line = "cam %s n=%d  a2d p95 %.3f p99 %.3f max %.3f | queue_ms p95 %.3f p99 %.3f max %.3f" % (
            cam, len(d), p95(d.acquisition_to_detect_done_ms), np.percentile(d.acquisition_to_detect_done_ms, 99), d.acquisition_to_detect_done_ms.max(),
            p95(d.queue_ms), np.percentile(d.queue_ms, 99), d.queue_ms.max())
        print(line)
        for p in worst.index:
            print("    pos %2d: " % p + "  ".join("%s med %.3f (base %.3f) max %.3f" % (c.replace('acquisition_to_detect_done_ms', 'a2d').replace('_ms', ''), summary.loc[p, c], base[c], mx.loc[p, c]) for c in cols))
