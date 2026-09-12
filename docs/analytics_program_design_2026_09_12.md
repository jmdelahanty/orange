# Analytics Programs: A Provider Design For Detect, Pose, And Whatever Comes Next

Design note, 2026-09-12. Follows `docs/pose_second_stage_review_2026_09_04.md`
(measurements) and `docs/pose_stage_design_notes_2026_09_12.md` (the fused
graph, the head crop, arbitrary engines). This document proposes how the
analytics side of the pipeline should be structured so that detect-only,
detect-plus-pose, pose-only on a close-up, and a third head stage are all
configurations of one mechanism rather than four thread topologies. Nothing
here is implemented.

## The problem in one sentence

Every analytics stage today is a thread, because every stage has a CPU
decision between two pieces of GPU work, and each new stage adds a thread, a
queue, a wake-up, and 0.2 to 0.4 ms of p95. The measurements say the GPU
work is fine (the detect graph is untouched by pose, the pose graph is 0.72
ms on a die and 0.43 ms on the A6000) and the host slack is what grows.

## Facts the design rests on

All verified in the tree at `5cf21a9` / review branch `d8d8c79`.

- **NMS is already on the device.** The detect engine ends in EfficientNMS
  and binds four outputs: `num_dets`, `boxes`, `scores`, `labels`
  (`src/yolov8_det.cpp:395-401`). The boxes exist in device memory the
  moment the engine finishes, in letterboxed input coordinates, sorted by
  score.
- **The detect CUDA graph is `enqueueV3` plus the device-to-host copies**
  of those four outputs into pinned host buffers
  (`src/yolov8_det.cpp:257-317`). Preprocess (the letterbox kernel from the
  camera buffer into the fixed input tensor) runs *before* the graph on the
  same stream, which is how the per-frame source address is handled: the
  graph only ever sees the fixed input buffer.
- **Today the crop waits for the CPU anyway.** After the completion event
  the YOLO thread runs `postprocess` (un-letterbox, clamp, build
  `entry->detections`), then the crop producer thread picks the
  highest-score box with `max_element` and enqueues the crop
  (`src/crop_producer_worker.cpp:508`). That round trip is a choice of the
  current code, not a requirement of the model.
- **Delayed consumers attach by event.** The recorder handoff, crop video,
  display and snapshot wait on the entry's ready event through
  `delayed_consumer_event()`; the recorder is additionally gated on
  detect done. None of them care how detections were produced.
- **Measured host slack with pose on** (two cameras, 2026-09-12): thread
  hops 0.17 and 0.06 ms mean, 0.40 and 0.18 ms p95; pose worker overhead
  around its graph 0.18 ms on a die and about 0.4 ms on the A6000; +0.13 ms
  on acquisition-to-detect from the extra threads, none of it GPU.

## The invariant

**One CPU wait per frame per camera.** Everything between the ingress event
and that wait is device work on one stream. Threads exist only at
boundaries where a different clock or a different process is inherent
(acquisition, the recorder, crop video, display, log writing). The thread
count scales with the number of sinks, not the number of analytics stages.

CUDA graphs are how a program with no CPU decisions is launched cheaply;
they are not what removes the decisions. Moving the decisions onto the
device is the design work. Once that is done, the graph executor and a
threaded executor can run the same program, and the graph one is simply
faster.

## Three layers

### Stages

A stage is a device operation with fixed buffers and a declared contract:
what it consumes, what it produces, shapes and dtypes. Stages never own a
thread and never synchronize. They are configured at build time and
launched per frame.

