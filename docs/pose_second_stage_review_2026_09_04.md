# Pose As A Second Stage After Lever 2d: Review, 2026-09-04

Scope: how the existing pose worker fits the frame lifecycle that landed on
2026-09-04 (copy after detection, `src/late_owned_copy.h`), where the crop
should be cut, which die the pose graph should run on, what ordering must be
enforced, and what to measure. Assessment only; nothing here was run on the
rig and no code was changed. Written in worktree
`/home/jeremy/orange-pose-review-2026-09-04` (branch
`review/pose-second-stage-2026-09-04`, same commit `f5ce5d0` as the shared
branch).

Inputs: `docs/detect_latency_review_2026_09_03.md` (lifecycle after 2d,
skipped frames, lever 2d landed), `docs/pose_second_stage_plan.md`,
`src/pose_worker.{h,cpp}`, `src/crop_producer.{h,cpp}`,
`src/crop_producer_worker.cpp`, `src/crop_pipeline_types.h`,
`src/yolo_worker.cpp`, `src/acquire_frames.cpp`, `src/video_capture.h`, the
pose keys in `src/orange_headless_client.cpp`, and the
`2010096_headless_real_yolo_pose_real_crop_size_{256,384}_a16_gpu5` specs.

Fixed facts (three cameras, 4512x4512 mono, 100 fps, split-GOP HEVC on two
dies per camera, one GA107 die of 10 SMs per camera for detection), not
re-derived here:

| Quantity | Value |
|---|---|
| TensorRT yolo11n FP16 graph | 2.00 ms, flat |
| Preprocess reading the camera buffer alone | 0.08 ms |
| Acquisition-to-detect, recorder on | 2.20 mean / 2.29 p95 |
| Engine-only floor | 2.12 / 2.14 |
| Die idle after detection | about 7.5 ms of every 10 ms |
| 20 MB copy concurrent with the graph, same die | graph 2.00 to 2.26 ms |
| NVENC reading the frame on the same die | graph +0.04, preprocess +0.08 |
| Pool copy issued after detection, complete | about 2.35 ms after arrival; camera buffer returns then |

## Summary

- Today the pose worker reads a pooled `CropFrame` cut by the crop producer
  from the **pool copy** (`d_analytics_image`), never the camera buffer, on a
  separate non-blocking stream on the **detect die**, after a GPU-side wait on
  the ready event (pool copy valid, about 2.35 ms). Two thread hops sit
  between detect done and the pose launch (YOLO thread to
  `CropProducerWorker` to `PoseWorker`); the inline path that removes the
  first hop exists behind `ORANGE_INLINE_CROP_PRODUCER`, default off, and has
  a lifetime hole (below).
- Earliest crop: detections are on the CPU at 2.2 ms and the pool copy is
  valid at 2.35 ms. Reading the camera buffer instead would save at most
  0.15 ms and would need a third gate on the SDK requeue. Keep the pool copy
  as the source; remove the thread hop instead, which is worth about the
  same and costs nothing in ownership.
- Placement: the pose graph fits inside the idle window on the detect die
  (expected detect-to-pose about 1.0 to 1.2 ms with the current synchronous
  worker, of which about 0.8 ms is the graph itself, estimated from the May
  runs). It adds nothing to the current frame's detect and adds to the next
  frame's detect only if it spills into the next graph, which nothing today
  prevents except timing. The other die of the same card costs about
  0.1 ms of hop for a guarantee by construction; the A6000 is fastest
  unloaded but shares with the citrus renderer and needs the p99-under-render
  measurement the roadmap already lists.
- Ordering: the rules that already exist cover every producer-to-consumer
  edge (copy after completion, crop after ready, pose after crop ready,
  recycle after pose). The missing rule is pose N against graph N+1. On one
  die it can only be enforced by making pose wait for the next graph when it
  is late, plus stream priority and a budget drop; the `infer_ms` column is
  the proof that it holds.
- Measure before building: a pose-on registered spec, a pose-on engine-only
  spec, and a pose-noop spec, each judged against the 2026-09-04 baselines
  with `infer_ms` unchanged as the hard gate; a trtexec loop for the pose
  engine on GPU 5 and on the A6000; a peer-copy microbenchmark for the crop.
  Add GPU event timing to the pose worker first, since
  `pose_start_to_pose_done` today lumps the crop-ready wait, the resize
  kernel, the graph, the output copy and the sync into one number.
- **Measured since (section F):** pose real on every frame on the detect
  die leaves the YOLO graph untouched (`infer_ms` 1.974 against 1.976 idle)
  and costs 0.13 ms mean / 0.15 ms p95 of acquisition-to-detect, all of it
  host-side (thread wake-ups and CPU work around the graph), with the pose
  stage itself at 0.90 ms p50 / 1.28 ms p95 and capture-to-pose-done at
  3.5 ms mean / 4.0 ms p95. On the A6000 (one camera, 2026-09-12) capture-to-pose-done is
  1.93 ms mean / 2.08 ms p95, almost entirely from the faster detect graph; the pose
  worker's fixed per-frame overhead, not the pose graph, then dominates the pose stage.
  The A6000 stays closed for production because recording cannot follow it there.

## A. Where The Pose Worker Gets Its Crop Today

### The chain, with evidence

1. **Detect done on the YOLO thread.** After the graph completes the YOLO
   worker issues the pool copy on the acquisition stream ordered after its
   completion event (`src/yolo_worker.cpp:1890-1898`,
   `src/late_owned_copy.h`), records `analytics_ready_event` and sets
   `analytics_ready_event_recorded`. Postprocess follows;
   `yolo_detect_done_host_ns` is stamped at `src/yolo_worker.cpp:2040`.
   Detections are now a CPU vector on the entry.
2. **Handoff to the crop producer.** `src/yolo_worker.cpp:2057-2065`: with
   `ORANGE_INLINE_CROP_PRODUCER` off (the default,
   `src/yolo_worker.cpp:221-231`; no spec key sets it, and the 2026-09-03
   review recorded it off in production) the YOLO worker retains the entry
   and pushes it onto `CropProducerWorker`'s bounded queue. That is thread
   hop one, a condition-variable wake. The inline branch calls
   `ProcessEntryInline` on the YOLO thread instead.
