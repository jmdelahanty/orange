# Plan: Second-Stage Pose TensorRT on Cropped ROI

Date: 2026-01-27
Author: Codex (outline)

## Goal
Add a second-stage pose model (TensorRT) that consumes a high-resolution GPU
crop and produces real-time pose results without breaking current display or
recording throughput.

## Goal Update (2026-04-22)

The intended path is pose inference on the high-resolution GPU crop before any
crop-video encoding. The encoded crop video is an optional recording artifact,
not the input to pose.

Implications:

- Crop generation should become a reusable producer stage.
- `CropAndEncodeWorker` should not own the only crop-generation path.
- Pose should consume a GPU crop payload before NVENC/NV12 conversion.
- Crop recording should consume the same crop payload, or a derived resized copy,
  after pose-compatible crop production.
- If the pose model wants a larger crop than the current default `256x256`,
  the pipeline should generate that larger crop first and downsample/convert
  only for crop-video encoding when needed.

Latency goal update (2026-04-23):

- Pose should happen as close as possible to the detection result that selected
  the ROI.
- The desired ordering is:
  - run detection on a reduced-resolution view of the frame,
  - resolve the ROI immediately,
  - produce the high-resolution crop once,
  - launch pose from that crop before lower-priority consumers such as preview
    or crop-video encode add avoidable delay.
- This means the long-term pose path should be treated as the primary low-
  latency continuation of detection, not merely as a background sidecar
  consumer of the crop pipeline.
- The current noop `PoseWorker` validates ownership and queueing, but it does
  not yet prove the final low-latency ordering.

High-frame-rate note (2026-04-23):

- A future `~2.8 MP Mono8 @ 500 fps` workflow is not automatically more
  bandwidth-heavy than the current `4512x4512 Mono8 @ 100 fps` workflow.
- Roughly:
  - current `20 MP @ 100 fps` is about `2.04 GB/s` per camera,
  - future `2.8 MP @ 500 fps` is about `1.4 GB/s` per camera.
- The harder part at `500 fps` is the frame budget, not just the byte budget:
  - `100 fps` leaves about `10 ms/frame`,
  - `500 fps` leaves about `2 ms/frame`.
- Therefore the pose architecture should optimize first for low per-frame host
  overhead, explicit ownership boundaries, and bounded best-effort fanout,
  rather than assuming raw bandwidth will be the dominant constraint.

## Current Assumptions (2026-02-13)
- Pose runs on every frame that produces a crop (i.e., when YOLO fires and produces detections).
- Output should be drawn on the overlay and published over IPC.
- Use single-frame payloads (no batching) for now.
- Target is to keep up with 60 FPS recordings.
- Note: YOLO does not necessarily run every frame (decimation is possible), so pose rate may be less than 60 FPS if YOLO is decimated.

## Constraints / Existing Architecture
- `CropAndEncodeWorker` already generates a configurable square crop on GPU.
- `CThreadWorker` stages rely on per-frame CUDA events for readiness.
- Most stages are asynchronous and do not pass items to output queues.
- `CropAndEncodeWorker` currently uses a single `d_cropped_rgba_` buffer.
- Detection is intentionally run on a downsampled representation of the full
  frame because `4512x4512` full-resolution detect is too slow to be practical.
- Therefore the pose path is inherently hybrid:
  - detect on reduced-resolution full-frame input,
  - pose on a high-resolution crop extracted from the original source frame.

## Proposed Architecture
Add a new worker stage:
- `PoseWorker` (new): consumes `PoseEntry` items containing a cropped GPU buffer + a readiness event.
- A small pool of `PoseEntry` buffers to avoid overwriting data in flight.
- Events to synchronize crop -> pose without CPU blocking.

High-level flow:
```
Acquisition -> YOLO -> CropProducer -> PoseWorker -> (IPC/UI/log)
                                |--> preview (PBO)
                                |--> crop encode (NVENC)
```

The crop producer owns ROI selection, source-to-crop geometry, GPU crop
production, buffer pooling, and readiness events. Preview, crop encoding, and
pose inference are consumers. A slow consumer should drop its own work rather
than backpressure acquisition or full-frame recording.

## Why Split Crop Before Pose

The current GUI crop path is useful as a validated transitional implementation,
but it should not be extended directly into pose. In the current combined path,
one worker owns crop selection, preview copy, crop-video encode submission,
metadata writes, synchronization, and source-frame release. The measured crop
worker tail latency can exceed one `100 fps` frame period even when there are no
drops. That tail latency is a warning about ownership coupling, not necessarily
about crop-kernel cost.

Pose should be added after crop production is separated from crop consumers:

```text
YOLO result
  -> CropProducer
      -> bounded CropFrame buffer
      -> crop_ready_event
      -> release original source frame
  -> consumers:
      -> preview
      -> crop video encoder
      -> pose TensorRT
```

This is the important source-frame lifetime boundary. The original camera frame
should only be held until the ROI has been copied into a crop-owned GPU buffer.
After that, preview, encoding, and pose can run independently. If a downstream
consumer is slow, it should drop or skip crop work rather than delaying original
camera-buffer reuse.

Implementation checklist before pose:

- Current first split slice: `CropAndEncodeWorker` snapshots frame/detection
      metadata, copies detected ROIs into a bounded crop-owned Mono8 GPU buffer,
      records a crop-ready event on a dedicated producer CUDA stream, and
      releases the source `WORKER_ENTRY` after that source-to-crop copy
      completes. Preview and crop video encoding now consume this internal crop
      payload from a downstream consumer worker instead of sharing the same
      worker hop as crop production.
- [x] Define the first internal `CropFrame` payload with frame, timestamp,
      geometry, detection, GPU pointer, and CUDA event fields needed by preview
      and crop recording.
