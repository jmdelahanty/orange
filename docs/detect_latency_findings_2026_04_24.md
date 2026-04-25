# Detect Latency Findings - 2026-04-24

Scope: summarize what the 2026-04-24 detect/crop/YOLO latency work has proven,
what remains uncertain, and which architecture changes are highest signal next.

This document is intentionally more experiment-focused than
`detect_hot_path_rework_plan.md`. Use it as the current evidence log before
changing the hot path again.

## Current Conclusion

The remaining detect tail under the real two-camera `100 fps` GUI workload is
not primarily YOLO compute, source readiness, crop staging, crop preview, or
full-frame OpenGL display.

The strongest current evidence points to host-side CUDA/NVENC driver contention
from the full-frame recording encode/output path:

- The actual YOLO preprocess kernel, `mono_to_yolo_optimized`, remains about
  `0.07 ms p95`.
- In full-frame encode runs, the host-side YOLO preprocess launch path grows to
  about `8 ms p95`.
- In `preprocess_only`, where full-frame preprocess routing remains active but
  full-frame encode/output is removed, the YOLO preprocess launch path collapses
  to about `0.26 ms p95`.
- Nsight shows full-frame `HW_Encoder_Cam_ nvEncLockBitstream` calls blocking
  around `11.5 ms p95` in the full encode run.
- When those full-frame HW encoder threads disappear, YOLO launch latency and
  `capture_to_detect_done` p95 collapse.
- The in-process split-harvest experiment moved `nvEncLockBitstream` off the
  encoder submit path and reduced submit p95 from about `11.85 ms` to about
  `0.31 ms`, but YOLO `cudaLaunchKernel_v7000` and `pthread_rwlock_rdlock`
  p95 remained about `8.3-8.4 ms`.

The short version:

```text
The YOLO GPU work is fast.
The YOLO CPU thread is sometimes blocked while submitting CUDA work.
Full-frame NVENC/output activity is the strongest observed source of that block.
```

The newest configuration-level result is that generic NVENC AQ and temporal AQ
are now suspect for this imaging regime:

- A focused headless real-YOLO plus real split-GOP VBR A/B showed
  `AQ on + temporalAQ on` at `10.487 ms` detect p95.
- The same setup with `AQ off + temporalAQ off` dropped to `3.820 ms` detect
  p95 while still sustaining about `100 fps` and about `150 Mbps`.
- This makes schema-level `recording.encode.aq = off` and
  `recording.encode.temporal_aq = off` the highest-signal next two-camera GUI
  validation before deeper process-isolation work.

The newest two-camera headless result is that PTP is required for a valid
dual-camera headless comparison on the local `100_cam4` setup:

- A free-run two-camera headless run produced valid-looking artifacts but
  `2010095` encoded an all-black stream at only about `24.6 Mbps`, while
  `2010096` encoded real dish content at about `151 Mbps`.
- The same headless real-YOLO plus real split-GOP test with
  `fixed.sync_mode = "ptp_gate"` produced real high-bitrate streams on both
  cameras: `2010095` about `150.8 Mbps`, `2010096` about `151.3 Mbps`.
- The PTP run sustained about `100 fps`, had `0` camera frame-ID gaps, and had
  `0` encode failures on both cameras.
- With both cameras producing real content, detect p95 returned to the
  `11-12 ms` range: `2010095` `11.009 ms`, `2010096` `12.181 ms`.
- YOLO queue wait stayed tiny, about `0.014-0.020 ms p95`; the tail was again
  mostly `cpu_pre_sync_ms` at about `6.8-7.9 ms p95`.

Interpretation:

```text
Free-run headless was not a clean two-camera content/load comparison.
PTP gate made both cameras produce real 20 MP streams at roughly equal bitrate.
Under that valid two-camera load, the detect tail remains CUDA/NVENC-side
submission/synchronization contention, not YOLO queue backlog.
```

Non-negotiable recording constraint:

- At the current `4512x4512 Mono8 @ 100 fps` full-frame resolution, one GPU /
  one NVENC path cannot be assumed to sustain the required recording stream.
- The validated production recording architecture is multi-GPU split-GOP.
- Any detect-latency fix must preserve split-GOP full-frame MP4 output unless
  the run is explicitly labeled as a diagnostic sink such as `preprocess_only`
  or `immediate_recycle`.
- Therefore, "put recording back on one GPU" is not an acceptable architecture
  solution. The real problem is isolating or scheduling the required split-GOP
  recording work so it stops inflating YOLO host-side CUDA submission latency.

## Streams Versus Driver Contention

Separate CUDA streams provide GPU execution-order independence. They do not make
all CPU-side CUDA and NVENC calls independent.

There are two different contention modes:

```text
GPU timeline contention:
  Stream A work waits behind Stream B work on the GPU.

CPU/driver submission contention:
  A CPU thread calls cudaLaunchKernel, cudaEventRecord, cudaMemcpyAsync, or
  NVENC/CUDA interop, then blocks inside the CUDA/NVIDIA driver before its work
  is submitted.
```

The current evidence mostly shows the second mode:

- source-ready stream waits are tiny,
- YOLO preprocess kernel duration is tiny,
- the wall time is in the host-side CUDA runtime calls,
- Nsight attributes long API calls to lock/wait behavior such as
  `pthread_rwlock_rdlock` and to overlapping CUDA/NVENC activity.

So the architecture problem is not "put YOLO and recording on separate
streams." They already have independent streams for much of their work. The
problem is that the threads still share the same process, CUDA runtime/driver,
GPU context state, memory registrations, events, NVENC resources, and output
copy machinery.

## NVENC Linux Async Status

As of the NVIDIA Video Codec SDK 13.0 programming guide, true asynchronous
NVENC output completion is still not available on Linux:

- The SDK supports synchronous encode/output on Windows, Linux, and Jetson
  Linux.
- Asynchronous encode mode, where NVENC signals a completion event for an
  output buffer, is documented as Windows 10+ with WDDM only.
- Linux, Windows TCC, and Jetson Linux are documented as synchronous-mode-only
  environments.
- In synchronous mode, the documented output path calls
  `NvEncLockBitstream` with `NV_ENC_LOCK_BITSTREAM::doNotWait = 0`, which
  blocks until the hardware encoder has finished the bitstream.
- The API also exposes `NV_ENC_LOCK_BITSTREAM::doNotWait = 1`; with that flag,
  `NvEncLockBitstream` can return `NV_ENC_ERR_LOCK_BUSY` instead of blocking.
  This is a polling/retry mechanism, not the same thing as Windows async event
  completion.
- The SDK also supports output-in-video-memory for CUDA/DX use cases, but the
  Linux synchronous path still relies on driver-managed synchronization before
  the output can be read or copied.

Official references:

- NVIDIA Video Codec SDK 13.0 programming guide:
  `https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html`
- NVIDIA Video Codec SDK overview:
  `https://developer.nvidia.com/video-codec-sdk`

Current repo state matches that model:

- `src/NvEncoder/NvEncoder.cpp` uses `doNotWait = false` before
  `nvEncLockBitstream`.