| Stage | Consumes | Produces | Notes |
|---|---|---|---|
| `Source` | a frame in device memory (camera buffer, pool copy, or a configured window of either) | a mono uint8 plane with pitch | The only stage whose address changes per frame. See "the varying address" below. |
| `Preprocess` | mono plane, target size, channels (1 or 3), dtype (FP32 or FP16), mode (letterbox or exact) | engine input tensor | The existing fused kernel with a channel and dtype option. |
| `Engine` | an input tensor matching the plan's binding | the plan's output bindings, in device memory | One TensorRT context, warmed before capture. Contract: binding names, shapes, dtypes, and an `output_format` the decoder understands (`yolo_nms`, `yolo_pose`, `heatmap`). |
| `Roi` | one of: an NMS output block (top box by score), a keypoint pair from a pose output block, a fixed rectangle from the spec, a device-side state buffer (temporal prior) | a crop origin, size, and optional rotation, plus a validity flag, in a small device struct | This is the stage that replaces the CPU round trip. Un-letterboxing uses the constant ratio and offsets of the `Preprocess` that fed the engine. |
| `Warp` | source plane, `Roi` struct, output size | a crop plane (axis-aligned crop, resize, or rotated sample) | One kernel. Also the producer of the `CropFrame` payload for crop video and preview when those are on. |
| `Signal` | nothing | an event record on the stream | Mid-program markers: `detect_done` for the recorder gate and the lever 2d copy; `pose_done` for IPC. |
| `Emit` | one or more device output blocks | pinned host copies plus a completion event | The single point the CPU waits on. |

Each stage validates its inputs against the previous stage's outputs when
the program is built, and refuses to build on a mismatch. That is the
"arbitrary engine of the right shape" rule made general: it applies to the
detector as much as to pose, and it is what lets a SLEAP heatmap engine or
a different YOLO-pose sit in the same slot.

### Programs

A program is an ordered list of stage configurations plus the buffers they
share. The provider builds it from the spec. The four we can name today:

```
detect_only:
  Source(camera) -> Preprocess(640, 3ch, fp32, letterbox) -> Engine(det, yolo_nms)
  -> Signal(detect_done) -> Emit(det outputs)

detect_pose:
  detect_only
  -> Roi(top box of det, crop 256) -> Warp(256) -> Preprocess(256, 3ch, fp32, exact)
  -> Engine(pose, yolo_pose) -> Signal(pose_done) -> Emit(det outputs, pose outputs, roi)

pose_only:                                  (close-up, embedded fish, no detector)
  Source(camera or window) -> Roi(fixed rectangle from spec) -> Warp(W) -> Preprocess(...)
  -> Engine(pose) -> Signal(pose_done) -> Emit(pose outputs, roi)

detect_pose_head:
  detect_pose
  -> Roi(eyes from pose output, size 128, rotate to eye-bladder axis) -> Warp(128, rotate)
  -> Preprocess(128, 1ch, fp16, exact) -> Engine(head, heatmap) -> Emit(+ head outputs, roi2)
```

In the spec this is one block, `fixed.analytics`, that replaces the
independent `yolo_worker` and `pose_worker` blocks over time (both remain
accepted and are translated into the equivalent program):

```json
"analytics": {
  "executor": "graph",
  "program": "detect_pose",
  "stages": {
    "det":  {"engine_path": "...yolo11n...engine", "input": 640, "output_format": "yolo_nms"},
    "roi":  {"source": "det_top_box", "crop_size_px": 256},
    "pose": {"engine_path": "...fish_v1...engine", "input": 256, "channels": 3,
             "dtype": "fp32", "output_format": "yolo_pose", "skeleton_path": "..."},
    "roi2": {"source": "pose_keypoints", "anchor": ["eye_left", "eye_right"],
             "axis": ["eye_mid", "bladder"], "crop_size_px": 128, "rotate": true},
    "head": {"engine_path": "...", "input": 128, "channels": 1, "dtype": "fp16",
             "output_format": "heatmap", "output_stride": 4}
  }
}
```

Every program emits the same **result record**: a validity flag per stage,
detections (optional), poses per stage (optional), ROI geometry, and
per-stage GPU timings from event pairs. The event log, the shaman v2 IPC
slot, the perf CSV and the verifier consume that record and never ask
which program produced it. Absent stages are absent fields, which the
existing `pose_status` and `detection_status` enums already express.

### Executors and the provider

