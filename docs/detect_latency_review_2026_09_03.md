# Detect Latency Review, Four Cameras, 2026-09-03

Scope: where the YOLO worker's time goes in the sustained four-camera run
`/home/jeremy/orange_data/exp/unsorted/2026_08_25_23_12_06/`, what modulates
it, and the ordered list of levers to try next. The per-run analysis is
reproducible with `scripts/analyze_yolo_latency_phases.py`.

Run: four HB-20000SBM cameras, 4512x4512 Mono8 at 100 fps, yolo11n TensorRT 10
fp16 640x640 (`bo5_avg32` engine), external split-GOP HEVC recording with two
NVENC shards per camera, PTP two-step, GUI preview at 10 fps.

## Headline

The threading architecture is not where the latency is. The CPU side of the
detect path costs about 60 us per frame. Of the 2.82 ms mean worker time,
2.76 ms is waiting on the GPU, and about a fifth of that wait is the recorder
sharing the same A16 die with the detector.

Run health: 182,278 frames per camera over 1,822.8 s, 0 drops, 0 gaps,
0 enqueue rejections, acquisition 100.0 fps, YOLO queue depth 0 at every
enqueue and every worker start, `ok=1` on every row.

| Camera | Acq to detect mean | p50 | p95 | p99 | max | Worker total mean | p95 |
|---|---|---|---|---|---|---|---|
| 2010093 | 2.873 | 2.770 | 3.872 | 4.177 | 7.04 | 2.824 | 3.815 |
| 2010094 | 2.882 | 2.793 | 3.885 | 4.248 | 6.96 | 2.827 | 3.818 |
| 2010095 | 2.899 | 2.850 | 3.898 | 4.291 | 8.21 | 2.845 | 3.831 |
| 2010096 | 2.900 | 2.840 | 3.893 | 4.296 | 8.20 | 2.847 | 3.827 |

All values in ms. Comparison rule: these are means and quantiles of the
August run's `total_ms` and `acquisition_to_detect_done_ms`. Do not set the
means against the earlier profiled run's four-camera p95 values (3.81-3.93 ms
in `docs/a16_tensorrt_detect_engine_rebuild.md`). That run is the only source
of direct TensorRT `infer_ms`; in the August run `infer_ms` and `sync_ms` are
-1 because `YOLO_PROFILE` was not compiled in.

Frame budget, camera 2010093, mean per frame:

| Segment | ms |
|---|---|
| acquisition to worker start | 0.057 |
| YOLO queue wait | 0.011 |
| CPU before sync (event query, preprocess launch, graph launch, event record) | 0.053 |
| GPU wait | 2.762 |
| CPU after sync (postprocess) | 0.009 |

## Findings

1. **Same-die encoder contention (placement).** Full-frame recording alternates
   25-frame GOPs between two NVENC shards. Shard 0 lives on the detect die.
   Joining each perf row to `external_recorder/Cam*_external_gop_routing.csv`
   by `recording_frame_id`:

   | Encoder active on | Worker total mean | p95 |
   |---|---|---|
   | other die (GPU 4) | 2.54 | 3.00 |
   | detect die (GPU 3) | 3.10 | 3.91 |

   The `total_ms` distribution is trimodal (2.45 / 3.0 / 3.8 ms) with a
   50-frame period. During the detect-die phase the die also runs a 20 MB
   device-to-device detach copy per frame (p50 2.0 ms, p95 10.5 ms) and NVENC
   reads the frame. The YOLO stream already runs at highest priority, so
   priority is not the lever; copy-engine and bandwidth traffic are.

2. **Completion sync polls with usleep.** `ORANGE_YOLO_SYNC_EVENT` was unset
   and no launch script sets it, so the worker sits in a 100 us `usleep` loop
   calling `cudaStreamQuery` about 27 times per frame. With timer slack that
   costs 50-150 us per frame and repeatedly takes the context lock other
   threads on the die need. The event path exists behind the flag.

3. **PTP latch in front of fanout.** The register latch (one command plus two
   register reads on the GigE Vision control channel) runs on the acquisition
   thread every 100th frame, before the frame is fanned out. Those 1,823 frames
   per camera reach the YOLO queue 1.66-1.84 ms late (max 3.7 ms). That is the
   entire acquisition-side tail: p99 0.57 ms, p99.9 2.0 ms. All other frames
   reach the queue in about 30 us.