- `WaitForCompletionEvent(...)` is effectively a Windows async-mode hook; on
  Linux it does not provide a real NVENC completion event wait.
- The Nsight runs measured the expected blocking site:
  full-frame `HW_Encoder_Cam_ nvEncLockBitstream` around `11.5 ms p95` while
  YOLO host-side CUDA launch calls stretched to about `8 ms p95`.

Design implication:

- Do not expect a newer Linux SDK setting to make full-frame NVENC output
  completion event-driven in-process.
- A useful in-process experiment is a nonblocking `doNotWait = 1`
  lock/harvest path with a pending-output state machine, retries, and delayed
  buffer retirement.
- That experiment must be treated as an output-harvest scheduling change, not
  as true async NVENC completion.
- If driver-level lock contention remains dominant, the stronger architecture
  candidate is process isolation for full-frame encode/output so YOLO and
  recording do not share the same process-level CUDA/NVENC driver state.

## Fundamental Boundary Versus Architecture Choice

The current issue has one fundamental part and one architecture-dependent part.

Fundamental:

- On Linux, NVENC output completion is synchronous at the API level.
- The practical completion boundary is `nvEncLockBitstream`.
- If the requested bitstream is not ready, the call can block unless the code
  uses `NV_ENC_LOCK_BITSTREAM::doNotWait = 1` and implements a retry/pending
  output state machine.
- There is no supported Linux equivalent of Windows NVENC event-driven
  `enableEncodeAsync` completion.

Not fundamental:

- The blocking bitstream harvest does not have to happen on the same logical
  submit path as encode submission.
- The low-latency YOLO path does not have to share all process-level CUDA
  runtime, CUDA context, NVENC interop, thread scheduling, and allocator state
  with bulk full-frame recording.
- YOLO does not necessarily have to expose a raw host-side
  `cudaLaunchKernel` call for every frame's preprocess step.

Current belief:

- The current single-process architecture is probably suboptimal for this
  workload because it mixes a hard real-time-ish detect path with a bulk
  full-frame split-GOP recording path inside one CUDA/NVENC runtime contention
  domain.
- The evidence does not prove that multiprocessing is the only possible fix,
  but it does make process isolation the cleanest long-term QoS boundary if
  in-process experiments fail to reduce YOLO's `cudaLaunchKernel` /
  `pthread_rwlock_rdlock` p95.
- The strongest non-multiprocess idea is not only moving
  `nvEncLockBitstream`; it is reducing or eliminating YOLO's exposed raw CUDA
  launch point, for example through CUDA graph capture or a TensorRT-integrated
  preprocess path.

## Non-Multiprocess Architecture Options

Option A: split NVENC submit and harvest in-process.

- Submit thread waits for preprocess readiness, maps/copies input as needed,
  calls `nvEncEncodePicture`, enqueues an output token, and returns quickly.
- Harvest thread consumes output tokens in submission order and calls
  `nvEncLockBitstream`, copies packets, unlocks/unmaps, and retires resources.
- Status: implemented behind `ORANGE_NVENC_SPLIT_HARVEST=1`.
- Result: encoder submit p95 improved as expected, but YOLO p95 did not move.
- Value: high as an architectural discriminator. It shows the remaining YOLO
  stall is not just a bad submit/harvest call-site structure.

Option B: nonblocking/polling NVENC harvest in-process.

- Use `NV_ENC_LOCK_BITSTREAM::doNotWait = 1` and retry when NVENC returns
  `NV_ENC_ERR_LOCK_BUSY`.
- This avoids parking a thread inside `nvEncLockBitstream`, but it is still
  synchronous Linux NVENC with polling.
- Expected result: better control over where CPU time is spent, not true async
  completion.
- Expected risk: excessive polling can create more driver/API traffic and make
  YOLO contention worse unless backoff and queue depth are tuned carefully.

Option C: reduce YOLO host CUDA submission exposure.

- Capture YOLO preprocess plus inference launch into a CUDA graph where
  practical, or move preprocess into a TensorRT plugin/integrated model input
  path.
- Nsight has repeatedly shown `cudaGraphLaunch` is much cheaper than the raw
  YOLO preprocess `cudaLaunchKernel` tail.
- Expected result: even if recording remains noisy, the detect path has fewer
  vulnerable host CUDA calls.
- Expected risk: implementation complexity and constraints around dynamic input
  addresses, source-frame ownership, and current CPU postprocess/ROI flow.

Option D: centralized in-process CUDA/NVENC submission scheduler.

- Route latency-critical CUDA/NVENC host API calls through a small number of
  priority submit threads instead of allowing many workers to contend directly
  inside the driver/runtime.
- YOLO submissions get first service; recording submits opportunistically.
- Expected result: better in-process fairness and fewer lock convoys.
- Expected risk: substantial redesign, possible throughput regression, and no
  guarantee if contention is below the user-space scheduling layer.

Option E: pure offload / cleaner GPU placement.

- Keep the source/YOLO GPU as clean as possible and push full-frame recording
  encode/output pressure to helper GPUs.
- Expected result: reduced source-GPU hardware and driver traffic.
- Expected risk: this does not remove same-process CUDA/NVENC runtime locks and
  must preserve the validated split-GOP output contract.

Option F: process isolation for recording encode/output.

- Move full-frame split-GOP encode/output into one or more separate processes.
- Expected result: strongest QoS boundary for low-latency detect versus bulk
  recording; likely reduces process-level CUDA/NVENC runtime lock coupling.
- Expected risk: larger architecture change with IPC/shared-buffer design,
  lifecycle management, crash handling, and throughput validation.
- Important caveat: process isolation will not remove all shared contention.
  The same kernel driver, GPUs, NVENC engines, PCIe fabric, CPU memory, and
  NUMA topology still exist.

## Current Pipeline Shape

The latency-critical detect path is:

```text
Acquisition
  -> record source/ingress-ready event
  -> enqueue WORKER_ENTRY to YoloWorker

YoloWorker
  -> check/wait source event
  -> launch YOLO preprocess into TensorRT input
  -> optionally record YOLO-input-ready event
  -> enqueue TensorRT graph/inference
  -> wait for model completion
  -> CPU postprocess / ROI selection
  -> hand off to CropProducer

CropProducer / Crop consumers
  -> produce owned crop frame
  -> offer to noop pose, crop encode, crop preview sidecars
```

The full-frame recording path is concurrent with that:

```text
Acquisition
  -> split-GOP recording ingress
  -> source/primary preprocess on source GPU
  -> helper peer-copy and helper preprocess where routed
  -> full-frame HW encoder threads
  -> NVENC bitstream lock/output
```

The important interaction is host-side:

```text
YoloWorker wants to submit a tiny CUDA preprocess kernel.
Full-frame recording threads are active in CUDA/NVENC calls.
The YOLO CPU thread can block inside the shared driver/runtime before launch.
```

## Experiment Log

### Baseline: inline crop was not a clean win