3. **Crop geometry on the crop producer thread.**
   `src/crop_producer_worker.cpp:306` (`ProcessEntryImpl`): the
   highest-probability detection is chosen (`:508`), the crop origin is the
   detection centre minus half the crop size, clamped to the frame; the crop
   size comes from `crop_pipeline.crop_size_px` (256, 320 or 384 in the sweep
   specs). A `CropFrame` is required whenever pose is enabled (`:537`), so
   every detected frame produces one. `Produce` is called at `:540` with
   `release_source_entry = true` in the threaded path.
4. **Source selection in `CropProducer::Produce`** (`src/crop_producer.cpp:637`).
   `cudaSetDevice(camera_params_->gpu_id)`: the detect die. The source is the
   pool copy when `ORANGE_ANALYTICS_EARLY_OWNED_FRAME` is on (default true,
   `:508` of the constructor) and the entry has an owned source (`:706-710`);
   acquisition sets `analytics_owned_frame_valid = true` and
   `d_analytics_image = d_image_pool` on every hybrid-path frame
   (`src/acquire_frames.cpp:2124-2150`). The producer then calls
   `wait_delayed_consumer_ready()` (`:718`, the CPU guard from lever 2d, which
   never blocks here because the YOLO worker recorded the event before the
   enqueue) and `cudaStreamWaitEvent(producer_stream_, analytics_ready_event)`
   (`:719`). The crop itself is a `cudaMemcpy2DAsync` of crop_w x crop_h bytes
   from `d_analytics_image` into a pooled `CropFrame::d_crop_mono` on
   `producer_stream_` (`:788-795`), a non-blocking stream on the detect die.
   256 squared is 64 KB, 384 squared is 147 KB. Then `crop_ready_event` is
   recorded (`:807`) and `crop_ready_host_ns` is stamped on the CPU at enqueue
   time (`:814`), not at GPU completion.
5. **Handoff to pose.** `:816-829`: a `CropFrameLease` is retained and
   `PoseWorker::TryEnqueueCrop` pushes the `CropFrame` onto the pose queue
   (depth `pose_worker.queue_depth`, 32 in the specs). Thread hop two. The
   source entry is then released through a source-release event recorded on
   the producer stream (`:831-833`, `defer_source_after_stream_work`), so the
   pool buffer is not recycled under the crop copy.
6. **Pose on the pose thread.** `src/pose_worker.cpp:721` (`WorkerFunction`):
   `cudaSetDevice(gpu_id)`, again the detect die; `cudaStreamWaitEvent(stream_,
   crop_ready_event)` (`:735`) on the worker's own non-blocking stream created
   at `:569` with default priority. `TensorRtPoseBackend::infer` (`:291-335`)
   launches the fused mono-to-planar-FP32 letterbox resize
   (`launch_optimized_yolo_preprocess`, crop to 1x3x256x256, 0.79 MB written),
   `enqueueV3` (plain enqueue, no CUDA graph), a device-to-host copy of the
   output (1x(5+3K)xN FP32; K = 3 keypoints, so 14 x N floats, tens of KB),
   then `cudaStreamSynchronize` (`:332`). Decode, the JSONL event record, and
   the shaman v2 IPC publish happen on the CPU; the lease is released after
   the stream via `recycle_event` (`:816`).

So: **pool copy, producer stream then pose stream, both on the detect die,
both default priority, starting no earlier than the ready event at about
2.35 ms.** The YOLO stream is created with the greatest priority
(`src/yolov8_det.cpp:143-174`); the crop and pose streams are at the lowest
(CUDA's default 0 is the least priority).

### The per-frame timeline with pose added

Times from frame arrival at 100 fps. Rows 0 to 2.35 are the landed lifecycle;
the rest is what the code above does, with the thread-hop and pose numbers
estimated from the May 2026 runs (`docs/pose_second_stage_plan.md`, 2026-05-10:
`crop_ready_to_pose_start` p50 0.017 ms, `pose_start_to_pose_done` p50
0.99 / p95 1.79 ms with the external recorder, p95 0.90 ms preprocess-only).
Those runs were one camera, GPU 5, synthetic centre box, before every
2026-09 lever, so they bound the pose stage, not today's detect stage.

| t (ms) | Acquisition thread and stream | YOLO thread and stream | Crop producer thread, producer stream | Pose thread, pose stream |
|---|---|---|---|---|
| 0 | Ingress event. Entry marked copy-pending. Handed to YOLO and recording. | | | |
| 0.03 to 0.14 | | Preprocess reads the camera buffer alone. Input-ready recorded. | | |
| 0.14 to 2.15 | | Graph, 2.00 ms, alone on the die. | | |
| 2.15 | | Completion event. Enqueue the 20 MB pool copy on the acquisition stream after it; record ready event; publish the CPU flag. | | |
| 2.20 | | Postprocess; detections on the CPU; `yolo_detect_done`. Retain and enqueue the entry to the crop producer (hop 1). | | |
| about 2.25 | Copy engine still writing the pool. | | Wake. Pick top detection, clamp the crop. `Produce`: stream-wait on the ready event, enqueue the 64 to 147 KB 2D copy, record `crop_ready_event`, stamp `crop_ready_host_ns` (CPU), enqueue to pose (hop 2), record source-release event. | |
| 2.35 | Copy done, ready event fires. Camera buffer back to the SDK (requeue loop, `src/acquire_frames.cpp:1728`, both conditions met). Recording handoff synchronizes on the ready event and sends the frame. | | Crop copy executes now (tens of microseconds). `crop_ready_event` fires. | Wake (about 2.3). Stream-wait on `crop_ready_event`; launch resize, `enqueueV3`, output copy; block in `cudaStreamSynchronize`. |
| about 2.4 to 3.3 | Same-die NVENC shard reads the pool buffer for 7 to 10 ms (during its GOPs). | | | Resize kernel, pose graph (about 0.8 ms, estimated), output copy. |
| about 3.3 to 3.5 | | | | Sync returns. Decode, event log, IPC publish. Record `recycle_event`; the `CropFrame` returns to its pool when it fires. |
| 10.0 | Next frame arrives. | Next preprocess at 10.03. | | Pose must be finished. Margin about 6.5 ms. |

Three things the timeline makes visible:

- The pose graph runs entirely inside the idle window after the copy. It
  never overlaps its own frame's YOLO graph, by construction (crop after
  ready, ready after completion).
