# Detect Hot Path Rework Plan

Date: 2026-04-24
Scope: reduce `capture_to_detect_done` and `capture_to_pose_done` latency under
real two-camera `100 fps` load by simplifying the latency-critical camera-local
execution path.

Current evidence log:

- See [detect_latency_findings_2026_04_24.md](/home/jeremy/orange-gop-split-a16/docs/detect_latency_findings_2026_04_24.md)
  for the current experiment chronology, metric interpretation, and next
  architecture options.

## Current Status Update - 2026-04-24 Evening

The strongest current result is the `preprocess_only` diagnostic:

- full-frame preprocess routing stayed active,
- full-frame encode/output was removed,
- crop MP4s and diagnostics were still produced,
- main full-frame `Cam<serial>.mp4` files were intentionally absent,
- YOLO `cpu_preprocess_ms p95` collapsed from about `8 ms` to about
  `0.26 ms`,
- `capture_to_detect_done p95` collapsed to about `4.4 ms` on both cameras.

Nsight showed the actual YOLO preprocess kernel still takes only about
`0.07 ms p95`; the long tail was host-side CUDA launch/driver delay. The best
current explanation is in-process CUDA/NVENC driver contention from the
full-frame recording encode/output path, especially full-frame
`HW_Encoder_Cam_ nvEncLockBitstream`.

Implications:

- Crop-side staging and crop preview are no longer the main detect bottleneck.
- Full-frame OpenGL display is not the main remaining detect bottleneck.
- Realtime YOLO thread scheduling did not solve the driver-lock wait.
- Stream priority/nonblocking regressed fairness and should not be the next
  mainline direction.
- The next architecture work should isolate full-frame recording encode/output
  from the YOLO submission path while preserving the split-GOP full-frame MP4
  artifact.
- The fix must not assume one GPU or one NVENC session can carry the full
  `4512x4512 Mono8 @ 100 fps` recording stream. Multi-GPU split-GOP recording
  is a production constraint, not an optional optimization.

## Problem Statement

The current crop/pose ownership work improved the post-detect path, but the
remaining latency is still dominated by detect-side orchestration rather than
model compute.

Earlier evidence before the full-frame encode/output isolation pass:

- Two-camera hybrid-owned-buffer runs still show:
  - `capture_to_detect_done p95` around `11-12 ms`,
  - `detect_to_crop_ready p95` around `6-7 ms`,
  - `capture_to_pose_done p95` around `13-16 ms`.
- YOLO inference itself is much smaller:
  - `infer_ms p95` about `3.18 ms`,
  - `pre_ms p95` about `0.35 ms`,
  - CPU postprocess/tracking/IPC are comparatively small.
- In-app instrumentation showed the ingress event was already ready before
  `cudaStreamWaitEvent(...)` on about `99%` of frames.
- Nsight Systems CLI/SQLite analysis showed that on the YOLO threads:
  - `cudaStreamWaitEvent` was only a few microseconds,
  - `cudaEventSynchronize` was only a few microseconds,
  - the largest scheduler-inflated CUDA API outliers were actually
    `cudaEventRecord` calls,
  - rare YOLO-thread deschedule gaps reached about `10.6 ms`.

Conclusion:

- The current latency tail is no longer mainly about GPUDirect readiness or
  crop ownership.
- The remaining detect-side tail is mostly host scheduling/runtime jitter and
  cross-thread CUDA orchestration overhead.

## Why Affinity Alone Is Not Enough

Pinning YOLO threads to dedicated CPUs is still useful, but it is a mitigation,
not the main architectural fix.

Observed behavior:

- Per-camera YOLO affinity produced only mixed gains.
- One camera improved modestly, the other was flat or regressed depending on
  the run.
- This is consistent with the current architecture:
  - only YOLO was pinned,
  - display / crop / preprocess / encode / helper threads still ran freely,
  - the hot path still crossed multiple worker threads and multiple CUDA API
    call sites.

Therefore:

- CPU affinity should remain available as an experiment and deployment lever.
- The durable win is to reduce the number of thread boundaries and CUDA control
  points on the detect-side hot path.

## Why Two Cameras Still Interfere

The two camera pipelines are logically separate, but they are not resource
independent.

What is actually shared:

- the same process,
- the same OS scheduler and CPU package,
- the same CUDA runtime/driver inside one process,
- shared host-side services such as logging, allocation, queueing, and helper
  threads.