- [x] Add a bounded crop buffer/event pool so crop payloads are not overwritten
      while internal consumers are still using them.
- [x] Extract `CropProducer` from the current crop-video worker.
- [x] Make crop-video encoding consume the internal `CropFrame`.
- [ ] Make GUI crop preview an independent `CropFrame` consumer with
      drop/rate-limit policy. It currently consumes the internal crop payload
      inside the same worker.
- [x] Add consumer lease/ref-count handling so a crop buffer returns to the pool
      only after all accepted consumers release it.
- [x] Add producer and per-consumer aggregate counters.
- [x] Add a noop `PoseWorker` consumer that waits on `crop_ready_event`,
      exercises the shared crop queue/lease path, and releases its crop lease
      without running TensorRT yet.
- [x] Split first-pass perf logging into producer timing and consumer timing
      with `crop_pool_wait_ms`, `crop_producer_cpu_ms`,
      `crop_preview_cpu_ms`, and `encode_submit_cpu_ms`.
- [ ] Revalidate GUI `100 fps` YOLO + full-frame record + crop record before
      adding pose.

Current noop pose slice (updated 2026-05-04):

- `PoseWorker` is now a real bounded worker-stage with its own CUDA stream.
- `CropProducer` can offer a `CropFrame` lease to that worker without creating
  a second crop copy.
- `CropAndEncodeWorker` retains one consumer lease for preview / crop-video
  encode and releases it independently from the noop pose worker.
- Shutdown ordering keeps the noop pose worker alive until crop production is
  fully drained so queued crop leases can return safely.
- `PoseWorker` now emits `Cam<serial>_pose_events.jsonl` rows for accepted
  recording crop frames. The rows capture frame identity, crop/detection
  geometry, backend/model metadata, and pose-stage latency.
- This validates the ownership model for future pose TensorRT work, but it does
  not yet implement model loading, tensor conversion, keypoint output, IPC
  output, or GUI pose overlays. Current event rows intentionally record
  `pose.backend = "noop"`, `pose.status = "no_result"`, and an empty `poses`
  array.

Current latency interpretation (2026-04-23):

- The new `Cam<serial>_pose_perf.csv` aggregate summaries show that the noop
  pose queue is not the main problem:
  - `queue_high_water` stayed at `1`,
  - `queue_full_drops` stayed at `0`,
  - `crop_ready_to_pose_start` is tiny relative to the rest of the path.
- The current tail is still upstream, mainly in `detect_to_crop_ready`, which
  includes queue residence in the current combined crop worker plus the staged
  detach / crop-producer host-side handoff.
- Therefore the next architectural optimization target is not "make noop pose
  faster"; it is to shorten and isolate the `detect -> crop_ready` path.

Headless pose wiring slice (updated 2026-05-09):

- Headless experiment specs now parse `fixed.pose_worker`.
- Supported schema modes are `off`, `noop`, and `real`.
- `noop` now wires the existing `CropProducerWorker -> CropProducer ->
  PoseWorker` path into headless runs when real YOLO is enabled. This is the
  same crop/lease/worker machinery used by the GUI path, not a separate
  headless pose implementation.
- `noop` validates `Cam<serial>_pose_events.jsonl` as a no-result pose
  artifact with `pose.backend = "noop"`, `pose.mode = "noop"`, and empty
  `poses`.
- `real` requires `engine_path` in the spec and now loads a TensorRT keypoint
  engine through the same `PoseWorker`. The first backend supports FP32/NCHW
  `1x3x256x256` input and FP32 `1x(5+3K)xN` YOLO-pose output.
- `fixed.pose_worker.prewarm_iterations` warms the pose engine before live
  frames. `fixed.pose_worker.crop_frame_pool_size` can raise the crop-frame
  pool for real pose tests where inference spikes briefly hold more crop
  buffers than the historical 8-slot default.
- The headless schema currently requires `fixed.yolo_worker.mode = "real"`.
  `roi_source = "yolo_top_detection"` is the production-relevant ROI source.
- `roi_source = "synthetic_center_box"` can inject a centered runtime detection
  for pose-plumbing diagnostics only. It must never be treated as validation of
  production YOLO detections, detection quality, real ROI selection, or real
  detection-to-pose behavior.
- Pose JSONL rows are emitted only for accepted recording crop frames, so a run
  with no YOLO detections can still fail pose validation with zero/missing pose
  rows. Use a detectable subject before treating noop pose validation as a full
  end-to-end pass.
- First headless wiring smoke on 2026-05-09:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_pose_noop_wiring_smoke_20260509_151301`.
  The shared workers started and shut down cleanly, `Cam2010096_pose_perf.csv`
  was written, and YOLO logged `401` zero-detection rows. No crop frames were
  produced (`pose_offered = 0`), so `Cam2010096_pose_events.jsonl` was absent
  and the run failed only the pose-event validation gate. This is the expected
  no-detection outcome, not evidence of missing headless wiring.
- First synthetic-center-box pose plumbing smoke on 2026-05-09:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_pose_noop_synthetic_center_box_a16_gpu5`.
  The run produced `401` YOLO event rows, `401` noop pose event rows, and
  passed the artifact validators. The YOLO rows are explicitly marked
  `synthetic_runtime_detection = true` and
  `production_detection_valid = false`, so this validates only the
  crop/pose/artifact plumbing.