```cpp
struct AnalyticsProgram {
    // Built once per camera by the provider. Owns buffers, engines, events.
    virtual cudaEvent_t launch(WORKER_ENTRY* entry) = 0;   // enqueue everything; returns the Emit event
    virtual const ResultRecord& collect() = 0;              // valid after the Emit event; decodes host buffers
    virtual cudaEvent_t signal(SignalId id) const = 0;      // detect_done, pose_done: for the recorder gate and IPC
    virtual const ProgramContract& contract() const = 0;    // stage list, shapes, engines, for the snapshot
};

class AnalyticsProvider {
    std::unique_ptr<AnalyticsProgram> build(const AnalyticsSpec&, const CameraParams&, cudaStream_t);
};
```

Two executors implement `AnalyticsProgram`:

- **Graph executor.** At build time, after warm-up, captures every stage
  from `Preprocess` through `Emit` into one CUDA graph on the camera's
  stream (the detect engine's existing capture is the first segment; the
  pose engine's `enqueueV3` captures the same way; `Signal` becomes an
  event record node). `launch()` updates the per-frame parameters and
  launches the graph. The entry's completion event is the `Emit` event.
- **Threaded executor.** Runs the same stage list with today's shape: the
  YOLO thread runs `Preprocess` and `Engine(det)`, the crop producer
  thread runs `Roi` and `Warp`, the pose thread runs the rest. It exists
  for stages that cannot be captured, for A/B measurements against the
  graph executor, and so that the migration can land stage by stage.

Which executor is used is a spec key. Both produce the same result record
and the same signals, so nothing downstream changes between them.

The analytics thread is the existing YOLO worker, renamed in time: it pops
the entry, calls `launch()`, waits once on the `Emit` event, calls
`collect()`, publishes, issues the lever 2d pool copy after the
`detect_done` signal exactly as now, and releases the entry.

## The four things graphs do not do, and how the design handles them

**Data-dependent control flow.** A program cannot branch on the CPU. Two
policies, chosen per `Roi`: *run-and-mask* (the `Roi` writes a validity
flag; downstream stages run on whatever is there; `Emit` carries the flag
and the CPU drops invalid results), which costs the pose graph's time on
frames with no detection and is affordable inside the 7.5 ms idle window;
or *conditional nodes* (CUDA 12.4 `IF` nodes keyed on a device flag), which
skip the work. Start with run-and-mask; it is simpler and the cost is
measured.

**The varying address.** The camera buffer and the pool entry differ per
frame, and a captured graph has fixed pointers. Three options, in order of
preference:

1. Keep `Preprocess` for the *first* stage outside the graph, as the
   detect graph does today. One kernel launch per frame from the varying
   address into the fixed input tensor; the graph starts at `Engine(det)`.
   Later stages read the pool copy or the crop buffer, which are fixed per
   program if `Warp` writes into program-owned buffers. This is the
   smallest change and preserves the proven capture.
2. Update the `Source` node's kernel parameters per launch with
   `cudaGraphExecKernelNodeSetParams` (microseconds, no re-instantiate).
3. Read through a device-side pointer table indexed by ring slot, written
   by acquisition. Fully static graph, but couples the program to the
   SDK's ring layout.

For any stage that must read the *camera buffer* after detection (the
crop at 1:1 before the pool copy lands), option 1 means the `Warp` source
is also outside the graph or uses option 2. Decide when the fused program
is prototyped; both are cheap.

**Fixed shapes.** A program is one shape set. A different crop size, input
size, or engine is a different program, built by the same provider. Two
programs may be built for one camera (for example `detect_only` and
`detect_pose`) and switched between recordings, not between frames.

**CPU consumers and other clocks.** The recorder, crop video, display and
the log writer stay threads and attach to the program's signals and the
entry's events exactly as the delayed consumers do now. `Warp` producing
the `CropFrame` payload keeps crop video and preview working without a
crop producer thread; they lease the buffer after the `pose_done` (or
`detect_done`) signal, and the `CropFrame` pool's recycle event still
gates reuse.

## What each future needs