So "one camera should not affect the other" is a good design goal, but it is
not automatically true just because each camera has its own worker objects.

Practical rule:

- data paths can be per-camera,
- execution environment is still shared.

This matters because a hot-path refactor can improve one camera while hurting
the other if it increases per-frame CPU residency or host-side CUDA API
pressure on a shared execution environment.

## Current Hot Path

Current camera-local ordering:

```text
Acquisition thread
  -> choose direct / ring-copy / hybrid-owned-buffer path
  -> record ingress-ready event
  -> queue WORKER_ENTRY to YoloWorker

YoloWorker thread
  -> wait on ingress-ready event
  -> set NPP stream
  -> preprocess full frame to 640x640 input
  -> enqueue TRT inference
  -> record yolo_completion_event
  -> poll for completion on host
  -> CPU postprocess / ROI selection
  -> queue WORKER_ENTRY to CropProducerWorker

CropProducerWorker thread
  -> wait on analytics-ready/source event
  -> ROI copy to owned crop
  -> record crop_ready_event
  -> fan out to PoseWorker + crop-video sidecar

PoseWorker thread
  -> wait on crop_ready_event
  -> (noop today; TRT later)
```

Resolved and remaining hot-path issues in code:

- `CThreadWorker` queue wakeups are now event-driven with condition variables:
  - `PutObjectToQueueIn()` blocks on `queueInNotFullCv` instead of
    busy-waiting in [threadworker.h](/home/jeremy/orange-gop-split-a16/src/threadworker.h:159)
  - `WaitForObjectFromQueueIn()` blocks on `queueInNotEmptyCv` instead of
    empty-queue polling in [threadworker.h](/home/jeremy/orange-gop-split-a16/src/threadworker.h:269)
- YOLO can still do host-side completion polling with `cudaStreamQuery` and
  `usleep(100)` when event-sync wait is not enabled in
  [yolo_worker.cpp](/home/jeremy/orange-gop-split-a16/src/yolo_worker.cpp:1303).
- The latency-critical path still crosses multiple worker threads:
  - acquisition event record in
    [acquire_frames.cpp](/home/jeremy/orange-gop-split-a16/src/acquire_frames.cpp:1566)
  - YOLO-to-crop handoff in
    [yolo_worker.cpp](/home/jeremy/orange-gop-split-a16/src/yolo_worker.cpp:1423)
  - crop producer event wait / ROI copy in
    [crop_producer.cpp](/home/jeremy/orange-gop-split-a16/src/crop_producer.cpp:475)
- The current highest-confidence remaining bottleneck is outside that worker
  wakeup layer: full-frame recording encode/output appears to inflate YOLO
  host-side CUDA submission latency through shared CUDA/NVENC driver/runtime
  contention.

## Design Goals

- Reduce wall-clock jitter on `capture_to_detect_done`.
- Keep detect on the ingress lease by default.
- Keep crop/pose/preview/encode on owned downstream payloads.
- Preserve correctness of source-frame reuse and crop-frame recycling.
- Avoid regressing two-camera `100 fps` throughput and drop-free operation.
- Maintain best-effort sidecars: preview, crop encode, debug, IPC.

## Non-Goals

- Do not replace the validated split-GOP full-frame recording architecture
  blindly. Current evidence says full-frame encode/output isolation is now the
  highest-signal area, but changes must preserve `100 fps` full-frame MP4
  output and zero-drop behavior.
- Do not "solve" detect latency by falling back to a single-GPU full-frame
  encoder path. The current resolution/framerate requires split-GOP style
  multi-GPU recording.
- Do not remove correctness-critical CUDA events blindly.
- Do not jump directly to a full `500 fps` architecture rewrite.
- Do not require GPU-resident postprocess/ROI selection in the first rework
  slice.

## Design Principles

### 1. One Camera-Local Submission Owner For The Hot Path

The detect-side hot path should have one execution owner per camera for the
latency-critical work:

```text
acquire -> detect -> crop -> pose
```

This does not require a single CUDA stream, but it should minimize the number
of CPU threads issuing CUDA API calls during that path.

Implication:

- the same worker/thread should own as much of the detect-to-crop path as
  practical,
- cross-thread CUDA handoffs should be reserved for best-effort sidecars.

### 2. Sidecars Must Not Participate In Hot-Path Readiness