- First real TensorRT pose plumbing smoke on 2026-05-09:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_pose_real_synthetic_center_box_pool32_a16_gpu5`.
  The pose engine loaded on GPU `5`, the run produced `401` YOLO event rows
  and `401` TensorRT pose event rows, and validation passed with `0` pose
  failures, `0` camera frame-id gaps, `0` encode failures, and `0` crop-pool
  misses. Pose inference p50 was about `0.894 ms` and p95 about `14.328 ms`.
  All pose rows were `no_result` because the ROI was synthetic/no-fish; this
  validates engine plumbing and artifact shape, not real keypoint quality.

Real TensorRT pose p95 interpretation (updated 2026-05-10):

- The high pose p95 in the real-recording smoke is not primarily a warmup
  issue. `prewarm_iterations = 3` was active before live frames.
- The slow path appears as occasional long `pose_start_to_pose_done` service
  rows, followed by multiple frames with high `crop_ready_to_pose_start` queue
  residence behind the single pose worker. In the real-recording smoke:
  `pose_start_to_pose_done p50 = 0.894 ms`, `p95 = 14.328 ms`,
  `max = 87.023 ms`; `crop_ready_to_pose_start p50 = 0.017 ms`,
  `p95 = 65.371 ms`, `max = 95.767 ms`; pose queue high-water was `11`.
- A matching diagnostic run with full encode/output removed used
  `recording_sink_mode = "preprocess_only"`:
  `/tmp/orange_pose_trt_compare/2010096_pose_real_pool32_preprocess_only_20260510`.
  It still produced `401` YOLO rows and `401` TensorRT pose rows with no
  crop-pool misses, but `pose_start_to_pose_done p95` dropped to
  `0.898 ms`, `crop_ready_to_pose_start p95` dropped to `0.114 ms`, and pose
  queue high-water dropped to `1`.
- Interpretation: the TensorRT pose model is fast once warmed. The real
  p95/p99 tail is same-process full-frame recording/CUDA/NVENC contention plus
  queue amplification in the current synchronous, one-frame-at-a-time pose
  worker. Process-isolated recording/external recorder routing is the more
  important production fix than more pose warmup.

External IPC pose discriminator (updated 2026-05-10):

- The same real TensorRT pose synthetic-center-box spec was run through the
  one-camera full-rate external split-GOP path:

```bash
scripts/run_external_recorder_smoke.sh \
  --spec experiment_specs/2010096_headless_real_yolo_pose_real_synthetic_center_box_pool32_a16_gpu5.json \
  --duration 3 \
  --warmup 1 \
  --encode-fps 100 \
  --encode-max-fps 0 \
  --queue-depth 32 \
  --shard-gpu-ids 5,6 \
  --output-dir /tmp \
  --skip-video-sanity
```

- Analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_pose_real_synthetic_center_box_pool32_a16_gpu5_2010096_20260509_201621`.
- Recorder artifact:
  `/tmp/orange_external_recorder_2010096_20260509_201621`.
- The run produced `401` YOLO event rows and `401` TensorRT pose event rows
  with `0` pose failures, `0` camera frame-id gaps, `0` preprocess drops, and
  `0` crop-pool misses.
- Pose stayed close to the `preprocess_only` control even while full-rate
  external split-GOP encode ran: pose queue high-water was `1`,
  `pose_start_to_pose_done p50 = 0.992 ms`, `p95 = 1.794 ms`,
  `max = 2.798 ms`; `crop_ready_to_pose_start p95 = 0.0205 ms`;
  `capture_to_pose_done p95 = 6.273 ms`.
- The external recorder received/ACKed/encoded `401/401` frames with `0`
  skips/drops. Detach copy p95 was `0.154 ms`; external encode/lock p95 was
  about `11.96 ms`, but that tail stayed in the recorder process and did not
  back up pose.
- A standalone MP4 sanity check passed after the smoke:
  `401` frames, mean luma about `225`, max stddev about `75.7`, and black
  fraction about `0.000002`.
- Interpretation: external IPC/process isolation preserves the warmed pose
  timing profile while full-frame recording continues in another process. This
  is the stronger production direction for pose plus recording than trying to
  tune pose warmup further inside the in-process recorder.

Sync and async pose design note (updated 2026-05-10):

- Treat synchronization as either correctness synchronization or convenience
  synchronization. Correctness syncs protect buffer lifetimes, output ordering,
  failure visibility, and artifact determinism. Convenience syncs make the code
  easier to reason about but force the CPU to wait for GPU work on every frame.
- The current real TensorRT pose worker uses the simple correctness-first
  shape: enqueue crop preprocess, enqueue TensorRT inference, enqueue
  device-to-host output copy, then wait before decoding/writing the result.
  This is acceptable as the baseline because the external IPC run kept
  `pose_start_to_pose_done_ms p95 = 1.794` and
  `capture_to_pose_done_ms p95 = 6.273`.
- Keep sync-like pressure while the pose contract is still stabilizing when it
  directly protects resource lifetime, ordered JSONL/CSV artifacts, clear
  per-frame failure reporting, and reproducible validation.
- Remove or decimate sync-like pressure from the hot path when it is only for
  immediate observability. Examples are per-frame CUDA stream synchronization
  solely to decode pose output immediately, and per-frame camera control-plane
  reads such as `GevTimestampValueHigh/Low` when embedded frame timestamps are
  already available. The PTP register-read decimation work is the model for
  this: keep full polling as a diagnostic mode, but do not make it the default
  hot-path cost.
- The future high-FPS pose shape should use a bounded pool of
  `PoseInferenceSlot` records containing device input, device output, pinned
  host output, a CUDA event, and frame metadata. The submit path should enqueue
  preprocess, TensorRT, device-to-host copy, and event recording, then advance
  without waiting on that frame. A result collector should poll completed
  events, decode host outputs, write JSONL/CSV rows in recording-frame order
  when needed, and surface late CUDA/TensorRT failures with frame ids.
- Backpressure must be explicit in that future shape. At the pool bound, the
  system should intentionally choose whether to drop pose for a frame, skip the
  frame before pose submission, or block. It should not accidentally block
  every frame because result decoding is coupled to submission.