4. **A 1 Hz driver stall.** 343-939 frames per camera saw `cudaEventRecord`
   block for about 2.7 ms, and 79% of them land in the same 50 ms slice of each
   second. Today the stall hides inside the GPU wait. **Root cause, found by
   A/B on 2026-09-03:** not `nvidia-smi dmon`. The acquisition thread's 1 Hz
   PTP summary writer called `build_gpu_runtime_info`, which calls
   `cudaGetDeviceProperties` and holds the driver lock for milliseconds.
   Disabling dmon left the stalls in place; caching the device properties per
   GPU removed them entirely (0 stalls on all cameras in the verification run).

5. **Production runs do not record their own flags.** Only affinity was
   written to the snapshot. Reconstructing the rest required reading code
   defaults and launch scripts, and the defaults disagreed with each other
   (`SYNC_EVENT` off, `DETACH_INPUT` on, `INLINE_CROP_PRODUCER` off). Fixed in
   this review: every resolved flag is in the snapshot, and since the defaults
   commit of 2026-09-03 the verified levers default on: `ORANGE_YOLO_SYNC_EVENT=1`,
   `ORANGE_PTP_LATCH_AFTER_FANOUT=1`, `ORANGE_EXTERNAL_RECORDER_DETECT_PRIORITY=1`.
   Set any of them to `0` to restore the August behaviour; the
   `*_detect_latency_baseline` specs do exactly that.

6. **Preview copies on the detect die.** The 10 fps GUI preview copies one
   frame in ten GPU-to-PBO on the detect die; worker time swings 0.2 ms across
   the 10-frame cycle. Headless runs will not see this.

## Architecture Verdict

What the numbers endorse: thread-per-stage with condition-variable bounded
queues, pooled ref-counted frames, event-based cross-stage GPU sync, no host
copies, CUDA graph launch, GPUDirect into the detect die, per-worker core
pinning. Queue wait of 11 us is as good as the pattern gets. Stop optimizing
threads.

The real design decision is GPU placement. A GA107 die has 10 SMs, one NVENC,
and about 200 GB/s; everything that touches it taxes the detector. NVENC budget
pins the encoders to the detect dies (four cameras at 20 MP and 100 fps need
all eight NVENCs), so the encoder cannot move; inference can.

The worker thread being blocked 97% of its busy time is not a flaw. One host
thread per stream that submits and then waits is the standard pattern, and
TensorRT execution contexts are not safe to share across threads. The flaw is
the wait implementation (finding 2).

Locality caveat: shipping the full 20 MB frame to the RTX A6000 loses
outright; shipping the 2.5 MB fp16 tensor costs 0.2-0.5 ms but the A6000 is
shared with the citrus renderer, and a graphics context and a CUDA context
time-slice at roughly millisecond granularity. Do not relocate inference
without a standalone p99 measurement under render load.

Against upstream (`moments-behavior/orange` v2.1.0): for this workload the
branch is the better fit and the only one of the two that sustains the pixel
rate on A16 dies (upstream moves 160 MB per frame through mono to RGBA plus a
second copy, uses one NVENC per camera, borrows the driver ring without
ownership, and samples the newest frame for detection rather than every
frame). Upstream is right-sized for multi-host capture, 7 MP color at 30 fps,
and trigger-style detection. The branch's cost is eight times the code and
duplicated GUI/headless session control.

Reported upstream "400 FPS inference" is a free-running service rate
(1000 / loop time). The branch's equivalent, 1000 / `total_ms`, is 354/s today
and 393/s on frames whose encoder ran on the other die, at nearly three times
the pixels. The colleague's rig uses the same A16 dies with about 7 MP sensors
but a different, unknown engine; the like-for-like test is the same engine
file on one die with no recorder running.

## A/B Results, 2026-09-03

Three-camera headless PTP external-IPC runs (2010096 was offline), 60 s each,
5,902 frames per camera, zero gaps, run through `orange-local-benchmark` with
`experiment_specs/threecam_detect_latency_*.json`. The baseline reproduces
the August figures to within a few hundredths of a millisecond, so a short
headless run is a valid proxy for the protocol runs.