Run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_12_41_01`

Flags of interest:

- `ORANGE_INLINE_CROP_PRODUCER=1`

Result:

- Run was healthy: about `100 fps`, no camera drops, no pose drops, no
  crop-sidecar drops.
- `Cam2010095` improved slightly.
- `Cam2010096` regressed badly, especially
  `acquisition_to_worker_start p95 = 9.2989 ms`.

Interpretation:

- Inline crop removed a worker hop, but it likely made the YOLO-side hot path
  heavy enough to create multi-camera service skew.
- Do not make inline crop default.
- Keep it as an experiment only.

### No-full-frame diagnostic

Run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_14_04_01`

Flags of interest:

- `ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME=1`

Result:

- `session.recording_sink_mode = "immediate_recycle"`
- `session.full_frame_video_enabled = false`
- Run was healthy at about `100 fps`.
- `Cam2010095 capture_to_detect_done p95 = 5.655 ms`
- `Cam2010096 capture_to_detect_done p95 = 5.682 ms`
- `capture_to_pose_done p95` was about `5.7 ms` on both cameras.

Interpretation:

- Removing main full-frame recording work removes most of the detect tail.
- Full-frame recording pressure is real.
- This is diagnostic only because full-frame video is required for production.

### Detect-priority split-GOP

Run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_14_24_39`

Flags of interest:

- `ORANGE_RECORDING_DETECT_PRIORITY=1`

Result:

- Real full-frame split-GOP recording stayed healthy.
- `Cam2010095 capture_to_detect_done p95 = 10.587 ms`
- `Cam2010096 capture_to_detect_done p95 = 12.538 ms`
- `detect_to_crop_ready p95` was about `0.07-0.10 ms`.

Interpretation:

- Prioritizing detect before source/primary recording helped the crop handoff,
  but it did not remove the full detect tail.
- The remaining issue is deeper than simple queue ordering.

### Stream priority and nonblocking stream experiment

Run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_14_58_09`

Flags of interest:

- `ORANGE_YOLO_STREAM_PRIORITY=high`
- `ORANGE_YOLO_STREAM_NONBLOCKING=1`

Result:

- Throughput stayed healthy.
- YOLO fairness regressed.
- `Cam2010096 capture_to_detect_done p95 = 20.493 ms`
- `Cam2010096 yolo_queue_wait_ms p95 = 7.706 ms`

Interpretation:

- Do not use this as the next architecture direction.
- Stream priority/nonblocking did not solve host-side contention and made
  service fairness worse in this workload.

### YOLO input detach baseline

Run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_15_39_15`

Flags of interest:

- `ORANGE_RECORDING_DETECT_PRIORITY=1`
- `ORANGE_YOLO_DETACH_INPUT=1`
- `ORANGE_YOLO_READY_EVENT_FASTPATH=1`

Result:

- Real full-frame MP4s were present.
- Run was healthy.
- `Cam2010095 cpu_preprocess_ms p95 = 2.937 ms`
- `Cam2010096 cpu_preprocess_ms p95 = 8.271 ms`
- `Cam2010095 capture_to_detect_done p95 = 7.645 ms`
- `Cam2010096 capture_to_detect_done p95 = 12.562 ms`

Interpretation:

- Input detach is architecturally valuable because it shortens source-buffer
  ownership, but it does not by itself eliminate the YOLO launch tail.
- Cam-to-cam asymmetry remains visible.

### Nsight baseline with YOLO detach

Artifacts:

- `/tmp/orange_yolo_detach_nsys_20260424_163041.nsys-rep`
- `/tmp/orange_yolo_detach_nsys_20260424_163041.sqlite`

Finding:

- `YOLO source event wait p95` was tiny, about `0.017-0.018 ms`.
- `YOLO preprocess_gpu call p95` was about `3.237 ms` for `Cam2010095` and
  `8.239 ms` for `Cam2010096`.
- Actual `mono_to_yolo_optimized` GPU kernel p95 stayed about `0.073 ms`.
- The long wall time matched long `cudaLaunchKernel` host calls.
- OSRT attribution showed the long launches were mostly blocked inside
  `pthread_rwlock_rdlock`.

Interpretation:

- The slow part is host-side CUDA launch/driver lock behavior, not GPU
  preprocess compute.

### YOLO realtime scheduling experiment

Artifacts:

- `/tmp/orange_yolo_detach_nsys_20260424_170327.nsys-rep`
- `/tmp/orange_yolo_detach_nsys_20260424_170327.sqlite`
- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_17_03_55`

Flags of interest:

- `ORANGE_YOLO_RT_PRIORITY=<positive int>`
- `ORANGE_YOLO_RT_POLICY=fifo|rr`

Result:

- Not a win.
- `Cam2010095 cpu_preprocess_ms p95` regressed to about `7.128 ms`.
- `Cam2010096 cpu_preprocess_ms p95` stayed about `8.086 ms`.

Interpretation:

- Linux realtime priority does not solve this issue because the YOLO thread is
  often blocked inside the CUDA/NVENC driver, not merely runnable and waiting
  for Linux scheduler time.

### Crop live-preview disabled

Artifacts:

- `/tmp/orange_yolo_detach_nsys_20260424_172559.nsys-rep`
- `/tmp/orange_yolo_detach_nsys_20260424_172559.sqlite`
- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_17_26_33`

Flags of interest:

- `ORANGE_CROP_PREVIEW_DISABLE=1`

Result:

- Full-frame and crop MP4s were present.
- No drops.
- `crop_preview_cpu_ms` and `display_sync_ms` became `0`.
- `detect_to_crop_ready p95` dropped to about `0.19/0.43 ms`.
- YOLO `cpu_preprocess_ms p95` stayed high at about `7.8/8.2 ms`.

Interpretation:

- Crop preview was materially hurting crop/pose handoff.
- Crop preview was not the remaining YOLO detect-tail culprit.

### Display throttle to 1 fps

Artifacts:

- `/tmp/orange_yolo_detach_nsys_20260424_175505.nsys-rep`
- `/tmp/orange_yolo_detach_nsys_20260424_175505.sqlite`
- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_17_55_35`

Flags of interest:

- `ORANGE_CROP_PREVIEW_DISABLE=1`
- `ORANGE_DISPLAY_PREVIEW_MAX_FPS=1`

Result:

- Full-frame and crop MP4s were present.
- No drops.
- Display selected only about `18` frames per camera.
- `Cam2010095 cpu_preprocess_ms p95 = 7.983 ms`
- `Cam2010096 cpu_preprocess_ms p95 = 8.371 ms`
- `Cam2010095 capture_to_detect_done p95 = 11.981 ms`
- `Cam2010096 capture_to_detect_done p95 = 12.320 ms`

Interpretation:

- Full-frame OpenGL display is not the main remaining culprit.
- The remaining contention survived with display almost removed.

### Preprocess-only recording sink

Artifacts:

