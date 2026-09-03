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
that is a second reason direct input needs engineering before it is a
production path.

Caveat that decides whether lever 2 is usable: the run finished with
`finalized external recording lacks a complete returned-NVENC frame identity
proof`. The direct-input path does not produce the schema-2 frame-identity
proof the recording contract requires, which is why the smoke runner labels
it a legacy diagnostic. The latency numbers are valid; the recording is not
accepted. Making direct input a production path means teaching it to emit the
returned-NVENC identity proof, or relaxing the contract for it deliberately.

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
| 2 | Measured. External recorder direct input (`ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT=1`, or `fixed.external_recorder_direct_input`); recording contract not yet satisfied, see A/B results | cycle-phase profile and die split from the analysis script; detach `copy_ms` | Same-die mass shrinks toward the other-die peak |
| 3 | Done. Deferred PTP latch (`ORANGE_PTP_LATCH_AFTER_FANOUT`, default on) | `acquisition_to_worker_start_ms` p99/p99.9; `ptp_latch_ns` in the cadence probe | p99 0.57 to about 0.06 ms |
| 4 | Done. Cache `build_gpu_runtime_info` per GPU (the real lock holder); `ORANGE_HEADLESS_GPU_DMON=0` remains available | stall fractional-second histogram | Verified: 0 stalls |
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

Follow-ups, in order:

1. Bring 2010096 back online (no link carrier on 2026-09-03) and run the three
   `fourcam_detect_latency_*` specs. This tests the card-level prediction for
   2010095 and gives a four-camera A/B directly comparable with the August run.
2. Reinstall the GUI wrapper before the next citrus run:
   `sudo scripts/install_orange_gui_validation_wrapper.sh`. The headless path
   needs no installer.
3. Decide defaults. Event sync and the deferred latch are verified wins with no
   downside seen; the stall fix is unconditional. Defaulting
   `ORANGE_YOLO_SYNC_EVENT` on is one line in `src/yolo_runtime_flags.h`.
4. Make direct input a production path or leave it an experiment flag. It
   needs the schema-2 returned-NVENC frame-identity proof, and its recorder
   runs at 7-10 ms per frame with no headroom at 100 fps.
5. Always-on GPU event timing in the perf row (checklist step 5), which turns
   the remaining 2.4 ms floor into preprocess, gap, and infer numbers in
   production runs.
6. Like-for-like engine test against the colleague's rig: same engine file on
   one die with no recorder running.
7. The stock fourcam supervised spec lacks
   `recording_control.record_for_seconds` and fails the verifier for a missing
   session manifest after a clean run; fix it the way the latency specs do.