Baseline (usleep poll, latch before fanout, dmon on) versus levers 1+3+4
(event sync, latch after fanout, dmon off, GPU-properties cache), steady after
frame 200, values in ms, this run minus baseline:

| Metric | 2010093 | 2010094 | 2010095 |
|---|---|---|---|
| acq to detect mean | 2.889 to 2.796 (-0.094) | 2.883 to 2.787 (-0.096) | 2.775 to 2.665 (-0.110) |
| acq to detect p95 | 3.880 to 3.770 (-0.111) | 3.848 to 3.753 (-0.095) | 3.621 to 3.535 (-0.087) |
| acq to detect p99 | 4.258 to 3.911 (-0.347) | 4.048 to 3.872 (-0.176) | 4.037 to 3.703 (-0.334) |
| worker total mean | 2.844 to 2.770 (-0.074) | 2.845 to 2.762 (-0.083) | 2.727 to 2.636 (-0.091) |
| acq to worker start p99.9 | 2.220 to 0.054 | 2.013 to 0.052 | 2.360 to 0.072 |
| latch-frame acq to worker start mean | 1.823 to 0.029 | 1.548 to 0.028 | 1.958 to 0.034 |
| cudaEventRecord stalls > 1 ms | 6 to 0 | 2 to 0 | 6 to 0 |
| same-die / other-die worker mean | 3.196 / 2.492 to 3.129 / 2.412 | 3.205 / 2.486 to 3.114 / 2.410 | 3.006 / 2.448 to 2.912 / 2.359 |

Reading: event sync is worth about 80 us on every frame, as predicted. The
deferred latch removes the acquisition-side tail completely. The stall fix
removes the periodic driver stall. The same-die encoder penalty is untouched
by these three levers, as expected; that is lever 2, below.

Overall: levers 1-4 together take camera 2010093 from 2.89 / 3.88 ms
(mean / p95, acquisition to result) to 2.57 / 3.10 ms, a 20% p95 reduction,
with every acquisition-side and driver-side tail gone. The remaining
structure is exactly what the August review predicted: an uncontended floor
near 2.4 ms set by yolo11n on a 10-SM die, plus a residual same-die encoder
penalty of a few tenths of a millisecond during shard-0 GOPs.

Lever 2, external recorder direct input, on top of the levers run
(this run minus levers, ms):

| Metric | 2010093 | 2010094 | 2010095 |
|---|---|---|---|
| acq to detect mean | 2.796 to 2.574 (-0.221) | 2.787 to 2.581 (-0.206) | 2.665 to 2.666 (+0.001) |
| acq to detect p95 | 3.770 to 3.095 (-0.675) | 3.753 to 3.096 (-0.657) | 3.535 to 3.309 (-0.225) |
| worker total p95 | 3.749 to 3.069 (-0.680) | 3.732 to 3.069 (-0.664) | 3.504 to 3.283 (-0.221) |
| same-die worker mean | 3.129 to 2.679 (-0.450) | 3.114 to 2.677 (-0.437) | 2.912 to 2.899 (-0.013) |
| other-die worker mean | 2.412 to 2.421 | 2.410 to 2.431 | 2.359 to 2.381 |

On 2010093 and 2010094 the detach slot copy was most of the same-die penalty:
removing it takes the p95 from 3.77 to 3.10 ms with zero recorder drops and
5,900 frames encoded per camera. On 2010095 (dies 7/8) the same-die mean did
not move. The per-shard recorder CSVs say why.

Interpretation. In the copy path, the detect-die shard on 2010093/2010094
shows `encode_total` p95 of 3.2-3.7 ms and `slot_reuse_wait` p95 of 2.5 ms:
the recorder is waiting on slot reuse and the encode submit blocks, so the
detach copy, the NVENC input copy, and NVENC itself all contend with YOLO on
that die for the whole 25-frame phase (worker mean flat at 3.1-3.3 ms across
the phase). On 2010095 the same shard shows `encode_total` p95 of 0.08 ms and
`slot_reuse_wait` p95 of 0.74 ms, and its phase profile ramps from 2.7 to
3.1 ms instead of sitting at 3.2: the copy path was already cheaper there
(same-die mean 2.91 versus 3.13). Direct input removes the slot copy and the
slot waiting; 2010095 had less of both to remove, so it gained less. Under
direct input all three cameras converge to a residual same-die penalty of
0.25-0.55 ms, which is what NVENC reading the frame plus the remaining Y-plane
copy cost on a GA107 die.