- `/tmp/orange_yolo_detach_nsys_20260424_180514.nsys-rep`
- `/tmp/orange_yolo_detach_nsys_20260424_180514.sqlite`
- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_18_05_41`

Flags of interest:

- `ORANGE_GUI_RECORDING_SINK_MODE=preprocess_only`
- `ORANGE_CROP_PREVIEW_DISABLE=1`
- `ORANGE_DISPLAY_PREVIEW_MAX_FPS=1`

Result:

- `session.recording_sink_mode = "preprocess_only"`
- `session.full_frame_video_enabled = false`
- Crop MP4s and telemetry were still produced.
- Main full-frame `Cam<serial>.mp4` files were intentionally absent.
- No camera drops, no preprocess drops, no crop-sidecar drops.
- `Cam2010095 cpu_preprocess_ms p95 = 0.263 ms`
- `Cam2010096 cpu_preprocess_ms p95 = 0.257 ms`
- `Cam2010095 worker_start_to_yolo_input_ready p95 = 0.274 ms`
- `Cam2010096 worker_start_to_yolo_input_ready p95 = 0.267 ms`
- `Cam2010095 capture_to_detect_done p95 = 4.425 ms`
- `Cam2010096 capture_to_detect_done p95 = 4.444 ms`

Nsight comparison:

- Full encode run:
  - YOLO `cudaLaunchKernel_v7000 p95` was about `7.95/8.32 ms`.
  - Full-frame `HW_Encoder_Cam_ nvEncLockBitstream p95` was about
    `11.55-11.68 ms` on the blocking HW encoder threads.
- Preprocess-only run:
  - YOLO `cudaLaunchKernel_v7000 p95` was about `0.24-0.25 ms`.
  - Full-frame HW encoder threads were not present.
  - Crop encoder `nvEncLockBitstream p95` stayed tiny, about `0.003 ms`.

Interpretation:

- This is the strongest isolation result so far.
- Full-frame encode/output, not crop encode, is the highest-confidence source
  of the remaining YOLO submission tail.

## Metric Interpretation

Important YOLO CSV fields:

- `acquisition_to_worker_start_ms`: time from frame acquisition to YOLO worker
  service start. High values usually mean queueing, worker wakeup, CPU
  scheduling, or multi-camera service skew before YOLO work begins.
- `yolo_queue_wait_ms`: time from YOLO enqueue to worker pop. In the current
  good runs this is near zero, so the queue itself is not the bottleneck.
- `worker_start_to_yolo_input_ready_ms`: host wall time from YOLO worker start
  until the YOLO input-ready event is recorded. This is the best current field
  for measuring the source wait plus preprocess-launch segment.
- `cpu_preprocess_ms`: host-side time around issuing YOLO preprocess work. This
  is not the GPU kernel duration. It includes CUDA runtime/driver delay before
  the kernel is submitted.
- `cpu_infer_call_ms`: host-side time to enqueue inference or graph work.
- `cpu_event_record_ms`: host-side time to record the YOLO completion event.
- `sync_ms`: old CPU-visible completion wait timing. With newer detach/event
  paths, this may be `-1` when not used the old way.
- `total_ms`: YOLO worker wall time for the sampled processing path.

Important Nsight distinction:

- `mono_to_yolo_optimized` duration measures GPU kernel execution.
- `cudaLaunchKernel_v7000` duration measures host API call wall time.
- Long host API calls with short kernels mean the CPU is blocked before or
  while submitting work. That is driver/runtime contention, not kernel cost.

## Implemented Diagnostic And Experimental Flags

Stable-enough diagnostic flags:

- `ORANGE_YOLO_PERF_LOG=1`: write `Cam<serial>_yolo_perf.csv`.
- `ORANGE_YOLO_PERF_SAMPLE=1`: sample every YOLO perf row.
- `ORANGE_CROP_COPY_TIMING=0`: keep crop copy timing overhead disabled unless
  specifically testing it.
- `ORANGE_CROP_STAGE_SOURCE=1`: use the current crop source staging mode.
- `ORANGE_ANALYTICS_EARLY_OWNED_FRAME=1`: use the hybrid owned-buffer path for
  delayed analytics consumers.
- `ORANGE_YOLO_AFFINITY_CAM_<serial>=<cpu>`: pin YOLO workers per camera.
- `ORANGE_CROP_PREVIEW_DISABLE=1`: disable only crop live-preview CUDA work.
- `ORANGE_DISPLAY_PREVIEW_MAX_FPS=1`: throttle full-frame GUI display for
  isolation.
- `ORANGE_GUI_RECORDING_SINK_MODE=preprocess_only`: diagnostic sink that keeps
  full-frame preprocess routing active but removes full-frame encode/output.

Still experimental:

- `ORANGE_YOLO_READY_EVENT_FASTPATH=1`: skip `cudaStreamWaitEvent` when
  `cudaEventQuery` proves the source event is already complete.
- `ORANGE_RECORDING_DETECT_PRIORITY=1`: enqueue/gate source recording around
  the detect path.
- `ORANGE_YOLO_DETACH_INPUT=1`: record YOLO-input-ready after preprocess so
  source lifetime can be shorter than full detection lifetime.
- `ORANGE_INLINE_CROP_PRODUCER=1`: not a clean two-camera win. Keep off by
  default.
- `ORANGE_YOLO_RT_PRIORITY=<n>`: not currently useful for this issue.
- `ORANGE_YOLO_STREAM_PRIORITY=high` and `ORANGE_YOLO_STREAM_NONBLOCKING=1`:
  regressed fairness in the measured workload. Do not use as the main path.

## Early Suggestions And Current Status

Early suggestion: add explicit fairness instrumentation.

- Status: done.
- Result: queue depth and queue wait are usually not the remaining issue in the
  best current runs. Fairness instrumentation was still useful because it
  caught the inline-crop regression.

Early suggestion: test one-camera inline crop to distinguish per-camera cost
from two-camera fairness.

- Status: still useful, but lower priority after the full-frame encode
  isolation result.
- Reason: inline crop is not currently the strongest culprit. It may be worth
  revisiting after recording interference is controlled.

Early suggestion: keep inline crop experimental.

- Status: still correct.
- Reason: the first two-camera inline run regressed `Cam2010096`
  `acquisition_to_worker_start` badly.

Early suggestion: focus on hot-path orchestration, not crop staging.

- Status: partially refined.
- Current interpretation: crop staging is not the main detect bottleneck, but
  full-frame recording-side CUDA/NVENC orchestration is a major detect
  interference source.

Early suggestion: move workers from poll/sleep to event-driven wakeups.

- Status: implemented in `CThreadWorker`.
- Result: helped but did not uniformly solve both cameras because the dominant
  remaining stall is inside CUDA/NVENC driver calls, not only worker wakeup.

Early suggestion: use Nsight-compatible instrumentation around the ambiguous
  YOLO source wait and preprocess/input-ready window.

- Status: implemented with NVTX ranges and `run_yolo_detach_nsys.sh`.
- Result: high signal. Nsight isolated the problem to host-side CUDA launch
  delay and then to full-frame encode/output interference.

Early suggestion: try a one-camera comparison.

- Status: still available as a sanity check.
- Current priority: lower than full-frame encode isolation because the
  two-camera preprocess-only test already shows both cameras collapse to about
  `4.4 ms` detect p95 when full-frame encode/output is removed.

## A16 And NUMA Notes

The A16 detail matters because the workload spans multiple logical GPUs and
potentially multiple PCIe/NUMA locality domains.

Relevant implications:

- A16 cards expose multiple GPUs. Two cameras may not be equivalent just
  because the software pipeline is symmetric.
- Camera source GPU, helper GPU, CPU core affinity, PCIe root complex, and GPU
  pair placement can all change host-side CUDA/NVENC behavior.
- Earlier GPU-pair/affinity swaps suggested the slow tail does not simply
  follow a YOLO CPU core.
- `Cam2010096` being consistently slower can come from source/helper placement,
  route asymmetry, camera content/detection differences, or driver contention
  timing. It is not explained by the YOLO preprocess GPU kernel.

Practical rule:

- Treat camera/GPU placement as part of the experiment matrix, but do not
  expect placement alone to solve shared CUDA/NVENC driver contention inside
  one process.

## What Remains To Be Done

### Highest-signal next isolation

Keep real full-frame recording enabled and isolate which full-frame recording
substage creates the YOLO launch tail:

- full-frame NVENC bitstream lock/output,
- full-frame encode submit,
- source/primary preprocess,
- helper peer-copy/preprocess,
- output copy or mux/write behavior.

The `preprocess_only` result strongly points at full-frame encode/output, but
it intentionally removed the final full-frame video artifact. The next test
should retain the full-frame artifact while reducing or relocating the
suspected encode/output pressure.

### Encoder/output instrumentation now available

The full-frame recording path now exposes targeted encoder/output timing in the
recording snapshot latency block:

- `encoder_cuda_set_device`
- `preprocess_complete_stream_wait_enqueue`
- `source_to_helper_copy_sync_wait`
- `source_to_helper_copy_elapsed_query`
- `source_to_helper_copy`
- `pre_encoder_reference_capture_enqueue`
- `nvenc_get_next_input_frame`
- `nvenc_copy_to_input`
- `nvenc_encode_frame_total`
- `nvenc_map_input_resource`
- `nvenc_map_reference_resource`
- `nvenc_encode_picture`
- `nvenc_completion_wait`
- `nvenc_lock_bitstream`
- `nvenc_bitstream_copy`
- `nvenc_unlock_bitstream`
- `nvenc_unmap_input_resource`
- `nvenc_unmap_reference_resource`
- `encoder_output_accounting`
- `shared_submit_total`
- `shared_submit_lock_wait`
- `shared_gop_buffering`
- `writer_push_packet_total`
- `writer_packet_alloc_copy`
- `writer_queue_push`
- existing `writer_queue_wait`, `packet_mux_write`, and
  `gop_release_to_last_write`

The NVTX build also labels the same phases in Nsight:

- full-frame encode/output ranges include camera serial, source GPU, encode
  GPU, source/helper route, recording frame, and GOP index,
- encoder-worker subranges cover `cudaSetDevice`, preprocess-ready stream-wait
  enqueue, source-to-helper copy synchronization, source-to-helper copy elapsed
  query, pre-encoder reference capture enqueue, and next-input-frame lookup,
- `NvEncoder` subranges cover map, encode submit, completion wait, bitstream
  lock, host bitstream copy, unlock, and unmap,
- `SharedRecordingOutput` ranges cover shared submit and GOP flush/push,
- `FFmpegWriter` ranges cover packet enqueue and `av_interleaved_write_frame`.

The Nsight wrapper has a heavy mode for one short correlation run:

```bash
cd /home/jeremy/orange-gop-split-a16
ORANGE_NSYS_HEAVY=1 ./run_yolo_detach_nsys.sh
```

Heavy mode enables CPU sampling plus CUDA and OSRT backtraces. It should be
used sparingly because it adds overhead, but it is the right mode for
correlating a long YOLO `cudaLaunchKernel` with overlapping
`pthread_rwlock`/`futex`/NVENC behavior.

### Encoder/output isolation result

Run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_19_34_59`
- `/tmp/orange_yolo_detach_nsys_20260424_193425.sqlite`