- Nothing orders pose N against graph N+1. With a queue of 32 and a
  synchronous one-frame-at-a-time worker, a backlog of about 7 ms would put
  pose kernels under the next graph, and the May run with the in-process
  recorder showed exactly that shape (pose p95 14 ms, queue high-water 11)
  before the recorder moved out of process.
- Two of the recorded pose timing fields are CPU stamps that do not mean
  what their names say: `crop_ready_host_ns` is when the copy was enqueued,
  and `pose_start_to_pose_done` includes the GPU wait for the crop, the
  resize, the graph, the output copy and the synchronize. The GPU event
  columns that the YOLO path gained on 2026-09-04 do not exist for pose.

### Two defects found while reading

- **Inline path has no source-release event.** `ProcessEntryInline` calls
  `ProcessEntryImpl(entry, false)`; with `release_source_entry = false`,
  `Produce` skips `defer_source_after_stream_work` (`src/crop_producer.cpp:682`),
  so no event orders the entry's recycle after the crop copy. The YOLO
  worker releases its reference at the end of `WorkerFunction`; if that is
  the last reference (headless with `immediate_recycle` and no other
  consumer) the pool buffer can be handed to a new frame's late copy while
  the crop copy is still pending on the producer stream. It is safe today
  only because the crop is enqueued microseconds after detect done and the
  pool has 62 entries. Fix before turning inline on: record the source
  release event regardless of who releases, or have the inline caller
  release after the producer stream.
- **`analytics_owned_wait_cpu_ms` will hide the 2d guard.** If any consumer
  ever reaches `Produce` before the YOLO worker records the ready event, the
  50 ms bounded wait in `wait_delayed_consumer_ready()` lands in this column.
  Not a bug, but worth knowing when reading crop perf rows: a nonzero value
  there is the lever 2d guard, not GPU time.

## B. The Earliest Crop

Constraints: the crop origin depends on the detections, which exist on the
CPU at 2.20 ms. The crop copy is a GPU operation and needs a source that is
valid when it executes, not when it is enqueued.

| Source | Earliest enqueue | Earliest execution | Camera buffer hold | New ownership work |
|---|---|---|---|---|
| Camera buffer (`d_image`), ordered after the YOLO input-ready or completion event | 2.20 (on the YOLO thread) | about 2.22 (completion fired at 2.15; only launch latency remains) | Extended from "copy done" to "copy done and crop done". In the common case the crop (tens of microseconds) finishes before the 20 MB copy at 2.35, so no extension; under a crop-producer backlog the hold follows a third thread. | The SDK requeue (`src/acquire_frames.cpp:2225-2239`, `:1728`) gates on the ready event and the YOLO consumer-done event only. The crop producer's source-release event gates entry recycling, not the requeue (hybrid entries have `camera_buffer_ptr = nullptr`). A third condition and its recorded-flag would have to be added. |
| Pool copy (`d_analytics_image`), ordered after the ready event (today) | 2.20 to 2.25 | 2.35 | Unchanged, 2.35 ms. | None; the entry refcount and the source-release event already exist. |

The camera-buffer read saves at most 0.13 to 0.15 ms of GPU-side wait and
nothing on the CPU side, because the detections are not available earlier
than 2.20 either way. It puts a second reader on the camera buffer while the
20 MB copy is reading it (147 KB against 20 MB, negligible bandwidth), and it
makes the SDK ring depend on the crop producer thread's health. The ring
itself is not the constraint: 100 buffers, a 2.35 ms hold at a 10 ms period
is a quarter of a buffer in flight, and even a 10 ms hold is one buffer. The
cost is a third correctness gate on the requeue path for a tenth of a
millisecond.

**Recommendation: keep the pool copy as the crop source.** Get the same
tenth of a millisecond from the CPU side instead, where it is free of
ownership consequences:

- Enqueue the crop on the YOLO thread right after detect done (the inline
  path), ordered on the ready event as now, so the GPU executes it the
  instant the copy lands instead of after a thread wake. The crop
  producer's CPU work is about 50 to 100 microseconds of enqueue and
  bookkeeping; on the YOLO thread that delays the pickup of frame N+1, which
  arrives 7.5 ms later, so it costs detect nothing. Fix the inline
  source-release hole first.
- Longer term, and only if the crop-video and preview consumers are not
  running: let pose read the ROI straight out of the pool copy with a fused
  ROI-resize kernel into its input tensor (the `launch_mono_roi_copy_kernel`
  and the letterbox resize are one kernel apart), skipping the `CropFrame`
  copy and one event. `CropFrame` stays for encode and preview, which
  need the raw crop.

Neither change moves the camera buffer's return; it stays at about 2.35 ms
as landed with lever 2d.

## C. Placement

Assumptions, named:

- P, the pose graph plus resize on a GA107 die, is about 0.8 ms. Basis: May
  2026 `pose_start_to_pose_done` p50 0.89 to 0.99 ms with the resize, output
  copy and a `cudaStreamSynchronize` inside it, on GPU 5, 256x256 FP32 input
  binding of an FP16 engine. Not measured in isolation; the trtexec loop in
  section E settles it.
- The A16's two dies share an on-card PCIe switch at Gen4 x4, about 6 GB/s in
  practice (the lever 6 analysis). The crop is 64 to 147 KB, so the peer
  copy is dominated by launch and completion latency, 30 to 60 microseconds,
  plus a cross-device event wait of similar size. Shipping the preprocessed
  FP32 tensor instead (0.79 MB) would be about 0.13 ms; ship the crop, not
  the tensor, and resize on the destination.