Why 2010095 was cheaper to begin with is a placement effect at the card
level, not the die level. 2010093 (dies 3/4) and 2010094 (dies 1/2) share one
A16 card and its PCIe switch, so that card carries two cameras' GPUDirect
inflow, two detectors, and four encoder shards. 2010095 (dies 7/8) sits on the
other card, whose second camera 2010096 (dies 5/6) was offline, so that card
carried half the load. Prediction: with 2010096 back online, 2010095's
copy-path same-die penalty rises to the 2010093/2010094 level and direct
input helps it as much. That is a one-run test with the fourcam specs.

Also visible in the direct-input CSVs: the recorder's per-frame `prepare`
(the Y-plane copy plus the event wait before ACK) is 7-10 ms p50, so the
recorder holds each source frame for most of a frame period and runs near
100% duty at 100 fps. It kept up here (0 drops), but it has no headroom, and
that is the reason direct input needs engineering (lever 2c) before it is a
production path.

Correction to an earlier reading: the first direct-input run finished with
`finalized external recording lacks a complete returned-NVENC frame identity
proof`, and this document briefly blamed the direct-input path. That was
wrong. The recorder started emitting a schema-2 identity proof in `e3cf98c`
(the commit before this review), while the C++ manifest check in
`src/session/recording_session.cpp` still accepted only schema 1, so every
headless external-IPC run on this branch head failed finalization regardless
of path. The checker now accepts schema 1 or 2 and, for schema 2, also
requires accepted, attempted, and written packet counts to equal the encoded
frame count with zero rejections or write failures. Rerun after the fix,
both the direct-input and the gate specs finish `completed` / `pass`.

What still stands against direct input as a production path is headroom:
its recorder prepare time of 7-10 ms per frame at 100 fps.

Lever 2b, the detect-priority handoff gate, on top of the levers run
(this run minus levers, ms):

| Metric | 2010093 | 2010094 | 2010095 |
|---|---|---|---|
| acq to detect mean | 2.796 to 2.511 (-0.284) | 2.787 to 2.501 (-0.286) | 2.665 to 2.465 (-0.200) |
| acq to detect p95 | 3.770 to 3.675 (-0.095) | 3.753 to 3.632 (-0.121) | 3.535 to 3.291 (-0.244) |
| worker total mean | 2.770 to 2.483 (-0.288) | 2.762 to 2.474 (-0.288) | 2.636 to 2.438 (-0.198) |
| same-die worker mean | 3.129 to 2.651 (-0.478) | 3.114 to 2.635 (-0.478) | 2.912 to 2.569 (-0.343) |
| other-die worker mean | 2.412 to 2.314 (-0.098) | 2.410 to 2.312 (-0.098) | 2.359 to 2.306 (-0.053) |

Gate counters: 5,901 frames gated per camera, 92-99% of them actually
waited, mean wait 1.7 ms, maximum 11 ms, zero timeouts. Recorder: 5,901
frames encoded, zero drops, enqueue-age p95 unchanged at 0.04 ms. A rerun
after the identity-proof checker fix reproduced the numbers within
0.03 ms and finished `completed` / `pass`. The gate
helps all three cameras, including 2010095, and even the other-die frames
improve by 0.1 ms, which says the recorder's copies were contending across
the card and not only on the detect die. Unlike direct input the recorder
path is unchanged, so this is the first same-die lever that is a candidate
for production as-is.

Two lessons from getting here. The first deferred-latch build still delayed
the latch frames because the normal-path YOLO enqueue sat *after* the
cadence probe, so the latch ran before the enqueue; the enqueue now precedes
the deferred latch. And `run_supervised_external_recorder_verifier` requires
`recording_session.json`, which the client only writes when
`fixed.recording_control.record_for_seconds` is set; specs that only set
`duration_s` fail the verifier after an otherwise clean run. The latency specs
now set both.