Preview, crop-video encode, IPC mirrors, and similar outputs should consume
already-owned payloads after the hot path has progressed.

Implication:

- no sidecar should gate detect completion,
- no sidecar should force additional ingress-lease coordination,
- slow sidecars should drop their own work.

### 3. Event-Driven Wakeups, Not Polling

The current poll-and-sleep worker model is likely amplifying scheduler jitter.

Implication:

- latency-critical workers should block on a condition variable, semaphore, or
  equivalent wakeup primitive instead of `usleep` polling,
- queue-full handling should not busy-wait on the producer thread.

### 4. Reduce CPU-Visible Sync In The Detect Path

The host should only wait when results are actually needed, and the wait
mechanism should not be an explicit polling loop.

Implication:

- replace `cudaStreamQuery + usleep(100)` with a cleaner completion wait for
  the current CPU-postprocess model,
- keep GPU sequencing on streams/events where possible,
- measure wall-clock effects after each change.

## Recommended Architecture

### Near-Term Target

Keep the current hybrid ownership model, but simplify the thread model:

```text
Acquisition
  -> event-driven detect hot-path worker
       -> wait on ingress-ready event
       -> preprocess
       -> TRT infer
       -> CPU postprocess / ROI selection
       -> inline crop production
       -> optional pose launch or pose handoff
  -> sidecars consume owned outputs later
       -> crop-video encode
       -> preview
       -> debug / IPC mirrors
```

Important point:

- The hot-path worker can still use separate CUDA streams internally.
- The main simplification is CPU-side ownership and wakeup behavior, not
  forcing everything onto one stream.

### Long-Term Target

If future measurements still require more reduction:

- move ROI selection and pose handoff closer to fully GPU-resident flow,
- treat `detect -> crop -> pose` as a graph-friendly island,
- keep recording and preview outside that island.

## Proposed Phases

### Phase 1: Replace Poll-Based Worker Wakeups

Replace poll/sleep worker behavior for the latency-critical path:

- `YoloWorker`
- `CropProducerWorker`
- `PoseWorker`

with an event-driven queue/wakeup mechanism.

Expected benefit:

- less scheduler churn,
- less wall-clock inflation around short CUDA API calls,
- cleaner queue-full semantics than busy-waiting.

Implementation notes:

- Keep existing queue sizing and drop policy.
- Only change wakeup behavior and queue blocking behavior first.
- Preserve current instrumentation so before/after comparisons stay valid.

### Phase 2: Collapse YOLO -> CropProducer Thread Boundary

Move crop production into the same camera-local hot-path worker that finishes
YOLO CPU-visible detection work.

Expected benefit:

- remove one worker queue boundary,
- reduce one source of scheduler jitter,
- reduce latency between ROI selection and crop submission.

Candidate model:

- `YoloWorker` becomes `AnalyticsHotPathWorker`, or
- `YoloWorker` directly owns a `CropProducer` instance and performs crop
  production inline after ROI selection.

Sidecars remain downstream consumers of the resulting owned crop payload.

### Phase 2 Status Note

The first experimental inline-crop version landed behind
`ORANGE_INLINE_CROP_PRODUCER=1` and did remove the `YoloWorker ->
CropProducerWorker` queue hop, but the first two-camera result was not a clean
win.

Observed behavior in the first inline test:

- `Cam2010095` improved slightly on `capture_to_detect_done` and
  `detect_to_crop_ready`.
- `Cam2010096` regressed materially, especially on
  `acquisition_to_worker_start`, which jumped to about `9.3 ms p95`.
- ingress readiness stayed effectively perfect, so the regression was not a
  source-ready problem.

Current interpretation:

- the first inline version likely made the YOLO-side hot path heavy enough to
  create multi-camera service skew,
- the regression appears to be fairness / host-scheduling pressure before YOLO
  really starts working on the frame, not a pure inference slowdown.

Implication for the plan:

- keep inline crop experimental only for now,
- do not make it the default Phase 2 implementation yet,
- treat multi-camera fairness as an explicit acceptance criterion for any
  future hot-path collapse.

### YOLO Ingress Event Fast-Path Status Note

Follow-up baseline runs with fairness instrumentation showed:

- YOLO enqueue/start queue depth stayed at p95 `0` for both cameras,
- `yolo_queue_wait_ms` stayed below `0.1 ms` p95,
- ingress events were already complete before the YOLO worker checked them on
  nearly every frame,
