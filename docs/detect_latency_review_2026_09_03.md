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
   second. The headless client's `nvidia-smi dmon` subprocess polls at 1 Hz and
   is the leading suspect. Today the stall hides inside the GPU wait.

5. **Production runs do not record their own flags.** Only affinity is written
   to the snapshot. Reconstructing the rest required reading code defaults and
   launch scripts. Defaults also disagree with each other (`SYNC_EVENT` off,
   `DETACH_INPUT` on, `INLINE_CROP_PRODUCER` off).

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

## Levers, In Order

Each step is one change and one measurement against the August baseline.

| # | Change | Measure | Expect |
|---|---|---|---|
| 1 | `ORANGE_YOLO_SYNC_EVENT=1` (no code change; `--orange-env` on the fourcam orchestrator) | `total_ms` mean/p95, `cpu_event_record_ms` stall count, `sync_mode` column | 50-150 us off the mean; fewer lock stalls elsewhere |
| 2 | External recorder direct input (`ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT=1`), see below | cycle-phase profile and die split from the analysis script; detach `copy_ms` | Same-die mass shrinks toward the other-die peak |
| 3 | Deferred PTP latch (`ORANGE_PTP_LATCH_AFTER_FANOUT`, default on) | `acquisition_to_worker_start_ms` p99/p99.9; `ptp_latch_ns` in the cadence probe | p99 0.57 to about 0.06 ms |
| 4 | `ORANGE_HEADLESS_GPU_DMON=0` for one run | stall fractional-second histogram | The 1 Hz bin goes flat; otherwise nsys for the other lock holder |
| 5 | Always-on GPU event timing in the perf row | `preprocess_gpu_ms`, `gap_ms`, `infer_gpu_ms` populated in production | Under 20 us overhead |
| 6 | GOP-aware tensor routing to the idle die on the same card, only if step 2 leaves most of the contention | die split; per-frame tensor copy time | All frames near the other-die figure plus 0.2-0.4 ms |
| 7 | Standalone engine loop on the A6000 while citrus renders | p99 under render load | Go only if p99 is comfortably under 1.5 ms |
| 8 | Fold the fused preprocess into the CUDA graph (upstream does this) | `cpu_pre_sync_ms` | One fewer launch, about 10 us |

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