- To the A6000 (GPU 0) the path is through the host root complex. Whether
  peer-to-peer is enabled between an A16 die and the A6000 is unknown; if
  not, the copy bounces through host memory as two copies. 0.1 to 0.3 ms.
- The A6000 has 84 SMs against 10; the pose graph should run three to four
  times faster there, about 0.25 ms, when it has the device. It does not:
  citrus renders on it, and a graphics context and a CUDA context time-slice
  at roughly millisecond granularity (2026-09-03 review, locality caveat).
- The "other die" is not idle. It runs the other GOP shard's NVENC for half
  of every 500 ms cycle, receives that shard's 20 MB peer copy per frame
  during its GOPs, and hosts the crop recorder when that is on. Its SMs are
  otherwise free.
- Pose remains best-effort and detection-gated, as the plan states. Nothing
  here lets pose delay detection or recording.

| Placement | Hop (crop in, results out) | Pose GPU time | Expected detect-result to pose-result | Effect on the next frame's detect | Cost |
|---|---|---|---|---|---|
| Detect die, idle window after the copy (today) | none | P about 0.8 ms; NVENC reads on the same die add a tail (May: p95 0.90 preprocess-only versus 1.79 with the split-GOP recorder running) | about 1.0 to 1.2 ms mean, p95 about 1.8 to 2.0 with the current synchronous worker and two thread hops; about 0.9 mean with the inline enqueue | Zero while pose finishes before 10.0 ms (margin about 6.5 ms). If pose spills into graph N+1: SM contention between two TensorRT graphs, worse than the 0.26 ms the 20 MB copy cost; up to about P added to `infer_ms` on that frame. Today this is prevented by timing only. | No code beyond ordering (section D) and pose GPU timing. |
| Other die of the same card | about 0.1 ms (peer copy of the crop, cross-device event, output copy to host from that die) | P about 0.8 ms; shares that die with the other shard's NVENC read pressure during its GOPs and the 20 MB inflow | about 1.1 to 1.3 ms | Zero by construction: different SMs, different memory. The crop's 147 KB read of the detect die's pool buffer is noise next to NVENC's 20 MB read. | A second CUDA context and TensorRT context per camera in `orange_client` on the other die; peer access enabled between the pair; `cudaMemcpyPeerAsync` for the crop; cross-device event waits; a `pose_worker.gpu_id` spec key. The crop-video encoder on that die, if on, competes with pose the way NVENC does on the detect die today. |
| A6000 (GPU 0) | 0.15 to 0.3 ms (host-routed unless peer access exists) | about 0.25 ms unloaded; unknown under citrus render, with millisecond time-slice tails possible | about 0.5 to 0.7 ms best case; p95 and p99 unknown | Zero. | Three pose contexts on a shared render GPU; results must come back to the host anyway. The roadmap's lever 7 measurement (standalone engine loop on the A6000 while citrus renders, go only if p99 is comfortably under 1.5 ms) is the gate. |

Reading the table: the same die is the right default because P fits in the
idle window with six milliseconds to spare and it needs no new contexts or
copies. Its one weakness is that the guarantee against overlapping the next
graph is not structural. The other die buys that guarantee for about 0.1 ms
and a second context per camera; it is the fallback if pose ever grows (a
larger input, more keypoints, two animals, a heavier backbone) toward the
window's edge, or if the measurements in section E show `infer_ms` moving
with pose on. The A6000 is a throughput win and a tail gamble; do not
relocate there without the p99-under-render number.

One more placement consideration the plan raises and this review confirms:
the crop size sweep (256, 320, 384) changes the crop copy by at most 83 KB
per frame and does not change P, because the model input is resized to
256x256 regardless. A 384 crop resized to 256 discards resolution; if the
science wants the pixels, the engine input has to grow, and P with it. That
is the number that would eventually push pose off the detect die.

## D. What Must Never Overlap The YOLO Graph, And How To Order It

On the detect die, during the graph (0.14 to 2.15 ms of each frame), the
measurements say:

- The 20 MB pool copy: +0.26 ms on the graph. Already excluded by lever 2d
  (copy waits on the completion event).
- NVENC's read of the frame: +0.04 ms. Unavoidable while the shard lives on
  this die; accepted.
- New with pose, unmeasured but worse in kind because it is SM work rather
  than copy-engine work: the resize kernel, the pose TensorRT graph, and the
  output copy. Two TensorRT graphs on 10 SMs timeslice; the YOLO graph would
  lose up to the pose graph's duration.
- Also new: the crop 2D copy (a copy-engine op, tiny) and, if crop video is
  ever encoded on this die, its preprocess and NVENC submission.

Existing ordering, all by events on the existing streams:

| Rule | Edge | Mechanism | Where |
|---|---|---|---|
| R1 | Pool copy after graph N | acquisition stream waits `yolo_completion_event(N)` | `src/late_owned_copy.h`, `src/yolo_worker.cpp:1890` |
| R2 | Crop after pool copy | producer stream waits `analytics_ready_event(N)` after the CPU flag | `src/crop_producer.cpp:718-719` |
| R3 | Pose after crop | pose stream waits `crop_ready_event(N)` | `src/pose_worker.cpp:735` |
| R4 | Pool buffer reuse after crop read | source-release event on the producer stream gates entry recycle | `src/crop_producer.cpp:831` (threaded path only; missing inline) |
| R5 | `CropFrame` reuse after pose read | `recycle_event` on the pose stream | `src/pose_worker.cpp:816`, `src/crop_producer.cpp` `RecycleAfterConsumerStream` |
| R6 | Camera buffer back to the SDK after both readers | requeue gated on the ready event and the YOLO consumer-done event, each behind its recorded flag | `src/acquire_frames.cpp:2225-2239`, `:1728` |

What is missing is the edge **pose N must not run during graph N+1**. It
cannot be expressed as "graph N+1 waits on pose N", because that would let
pose delay detection. On a single die there are three tools, and the honest
answer is that they combine into a guarantee with a bounded residual rather
than a pure event chain:

1. **Priority, already in place.** The YOLO stream is created at the
   greatest priority (`src/yolov8_det.cpp:143-174`); the pose and producer
   streams are at the default, which is the least. Priority governs which
   pending blocks the hardware dispatches next, not preemption of running
   blocks, so its residual is one pose kernel's tail, tens of microseconds.
2. **Yield-to-the-next-graph, by event.** Give the YOLO worker a per-camera
   "latest completion event" slot: the event pointer and the entry's
   `yolo_completion_event_recorded` flag it last recorded. Before the pose
   worker enqueues pose N it reads the slot and issues
   `cudaStreamWaitEvent(pose_stream, latest_completion)`. If YOLO N+1 has
   already been enqueued, that event belongs to N+1 and pose N lands
   *behind* graph N+1 instead of inside it; if not, the event is N's own,
   already satisfied, and pose runs at once. The reusable-handle trap from
   lever 2d applies: only wait on an event whose recorded flag is set. The
   residual is the race between the pose worker's read and YOLO N+1's
   enqueue; it only bites if pose N is enqueued within about 0.1 ms of
   frame N+1's arrival, and priority bounds the damage to one kernel.
3. **Budget drop, by policy.** Pose is best-effort. If `now -
   acquisition_receive_host_ns` exceeds a budget (about 7 ms at 100 fps)
   when the pose worker dequeues the crop, skip the frame with a
   `pose_late_drops` counter and a `late` status in the event log rather
   than launching into the next graph. This also bounds the queue backlog
   that the May in-process run showed (high-water 11), which is the only way
   pose ever reaches the window's edge.

Two further rules for the implementation, so the ordering is not undone by
stream choice:

- Never put the resize, the pose graph, or the crop copy on the acquisition
  stream or the YOLO stream. On the acquisition stream they would serialize
  behind the next frame's copy; on the YOLO stream they would run ahead of
  the next preprocess.
- Any pose placement off the detect die needs the same R2 to R5 edges
  expressed with cross-device event waits (`cudaStreamWaitEvent` accepts an
  event recorded on another device) and the crop's peer copy ordered after
  `analytics_ready_event` and before the source-release event.

**The proof is `infer_ms`.** The GPU event timing landed on 2026-09-04
records the graph's own duration per frame. With pose on, the distribution
of `infer_ms` must be the one measured without it: 2.00 mean, p95 within
0.03. Any pose kernel under the graph shows up there as the 2.26-style
signature. That column, not the pose log, is the gate for every experiment
below.

## E. Measurements That Settle Placement

Do not run any of this until the rig is released (a 30-minute endurance run
is in progress). Everything runs through
`scripts/run_detect_latency_spec.sh <spec> [baseline latency_phases.json]`
with `sudo -n /usr/local/bin/orange-local-benchmark`; specs live in
`experiment_specs/` and are stamped into `/tmp` by the script. The trtexec
and peer-copy microbenchmarks need the dies but not the cameras; they still
wait for the release because the endurance run holds the dies at ceiling.

### Instrumentation first, no rig needed to build

- **Pose GPU event timing.** Mirror the YOLO worker's `prof_events` on the
  pose stream: events around the crop-ready wait, the resize, `enqueueV3`,
  and the output copy; new columns in `Cam*_pose_perf.csv` and the JSONL
  timing block: `pose_wait_ms`, `pose_pre_ms`, `pose_infer_ms`,
  `pose_out_ms`. Behind `yolo_gpu_timing` (the same flag), since the elapsed
  reads are free once the worker already synchronizes. Without this,
  `pose_start_to_pose_done` cannot separate the graph from the wait for the
  crop.
- **Crop GPU timing on.** `ORANGE_CROP_COPY_TIMING` exists (default off) and
  fills `crop_copy_gpu_ms`; add a spec key so the wrapper's env allowlist is
  not an obstacle.
- **Reference the pose log to the perf row.** Both carry
  `recording_frame_id`; the analysis script's join by that id (as it does
  for the recorder routing CSV) gives per-frame `infer_ms` next to
  `pose_infer_ms`, which is how an overlap would be attributed.

### Specs, in the style of `threecam_detect_latency_engine_only`

All three cameras (2010093/94/95 on dies 3, 1, 7), `ptp_gate`, 60 s, warmup
2 s, `yolo_gpu_timing` on, `headless_gpu_dmon` on, real YOLO with the
current engine. Pose block as in the `crop_size_256` spec (`mode: real`,
the fish_v1 engine, `roi_source: yolo_top_detection`, `queue_depth: 32`,
`crop_frame_pool_size: 32`, `write_events_jsonl: true`), `crop_size_px` 256
first, 384 second.