- the remaining p95 tail was inside YOLO worker service, especially
  `cpu_wait_event_ms` / `cpu_pre_sync_ms` on `Cam2010096`.

An experimental fast path now lives behind
`ORANGE_YOLO_READY_EVENT_FASTPATH=1`:

- run `cudaEventQuery` on the acquisition ingress event,
- skip `cudaStreamWaitEvent` when the event is already complete,
- preserve the existing stream wait when the event is not ready,
- log `cpu_ingress_event_query_ms` and `cpu_stream_wait_event_ms` separately.

### Full-Frame Recording Isolation Test

GPU-pair and CPU-affinity swaps showed:

- the slow p95 tail does not simply follow the YOLO CPU core,
- `Cam2010096` remains slower even after moving from `GPU5+6` to `GPU7+8`,
- `Cam2010095` worsens when moved onto `GPU5+6`, so that pair still appears
  somewhat more pressured,
- primary/source-GPU recording GOP phases are slower than helper phases for
  both cameras.

Current interpretation:

- the remaining `cpu_preprocess_ms` / `cpu_pre_sync_ms` tail is likely a mix of
  camera/input-specific behavior and source-GPU recording pressure,
- the highest-signal next isolation is to keep a GUI recording session and
  diagnostics active, but remove main full-frame video work.

Experimental GUI flag:

- `ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME=1`

Behavior:

- maps the GUI recording pipeline to the existing `immediate_recycle` recording
  sink,
- keeps `recording_frame_id`, recording folder creation, YOLO perf CSVs,
  pose/crop artifacts, and pipeline metrics,
- avoids full-frame preprocess/encode workers and does not write main
  `Cam<serial>.mp4` files,
- still routes a lightweight recording consumer so acquisition-side accounting
  remains comparable.

Lower-level override:

- `ORANGE_GUI_RECORDING_SINK_MODE=<real|immediate_recycle|preprocess_only|threaded_handoff_only>`

For this diagnostic, prefer `ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME=1` rather than
`pure_offload`; `pure_offload` is still not the validated GUI split-GOP path.

### Detect-Priority Split-GOP Experiment

The no-full-frame diagnostic showed the old `10-12 ms` detect p95 tail largely
disappears when main full-frame video work is removed, but split-GOP full-frame
recording is required for the real `100 fps` workload.

Experimental flag:

- `ORANGE_RECORDING_DETECT_PRIORITY=1`

Behavior:

- keeps the existing source/helper split-GOP recording architecture,
- enqueues YOLO before full-frame recording when both consumers are active,
- gates only source/primary recording preprocess work until YOLO detection has
  completed for that same frame,
- leaves helper GOP routes unchanged so full-frame throughput should still use
  the validated split-GOP lanes.

Follow-up future-architecture flag:

- `ORANGE_YOLO_DETACH_INPUT=1`

Behavior:

- records a YOLO-input-ready event after YOLO has copied/preprocessed the
  source frame into its owned TensorRT input buffer,
- lets camera-buffer requeue wait on YOLO-input-ready instead of full YOLO
  completion when the analytics hybrid path is active,
- with `ORANGE_RECORDING_DETECT_PRIORITY=1`, lets source/primary recording
  wait for YOLO-input-ready rather than full detection completion.

This is the first step toward making the detect path own only the small model
input tensor, not the full-frame source lease.

Crop live-preview isolation flag:

- `ORANGE_CROP_PREVIEW_DISABLE=1`

Behavior:

- keeps the GUI session, full-frame split-GOP recording, YOLO, pose, crop
  production, and crop MP4 encoding active,
- disables only the crop live-preview CUDA path in `CropAndEncodeWorker`,
  including the RGBA preview kernel, cross-GPU preview staging copy, and preview
  stream synchronization,
- leaves `Cam<serial>_crop.mp4`, crop metadata, crop sidecar perf, YOLO perf,
  and full-frame `Cam<serial>.mp4` artifacts enabled.

Use this after Nsight shows YOLO launch stalls overlapping crop/display CUDA
runtime lock pressure. A useful result reduces
`cpu_preprocess_ms` / `worker_start_to_yolo_input_ready_ms` p95 without changing
recording throughput or introducing drops.

Instrumentation:

- `Cam<serial>_pipeline_perf.csv` now includes
  `detect_priority_gated_frames`, `detect_priority_waited_frames`,
  `detect_priority_wait_timeouts`, `detect_priority_wait_total_ns`, and
  `detect_priority_wait_max_ns`.
- `Cam<serial>_yolo_perf.csv` includes
  `acquisition_to_yolo_input_ready_ms`,
  `worker_start_to_yolo_input_ready_ms`, and
  `cpu_input_ready_event_record_ms`.

Acceptance rule:

- a useful result reduces `capture_to_detect_done` / `cpu_pre_sync_ms` p95
  while keeping `100 fps`, zero camera drops, zero preprocess drops, and zero
  detect-priority wait timeouts.

### Full-Frame Encode/Output Contention Status Note

Follow-up Nsight runs after detect-priority, YOLO input detach, crop preview
disable, and display throttle showed:

- `YOLO source event wait` stayed tiny.
- The actual `mono_to_yolo_optimized` preprocess kernel stayed about
  `0.07 ms p95`.
- In real full-frame encode runs, YOLO `cudaLaunchKernel_v7000` grew to about
  `8 ms p95`.
- With `ORANGE_GUI_RECORDING_SINK_MODE=preprocess_only`, YOLO
  `cudaLaunchKernel_v7000` dropped to about `0.24-0.25 ms p95`.
- In the full encode run, full-frame `HW_Encoder_Cam_ nvEncLockBitstream`
  blocked around `11.5 ms p95` on the blocking encoder/output threads.
- In the preprocess-only run, those full-frame HW encoder threads were absent,
  while crop encode remained lightweight.

Current interpretation:

- the remaining detect p95 tail is primarily host-side CUDA/NVENC
  submission/driver contention caused by full-frame recording encode/output,
  not GPU preprocess compute,
- separate streams are not enough to prevent this because the threads still
  share the same process, CUDA runtime/driver, context/resource state, and
  NVENC interop paths,
- `preprocess_only` is a diagnostic control, not a production mode, because it
  intentionally omits main full-frame MP4 output.

NVENC Linux SDK status:

- NVIDIA Video Codec SDK 13.0 still documents asynchronous NVENC encode/output
  completion as Windows 10+ WDDM only.
- Linux and Jetson Linux are documented as synchronous-mode-only for NVENC
  output completion.
- `NV_ENC_LOCK_BITSTREAM::doNotWait = 1` can avoid blocking by returning
  `NV_ENC_ERR_LOCK_BUSY`, but that requires a polling/retry and pending-output
  state machine. It is not the same as true async completion events.
- Therefore the best next work should not assume a newer Linux SDK will make
  full-frame encode/output naturally nonblocking. The practical choices are
  better full-frame encoder/output scheduling, helper-only/pure-offload routing
  tests, or process isolation if in-process CUDA/NVENC driver contention
  remains dominant.

Next useful architecture tests:

- use the new full-frame encoder/output timing and NVTX ranges to isolate
  `cudaSetDevice`, preprocess-ready stream-wait enqueue, source-to-helper copy
  synchronization, source-to-helper copy elapsed-query overhead, pre-encoder
  reference capture enqueue, `nvEncEncodePicture`, `nvEncLockBitstream`,
  output copy, shared-output buffering, and writer/mux timing,
- latest light Nsight run identified helper-route `nvEncLockBitstream` as the
  dominant full-frame recording stall, with encode submit, bitstream copy,
  shared-output buffering, and mux/write much smaller,
- depth `8` (`ORANGE_NVENC_EXTRA_OUTPUT_DELAY=7`) reduced average
  `nvEncLockBitstream` time but did not improve YOLO p95, so do not chase
  GOP-sized NVENC buffers as the main production fix,
- normal-depth helper-route substage instrumentation showed helper-route
  `nvEncLockBitstream` remains about `11.6 ms p95`, while source-route
  `nvEncLockBitstream`, input copy, encode submit, writer/mux, and
  shared-output submission are not the p95 bottleneck,
- source-to-helper copy readiness is a secondary wait at about `5 ms p95`,
  but the main helper-route p95 stall is still Linux synchronous NVENC harvest,
- reserve `ORANGE_NSYS_HEAVY=1 ./run_yolo_detach_nsys.sh` for cases where the
  light trace no longer exposes the driver/runtime wait callchain,
