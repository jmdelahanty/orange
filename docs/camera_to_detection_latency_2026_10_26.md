# From exposure to bounding box: where the time goes (2026-10-26)

Status: measurement and explanation, docs only. Nothing changed.

## Summary

A fish's bounding box is available to Citrus about **11.0 ms after the camera
starts the exposure** (p50 11.0, p95 11.2, p99 11.25 ms; max 15–18 ms over
1.5 hours). Of that:

| Stage | p50 | p95 | p99 | max |
|---|---|---|---|---|
| Exposure start → frame complete on the host | 9.17 ms | 9.36 | 9.47 | 13–16 |
| Host receive → detection done (fused graph) | 1.83 ms | 1.87 | 1.89 | ~5.2 |
| Publish to the SHAMAN v2 queue | 0.01 ms | 0.01 | 0.01 | 0.03 |
| **Exposure start → box in the queue** | **11.0 ms** | **11.2** | **11.25** | **15–18** |

Source: the 10-18 fish recording `2026_10_18_11_32_33`, all 544,032 frames per
camera. The four cameras agree to within 0.03 ms at every percentile, so the
table shows Cam2010093.

Orange's own latency numbers (`acquisition_to_detect_done_ms`,
`latency_ms.capture_to_detect_done` / `capture_to_pose_done`, the "detect p95
1.85 ms" in AGENTS.md) **start at host receive**, not at exposure. They leave
out the camera-to-host part, which is about 5× larger.

## How it was measured

Per frame, from `Cam<serial>_yolo_perf.csv`:

- Camera time: `timestamp`, the camera's PTP clock (TAI). Host clock at
  exposure is `timestamp − 37 s`. pancake0 is the PTP grandmaster, so both are
  the same clock; see AGENTS.md "Camera time on the host clock".
- Host receive: `timestamp_sys`, host `CLOCK_REALTIME` taken in the acquisition
  thread right after the SDK hands over the completed frame
  (`src/acquire_frames.cpp:2525`).
- Exposure start → host receive = `timestamp_sys − (timestamp − 37 s)`.
- Then `acquisition_to_detect_done_ms` and `ipc_ms`, summed per frame (not by
  adding percentiles).

**What the camera timestamp marks.** Under `ptp_gate` the stream starts at a
gate time and runs on a 10 ms grid. This run's sealed gate start
(`recording_snapshot.json` `sync.gate_times.start_ns`) sits 3,470.4 µs into its
10 ms period. Every frame timestamp on all four cameras sits 3,470.8 µs in
(spread < 1 µs). So the camera stamps each frame at the gated trigger, i.e. at
exposure start, 0.4 µs after the grid point. The exposure itself is 100 µs, so
exposure start and end differ by 0.1 ms. It doesn't stamp at the end of readout.

## Why camera to host takes 9.2 ms

The frame is 4512 × 4512 Mono8, 20,358,144 bytes. It crosses one 25 Gbit/s
link per camera (MTU 9000, the camera's maximum packet size,
`src/camera.cpp:1777`; Orange sets no inter-packet delay).

- **Moving 20.4 MB at 25 Gbit/s takes about 6.6 ms**, including packet and
  Ethernet overhead (~0.8% at 9000-byte packets). Nothing faster is possible on
  this link.
- **The other ~2.6 ms** is sensor readout not overlapped with sending, buffering
  in the camera, and the NIC and SDK finishing the frame into the GPUDirect
  ring. The split between these hasn't been measured. Only Emergent could say
  how much of the readout overlaps the transfer.
- The stage is very steady: p50 to p99 is only 0.3 ms.

## Is it tied to the frame rate?

**Not directly.** The 9.2 ms is set by frame size and link speed, not by the
10 ms frame period.

- **What the frame rate does limit:** each frame's transfer must fit inside one
  frame period, or the link falls behind. At 100 fps, 6.6 ms of a 10 ms period
  is 66% link utilisation. Full frames couldn't go much above ~150 fps on 25G.
- **At a lower frame rate** the per-frame transfer would stay about 9.2 ms
  unless the camera spreads packets out. Not measured: every recording since
  2026-10-11 ran at 100 fps. To confirm, one 60 s headless run at 50 fps, with
  the same table computed, would settle it.
- **The frame rate does add sampling delay.** An event that happens between
  two exposures isn't seen until the next one: 0–10 ms later at 100 fps, 5 ms
  on average.

So "how far behind real time":

| Question | Answer at 100 fps |
|---|---|
| Exposure start → box available to Citrus | ~11.0 ms (p99 11.25) |
| Something happens → box available to Citrus | 11–21 ms, ~16 ms on average (sampling + 11 ms) |
| Box → stimulus on screen | Not measured here: Citrus reading the queue, rendering, the 120 Hz display (up to 8.3 ms per refresh) and the projector |

## What could make it shorter

These are options, not plans. Each needs its own measurement and Jeremy's
decision.

- **Fewer bytes per frame** (camera ROI or binning): transfer time scales with
  frame size, e.g. a 2256 × 2256 ROI is ¼ of the bytes, ~1.6 ms on the wire.
  Recordings would then cover less of the arena or at lower resolution.
- **Higher frame rate:** cuts the sampling wait (0–10 ms) but not the 9.2 ms
  transfer, and full frames are near the link limit already.
- **Orange's part** (1.8 ms) is already small. Halving it would save < 1 ms of
  the 11.

## Reproduce

```bash
D=<recording folder>   # any PTP-gated recording with Cam*_yolo_perf.csv
~/miniforge3/envs/juicebox/bin/python - "$D" <<'PY'
import glob, sys, pandas as pd
for f in sorted(glob.glob(sys.argv[1] + "/Cam*_yolo_perf.csv")):
    d = pd.read_csv(f, usecols=["timestamp", "timestamp_sys", "ok",
                                "acquisition_to_detect_done_ms", "ipc_ms"])
    d = d[d.ok == 1]
    e2r = (d.timestamp_sys - (d.timestamp - 37_000_000_000)) / 1e6
    tot = e2r + d.acquisition_to_detect_done_ms + d.ipc_ms
    for name, s in (("exposure->receive", e2r),
                    ("receive->detect", d.acquisition_to_detect_done_ms),
                    ("exposure->box", tot)):
        print(f.split("/")[-1][:10], name,
              *(f"{q}:{s.quantile(x):.2f}" for q, x in
                (("p50", .5), ("p95", .95), ("p99", .99))),
              f"max:{s.max():.2f}")
PY
```

The 37 s is the TAI–UTC offset valid since 2017. Recordings before
2026-10-11 21:50 classify the camera clock as `device_defined`; use this only
on later ones.