Command shape:

- normal `./run_yolo_detach_nsys.sh`
- not heavy Nsight mode
- real full-frame recording sink

Run health:

- full-frame `Cam<serial>.mp4` artifacts were present,
- crop MP4 artifacts were present,
- both cameras held about `100 fps`,
- no camera drops,
- no preprocess drops,
- no helper fallback,
- split-GOP remained `hybrid_split`.

YOLO latency remained in the bad/full-frame regime:

- `Cam2010095`:
  - `cpu_preprocess_ms p95 = 8.321 ms`
  - `capture_to_detect_done_ms p95 = 12.654 ms`
- `Cam2010096`:
  - `cpu_preprocess_ms p95 = 8.392 ms`
  - `capture_to_detect_done_ms p95 = 12.587 ms`

The new full-frame encoder/output instrumentation identified the blocking
boundary:

- `NVENC lock bitstream p95 = 11.755 ms`, max `13.115 ms`
- `NVENC bitstream host copy p95 = 0.021 ms`
- `FFmpeg av_interleaved_write_frame p95 = 0.334 ms`
- `SharedRecordingOutput submit frame p95 = 0.373 ms`

Snapshot aggregates agreed:

- `nvenc_lock_bitstream` mean was about `4.5-4.6 ms` per camera, with max
  about `13 ms`.
- `nvenc_encode_picture` mean was only about `0.17-0.21 ms` per camera.
- `nvenc_bitstream_copy`, unlock, unmap, writer enqueue, and mux/write were
  much smaller than the lock.

Route split:

- source route full-frame encode/output:
  - `p95 = 3.345 ms` for `Cam2010095`
  - `p95 = 3.601 ms` for `Cam2010096`
- helper route full-frame encode/output:
  - `p95 = 12.645 ms` for `Cam2010095`
  - `p95 = 12.784 ms` for `Cam2010096`

Therefore the strongest current finding is:

```text
The dominant full-frame recording stall is helper-route nvEncLockBitstream.
Encode submit, bitstream host copy, shared-output buffering, and FFmpeg mux
are not the dominant stall.
```

The light Nsight trace also captured enough OSRT callchain detail that heavy
mode is not required yet:

- YOLO CUDA runtime:
  - `cudaLaunchKernel_v7000 p95 = 8.256 ms`
  - `pthread_rwlock_rdlock p95 = 8.145 ms`
  - callchain: `pthread_rwlock_rdlock -> libcuda -> libcudart ->
    cudaLaunchKernel -> launch_optimized_yolo_preprocess -> YoloWorker`
- full-frame encoder:
  - `poll -> libnvcuvid/libnvidia-encode -> nvEncLockBitstream ->
    NvEncoder::GetEncodedPacket`

This confirms the practical driver-contention model:

```text
The helper encoder thread blocks in Linux synchronous NVENC output harvest.
The YOLO worker sometimes blocks in libcuda while submitting a tiny preprocess
kernel.
The tiny kernel itself is not the latency problem.
```

### NVENC output-depth diagnostic result