Reproduce: stamp a copy of the spec into `/tmp` with a unique
`experiment_id` and `external_recorder_contract.artifact_root` (the client
refuses to reuse a non-empty run folder), then:

```bash
sudo -n /usr/local/bin/orange-local-benchmark \
    --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
    --yolo-perf-log --yolo-perf-sample 1 /tmp/<stamped spec>.json
python3 scripts/analyze_yolo_latency_phases.py <run folder> \
    --external-recorder-dir <artifact_root> --steady-after 200 \
    --json levers.json --baseline-json baseline.json
```

The client starts the host PTP stack itself for `ptp_gate` runs and now keeps
ownership across its two status checks, so it stops a stack it started.

## Levers, In Order

Each step is one change and one measurement against the August baseline.

| # | Change | Measure | Expect |
|---|---|---|---|
| 1 | Done. `ORANGE_YOLO_SYNC_EVENT=1`, or `fixed.yolo_sync_event` in a spec, or `--orange-env` on the fourcam orchestrator | `total_ms` mean/p95, `cpu_event_record_ms` stall count, `sync_mode` column | 50-150 us off the mean; fewer lock stalls elsewhere |
| 2 | Measured, passes the contract. External recorder direct input (`ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT=1`, or `fixed.external_recorder_direct_input`); recorder has no headroom at 100 fps, see A/B results | cycle-phase profile and die split from the analysis script; detach `copy_ms` | Same-die mass shrinks toward the other-die peak |
| 2b | Measured. Detect-priority handoff gate (`fixed.external_recorder_detect_priority`), see A/B results and below | die split and cycle phase; `detect_priority_*` counters in pipeline perf | Same-die mean falls toward the other-die figure; no recorder drops |
| 2c | Zero-copy NVENC input from an NV12-shaped acquisition buffer, see below | die split; recorder `prepare_ms`; drops | Same-die penalty down to NVENC DMA; prepare well under 1 ms |
| 3 | Done. Deferred PTP latch (`ORANGE_PTP_LATCH_AFTER_FANOUT`, default on) | `acquisition_to_worker_start_ms` p99/p99.9; `ptp_latch_ns` in the cadence probe | p99 0.57 to about 0.06 ms |
| 4 | Done. Cache `build_gpu_runtime_info` per GPU (the real lock holder); `ORANGE_HEADLESS_GPU_DMON=0` remains available | stall fractional-second histogram | Verified: 0 stalls |
| 5 | Always-on GPU event timing in the perf row | `preprocess_gpu_ms`, `gap_ms`, `infer_gpu_ms` populated in production | Under 20 us overhead |
| 6 | GOP-aware tensor routing to the idle die on the same card, only if step 2 leaves most of the contention | die split; per-frame tensor copy time | All frames near the other-die figure plus 0.2-0.4 ms |
| 7 | Standalone engine loop on the A6000 while citrus renders | p99 under render load | Go only if p99 is comfortably under 1.5 ms |
| 8 | Fold the fused preprocess into the CUDA graph (upstream does this) | `cpu_pre_sync_ms` | One fewer launch, about 10 us |

### Same-die levers 2b and 2c

One NVENC shard has to live on the detect die at four cameras, so the
remaining same-die penalty has to be attacked by changing *when* and *whether*
the recorder's copies touch that die, not by moving the encoder.

**2b. Detect-priority handoff gate (time-shift the copies).** The frame
arrives at t=0, YOLO finishes by about 2.5 ms, and the die idles until the
next frame at 10 ms. The external IPC handoff used to send the frame
descriptor to the recorder immediately, so the detach copy and the NVENC
input copy landed on top of inference. `ORANGE_EXTERNAL_RECORDER_DETECT_PRIORITY`
(spec key `fixed.external_recorder_detect_priority`) makes the handoff worker
wait on the frame's YOLO completion event, or `detections_ready`, before the
detach, with a 50 ms timeout and counters that surface in the pipeline perf
CSV (`detect_priority_gated_frames`, `detect_priority_waited_frames`,
`detect_priority_wait_timeouts`, `detect_priority_wait_max_ns`). It mirrors
the in-process `ORANGE_RECORDING_DETECT_PRIORITY` gate in
`EncoderPreprocessWorker`, but waits on completion rather than input-ready
because the copies contend with inference, not only preprocess. Cost: about
2.5 ms more enqueue age at the recorder and one frame held 2.5 ms longer,
against a pool of 62 and a recorder queue of 32. Expect the same-die worker
mean to fall toward the other-die figure while the recorder's own timings
are unchanged; NVENC's DMA still overlaps, but the copies were the larger
term. Measure with `threecam_detect_latency_levers_gate.json` against the
levers run.

