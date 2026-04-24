# Detect Hot Path Rework Plan

Date: 2026-04-24
Scope: reduce `capture_to_detect_done` and `capture_to_pose_done` latency under
real two-camera `100 fps` load by simplifying the latency-critical camera-local
execution path.

## Problem Statement

The current crop/pose ownership work improved the post-detect path, but the
remaining latency is still dominated by detect-side orchestration rather than
model compute.

Current evidence:

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

Current hot-path issues in code:

- `CThreadWorker` is poll-based:
  - `PutObjectToQueueIn()` busy-waits with `usleep(1000)` when the queue is
    full in [threadworker.h](/home/jeremy/orange-gop-split-a16/src/threadworker.h:143)
  - `ThreadRunning()` sleeps with `usleep(interval)` when the queue is empty in
    [threadworker.h](/home/jeremy/orange-gop-split-a16/src/threadworker.h:267)
  - the default `interval` is `10` microseconds in
    [threadworker.h](/home/jeremy/orange-gop-split-a16/src/threadworker.h:91)
- YOLO still does host-side completion polling with `cudaStreamQuery` and
  `usleep(100)` in
  [yolo_worker.cpp](/home/jeremy/orange-gop-split-a16/src/yolo_worker.cpp:835)
- The latency-critical path still crosses at least three worker threads:
  - acquisition event record in
    [acquire_frames.cpp](/home/jeremy/orange-gop-split-a16/src/acquire_frames.cpp:1495)
  - YOLO-to-crop handoff in
    [yolo_worker.cpp](/home/jeremy/orange-gop-split-a16/src/yolo_worker.cpp:937)
  - crop producer event wait / ROI copy in
    [crop_producer.cpp](/home/jeremy/orange-gop-split-a16/src/crop_producer.cpp:465)

## Design Goals

- Reduce wall-clock jitter on `capture_to_detect_done`.
- Keep detect on the ingress lease by default.
- Keep crop/pose/preview/encode on owned downstream payloads.
- Preserve correctness of source-frame reuse and crop-frame recycling.
- Avoid regressing two-camera `100 fps` throughput and drop-free operation.
- Maintain best-effort sidecars: preview, crop encode, debug, IPC.

## Non-Goals

- Do not redesign the full-frame recording path yet.
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

- [ ] Introduce an event-driven queue primitive for latency-critical workers.
- [ ] Remove `usleep(interval)` empty-queue polling from the hot-path workers.
- [ ] Remove busy-wait `PutObjectToQueueIn()` behavior from hot-path producers.
- [ ] Preserve explicit best-effort queue-full behavior for pose and crop
      sidecars.
- [ ] Re-run the two-camera `100 fps` benchmark and compare:
      - `capture_to_detect_done`
      - `worker_start_to_detect_done`
      - `cpu_wait_event_ms`
      - `capture_to_pose_done`

### Phase 2 Checklist

- [ ] Inline crop production after YOLO postprocess/ROI selection.
- [ ] Remove the YOLO -> CropProducer worker queue from the hot path.
- [ ] Keep crop-video encode and preview as downstream consumers only.
- [ ] Verify source-frame release and crop-frame lease accounting still remain
      exactly once.
- [ ] Add explicit multi-camera fairness instrumentation:
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