Current recommendation: keep the synchronous real-pose backend as the
correctness baseline while external IPC and artifact contracts are still being
hardened. Revisit async pose collection when targeting much higher rates such
as `500 fps`, adding multi-camera pose, or making pose results part of a fast
tracking/control loop.

Shaman v2 IPC update: `PoseWorker` now publishes pose latest-state updates
through the shared `FrameIPCManager` when `/shm_cam_<serial>_v2` is enabled.
The worker converts crop-local keypoints back into source-frame camera pixels
before publishing. Late older-frame pose results remain in
`Cam<serial>_pose_events.jsonl` but are suppressed from the live v2 queue by
the same monotonic stale-update rule used for YOLO.

## Data Structures
### New: CropFrame / PoseEntry (pool item)
- `unsigned char* d_crop` in the crop producer's canonical GPU layout
- Optional derived tensor buffer for pose if model layout differs
- `cudaEvent_t* crop_ready_event`
- `uint64_t frame_id / recording_frame_id`
- `uint64_t timestamp / timestamp_sys`
- camera id/serial
- source frame dimensions
- crop rectangle in source-frame coordinates
- detection rectangle/confidence
- transform from crop-local coordinates back to full-frame coordinates

### New: PoseResults (output)
- Minimal output struct (pose keypoints + score)
- Stored in IPC, shared queue, or directly into a results ring buffer

## Worker Integration
### CropAndEncodeWorker changes
- Maintain a pool of crop buffers/events (size ~4-8 to start).
- When a detection exists:
  - Acquire a free crop frame from the pool.
  - Write the high-resolution crop into the crop frame using a crop stream.
  - Record `crop_ready_event` on that stream.
  - Offer the crop frame to pose, preview, and crop encode consumers according
    to enabled config.
- If no crop frame is available, drop crop/pose for that frame and count the
  drop. Do not block full-frame recording.

### PoseWorker
- Owns TRT engine and a CUDA stream.
- On `WorkerFunction`:
  - `cudaStreamWaitEvent` on `crop_ready_event`.
  - Convert or bind the crop buffer into the model input layout if needed.
  - Run TRT inference on the crop tensor.
  - Write results to output (IPC/UI/log).
  - Return buffer and event to pool.

## Event Model
- `crop_ready_event` recorded in crop worker stream.
- Pose worker waits on the event in its own stream.
- Long-term target: no per-frame CPU sync in the submit hot path. The current
  real TensorRT backend intentionally keeps a correctness sync after the output
  copy until the artifact contract, lifetime model, and failure handling are
  hardened enough for an async collector.

## Configuration
- Add a toggle in `CameraEachSelect` (e.g., `pose` flag).
- Optional decimation: `ORANGE_POSE_DECIMATE` (like YOLO).
- TRT engine path configurable per camera.

## Performance Notes
- Keep pose model small enough for the required rate; do not assume the default
  `256x256` crop if the scientific need is a higher-resolution crop.
- Run only when detection exists.
- Keep queue sizes bounded and drop pose when overloaded.
- Avoid extra GPU->CPU copies; only copy results.
- Minimize latency from `detect_done -> crop_ready -> pose_start`. This matters
  more than preview or crop-video latency if pose is intended for reactive use.
- Because detect runs on a downsampled frame while pose needs a high-resolution
  crop from the original source, the key optimization target is the handoff
  between those two stages, not the elimination of downsampled detect itself.

## Fast Path vs Sidecar Work

- The intended low-latency path is:
  - `detect -> crop produce -> pose`
- Crop-video encoding is important, but it is not latency-critical in the same
  way. It should behave as a downstream sidecar consumer of the produced crop,
  not as a stage that pose must wait behind.
- This means "decouple crop from encode" really means:
  - crop production is realtime-critical,
  - pose is realtime-critical,
  - crop-video encode is throughput-critical and should be allowed to lag
    behind without stalling pose.
- This change alone does not remove the staged detach cost. It removes preview /
  encode work from the critical path after crop production so that pose waits on
  as little as possible.

## Hybrid Ownership Model

- The right long-term design is not "one memory model for everything." It is a
  hybrid model chosen per consumer type.
- The working categories are:
  - `ingress lease`: the SDK/GPUDirect source frame, used by immediate
    consumers that can act now and release it quickly,
  - `owned analytics workspace`: a stable `cudaMalloc` frame or crop used by
    consumers that may lag, queue, branch, or fan out,
  - `sidecar outputs`: archival or debug consumers that should not sit on the
    low-latency path.
- Placement guide:
  - detection:
    - preferred model: `ingress lease`
    - reason: detect is the immediate continuation of acquisition and can use
      the freshest frame without needing a long-lived stable full-resolution
      copy.
  - current full-frame recording/preprocess path:
    - preferred model: `ingress lease` as long as it remains stable under
      measurement
    - reason: it is already validated and benefits from minimizing extra
      copies; do not move it off the lease unless it shows the same
      source-touch pathology.
  - crop production for pose:
    - preferred model: `owned analytics workspace`
    - reason: pose is inherently behind detection, and crop/pose should not
      depend on a live SDK-owned source lease.
  - pose inference:
    - preferred model: `owned analytics workspace`
    - reason: pose is a delayed consumer by design and should read from a
      stable crop or frame buffer.
  - crop-video encode:
    - preferred model: `sidecar output` from the owned crop payload
    - reason: archival crop video is throughput-critical, not latency-critical,
      and must not block pose.
  - debug dumps, overlays, optional IPC mirrors, or future branching analytics:
    - preferred model: `sidecar output` from an owned buffer
    - reason: anything that may queue, retry, or branch should not touch the
      ingress lease.