Run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_20_13_27`
- `/tmp/orange_yolo_detach_nsys_20260424_201258.sqlite`

Command shape:

- normal `./run_yolo_detach_nsys.sh`,
- real full-frame recording sink,
- `ORANGE_NVENC_EXTRA_OUTPUT_DELAY=7`,
- effective full-frame encoder buffer count `8`.

Run health:

- full-frame and crop MP4 artifacts were present,
- both cameras held about `100 fps`,
- zero camera drops,
- zero preprocess drops,
- zero helper fallback.

Main comparison against the depth-control run
`/home/jeremy/orange_data/exp/unsorted/2026_04_24_19_34_59`:

- `nvEncLockBitstream` mean improved from about `4.5-4.6 ms` to about
  `0.25-0.26 ms`.
- NVTX `NVENC EncodeFrame p95` improved from about `11.878 ms` to about
  `4.448 ms`.
- YOLO did not improve:
  - `Cam2010095 acquisition_to_detect_done_ms p95` was
    `12.654 -> 12.747 ms`,
  - `Cam2010096 acquisition_to_detect_done_ms p95` was
    `12.588 -> 12.992 ms`,
  - YOLO-thread `cudaLaunchKernel_v7000 p95` was
    `8.257 -> 8.552 ms`,
  - YOLO-thread `pthread_rwlock_rdlock p95` was
    `8.145 -> 8.361 ms`.
- Helper-route full-frame encode/output became burstier:
  - `Cam2010095 helper route p95` was `12.646 -> 19.713 ms`,
  - `Cam2010096 helper route p95` was `12.788 -> 20.503 ms`.

Interpretation:

- extra output depth proves Linux NVENC can be pipelined enough to avoid
  blocking most `nvEncLockBitstream` calls,
- but it does not remove the YOLO CUDA driver lock tail,
- therefore GOP-sized or larger NVENC buffering is not the highest-signal
  production direction by itself,
- the next useful measurement is the newly added helper-route substage timing
  around CUDA device selection, preprocess-ready wait enqueue,
  source-to-helper copy synchronization, copy elapsed-time query,
  pre-encoder reference capture enqueue, next-input-frame lookup, total
  `EncodeFrame`, and encoder output accounting.

### Helper-route substage instrumentation result

Run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_20_37_09`
- `/tmp/orange_yolo_detach_nsys_20260424_203636.sqlite`

Command shape:

- normal-depth full-frame recording,
- real full-frame recording sink,
- crop preview disabled,
- display preview throttled to `1 fps`,
- newly added helper-route substage instrumentation enabled by the build.

Run health:

- full-frame and crop MP4 artifacts were present,
- both cameras held about `100 fps`,
- zero camera drops,
- zero preprocess drops,
- zero helper fallback,
- encoder slow-frame count was lower than prior full-GUI/control runs.

Main result:

- Source-route full-frame encode/output was not the p95 bottleneck:
  - `Cam2010095 source route p95 = 3.377 ms`,
  - `Cam2010096 source route p95 = 3.277 ms`.
- Helper-route full-frame encode/output remained the p95 bottleneck:
  - `Cam2010095 helper route p95 = 11.942 ms`,
  - `Cam2010096 helper route p95 = 11.979 ms`.
- Nested helper-route NVTX showed:
  - `nvEncLockBitstream p95 = 11.593 ms` for `Cam2010095`,
  - `nvEncLockBitstream p95 = 11.650 ms` for `Cam2010096`,
  - source-to-helper copy sync wait p95 was about `5.0-5.2 ms`,
  - preprocess-ready stream-wait enqueue was about `0.011 ms p95`,
  - source-to-helper copy elapsed-time query was about `0.012-0.014 ms p95`,
  - next-input-frame lookup was about `0.001 ms p95`,
  - NVENC input copy p95 was about `0.060 ms` on helper routes,
  - `nvEncEncodePicture p95` was about `0.199-0.226 ms` on helper routes,
  - shared output submit p95 was about `0.132-0.259 ms`.

YOLO result:

- YOLO launch tails remained:
  - YOLO-thread `cudaLaunchKernel_v7000 p95 = 8.241 ms`,
  - YOLO-thread `pthread_rwlock_rdlock p95 = 8.271 ms`.
- `Cam2010096 acquisition_to_detect_done_ms p95` improved slightly versus the
  prior control run (`12.588 -> 12.338 ms`).
- `Cam2010095 acquisition_to_detect_done_ms p95` worsened versus the prior
  control run (`12.654 -> 14.567 ms`) because its
  `acquisition_to_worker_start_ms` / `yolo_queue_wait_ms` tail grew in this
  shorter run, while worker-start-to-input-ready remained about `8.4 ms p95`.

Interpretation:

- display/crop preview work is not the dominant YOLO p95 cause,
- helper-route copy readiness contributes a secondary wait, but not the main
  helper-route p95 stall,
- helper-route `nvEncLockBitstream` is still the primary in-process recording
  stall at normal depth,
- since depth `8` hid much of the lock wait without improving YOLO p95, the
  problem is likely broader same-process CUDA/NVENC driver contention rather
  than only the blocking location of `nvEncLockBitstream`.

### Split-harvest experiment result

Baseline run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_23_32_32`
- `/tmp/orange_yolo_detach_nsys_20260424_233157.sqlite`

Split-harvest run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_23_39_55`
- `/tmp/orange_yolo_detach_nsys_20260424_233912.sqlite`
- `ORANGE_NVENC_SPLIT_HARVEST=1`
- `recording_snapshot.json` confirmed `path = hw_split_harvest` and
  `split_harvest_enabled = true`.

Mechanical result:

- `NVENC EncodeFrame p95` on the original submit+harvest path was about
  `11.85 ms`.
- `NVENC SubmitFrameOnly p95` on the split path was about `0.31 ms`.
- Full-frame recording remained healthy at about `100 fps`.
- There were no camera drops, no preprocess drops, no encoder failures, and no
  helper fallback.

Negative result:

- `nvEncLockBitstream p95` stayed about `11.67 -> 11.69 ms`, now on the harvest
  thread.
- YOLO `cudaLaunchKernel_v7000 p95` stayed about `8.29 -> 8.29 ms`.
- YOLO `pthread_rwlock_rdlock p95` stayed about `8.44 -> 8.45 ms`.
- `Cam2010095 cpu_preprocess_ms p95` stayed about `7.53 -> 7.60 ms`.
- `Cam2010096 cpu_preprocess_ms p95` stayed about `8.63 -> 8.64 ms`.
- The actual `mono_to_yolo_optimized` GPU kernel stayed about `0.073 ms p95`.

Interpretation:

- Split harvest fixed the encoder worker submit-path design.
- It did not fix the cross-subsystem contention visible to YOLO.
- The remaining issue is likely same-process CUDA/NVENC runtime or driver-lock
  contention, because moving the blocking harvest to another in-process thread
  did not reduce YOLO's libcuda `pthread_rwlock_rdlock` tail.
- Further in-process submit/harvest tweaks are now lower signal than a process
  isolation experiment.

Follow-up diagnostic:

- `ORANGE_NVENC_HARVEST_DELAY_US=<microseconds>` intentionally delays the
  split-harvest thread before it tries to retrieve bitstreams with
  `nvEncLockBitstream`.