| Spec | Differs from | Isolates | Pass looks like |
|---|---|---|---|
| `threecam_detect_latency_pose_registered` | the registered spec (recorder on, all levers, copy after detection) plus pose real | pose's total cost to detect and recording in the production configuration | acquisition-to-detect within +0.05 ms of the registered baseline (2.20 mean / 2.29 p95); `infer_ms` 2.00 ± 0.02 mean, p95 ≤ 2.06; `pre_ms` unchanged; 5900/5900 submitted and acked, zero cap skips, zero copy fallbacks after warmup; pose queue high-water ≤ 2, zero `queue_full_drops`; `pose_start_to_pose_done` p95 < 2.0 ms; `capture_to_pose_done` p95 < 4.5 ms |
| `threecam_detect_latency_pose_engine_only` | `threecam_detect_latency_engine_only` (`immediate_recycle`, no recorder) plus pose real | pose's cost to detect with no NVENC on the die; pose's own time with no NVENC | acquisition-to-detect within +0.05 ms of the engine-only floor (2.12 / 2.14); `infer_ms` unchanged as above; `pose_infer_ms` mean is P, expected about 0.6 to 0.8; `pose_start_to_pose_done` p95 < 1.2 ms |
| `threecam_detect_latency_pose_noop` | the registered spec plus pose `mode: noop` | the crop producer, both thread hops and the crop copy without any pose GPU work | detect unchanged; `crop_ready_to_pose_start` p95 < 0.1 ms; `crop_copy_gpu_ms` p95 < 0.05 ms; `detect_to_crop_ready` p95 < 0.3 ms |
| `threecam_detect_latency_pose_inline` | `pose_registered` with `ORANGE_INLINE_CROP_PRODUCER=1` (needs a spec key and the source-release fix) | the value of removing hop one | `detect_to_crop_ready` p95 drops by about 0.1 ms; `total_ms` on the YOLO row grows by the crop enqueue cost only; detect unchanged |
| `threecam_detect_latency_pose_overlap_probe` (diagnostic) | `pose_engine_only` with a debug delay (`ORANGE_POSE_DEBUG_DELAY_MS`, about 7.6) so pose lands under graph N+1 on purpose | what an overlap costs, the number rule D protects against | expected to fail the `infer_ms` gate; the size of the rise (up to P) is the result. Remove the flag afterwards or leave it diagnostic-only. |
| `threecam_detect_latency_pose_other_die` | `pose_registered` with `pose_worker.gpu_id` = the partner die (4, 2, 8) | the other-die placement | `infer_ms` unchanged; detect-to-pose within +0.15 ms of the same-die number; no crop-video or recorder regression on the partner die (its shard's routing gap and encode time unchanged) |
| `threecam_detect_latency_pose_a6000` | `pose_registered` with `pose_worker.gpu_id: 0`, run twice: citrus idle and citrus rendering | the A6000 placement and its tail | `infer_ms` unchanged; `pose_infer_ms` p99 < 1.5 ms under render (the roadmap's lever 7 threshold), else the placement is dead |

The last two need the cross-device code from section C before they can
run; list them so the spec names and gates are fixed now.

### Microbenchmarks that bound the table without the pipeline

- `trtexec --loadEngine=<pose engine> --iterations=2000 --useCudaGraph` on
  GPU 5 (idle die) and GPU 0 (A6000, idle and while citrus renders). This
  is P and the A6000 tail directly. Also with `--noDataTransfers` off, since
  the output copy is part of the stage.
- Peer copies of 147 KB and 0.79 MB between dies 3 and 4 and between die 3
  and GPU 0, from `scripts/check_cuda_peer_access.cu`, reporting whether
  peer access is enabled on each pair and the latency of a single copy plus
  a cross-device event wait. That is the hop column.

### What decides

- If `pose_registered` passes every gate with `infer_ms` flat, pose stays
  on the detect die and the work is rule D (the yield event and the budget
  drop) plus the inline path with its fix. Expected outcome given the
  numbers above.
- If `infer_ms` moves in `pose_engine_only` (no NVENC to blame), pose
  kernels are reaching the graph even without a backlog, and the other die
  is the answer; run `pose_other_die` next.
- If `infer_ms` is flat but `capture_to_pose_done` p95 is over 4.5 ms in
  `pose_registered` and under it in `pose_engine_only`, the tail is the
  same-die NVENC read on pose (the May pattern), and the choice is between
  accepting it for a best-effort stage and paying the other die's 0.1 ms
  hop to escape it.
- The A6000 enters only on its p99 under render; at 0.25 ms per pose it is
  the throughput winner if that number is under 1.5 ms, and not otherwise.

## F. Measured: Pose Real On The Detect Die (2026-09-04 and 2026-09-12)

Two runs of the `pose_engine_only` spec from section E, through
`scripts/run_detect_latency_spec.sh` with the shared worktree's release
binary (built at commit `e2417c4` on 2026-09-04, sources unchanged through
`5cf21a9`). Specs in this worktree:
`experiment_specs/threecam_detect_latency_pose_engine_only.json`,
`..._synthetic.json`, `twocam_..._synthetic.json`; config folder
`/tmp/orange_pose_engine_only_config_a16` (the validated fourcam config plus
`crop_pipeline.crop_size_px = 256`). No recorder (`immediate_recycle`), real
YOLO, real pose (`fish_v1`, 1x3x256x256 FP32 input, 3 keypoints), GPU event
timing on, PTP gate, 60 s.

**Run 1, 2026-09-04 16:48, three cameras, `roi_source: yolo_top_detection`.**
The verifier failed it ("pose event log validation failed"): the pose worker
enqueued zero frames on every camera. Every YOLO row in the run has
`detection_count: 0`, and so does every row of the day's other runs (the
10:35 engine-only run, the registered run, and the 30-minute endurance run,
179,900 rows): the tank had nothing detectable, so no crop was ever cut. The
run is still the cleanest "pose loaded but idle" reference, and it confirms
the section A reading that the crop producer only produces on a detection.
The other session's crop work the same evening reached the same conclusion
and adopted the synthetic centre box for empty-tank runs.

**Run 2, 2026-09-12 15:30, two cameras, `roi_source: synthetic_center_box`
every frame (64x64 box at the frame centre, crop 256).** Camera 2010093 had
no link carrier that day (its NIC port and 2010096's both show
`NO-CARRIER`), so the spec was reduced to 2010094 on die 1 and 2010095 on
die 7. The verifier passed. Pose processed 5901 of 5901 crops per camera,
zero drops, queue high-water 1, every row `no_result` by construction.

| Acquisition-to-detect, ms (mean / p95 / p99) | 2010094, die 1 | 2010095, die 7 |
|---|---|---|
| Run 1: pose loaded, idle (no crops) | 2.149 / 2.180 / 2.200 | 2.153 / 2.191 / 2.212 |
| Run 2: pose real on every frame | 2.275 / 2.326 / 2.357 | 2.278 / 2.329 / 2.356 |
| Delta | +0.126 / +0.146 / +0.157 | +0.125 / +0.138 / +0.144 |
| TensorRT graph (`infer_ms` mean / p95), run 1 | 1.976 / 1.982 | 1.977 / 1.982 |
| TensorRT graph, run 2 | 1.974 / 1.980 | 1.976 / 1.982 |
| Preprocess (`pre_ms`), run 1 and run 2 | 0.075, 0.076 | 0.076, 0.076 |

The pose stage itself (run 2, per-camera perf summary; ms mean / p50 / p95 /
p99 / max):

| Stage | 2010094 | 2010095 |
|---|---|---|
| detect done to crop-producer thread start | 0.167 / 0.084 / 0.403 / 0.503 / 1.205 | 0.170 / 0.085 / 0.410 / 0.489 / 1.358 |
| crop-producer start to crop enqueued | 0.096 / 0.095 / 0.112 / 0.125 / 0.221 | 0.097 / 0.096 / 0.114 / 0.129 / 0.372 |
| crop enqueued to pose thread start | 0.064 / 0.053 / 0.183 / 0.256 / 0.882 | 0.060 / 0.054 / 0.089 / 0.230 / 0.844 |
| pose start to pose done (crop-ready wait, resize, graph, output copy, sync) | 0.957 / 0.900 / 1.279 / 1.311 / 1.383 | 0.958 / 0.903 / 1.273 / 1.302 / 1.378 |
| capture to pose done | 3.559 / 3.525 / 3.974 / 4.146 / 5.876 | 3.564 / 3.485 / 3.980 / 4.149 / 4.702 |

Die utilisation from `nvidia_smi_dmon` (dies 1 and 7, mean over the run):
SM 20.7% idle-pose to 29.0% with pose, memory 11.7% to 12.9%. The 8.3-point
SM rise over a 10 ms period is about 0.83 ms of SM time per frame, which is
P, the pose graph plus resize, consistent with the 0.90 ms p50 above minus
the sync overhead.

### Where the 0.13 ms went

Per-column deltas, 2010094, run 2 minus run 1, steady state after 200
frames (ms, mean / p95). The GPU phases did not move; the host phases did:

| Column | Run 1 | Run 2 | Delta |
|---|---|---|---|
| `infer_ms` (graph) | 1.976 / 1.982 | 1.974 / 1.980 | -0.002 / -0.002 |
| `pre_ms` (preprocess) | 0.075 / 0.077 | 0.076 / 0.077 | +0.001 / 0.000 |
| `gap_ms` | 0.002 / 0.002 | 0.002 / 0.002 | 0.000 |
| `sync_ms` (GPU wait after enqueue) | 2.014 / 2.027 | 1.990 / 2.005 | -0.023 / -0.021 |
| `queue_ms` (sync minus infer) | 0.038 / 0.051 | 0.018 / 0.031 | -0.020 / -0.020 |
| `acquisition_to_worker_start_ms` | 0.023 / 0.030 | 0.080 / 0.093 | +0.057 / +0.063 |
| of which `yolo_queue_wait_ms` | 0.018 / 0.024 | 0.053 / 0.063 | +0.036 / +0.039 |
| of which `acquisition_to_yolo_enqueue_ms` | 0.005 / 0.008 | 0.027 / 0.032 | +0.021 / +0.024 |
| `worker_start_to_yolo_input_ready_ms` | 0.016 / 0.021 | 0.055 / 0.063 | +0.038 / +0.042 |
| `cpu_pre_sync_ms` | 0.056 / 0.065 | 0.111 / 0.124 | +0.055 / +0.059 |
| `cpu_post_sync_ms` | 0.065 / 0.092 | 0.135 / 0.182 | +0.071 / +0.090 |
| `acquisition_to_detect_done_ms` | 2.149 / 2.180 | 2.275 / 2.326 | +0.126 / +0.146 |

So the pose graph never touched the YOLO graph (the section D gate,
`infer_ms` unchanged, holds to 2 microseconds) and the GPU-side wait went
*down* slightly. The whole 0.13 ms is on the host: the acquisition thread
hands the frame to YOLO later, the YOLO thread wakes later, and its CPU work
before and after the graph is slower. This is the signature of two more
threads per camera (crop producer, pose worker) plus the pose event logger
competing for cores and for the CUDA driver lock on the same device
context; no thread is pinned in this run (`ORANGE_YOLO_AFFINITY` unset).

**A two-frame alternation.** Run 2 has a strict even/odd pattern that run 1
does not: odd recording frames cost +0.048 ms in acquisition-to-detect, and
the difference sits entirely in `cpu_post_sync_ms` (0.113 even, 0.157 odd)
while the crop producer thread also wakes later on odd frames (0.100 even,
0.233 odd) and the pose graph time is identical on both (0.958 / 0.959).
Consecutive frames switch sides of the median 77% of the time against 46%
in run 1. The 50-frame "cycle" spread of 0.066 ms the analysis script
reported is this alternation, not a GOP effect (there is no recorder). The
mechanism is not yet identified; candidates are a two-row cadence in the
pose event logger's write path or in the crop producer's drain, and CPU
scheduling of the three per-camera threads on shared cores. It is a host
effect, small, and worth one Nsight trace before any pinning experiment.

### Against the section E gates

| Gate | Result |
|---|---|
| `infer_ms` unchanged (2.00 ± 0.02 mean, p95 ≤ 2.06) | **Pass**: 1.974 / 1.980 |
| Acquisition-to-detect within +0.05 ms of the pose-idle run | **Fail**: +0.126 mean, +0.146 p95, all host-side |
| `pose_start_to_pose_done` p95 < 1.2 ms | **Fail, marginal**: 1.28 (p50 0.90) |
| Pose queue high-water ≤ 2, zero drops | **Pass**: 1, 0 |
| 5900/5900 frames, zero cap skips | **Pass** |

Reading: placement on the detect die is right for the GPU; the pose graph
fits in the idle window and leaves the YOLO graph alone. The cost is on the
CPU side and is the same order as the recorder's presence (the registered
path with the recorder and pose in noop mode measured 2.21 / 2.30 / 2.32 in
the other session's crop work). The levers for the remaining 0.13 ms are
host levers: the inline crop enqueue (one thread fewer to wake), core
pinning for the YOLO threads, and fewer per-frame driver calls on the pose
thread (a CUDA graph for resize plus inference plus output copy, and an
event-based completion instead of `cudaStreamSynchronize`). Off-die
placement would not remove any of it, since the threads and the driver
lock stay on the host.

### The pose stage on the A6000, one camera (2026-09-12 16:22)

Asked for after the design notes: what a big GPU buys the pose stage, even
though recording cannot follow it there. Spec
`experiment_specs/onecam_a6000_pose_engine_only_synthetic.json` (this
worktree): the other session's `onecam_engine_only_direct_read_omnifin_a6000`
(camera 2010094 acquired by GPUDirect into GPU 0, true direct read, the
A6000-built omnifin detect engine, `immediate_recycle`) plus the same pose
block as run 2. The pose engine is the GA107-built plan loaded on the GA102
(same compute capability, TensorRT warns and runs). Citrus was rendering on
GPU 0 throughout (about 30% SM before the run). Verifier passed, 5900 of
5900 crops, zero drops, queue high-water 1. Because the read is direct
there is no pool copy; the crop producer took its source-stage path (a
20 MB copy into a staging buffer on GPU 0, then the ROI).

First the engine alone, `trtexec --useCudaGraph --noDataTransfers`, 2000
iterations (ms):

| Device | median | mean | p95 | p99 |
|---|---|---|---|---|
| RTX A6000, GPU 0, citrus rendering | 0.433 | 0.461 | 0.676 | 1.115 |
| A16 die 1, idle | 0.720 | 0.719 | 0.721 | 0.721 |

Then the pipeline (ms, mean / p50 / p95 / p99), A6000 one camera against
run 2 on die 1:

| Stage | A6000, GPU 0 | A16 die 1 (run 2) |
|---|---|---|
| Acquisition to detect done | 0.910 / 0.911 / 0.955 / 0.997 | 2.275 / 2.274 / 2.326 / 2.358 |
| Detect graph (`infer_ms` mean / p95) | 0.706 / 0.710 | 1.974 / 1.980 |
| Preprocess (`pre_ms`) | 0.097 / 0.112 | 0.076 / 0.077 |
| Detect done to crop thread start | 0.061 / 0.061 / 0.065 / 0.068 | 0.167 / 0.084 / 0.403 / 0.503 |
| Crop thread to crop enqueued | 0.073 / 0.072 / 0.079 / 0.084 | 0.096 / 0.095 / 0.112 / 0.125 |
| Crop enqueued to pose thread start | 0.033 / 0.027 / 0.061 / 0.073 | 0.064 / 0.053 / 0.183 / 0.256 |
| Pose start to pose done | 0.849 / 0.823 / 0.992 / 1.160 | 0.957 / 0.900 / 1.279 / 1.311 |
| Capture to pose done | 1.926 / 1.893 / 2.082 / 2.203 | 3.559 / 3.525 / 3.974 / 4.146 |

Reading:

- **End to end, the A6000 is 1.6 ms faster (3.56 to 1.93 mean, 3.97 to
  2.08 p95),** and nearly all of it is the detect graph (1.97 to 0.71 ms).
  The detect-side numbers match what the other session measured on the
  A6000 without pose (0.89 / 0.94 / 1.01), so pose costs detect nothing
  there either.
- **The pose stage gained far less than the engine did.** trtexec says the
  graph is 0.43 ms on the A6000 against 0.72 on the die, but
  `pose_start_to_pose_done` only moved from 0.90 to 0.82 ms at p50. On the
  die the worker's overhead around the graph (crop-ready wait, resize
  launch, plain `enqueueV3`, output copy, `cudaStreamSynchronize`) is about
  0.18 ms; on the A6000 it is about 0.4 ms, and the p99 tail (1.16 ms, max
  6.3 ms) is the render context time-slicing that trtexec also showed (p99
  1.1 ms). A big GPU makes the fixed per-frame cost of the current
  synchronous pose worker the dominant term, which is the same conclusion
  as the host-side attribution in run 2 and the same fix: the fused graph
  with an event-based completion.