- Practical rule:
  - if a consumer might be delayed, queued, retried, rate-limited, or fan out
    to more work, it should not read from the ingress lease.
  - if a consumer is the immediate first continuation of acquisition and can
    release quickly, it may use the ingress lease.
- This means the likely future structure is:
  - acquisition receives a GPUDirect ingress lease,
  - immediate consumers use that lease where beneficial,
  - once delayed analytics are needed, one owned buffer or crop is created,
  - all delayed consumers read from that owned payload, not the lease.

### Consumer Placement Thought Process

- For each workload, decide placement by asking four questions:
  1. Is this the immediate continuation of acquisition?
  2. Can it lag, be rate-limited, or be dropped without breaking correctness?
  3. Does it branch into more work or fan out to multiple downstream
     consumers?
  4. Does it need the full frame, or only a crop / derived payload?
- These questions map directly to the three ownership models:
  - `ingress lease`:
    - best for the immediate first consumer that can act quickly and release
      the frame
  - `owned analytics workspace`:
    - best for delayed, queued, branching, or multi-consumer work
  - `sidecar output`:
    - best for archival, debug, UI, or other non-latency-critical work
- Short rule:
  - immediate + single-step + no fanout -> `ingress lease`
  - delayed or branching -> `owned analytics workspace`
  - non-critical archival/debug/UI -> `sidecar output`

### Placement Decision Table

| Workload | Immediate continuation of acquisition? | Can lag / drop / queue? | Needs full frame or crop? | Preferred model | Why |
| --- | --- | --- | --- | --- | --- |
| Detection | Yes | Usually no | Downsampled full-frame view | `ingress lease` | Detect is the first urgent consumer and benefits most from the freshest source with no extra detach. |
| Current full-frame recording / preprocess | Yes, in the current pipeline | Ideally no | Full frame | `ingress lease` for now | This path is already validated at `100 fps`; do not move it unless measurement shows the same source-touch pathology. |
| Crop production for pose | No, it happens after detect accepts a frame | Yes, but best-effort | High-resolution crop from original frame | `owned analytics workspace` | Crop/pose is inherently behind detect and should not depend on a live SDK-owned source lease. |
| Pose inference | No | Yes, bounded and best-effort | Crop / derived tensor | `owned analytics workspace` | Pose is a delayed consumer by design and should run from a stable crop buffer. |
| Crop-video encode | No | Yes | Owned crop payload | `sidecar output` from owned crop | Crop video is throughput-critical, not latency-critical, and must not block pose. |
| Preview / overlays / GUI crop display | No | Yes, and should be droppable | Owned crop payload | `sidecar output` | Display work should never hold scarce crop buffers or ingress leases. |
| IPC mirrors / debug dumps / future branching analytics | No | Yes | Usually derived payload or owned crop/frame | `sidecar output` or `owned analytics workspace` | Anything that may retry, branch, or fan out should not touch the ingress lease. |

### Why Detection Still Fits the Ingress Lease

- Detection is analytics compute, but it is still the immediate first consumer
  of the acquired frame in the current design.
- Its job on the source frame is narrow:
  - read the frame,
  - preprocess into the model input,
  - run inference,
  - emit an ROI decision for downstream work.
- That is different from crop/pose, which needs stable downstream ownership of
  high-resolution pixels after detection has already completed.
- So in the current architecture:
  - detection is part of the analytics workflow,
  - but `CropProducer` is the stage that creates the long-lived owned payload
    for delayed analytics consumers.
- This is why "analytics workload" and "analytics payload creation stage" are
  not the same thing.
- Keep this as the default unless measurement later shows that even immediate
  detect-on-lease access becomes too expensive at higher frame rates.

### Provisional Default Answers

- Until measurements prove otherwise, the default future design should be:
  - detection stays on the `ingress lease`
  - full-frame recording stays on the `ingress lease`
  - pose moves to a detection-gated `owned analytics workspace`
  - crop-video encode remains a downstream `sidecar output`
- This intentionally does **not** assume that every frame gets a detached
  full-resolution analytics copy.
- The safest near-term scope is:
  - keep the validated full-frame path where it is,
  - create owned payloads only for delayed analytics work,
  - and prefer a detection-gated owned crop before inventing a full-frame
    analytics ring.

### Detection-Gated Semantics vs Capacity Planning

- Crop/pose is still detection-gated semantically:
  - `detect -> choose ROI -> crop -> pose`
  - if no ROI exists, there is no crop/pose for that frame unless a later
    policy such as hold-last-good or tracker-assisted ROI is introduced.
- That semantic contract does **not** imply the workload is sparse.
- If detections are expected on nearly every frame, the system should be sized
  and evaluated as a near-every-frame high-resolution analytics workload even
  though crop/pose remains logically detection-gated.
- Practical implication:
  - sparse detections:
    - favor a late, detection-gated owned crop
  - near-continuous detections:
    - the advantage of "only detach when needed" shrinks,
    - and an earlier owned full-frame analytics buffer becomes more
      defensible because high-resolution ownership cost is being paid almost
      every frame anyway.
- So the planning rule is:
  - keep the semantic contract detection-gated,
  - but size the architecture for the high-duty-cycle case if best-case
    behavior means a detection on almost every frame.

### Strategic Choice: Intermediate Path Now, Stricter Path Later

- The next implementation step should **not** be a full `500 fps`-optimized
  redesign.
- The better pattern is:
  - build the simplest intermediate path that is testable and benchmarkable at
    `100 fps`,
  - but shape the boundaries so it can evolve into the stricter `500 fps` path
    without a rewrite.
- Concretely, that means preserving these seams now:
  - explicit `ingress lease` vs `owned analytics workspace` boundary,
  - one stage that creates analytics payloads,
  - pose / encode / preview as separate consumers with explicit drop policy,
  - exactly-once source release and crop recycle accounting,
  - per-stage timing and queue metrics that can prove where latency lives.