| Future | Program | New code beyond the provider |
|---|---|---|
| Detections only (current production without pose) | `detect_only` | none; it is the existing graph plus `Emit` |
| Two-stage detect and pose (current pose work) | `detect_pose` | the `Roi(det_top_box)` kernel and `Warp`; the pose decoder as a `collect()` step |
| Pose only on a close-up of an embedded fish | `pose_only` | `Roi(fixed)`; a `Source` window; nothing else |
| Head components at full resolution | `detect_pose_head` | `Roi(keypoints)` with rotation, `Warp(rotate)`, a heatmap decoder, the one-channel FP16 `Preprocess` option |
| Several cameras on one large GPU (deferred) | a batched program: `Gather` stage across cameras' input tensors, batch-N engines | the gather stage and a per-period scheduler; the same stage contracts |
| A SLEAP or other framework's engine | any program | nothing; the `Engine` contract and the `heatmap` decoder cover it |

## Migration, in order, each step measurable

1. **Result record and signals.** Define `ResultRecord` and route the
   event log, IPC slot and perf CSV through it, with the current threaded
   code populating it. No behaviour change; the verifier must pass the
   registered spec unchanged.
2. **Device-side `Roi` for the top box.** A kernel that reads `num_dets`,
   `boxes`, `scores` on the device, un-letterboxes with the program's
   constants, writes the ROI struct. The crop producer reads the struct
   instead of `entry->detections`. Gate: crop origin identical to the CPU
   path on every frame of a synthetic run (a diff of the pose event log's
   crop fields), detect latency unchanged.
3. **Graph executor for `detect_pose`, behind `analytics.executor`.**
   Capture from `Engine(det)` through `Emit` with `Preprocess` outside
   (option 1). Pose worker becomes a decoder call on the analytics thread.
   Gate: `infer_ms` unchanged, capture-to-pose-done p95 under 3.3 ms on the
   two-camera synthetic spec (today 3.97), thread count per camera down by
   two.
4. **`pose_only` and `detect_only` as programs.** Mostly spec and
   validation; run the existing engine-only specs through `detect_only`
   and confirm identical numbers.
5. **Head stage.** `Roi(keypoints)`, `Warp(rotate)`, heatmap decoder, the
   one-channel preprocess option. Needs the head model first.
6. **Retire the threaded executor for stages the graph covers**, once the
   endurance spec has passed on the graph executor with every consumer on.

## Risks and open questions

- **Capture hygiene.** `enqueueV3` capture is proven for the detect engine.
  The pose engine must be warmed before capture (TensorRT allocates on
  first enqueue), and no stage may call a synchronizing API inside
  capture. The NPP-based paths in the old preprocess are not in the graph
  today and should stay out.
- **Pinned host buffers and pipelining.** With one frame in flight per
  camera the single pinned output set is fine. If the analytics thread
  ever launches frame N+1 before collecting N, `Emit` needs a small ring
  of host buffers. Not needed at 100 fps with a 3 ms program.
- **The lever 2d copy and the pose graph overlap.** With pose inside the
  program, the pool copy (issued after `detect_done`) runs alongside the
  pose engine. Expect a cost to pose of the order the copy cost detect
  (0.26 ms); measure it, and if it matters, order the copy after
  `pose_done` at the cost of a later recorder handoff, or crop from the
  camera buffer and let the copy wait.
- **Conditional nodes.** Available from CUDA 12.4; the rig's toolkit
  version needs checking before relying on them. Run-and-mask needs
  nothing.
- **Two programs per camera.** Switching programs between recordings is
  easy; between frames it is not, and should not be a goal.
- **Per-program timing.** Event pairs inside the graph give per-stage GPU
  time for free; the `pose_pre_ms` / `pose_infer_ms` columns proposed in
  the review fall out of this rather than needing separate plumbing.

## Decision requested

Adopt the three-layer structure (stages, programs, executors) and the
one-CPU-wait invariant as the target, keep the threaded executor as the
migration and fallback path, and take the migration steps in the order
above with each step's gate as written. Step 2 is the first thing to build;
it is independent of graphs and removes the CPU round trip that the pose
stage currently waits on.