- after depth testing, split encode submit from bitstream harvest if needed,
- consider process isolation for full-frame recording if in-process
  CUDA/NVENC driver contention remains the dominant effect.

### Phase 3: Revisit Pose Placement

Once real pose TRT exists, decide whether pose should:

- remain a separate worker consuming `CropFrame`, or
- be launched inline by the camera-local hot-path worker and only publish
  results outward.

Decision rule:

- if pose queueing remains tiny and latency is acceptable, keep the worker
  split,
- if `detect -> pose_done` still needs reduction, move pose launch closer to
  the hot path.

### Phase 4: Longer-Term GPU-Resident ROI / Graph Work

Only after Phases 1-3 are measured:

- consider GPU-resident detect result selection,
- consider `detect_only` and `detect_plus_pose` graph islands,
- avoid forcing the current CPU postprocess design into a partial graph path
  prematurely.

## Implementation Checklist

### Phase 1 Checklist

- [x] Introduce an event-driven queue primitive for latency-critical workers.
- [x] Remove `usleep(interval)` empty-queue polling from the hot-path workers.
- [x] Remove busy-wait `PutObjectToQueueIn()` behavior from hot-path producers.
- [ ] Preserve explicit best-effort queue-full behavior for pose and crop
      sidecars.
- [x] Add YOLO ingress-event query/wait split and experimental ready-event
      fast path.
- [x] Add GUI diagnostic no-full-frame recording sink for isolating detect
      latency from full-frame video preprocess/encode pressure.
- [x] Add full-frame encoder/output timing and NVTX around NVENC map, encode,
      bitstream lock/copy/unlock, unmap, shared-output buffering, and FFmpeg
      enqueue/write.
- [x] Re-run the two-camera `100 fps` benchmark and compare:
      - `capture_to_detect_done`
      - `worker_start_to_detect_done`
      - `cpu_wait_event_ms`
      - `cpu_ingress_event_query_ms`
      - `cpu_stream_wait_event_ms`
      - `capture_to_pose_done`

### Phase 2 Checklist

- [ ] Inline crop production after YOLO postprocess/ROI selection.
- [ ] Remove the YOLO -> CropProducer worker queue from the hot path.
- [ ] Keep crop-video encode and preview as downstream consumers only.
- [ ] Verify source-frame release and crop-frame lease accounting still remain
      exactly once.
- [x] Add explicit multi-camera fairness instrumentation:
      - per-camera YOLO queue depth,
      - oldest-frame age before worker start,
      - per-camera hot-path service skew.
- [ ] Re-run one-camera and two-camera comparisons.

### Phase 3 Checklist

- [ ] Add real pose TRT.
- [ ] Measure whether pose worker queueing is still negligible.
- [ ] Decide whether pose stays as a worker or moves inline.

## Validation Plan

Primary benchmark:

- two-camera GUI run
- `100 fps`
- detect + record + crop + pose enabled
- hybrid-owned-buffer path enabled

Required outputs:

- `Cam*_yolo_perf.csv`
- `Cam*_pose_perf.csv`
- `Cam*_crop_perf.csv`
- `Cam*_crop_sidecar_perf.csv`
- pipeline perf CSVs

Key success metrics:

- reduce `capture_to_detect_done p95`
- reduce `worker_start_to_detect_done p95`
- reduce scheduler-inflated wall time around detect-side CUDA API regions
- keep:
  - `0` camera drops,
  - `0` pose queue drops,
  - `0` crop-sidecar queue drops under the benchmarked workload

## Risks

- Replacing the worker wakeup model can surface lifecycle/shutdown bugs if stop
  signaling is not designed carefully.
- Collapsing YOLO and crop production can accidentally re-couple hot-path work
  with sidecar work if ownership boundaries are not kept explicit.
- A smaller number of threads can reduce jitter but also reduce accidental
  overlap if the streams and queueing model are not preserved correctly.

## Recommendation

Start with Phase 1.

Reason:

- It directly targets the strongest architectural smell in the current hot
  path: poll-based worker orchestration.
- It is smaller and safer than immediately merging YOLO and crop into one
  worker.
- It keeps the current ownership model and instrumentation intact, so the
  before/after comparison will be much clearer.

If Phase 1 does not materially improve `capture_to_detect_done`, proceed to
Phase 2 and collapse the YOLO -> crop thread boundary.