- The intended progression is:
  - today:
    - `detect on ingress lease -> owned crop -> pose`
  - later, only if the data forces it:
    - earlier acquisition/analytics-owned frame -> `detect + pose` fast path
- This is the recommended strategy unless one of these becomes true:
  - pose is expected on almost every frame,
  - the post-detect detach remains the dominant latency cost,
  - higher-rate tests show the intermediate ownership model does not survive.
- So the engineering choice is:
  - future-proof the interfaces now,
  - benchmark the intermediate design first,
  - and only promote to the stricter path when measurements justify the added
    complexity.

### Contracts To Make Explicit Before Larger Refactors

- Ownership contract:
  - exactly one stage must own creation of the owned analytics payload and
    exactly one stage must own returning the SDK frame / ingress lease
  - avoid split responsibility between acquisition and downstream consumers
- Admission contract:
  - for each delayed consumer, define whether full queues mean drop-latest,
    drop-oldest, skip, or disable
  - delayed consumers must not silently block the fast path
- Scope contract:
  - decide whether the next owned payload is:
    - a detection-gated owned crop, or
    - a full-frame analytics ring
  - do not implement both at once
- Observability contract:
  - count accepted, dropped, and processed work per consumer
  - track ingress-lease hold time and exactly-once release / recycle behavior

### Current Admission Policy

- `PoseWorker` currently uses a bounded queue with drop-newest behavior:
  - if the queue is full, the new pose crop is rejected and its extra lease is
    released
  - pose is therefore best-effort and cannot block crop production
- `CropAndEncodeWorker` currently uses a bounded sidecar queue with drop-newest
  behavior:
  - if the queue is full, the new crop sidecar job is rejected
  - crop preview and crop-video encode are both skipped for that frame
  - the owned crop payload is recycled instead of blocking the fast path
- This is the intended contract:
  - delayed consumers may lose work
  - latency-critical producers may not block on sidecar saturation
- The relevant counters now live in worker summaries:
  - `PoseWorker`: enqueued / processed / queue-full drops / queue-high-water
  - `CropProducerWorker`: jobs offered / accepted / queue-full drops
  - `CropAndEncodeWorker`: jobs enqueued / queue-full drops / queue-high-water
  - Shaman v2 frame IPC: pose updates published, stale suppressed, queue drops,
    and push failures in `frame_ipc_summary.json`
- Recording artifacts now also persist the crop sidecar admission summary in
  `Cam<serial>_crop_sidecar_perf.csv`, and `recording_snapshot.json` lists that
  file under crop outputs.

### Earlier-Owned-Frame Experiment Path

- There is now an opt-in experiment path for overlapping the owned
  high-resolution frame copy with detection:
  - env: `ORANGE_ANALYTICS_EARLY_OWNED_FRAME=1`
- In this mode:
  - acquisition keeps `d_image` pointed at the ingress lease for immediate
    consumers such as YOLO,
  - acquisition also starts an async copy into the entry's owned
    `d_image_pool`,
  - `CropProducer` can then use that earlier owned full-frame buffer as its
    crop source instead of doing the later post-detect staged full-frame copy.
- This is still an intermediate experiment, not the final stricter ownership
  redesign:
  - it is meant to measure whether earlier ownership transfer plus overlap helps
    the `detect -> crop_ready` latency path,
  - while keeping detection on the ingress lease by default.
- `Cam<serial>_crop_perf.csv` now exposes
  `analytics_owned_wait_cpu_ms` so the experiment can be distinguished from the
  later staged-source path.

## Lease Lifecycle and Synchronization

- `CropFrame::active_leases` is the correct lifetime tool for the shared crop
  payload:
  - producer allocates a crop frame from the bounded pool,
  - producer starts with one lease for the downstream crop sidecar,
  - pose accepts an additional lease if enabled,
  - each accepted consumer releases its own lease independently,
  - the crop frame returns to the pool only when the last lease is released.
- That per-frame lifecycle looks like:
  - `YoloWorker` finishes detect and hands the source frame to
    `CropProducerWorker`,
  - `CropProducer` copies the ROI into a bounded `CropFrame`,
  - `CropProducer` records `crop_ready_event`,
  - `CropProducer` offers one lease to `PoseWorker`,
  - `CropProducerWorker` offers one lease to `CropAndEncodeWorker`,
  - `PoseWorker` waits on `crop_ready_event`, does its work, and releases its
    lease,
  - `CropAndEncodeWorker` waits on `crop_ready_event`, performs preview/encode
    sidecar work, and releases its lease,
  - only then does the crop frame recycle back into the free pool.
- Ref counting is necessary here because the object lifetime is genuinely
  shared across asynchronous consumers.
- Ref counting is not sufficient by itself:
  - it answers "when is this crop frame no longer in use?"
  - it does not answer "how do multiple threads safely mutate the same pending
    release/recycle container?"
- So the design still needs both:
  - atomic lease/ref-count state on each shared `CropFrame`,
  - mutex-protected bookkeeping for deferred source-release and deferred
    crop-frame recycle queues.
- Rule of thumb for similar designs:
  - use ref counting when multiple consumers share one payload and finish
    independently,
  - use a mutex or other synchronized queue when multiple threads mutate the
    same container or state machine and an action must happen exactly once.

## Why GPUDirect Detach Looks Slow

- A normal same-GPU VRAM-to-VRAM copy of a `4512x4512 Mono8` frame (`~20 MB`)
  should be well under `1 ms` on modern hardware.
- The measured `~6 ms` staged-detach tail is therefore not raw device-memory
  bandwidth. It is mostly the cost of safely touching the GPUDirect-backed
  source buffer from CUDA:
  - external buffer ownership / fencing,
  - driver/runtime synchronization before the detach can be submitted,
  - then the actual copy.