- **The thread hops were shorter here** (0.06 and 0.03 ms against 0.17 and
  0.06) with one camera's threads on the host instead of two; that is a
  CPU contention effect, not a GPU one.
- **Utilisation:** GPU 0 averaged 16.7% SM over the run with citrus
  included, so one camera's detect plus pose uses a small fraction of the
  card. Three cameras would share it well for pose; the other session's
  three-camera detect measurement (1.43 / 1.78 / 2.40) shows the batch-1
  graphs only partly overlap, which is where a batched launch would pay.

What this does not change: the 2026-09-05 deferral stands. Recording
cannot move to the A6000 (one NVENC, no peer access to the A16 dies), so
this is what a second card would buy the pose loop, not a configuration
the four-camera pipeline can run today. Artifacts:
`orange_data/exp/unsorted/onecam_a6000_pose_engine_only_synthetic_20260912_162217`.

### Updates to sections C and E from the other session's work

- **The A6000 row is dead.** The 2026-09-04 crop placement runs found no
  peer access between the A16 dies and the A6000 (`cudaIpcOpenMemHandle`
  fails), so a crop could only reach it through a host bounce. Big-GPU
  inference is deferred (`docs/big_gpu_inference_deferred_2026_09_05.md`);
  do not spend rig time there.
- **Crop placement and cost are settled.** The crop is always cut in the
  analytics process on the detect die from the pool copy at about 2.35 ms,
  about 0.03 ms of CPU; crop video goes to an external recorder with
  GOP-parity interleave. The 30-minute endurance run with every consumer on
  (full-frame recording, detect, crop, interleaved crop video, pose noop)
  measured 2.26 / 2.34 / 2.43 ms on the two-camera card and 2.25 / 2.31 /
  2.34 on the single-camera card. Pose real adds its host cost on top of
  that; a `pose_registered` run with the synthetic box is the number still
  missing.
- **Empty-tank rule.** Any pose or crop measurement on this rig must use
  `roi_source: synthetic_center_box`; the `yolo_top_detection` specs produce
  nothing and fail the pose-log gate.

Artifacts: run 1
`orange_data/exp/unsorted/threecam_detect_latency_pose_engine_only_20260904_164819`,
run 2
`orange_data/exp/unsorted/twocam_detect_latency_pose_engine_only_synthetic_20260912_153035`,
each with `latency_phases.json`, `Cam*_yolo_perf.csv`, `Cam*_pose_perf.csv`
and `Cam*_pose_events.jsonl`.