**2c. Zero-copy NVENC input from the acquisition buffer.** Allocate the
acquisition pool NV12-shaped: the Y plane is the GPUDirect target and a
chroma plane, prefilled to 128 once, follows it. Register the IPC-imported
buffer with NVENC in the recorder and hold the frame with the existing
deferred-source-release protocol until NVENC has read it. This removes the
20 MB copy entirely and with it the 7-10 ms prepare serialization that leaves
direct input with no headroom. The pieces exist separately: the in-process
direct-input v1 has the registration machinery, deferred release exists in the
recorder, and `tools/nv12_prefill_validation.cpp` already explores the chroma
prefill. The open question is whether the EVT GPUDirect allocation allows the
1.5x buffer footprint and the pitch NVENC wants (4512 is a multiple of 32).
Measure the same way; expect the same-die penalty to drop to NVENC's own DMA
cost, a few tenths of a millisecond at most, and the recorder's per-frame
prepare to fall from 7-10 ms to well under 1 ms.

### The frame lifecycle, and what the handoff copy buys

The buffer the camera writes into over GPUDirect is one of the acquisition
pool's 62 entries. The detach copy exists so that entry can return to the
pool after about 2 ms instead of after NVENC is done with it, which during a
GOP is a 10 ms wait with jitter on top. The pool never sees the encoder's
timeline and a stuck encoder can never starve the camera. That is the right
design goal, and the copy achieves it.

What it costs is not bandwidth. The traffic on the detect die during a
shard-0 GOP (the RDMA write, the YOLO read, the detach copy, the NVENC input
copy, NVENC's own read) is roughly 140 MB per 10 ms frame, about 14 GB/s on a
die that can move 200 GB/s. What is expensive is occupancy: a 20 MB copy takes
about 2 ms of a copy engine shared with the peer copies to the other shard
and with NVENC's DMA, and while it runs it steals bandwidth from a
latency-critical kernel sequence on a 10-SM die. That is the same-die
penalty. The gate removes most of it by moving the copy into the 7 ms the die
would otherwise idle, which is why it was nearly free.

The copy path pays twice on the recorder side: the detach copy into a
recorder slot, then a second copy from the slot into NVENC's input buffer.
Direct input removes the first and keeps the second, but re-couples the
handoff to NVENC's buffer availability, which is where its headroom went.

The cheaper way to buy the same decoupling is depth instead of a copy. At
100 fps, letting NVENC read the acquisition buffer directly and holding the
entry until NVENC finishes means one to three extra pool entries in flight
out of 62. The deferred-release protocol already exists to do exactly that
without blocking any thread: the recorder ACKs at once so the handoff moves
on, tags the ACK `deferred_release`, and sends a release message when it has
finished reading; the ingress worker recycles the entry then. No frame is
dropped by this; it only changes when the entry returns. The entry is held
for NVENC's read time (about 10 ms during a GOP, longer if the recorder has a
backlog) rather than the 2 ms copy, so the pool absorbs it easily, and the
only new failure mode is a stalled recorder holding many entries, which needs
a cap on outstanding deferred entries with a recording-side (not
acquisition-side) fallback. That is lever 2c: zero copies, buffers returned
by message, pool kept deep enough that a slow encode cannot starve the camera.

### Why direct input has no headroom

It comes down to which thread waits for NVENC.

The copy path has two stages in the recorder process. The handoff stage
copies the incoming frame into one of 32 recorder-owned slots (about 2 ms),
records an event, and ACKs the source. A separate encode thread then feeds
those slots to NVENC at whatever pace NVENC manages. The slot queue decouples
the two, so the handoff finishes early and the queue absorbs encode hiccups;
that is why enqueue age could reach 25 ms in August without a drop.