- This matches the measurements:
  - once data is in ordinary `cudaMalloc` device memory, ROI crop submission is
    cheap,
  - the long tail follows "touch the GPUDirect source", not "copy bytes on the
    GPU".
- An earlier full-resolution stable copy can still help latency, but mostly by
  moving and overlapping the detach, not by making the underlying GPUDirect
  detach magically cheap.
- Best case, if the camera/SDK can deliver directly into a stable buffer we own,
  later copies behave like normal GPU memory operations. Until then, the
  software plan should assume:
  - GPUDirect buffer = ingress lease,
  - owned device buffer = analytics workspace.

## Detect-Side Synchronization Findings (2026-04-23)

- Detailed follow-up design for reducing the remaining detect-side latency now
  lives in `docs/detect_hot_path_rework_plan.md`.
- Latest in-app detect instrumentation says the detect path is usually **not**
  waiting on real ingress readiness:
  - on run `2026_04_23_22_12_54`, `ingress_event_ready_before_wait` was already
    true on `1023/1027` frames for `Cam2010095` and `1018/1029` frames for
    `Cam2010096`,
  - but `cpu_wait_event_ms p95` was still `7.30 ms` / `6.54 ms`,
  - while GPU-side `wait_ms p95` stayed tiny at `0.018 ms` / `0.266 ms`.
- Therefore the large wall-clock region currently labeled `cpu_wait_event_ms`
  is not mostly true upstream waiting. The ingress event is usually already
  satisfied by the time YOLO reaches `cudaStreamWaitEvent(...)`.
- A follow-up Nsight Systems CLI/SQLite trace with CPU context-switch capture
  confirmed the same conclusion:
  - on the two `YoloWorker_Cam_` threads, `cudaStreamWaitEvent` averaged only
    about `2.5 us` and maxed at `17-32 us`,
  - `cudaEventSynchronize` on those threads averaged only `4.5-4.9 us` and
    maxed at `23-27 us`,
  - the largest scheduler-inflated CUDA API outliers on the YOLO threads were
    actually `cudaEventRecord` calls, with worst cases around `7.6-10.7 ms`.
- The Nsight trace also showed why those outliers were so long:
  - each worst-case `cudaEventRecord` call contained a sched-out followed by a
    sched-in pair spanning almost the full wall-clock duration,
  - YOLO-thread deschedule gaps had `p95` around `154 us`, `p99` around
    `156 us`, and rare maxima around `10.6 ms`.
- Current interpretation:
  - detect-side ingress readiness is usually already available,
  - TRT inference itself is not the main latency issue,
  - the dominant remaining detect-side tax is host scheduling/runtime jitter on
    the YOLO threads and hot-path CUDA API chatter, not crop or source-buffer
    readiness.
- One caveat from this instrumentation pass:
  - `ingress_event_record_to_worker_start_ms` is currently invalid because the
    timestamp is being reset later in acquisition; do not use that field for
    decisions until it is fixed.
- Near-term optimization direction should therefore shift toward:
  - CPU affinity / thread isolation for the YOLO workers,
  - reducing unnecessary YOLO-thread CUDA API calls on the hot path,
  - and only revisiting detect-side buffer ownership again if those scheduler
    experiments fail to help.

## CUDA Graph Capture (Pose TRT)
- Prefer CUDA Graph launch for steady-state pose inference to reduce host enqueue
  overhead and internal TRT contention.
- A future low-latency graph strategy may need two graph families rather than
  one monolithic graph:
  - `detect_only`: reduced-resolution detect path for frames with no pose work
    accepted,
  - `detect_plus_pose`: reduced-resolution detect followed immediately by
    high-resolution crop preparation and pose launch for accepted ROI frames.
- This is attractive because the detect stage and the pose stage do not consume
  the same image representation:
  - detect uses a downsampled full-frame view,
  - pose uses a crop derived from the original-resolution source.
- A single monolithic graph for every camera path is unlikely to fit the
  conditional nature of the workflow. A pair of steady-state graphs, or graph
  islands around the accepted detect->crop->pose path, is more plausible.
- Proposed behavior:
  - default graph-capture enabled (with runtime opt-out flag),
  - warmup once, capture once per pose worker/model instance, then launch graph
    per frame.
- If detect+pose graphing is explored later, the first timing comparison should
  be:
  - `detect_done -> pose_done` with ordinary enqueue,
  - versus `detect_done -> pose_done` with the combined graph path.
- Required fallback:
  - if capture fails or unsupported path is detected, continue with normal TRT
    enqueue path (no pipeline interruption).
- Recapture triggers:
  - model/engine swap,
  - input tensor shape/layout change.
- Instrumentation:
  - capture success/failure counters,
  - graph launch vs enqueue timing,
  - end-to-end pose latency and drop impact.

## Risks / Unknowns
- Crop buffer lifetime (must be pooled to avoid overwrite).
- TRT engine latency may compete with YOLO on GPU.
- Requires a clean path for pose results into UI/IPC.

## Implementation Steps (high level)
1. Define `CropFrame`/`PoseEntry` + pool (similar to encoder preprocess pool).
2. Extract a crop producer from the current crop/encode path.
3. Add `PoseWorker` class (similar to YOLO but simpler TRT-only).
4. Wire crop producer outputs to preview, crop encode, and pose consumers.
5. Wire `PoseWorker` creation in `orange.cpp` when enabled.
6. Add output path (IPC or UI overlay).
7. Add configs + decimation + logging.

## Concrete implementation plan
### New files
1. `src/pose_worker.h` and `src/pose_worker.cpp`
2. Optional: `src/pose_types.h` (if you want to keep structs out of `video_capture.h`)