- Use it only with `ORANGE_NVENC_SPLIT_HARVEST=1`.
- This tests whether a small YOLO launch window, for example `500`, `1000`, or
  `2000 us`, reduces YOLO `cudaLaunchKernel_v7000` /
  `cpu_preprocess_ms` p95.
- A positive result would mean same-process phasing can help; a negative result
  would further support process isolation or a broader driver/hardware
  contention explanation.

Measured `1000 us` harvest-delay result:

- run:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_25_00_18_44`
- Nsight SQLite:
  `/tmp/orange_yolo_detach_nsys_20260425_001810.sqlite`
- `recording_snapshot.json` confirmed `path = hw_split_harvest`,
  `split_harvest_enabled = true`, and `nvenc_harvest_delay_us = 1000`.
- Nsight confirmed the delay actually happened:
  `NVENC split harvest delay p95 = 1.105 ms`.
- YOLO did not improve:
  - `cudaLaunchKernel_v7000 p95` changed from about `8.287 ms` without delay
    to about `8.396 ms` with the delay,
  - `pthread_rwlock_rdlock p95` changed from about `8.448 ms` to about
    `8.444 ms`,
  - `Cam2010095 cpu_preprocess_ms p95` changed from about `7.601 ms` to
    about `7.593 ms`,
  - `Cam2010096 cpu_preprocess_ms p95` changed from about `8.639 ms` to
    about `8.629 ms`.
- NVENC harvest p95 moved slightly:
  - `nvEncLockBitstream p95` changed from about `11.689 ms` to about
    `10.774 ms`,
  - `NVENC SubmitFrameOnly p95` changed from about `0.308 ms` to about
    `0.281 ms`.
- The run was otherwise healthy: about `100 fps`, no camera drops, no
  preprocess drops, no encoder failures, and no crop-sidecar drops.

Interpretation:

- The harvest-delay phase window was real and was visible in Nsight.
- It changed NVENC timing slightly, but did not reduce YOLO's host-side
  CUDA/runtime lock tail.
- This is another negative result for in-process phasing as the main
  architecture fix.
- A final `2000 us` point could be used as a sanity check, but the higher-signal
  next experiment remains process isolation for full-frame
  encode/harvest/output.

### External NVENC process discriminator

Artifacts:

- baseline analytics-only:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_preprocessonly_a16_gpu5_run2`
- same-GPU external solid-frame NVENC:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_preprocessonly_a16_gpu5_external_nvenc_20260425_151858`
- same-GPU external host-noise NVENC:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_preprocessonly_a16_gpu5_external_nvenc_20260425_152234`

Command shape:

- Process A: headless real-YOLO analytics on camera `2010096`, A16 GPU `5`,
  full-frame recording disabled.
- Process B: separate synthetic NVENC process on the same GPU at
  `4512x4512 @ 60 fps`.
- Tested both `solid` and `host-noise` synthetic input patterns.

Result:

- analytics-only baseline:
  - `acquisition_to_detect_done_ms p95 = 3.4894 ms`
  - `total_ms p95 = 3.4606 ms`
  - `cpu_preprocess_ms p95 = 0.0165 ms`
  - `sync_ms p95 = 3.3545 ms`
- external solid-frame same-GPU NVENC:
  - `acquisition_to_detect_done_ms p95 = 4.4383 ms`
  - `cpu_preprocess_ms p95 = 0.0137 ms`
  - external `nvEncLockBitstream p95 = 0.0030 ms`
- external host-noise same-GPU NVENC:
  - `acquisition_to_detect_done_ms p95 = 4.4115 ms`
  - `cpu_preprocess_ms p95 = 0.0154 ms`
  - external `nvEncLockBitstream p95 = 0.0031 ms`

Interpretation:

- Separate-process same-GPU NVENC added about `0.9 ms` of shared-GPU work to
  YOLO completion time.
- It did not recreate the same-process GUI/NVENC `cpu_preprocess_ms` tail of
  about `8 ms`.
- This is evidence that process isolation is still plausible as a QoS
  boundary, but the synthetic external process did not reproduce the long
  Linux NVENC harvest waits seen in the real GUI recording path.
- The next process-isolation discriminator needs either real camera-frame
  detach/copy into the recorder process or a synthetic source that actually
  blocks in `nvEncLockBitstream` at production-like depth.

### Encoder rate-control and AQ result

Artifacts:

- broad short matrix:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_encoder_settings_a16_gpu5`
- focused VBR AQ/temporal-AQ A/B:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_vbr_aq_ab_a16_gpu5`

Command shape:

- camera `2010096`, A16 GPU `5`,
- headless real YOLO,
- real full-frame split-GOP recording,
- `4512x4512 @ 100 fps`,
- HEVC `p1`, `ll`, GOP `25`, about `150 Mbps`,
- focused run length `15 s` plus `2 s` warmup.

Focused VBR result:

```text
VBR AQ on  + temporalAQ on : detect p95 10.487 ms, total p95 10.086 ms, pre_sync p95 6.891 ms
VBR AQ on  + temporalAQ off: detect p95  4.312 ms, total p95  4.130 ms, pre_sync p95 3.623 ms
VBR AQ off + temporalAQ on : detect p95  4.141 ms, total p95  4.110 ms, pre_sync p95 3.632 ms
VBR AQ off + temporalAQ off: detect p95  3.820 ms, total p95  3.717 ms, pre_sync p95 0.346 ms
```

Run health:

- all focused runs sustained about `100 fps`,
- no encoder failures,
- no camera frame-id gaps,
- output bitrate remained about `150 Mbps`.

Interpretation:

- In the headless real-YOLO plus real split-GOP path, the strongest signal is
  not rate-control mode by itself.
- The strongest signal is generic NVENC AQ/temporal-AQ.
- Disabling both AQ features moved the focused VBR run from a `10.487 ms`
  detect p95 tail to `3.820 ms`, while preserving throughput and output
  bitrate.
- The broad short matrix was noisier, including duplicate/equivalent rows with
  different p95 tails, but it pointed in the same direction.
- This does not prove the two-camera GUI tail is solved, because the focused
  run was single-camera headless. It is still the highest-signal production
  config change to validate before spending more time on process isolation.

Design consequence:

- `recording.encode.aq` and `recording.encode.temporal_aq` should be
  first-class camera config fields, not hidden environment flags.
- Schema 4 promotes those fields as tri-state `auto|off|on`.
- The current low-latency candidate for the A16 split-GOP 100 fps configs is
  `aq = off` and `temporal_aq = off`.

## Recommended Next Plan

1. Validate schema-4 AQ-off recording configs in the real two-camera GUI path.

Reason:

- this is a persistent encoder config change, not an env-only diagnostic,
- the focused headless VBR A/B moved `acquisition_to_detect_done_ms p95` from
  `10.487 ms` to `3.820 ms`,
- throughput and output bitrate remained healthy,
- it preserves the required split-GOP full-frame recording architecture.

Command shape:

```bash
cd /home/jeremy/orange-gop-split-a16
sudo env DISPLAY="$DISPLAY" XAUTHORITY="$XAUTHORITY" \
  ORANGE_YOLO_PERF_LOG=1 \
  ORANGE_YOLO_PERF_SAMPLE=1 \
  ORANGE_CROP_COPY_TIMING=0 \
  ORANGE_CROP_STAGE_SOURCE=1 \
  ORANGE_ANALYTICS_EARLY_OWNED_FRAME=1 \
  ORANGE_YOLO_AFFINITY_CAM_2010095=2 \
  ORANGE_YOLO_AFFINITY_CAM_2010096=4 \
  ORANGE_YOLO_READY_EVENT_FASTPATH=1 \
  ./targets/release/orange
```

Expected GUI setup:

- use schema-4 `100_cam4` or equivalent split-GOP configs,
- recording panel should show `spatial AQ = off` and `temporal AQ = off`,
- full-frame split-GOP recording remains enabled.

Success criteria:

- both cameras sustain about `100 fps`,
- full-frame MP4s are present and valid,
- zero camera drops,
- zero preprocess drops,
- zero crop-sidecar drops,
- `capture_to_detect_done p95` and `cpu_preprocess_ms p95` move materially
  toward the headless AQ-off result.

2. If the two-camera GUI tail remains, prioritize process isolation for
full-frame encode/output.

Reason:

- split harvest reduced encoder submit p95 from about `11.85 ms` to about
  `0.31 ms`,
- split-harvest plus a `1000 us` harvest delay did not reduce YOLO
  `cudaLaunchKernel_v7000`, `pthread_rwlock_rdlock`, or `cpu_preprocess_ms`
  p95,
- synthetic external same-GPU NVENC did not recreate the same-process
  `cpu_preprocess_ms` tail,
- if AQ-off does not fix the real GUI path, the remaining issue is probably a
  same-process CUDA/NVENC runtime contention boundary or a real-recording
  resource path that the synthetic external load did not exercise.

Smallest useful process-boundary experiment:

```text
Process A:
  acquisition + YOLO/TensorRT latency-critical path

Process B:
  full-frame split-GOP encode/harvest/output path
```

Compare:

- YOLO p95 with no full-frame encode,
- YOLO p95 with same-process encode,
- YOLO p95 with separate-process encode,
- YOLO p95 with separate-process encode on another GPU where possible.

Interpretation:

- separate process helps: process-level CUDA/NVENC runtime lock coupling is a
  major factor,
- separate process does not help: contention is mostly below the process
  boundary, such as kernel driver, GPU/NVENC hardware, PCIe/NUMA fabric, or CPU
  scheduling,
- separate GPU helps: shared GPU or local GPU fabric pressure matters,
- separate GPU does not help: global driver/runtime/scheduling remains the
  likely cause.

Success criteria:

- full-frame and crop artifacts remain present,
- both cameras sustain about `100 fps`,
- zero camera drops,
- zero preprocess drops,
- zero helper fallback,
- YOLO `cudaLaunchKernel_v7000`, `pthread_rwlock_rdlock`, and
  `cpu_preprocess_ms` p95 drop materially.

Stop criteria:

- artifact correctness regresses,
- throughput drops or queues grow without bound,
- YOLO p95 remains around the current `8 ms` driver-lock tail after the full
  encode/output process boundary is introduced.

3. Keep split harvest as an experimental architecture checkpoint, not a proven
latency fix.

Use it when we need to:

- keep encoder submit from blocking on bitstream harvest,
- isolate encoder-worker accounting from harvest latency,
- compare in-process versus out-of-process harvest behavior.

4. Investigate YOLO launch exposure reduction in parallel or next.

This is the strongest non-multiprocess alternative if split harvest is not
enough:

- CUDA graph capture for YOLO preprocess plus inference where possible,
- TensorRT plugin or integrated input preprocessing path,
- fewer per-frame host CUDA calls on the latency-critical thread.

Acceptance rule:

- YOLO `cudaLaunchKernel_v7000` / `cpu_preprocess_ms` p95 moves materially
  toward the `preprocess_only` control while full-frame split-GOP recording
  stays enabled.

5. Keep the explicit full-frame encoder/output instrumentation.

Measure these with per-camera thread labels and NVTX:

- full-frame `nvEncEncodePicture`,
- full-frame `nvEncLockBitstream`,
- bitstream copy/output timing,
- writer/mux timing,
- source/primary route versus helper route,
- CPU thread id and camera serial for the output threads.

6. Use the `preprocess_only` run as the control.

Any candidate architecture should be compared against:

- full encode with display throttled,
- preprocess-only,
- no-full-frame immediate recycle.

7. Do not optimize crop or display further for detect p95 until full-frame
encode/output is addressed.

Crop preview disable remains useful for crop/pose latency. Display throttle is
useful as a diagnostic. Neither is the main remaining detect bottleneck.

## Current Command Recipes

Baseline full-frame recording with YOLO detach/Nsight wrapper:

```bash
cd /home/jeremy/orange-gop-split-a16
./run_yolo_detach_nsys.sh
```

Crop preview disabled:

```bash
cd /home/jeremy/orange-gop-split-a16
ORANGE_CROP_PREVIEW_DISABLE=1 ./run_yolo_detach_nsys.sh
```

Crop preview disabled and display throttled:

```bash
cd /home/jeremy/orange-gop-split-a16
ORANGE_CROP_PREVIEW_DISABLE=1 ORANGE_DISPLAY_PREVIEW_MAX_FPS=1 ./run_yolo_detach_nsys.sh
```

Preprocess-only diagnostic:

```bash
cd /home/jeremy/orange-gop-split-a16
ORANGE_GUI_RECORDING_SINK_MODE=preprocess_only ORANGE_CROP_PREVIEW_DISABLE=1 ORANGE_DISPLAY_PREVIEW_MAX_FPS=1 ./run_yolo_detach_nsys.sh
```

Split-harvest diagnostic:

```bash
cd /home/jeremy/orange-gop-split-a16
ORANGE_CROP_PREVIEW_DISABLE=1 ORANGE_DISPLAY_PREVIEW_MAX_FPS=1 ORANGE_NVENC_SPLIT_HARVEST=1 ./run_yolo_detach_nsys.sh
```

Important caveat:

- `preprocess_only` is not a production recording mode for this workload
  because it intentionally omits main full-frame MP4 output.
- It is the current best control for separating full-frame preprocess routing
  from full-frame encode/output.

## Acceptance Criteria For Future Work

Every proposed change should be evaluated against:

- two-camera GUI `100 fps`,
- real full-frame split-GOP recording when the test is not explicitly a
  diagnostic sink,
- crop recording active when relevant,
- no camera drops,
- no preprocess drops,
- no pose queue drops,
- no crop-sidecar drops,
- `capture_to_detect_done p95`,
- `worker_start_to_yolo_input_ready_ms p95`,
- `cpu_preprocess_ms p95`,
- `worker_start_to_detect_done_ms p95`,
- Nsight host CUDA API attribution for YOLO worker threads,
- full-frame MP4 presence and validity when the run claims to test production
  recording.