Direct input collapses the two stages into one. For each frame the recorder
waits until NVENC has released one of its own input buffers, copies the Y
plane straight into it, blocks on a CUDA event until the copy has landed, and
only then ACKs the source. NVENC has a handful of input buffers and releases
one only when it has finished encoding the frame that used it, so the wait at
the front of that sequence is bounded by NVENC's encode time.

That time is 7-10 ms. A 4512x4512 HEVC frame at 150 Mb/s takes a GA107's
NVENC about 10 ms. During a 25-frame GOP the shard receives a frame every
10 ms, NVENC runs flat out for the whole GOP, and a free input buffer appears
every 10 ms. The measured `prepare` p50 of 7-10 ms is almost entirely that
wait, not the copy.

Headroom, then, means this: the recorder thread is busy 7-10 ms of every
10 ms frame period and holds the source frame the whole time. It kept up in
the 60 s runs with zero drops, but one slow encode, a PTP hiccup, or a burst
of bitstream fetches pushes it past 10 ms per frame, and unlike the copy path
there is no queue to absorb that. It also holds source frames longer, which
eats into the acquisition pool.

Lever 2c fixes this rather than repeating it. Registering the acquisition
buffer itself as the NVENC input removes the copy, and the deferred-release
protocol makes the wait for NVENC asynchronous: the handoff returns at once
and the source is released by a message when NVENC is done. The wait still
exists, but nothing blocks on it. The gate (lever 2b) adds no NVENC coupling
at all, which is why it was safe to default on.

Lower-probability options, for completeness: INT8 (halves activation bytes,
so inference is both faster and less sensitive to bandwidth contention; there
is an INT8 plan in the tree), and GOP-aware tensor routing (lever 6). Stream
priority is already highest and preset p1/ll is already the cheapest NVENC
setting, so neither is a lever.

### On `nvenc_direct_input`

Two different mechanisms carry that name, and only one applies to the August
run:

- `recording.encode.nvenc_direct_input` (camera config /
  `ORANGE_NVENC_DIRECT_INPUT`) is the modern in-process path
  (`EncoderPreprocessWorker` writes NV12 into NVENC-registered CUDA surfaces,
  `EncoderHwWorker` submits them). It is Linux-native; the codebase is
  Linux-only and NVENC registration of CUDA device pointers is a Linux feature
  of the Video Codec SDK. It was validated in April 2026 on the RTX A6000 at
  60 fps (`docs/nvenc_direct_input_v1_todo.md`) with long-run, mono, and drain
  tests still unchecked. It does not touch the external IPC recorder, which is
  what the August run used (`recording_outputs/*/full/backend = external_ipc`).
- The external recorder process (`tools/external_recorder_ipc_probe.cpp`) has
  its own `--direct-input-source` / `ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT`.
  With it, the recorder skips the detach slot copy and copies the Mono8 Y plane
  straight from the IPC-imported source into the NVENC input buffer (one 20 MB
  copy instead of two), waiting on a CUDA event before it ACKs. The supervisor
  spawns recorders with `execvp`, so the environment variable reaches them.
  `scripts/run_external_recorder_smoke.sh` labels the option a legacy
  diagnostic and rejects it; the contract verifier accepts
  `detach_copied == encode_enqueued` either way and reads
  `direct_input_source` from the summary to interpret it. Treat it as an
  experiment: check `scripts/verify_external_recorder_session.py` passes on the
  result before trusting the recording.

## Reproducing The Analysis

```bash
python3 scripts/analyze_yolo_latency_phases.py \
    /home/jeremy/orange_data/exp/unsorted/2026_08_25_23_12_06 \
    --json /tmp/latency_phases.json
```

Needs numpy only. The script reports per-camera quantiles, the encoder-die
split, the cycle-phase profile, the PTP latch spike, the stall histogram by
fractional second, the preview cadence, and detach copy times per GPU. Use
`--steady-after` to trim warmup and `--cycle` to override the derived
split-GOP period. `tools/analyze_yolo_latency_phases_tests.py` checks it
against a synthetic run.

New instrumentation from this review:

- `Cam*_yolo_perf.csv` gains a trailing `sync_mode` column (`event` or
  `poll`).
- `recording_snapshot_start.json` `session/yolo_worker/runtime_flags` records
  every resolved detect-path flag (`src/yolo_runtime_flags.h` is the single
  source of defaults).
- `Cam*_acquisition_cadence_probe.csv` gains `ptp_latch_deferred` and
  `ptp_latch_ns`.
- `ORANGE_HEADLESS_GPU_DMON=0` disables the headless `nvidia-smi dmon`
  subprocess; the snapshot records `disabled_by_env`.
- Experiment specs accept `fixed.yolo_sync_event`, `fixed.ptp_latch_after_fanout`,
  `fixed.headless_gpu_dmon`, and `fixed.external_recorder_direct_input`;
  `orange_client` exports the matching variables itself, so the sudo wrapper's
  allowlist is not involved. The GUI wrapper and
  `run_gui_aq_off_validation.sh` forward the same variables for GUI runs.

## Landed Commits And Follow-Ups

Pushed to `origin/agent/acquisition/shaman-v2-authoritative-20260824` on
2026-09-03 (`e3cf98c` to `0ed660f`):

- `c0e9b5d`: deferred PTP latch, `ORANGE_HEADLESS_GPU_DMON`,
  `src/yolo_runtime_flags.h`, resolved flags in the start snapshot, the
  `sync_mode` perf column, `scripts/analyze_yolo_latency_phases.py` and its
  test, this document.
- `260eabb`: spec-driven levers in `orange_client`, YOLO enqueue ahead of the
  deferred latch, `build_gpu_runtime_info` cached per GPU (the stall fix),
  PTP stack ownership kept across both status checks, GUI wrapper and
  validation script forwarding, six threecam/fourcam latency specs,
  `--external-recorder-dir` and `--baseline-json` for the analysis script.
- `0ed660f`: the direct-input interpretation above.
- `9b2f261`: this section.
- `d33c76b`: the detect-priority handoff gate (lever 2b) with its A/B, the
  schema-2 identity-proof checker fix, and the correction of the earlier
  direct-input claim.

Follow-ups, in order:

1. Bring 2010096 back online (no link carrier on 2026-09-03) and run the three
   `fourcam_detect_latency_*` specs. This tests the card-level prediction for
   2010095 and gives a four-camera A/B directly comparable with the August run.
2. Reinstall the GUI wrapper before the next citrus run:
   `sudo scripts/install_orange_gui_validation_wrapper.sh`. The headless path
   needs no installer.
3. Done: event sync, the deferred latch, and the handoff gate default on;
   the stall fix is unconditional.
4. Decide between lever 2b (the handoff gate, production-compatible today,
   -0.29 ms mean) and lever 2c (zero-copy NVENC input, larger win, needs
   engineering). Direct input as it stands passes the contract but its
   recorder runs at 7-10 ms per frame with no headroom at 100 fps.
4b. Before lever 2c can rely on deferred release: cap the number of pool
   entries a recorder may hold pending. Today nothing bounds
   `pending_release_entries_` in `ExternalIpcHandoffWorker`; a stalled
   recorder would hold entries until the pool starved and acquisition itself
   stalled. When the cap is hit, skip or drop that frame on the recording side
   with a counter in the pipeline perf CSV, never on the acquisition side, so
   the camera never waits on the encoder. Size the cap from the pool (62) minus
   the entries the other consumers need in flight; something like 16 leaves
   room for a 160 ms recorder stall at 100 fps.
5. Always-on GPU event timing in the perf row (checklist step 5), which turns
   the remaining 2.4 ms floor into preprocess, gap, and infer numbers in
   production runs.
6. Like-for-like engine test against the colleague's rig: same engine file on
   one die with no recorder running.
7. The stock fourcam supervised spec lacks
   `recording_control.record_for_seconds` and fails the verifier for a missing
   session manifest after a clean run; fix it the way the latency specs do.
8. Regression note for the branch head: `e3cf98c` moved the recorder's
   frame-identity proof to schema 2 without updating the C++ manifest check,
   so every headless external-IPC run failed finalization until the checker
   fix in this review. Worth a test that pins the recorder's proof schema to
   what the checker accepts.