### Existing files to touch
1. `src/video_capture.h`
   - Add per-camera toggles and model path fields (e.g. `pose`, `pose_model`).
2. Crop producer files (new or extracted from `src/crop_and_encode_worker.*`)
   - Add a bounded crop buffer/event pool.
   - Add consumer registration for preview, crop encode, and pose.
   - Enqueue pose work from produced `CropFrame` payloads when detections exist.
3. `src/crop_and_encode_worker.h` / `src/crop_and_encode_worker.cpp`
   - Convert to a crop-video consumer, or keep as transitional code until the
     crop producer split is complete.
4. `src/orange.cpp`
   - Create and start `PoseWorker` per camera when enabled.
   - Wire to the crop producer, not directly to crop-video encoding.
   - Add UI toggles + model selection.
5. `CMakeLists.txt`
   - Probably no changes (glob on `src/*.cpp`), but verify on orange_client if needed.

## Skeletons (code outline)
### pose_types.h (optional)
```cpp
#pragma once
#include <cuda_runtime.h>
#include <vector>

struct CropFrame {
    unsigned char* d_crop = nullptr;
    int crop_width = 0;
    int crop_height = 0;
    int source_width = 0;
    int source_height = 0;
    float crop_x = 0.0f;
    float crop_y = 0.0f;
    float detection_x = 0.0f;
    float detection_y = 0.0f;
    float detection_w = 0.0f;
    float detection_h = 0.0f;
    float detection_confidence = 0.0f;
    cudaEvent_t* crop_ready_event = nullptr;
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
    int camera_id = 0;
};

struct PoseResult {
    uint64_t frame_id = 0;
    std::vector<float> keypoints; // x,y,score...
};
```

### pose_worker.h
```cpp
#pragma once
#include "threadworker.h"
#include "thread.h" // SafeQueue
#include "pose_types.h"
#include <atomic>

class PoseWorker : public CThreadWorker<CropFrame> {
public:
    PoseWorker(const char* name, SafeQueue<CropFrame*>& free_crop_frames);
    ~PoseWorker() override;

protected:
    bool WorkerFunction(CropFrame* entry) override;

private:
    cudaStream_t m_stream = nullptr;
    // TensorRT engine wrapper (similar to yolov8_det)
    // PoseTrtEngine engine_;
    SafeQueue<CropFrame*>& free_crop_frames_;
};
```

### pose_worker.cpp (core logic sketch)
```cpp
bool PoseWorker::WorkerFunction(CropFrame* entry) {
    if (!entry) return false;

    // Wait for crop to be ready
    if (entry->crop_ready_event) {
        ck(cudaStreamWaitEvent(m_stream, *entry->crop_ready_event, 0));
    }

    // Convert/bind entry->d_crop into the pose model input layout.
    // Run TRT inference on entry->d_crop or a derived tensor.
    // engine_.infer(entry->d_crop, ...);

    // Publish results (IPC/UI/log)

    // Return buffer/event to pool
    free_crop_frames_.push(entry);
    return false;
}
```

### crop producer sketch
```cpp
class PoseWorker;
class CropEncodeWorker;

class CropProducer : public CThreadWorker<WORKER_ENTRY> {
public:
    void SetPoseWorker(PoseWorker* pose_worker);
    void SetCropEncodeWorker(CropEncodeWorker* crop_encode_worker);
    SafeQueue<CropFrame*>& GetCropPool() { return free_crop_frames_; }
private:
    PoseWorker* pose_worker_ = nullptr;
    CropEncodeWorker* crop_encode_worker_ = nullptr;
    SafeQueue<CropFrame*> free_crop_frames_;
    std::vector<CropFrame> crop_frame_pool_;
    std::vector<cudaEvent_t> crop_event_pool_;
};
```

### crop producer enqueue sketch
```cpp
if (pose_worker_ && has_detection) {
    CropFrame* crop = nullptr;
    if (free_crop_frames_.pop(crop)) {
        // Write high-resolution crop into crop->d_crop.
        gpu_crop_and_resize_rgba(entry->d_image, crop->d_crop, ...);
        ck(cudaEventRecord(*crop->crop_ready_event, crop_stream_));

        crop->frame_id = entry->frame_id;
        crop->recording_frame_id = entry->recording_frame_id;
        crop->timestamp = entry->timestamp;
        crop->timestamp_sys = entry->timestamp_sys;

        pose_worker_->PutObjectToQueueIn(crop);
    }
}
```

### orange.cpp wiring sketch
```cpp
// Create
if (cameras_select[i].pose) {
    pose_workers[i] = new PoseWorker(name.c_str(), cropProducers[i]->GetCropPool());
    cropProducers[i]->SetPoseWorker(pose_workers[i]);
}

// Start/stop alongside other workers.
```

## Notes on output path
- Easiest: publish pose results to IPC (similar to YOLO detections).
- UI overlay: store last pose per camera and render in ImGui / overlay shader.

## Open Questions
1. TRT engine input: size, layout, data type, normalization, batch size?
2. Detection selection: pose on all detections or only top confidence?
3. Output schema: what keypoints + coordinate system are produced?
4. IPC payload: ok with per-frame list of keypoints + scores?
5. If YOLO is decimated, should pose run at YOLO rate or be forced to full rate?

## Testing / validation
1. Run with pose disabled (should behave exactly as today).
2. Enable pose and verify:
   - No memory leaks (pose pool returns).
   - FPS stays within target.
   - Pose output matches expected frames.
3. Stress test with decimation, larger queues, and multi-camera load.

## Success Criteria
- Pose inference runs without lowering main recording FPS below target.
- Display + crop preview remain smooth.
- Pose results are delivered with bounded latency.
