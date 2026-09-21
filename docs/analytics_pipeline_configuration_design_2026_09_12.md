# Analytics Pipeline Configurations: A Provider Design For Detect, Pose, And Whatever Comes Next

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

One clarification on 2026-09-12 shortened the plan: the detector's boxes
already enclose only the head and the swim bladder, and the fish cannot
change apparent size, so the "head stage" is not a stage. It is the pose
stage with a smaller crop and a model retrained for it.

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
- **The detector's boxes are already head-and-bladder boxes.** The
  training labels came from background subtraction with erosion, which
  trimmed the tail, so a box encloses the head and the swim bladder and
  its centroid is the head centroid. The detector is therefore already the
  head localiser, and the existing three-keypoint pose model (eyes and
  bladder) is already a head model that is fed a 256 window mostly made of
  background.
- **Scale is constant.** The fish are held in about 3 mm of water under a
  top-down camera that views the whole space, so a fish cannot appear
  nearer or farther, and the head spans the same number of sensor pixels
  everywhere in the frame. A fixed crop size is correct; no per-frame
  scale normalisation is needed.
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

CUDA graphs are how a configuration with no CPU decisions is launched cheaply;
they are not what removes the decisions. Moving the decisions onto the
device is the design work. Once that is done, the graph executor and a
threaded executor can run the same pipeline configuration, and the graph one is simply
faster.

## What a pipeline configuration is, and what a CUDA graph is

The two are easy to conflate, so the vocabulary first.

**A CUDA graph is not an executable and not a thread.** It is an in-memory
recording of GPU work. A stream is put into capture mode, the ordinary
calls are issued (kernel launches, TensorRT's `enqueueV3`, async copies,
event records), and instead of running they are recorded as nodes with
their exact arguments: which kernel, which buffers, which sizes. Ending
capture yields a graph object; instantiating it yields an executable graph
handle; launching that handle on a stream replays the whole recording with
one driver call instead of one per node. It never leaves the process and it
is not a file. The detect engine already works this way:
`capture_infer_graph` records `enqueueV3` plus four device-to-host copies
once at startup, and every frame after that is one `cudaGraphLaunch`. The
price of that speed is that everything in the recording is frozen: buffer
addresses, tensor shapes, kernel arguments, order.

**A TensorRT engine is different again.** It is a compiled network: a file
on disk built for a specific GPU, loaded into an execution context in
memory. It is one node's worth of work inside a graph, not the graph.

**A pipeline configuration is an ordinary C++ object inside
`orange_client`,** built once per camera at startup from the spec. It owns
the engines it loaded, the kernels it will launch, the buffers between
them, the events it records, and, under the graph executor, the captured
graph of all of that. Its per-frame interface is `launch(entry)` and
`collect()`. `orange_client` stays one executable however many
configurations exist.

**"A different configuration" therefore means a second such object with
different constants,** built by the same code path. If the crop size
changes from 256 to 128, the crop buffer, the pose input tensor and the
kernel arguments all change, so the recording made for 256 is wrong for
128. Rather than patching a recording, the provider builds another object
with the new constants and captures its own graph. Both can exist for the
same camera at once; one is selected per recording session, never per
frame. `detect_only` and `detect_pose` are the same idea: two
configurations of the same stage code, each with its own frozen recording.

**Why not one recording for every case.** A recording cannot branch on data
in the general case, cannot change shapes, and cannot change pointers
without an explicit parameter update. The three things that vary per frame
are handled three ways: the source address by keeping the first preprocess
outside the recording; the "is there a detection" question by writing a
validity flag on the device and running anyway; everything else by building
a different configuration when the constants differ.

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
| `Roi` | one of: an NMS output block (top box by score), a fixed rectangle from the spec, a device-side state buffer (temporal prior), or, as a future option only, a keypoint pair from a pose output block | a crop origin and a fixed size, plus a validity flag, in a small device struct | This is the stage that replaces the CPU round trip. For `det_top_box` the origin is the box centroid minus half the crop, clamped to the frame, and the size is a constant from the spec (see the crop rule below). Un-letterboxing uses the constant ratio and offsets of the `Preprocess` that fed the engine. |
| `Warp` | source plane, `Roi` struct, output size | a crop plane (axis-aligned crop, resize, or rotated sample) | One kernel per crop. Two crops share one centroid and differ in size: the **video crop** (`crop_recording.crop_size_px`, whole fish, for the crop recorder and preview, the `CropFrame` payload) and the **pose crop** (`pose_worker.crop_size_px` = S, head-sized, into the pose input, never encoded). `Roi` emits both origins; each is clamped to the frame independently, so the pose crop is cut from the source, not as a sub-window of the video crop. |
| `Signal` | nothing | an event record on the stream | Mid-pipeline configuration markers: `detect_done` for the recorder gate and the lever 2d copy; `pose_done` for IPC. |
| `Emit` | one or more device output blocks | pinned host copies plus a completion event | The single point the CPU waits on. |

Each stage validates its inputs against the previous stage's outputs when
the pipeline configuration is built, and refuses to build on a mismatch. That is the
"arbitrary engine of the right shape" rule made general: it applies to the
detector as much as to pose, and it is what lets a SLEAP heatmap engine or
a different YOLO-pose sit in the same slot.

### Pipeline configurations

A pipeline configuration is an ordered list of stage configurations plus the buffers they
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

detect_pose, head-sized:                    (the production target; same pipeline configuration, smaller constants)
  Source(camera) -> Preprocess(640) -> Engine(det) -> Signal(detect_done)
  -> Roi(top box centroid, crop S) -> Warp(S) -> Preprocess(S, exact, no letterbox)
  -> Engine(pose trained on S crops) -> Signal(pose_done) -> Emit
```

There is no separate head stage. Because the box already encloses head and
bladder at constant scale, the head crop is the pose crop with a smaller
constant, and the pose model retrained on that crop is the head model. A
keypoint-driven second `Roi` (a crop centred between the eyes, rotated to
the body axis) remains expressible for a future rig where the box is not
the head, and is deliberately not part of this plan.

**Two crops, not one.** Today's code cuts a single `CropFrame` per camera
and feeds it to the crop recorder, the preview and the pose worker alike.
The video crop must keep capturing the whole fish (384 in the crop
recording specs) and the pose crop must be head-sized, so the pipeline
gains a second crop with the same centroid: `pose_worker.crop_size_px`
(exported as `ORANGE_POSE_CROP_SIZE_PX`; 0 means "same as the video crop",
which is today's behaviour). The device `Roi` kernel already computes both
origins (`crop_x/y` for the video crop, `pose_crop_x/y` for the pose crop)
and the comparison checks both against the CPU arithmetic. Wiring the pose
worker to cut and consume its own S-sized crop is part of migration step 3.

**The crop rule.** One constant per rig, set from data: S = the 95th
percentile of the box's longer side, times a margin of about 1.3 so an
eroded box never puts an eye on the edge, rounded up to a multiple of 32
(the YOLO stride). The box percentiles can come from the training labels
(same tank, same optics, constant scale) rather than from a rig run. The crop is cut at 1:1 and fed to the model without
resizing, so `Preprocess` runs in exact mode: no letterbox, no
interpolation, every pixel the sensor produced. If S came out above about
192 the model input could be smaller than S with one resize, but at
constant scale that is a training-time choice, not a per-frame one. S is
not known yet; see step 0 of the migration.

In the spec this is one block, `fixed.analytics`, that replaces the
independent `yolo_worker` and `pose_worker` blocks over time (both remain
accepted and are translated into the equivalent pipeline configuration):

```json
"analytics": {
  "executor": "graph",
  "pipeline": "detect_pose",
  "stages": {
    "det":  {"engine_path": "...yolo11n...engine", "input": 640, "output_format": "yolo_nms"},
    "roi":  {"source": "det_top_box", "video_crop_size_px": 384, "pose_crop_size_px": 128},
    "pose": {"engine_path": "...head crop model...engine", "input": 128, "channels": 3,
             "dtype": "fp32", "output_format": "yolo_pose", "skeleton_path": "..."}
  }
}
```

Every pipeline configuration emits the same **result record**: a validity flag per stage,
detections (optional), poses per stage (optional), ROI geometry, and
per-stage GPU timings from event pairs. The event log, the shaman v2 IPC
slot, the perf CSV and the verifier consume that record and never ask
which configuration produced it. Absent stages are absent fields, which the
existing `pose_status` and `detection_status` enums already express.

### Executors and the provider

```cpp
struct AnalyticsPipeline {
    // Built once per camera by the provider. Owns buffers, engines, events.
    virtual cudaEvent_t launch(WORKER_ENTRY* entry) = 0;   // enqueue everything; returns the Emit event
    virtual const ResultRecord& collect() = 0;              // valid after the Emit event; decodes host buffers
    virtual cudaEvent_t signal(SignalId id) const = 0;      // detect_done, pose_done: for the recorder gate and IPC
    virtual const PipelineContract& contract() const = 0;    // stage list, shapes, engines, for the snapshot
};

class AnalyticsProvider {
    std::unique_ptr<AnalyticsPipeline> build(const AnalyticsSpec&, const CameraParams&, cudaStream_t);
};
```

Two executors implement `AnalyticsPipeline`:

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

**Data-dependent control flow.** A pipeline configuration cannot branch on the CPU. Two
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
   pipeline configuration if `Warp` writes into configuration-owned buffers. This is the
   smallest change and preserves the proven capture.
2. Update the `Source` node's kernel parameters per launch with
   `cudaGraphExecKernelNodeSetParams` (microseconds, no re-instantiate).
3. Read through a device-side pointer table indexed by ring slot, written
   by acquisition. Fully static graph, but couples the pipeline configuration to the
   SDK's ring layout.

For any stage that must read the *camera buffer* after detection (the
crop at 1:1 before the pool copy lands), option 1 means the `Warp` source
is also outside the graph or uses option 2. Decide when the fused configuration
is prototyped; both are cheap.

**Fixed shapes.** A configuration is one shape set. A different crop size, input
size, or engine is a different configuration, built by the same provider. Two
pipeline configurations may be built for one camera (for example `detect_only` and
`detect_pose`) and switched between recordings, not between frames.

**CPU consumers and other clocks.** The recorder, crop video, display and
the log writer stay threads and attach to the configuration's signals and the
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
| Head components at full resolution | `detect_pose` with the head-sized constant S | no new stage; a pose model retrained on S crops of the existing labels (yolo11n-pose exported to ONNX, built on the A16 with the existing engine build script) and the exact-mode `Preprocess` |
| Several cameras on one large GPU (deferred) | a batched configuration: `Gather` stage across cameras' input tensors, batch-N engines | the gather stage and a per-period scheduler; the same stage contracts |
| A SLEAP or other framework's engine | any pipeline configuration | nothing; the `Engine` contract and the `heatmap` decoder cover it |

## Migration, in order, each step measurable

0. **Measure S.** One run with a fish in the tank and
   `roi_source: yolo_top_detection`; the YOLO event log records every box,
   and the 5th, 50th and 95th percentiles of the longer side give S. In
   parallel, re-crop the persisted training set around the same box
   centroids at S (labels are in source pixels, so this is a re-crop, not
   new annotation), train yolo11n-pose on those crops, export ONNX, build
   the engine on an A16 die, and check its contract against the `Engine`
   stage (1x(5+3K)xN output; the YOLO11 pose head decodes exactly as the
   current v8 one).
1. **Result record and signals.** Define `ResultRecord` and route the
   event log, IPC slot and perf CSV through it, with the current threaded
   code populating it. No behaviour change; the verifier must pass the
   registered spec unchanged.
2. **Device-side `Roi` for the top box.** A kernel that reads `num_dets`,
   `boxes`, `scores` on the device, un-letterboxes with the configuration's
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
4. **`pose_only` and `detect_only` as pipeline configurations.** Mostly spec and
   validation; run the existing engine-only specs through `detect_only`
   and confirm identical numbers.
5. **Swap in the head-sized configuration.** Change the three constants in
   the spec: the video crop size where it already lives, `pose_worker.crop_size_px`
   = S, and the pose engine path; nothing structural. Gate: pose graph time
   on a die below the current 0.72 ms, keypoint error on held-out crops no
   worse than the 256 model's, and every other gate from step 3 unchanged.
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
  of host buffers. Not needed at 100 fps with a 3 ms pipeline configuration.
- **The lever 2d copy and the pose graph overlap.** With pose inside the
  pipeline configuration, the pool copy (issued after `detect_done`) runs alongside the
  pose engine. Expect a cost to pose of the order the copy cost detect
  (0.26 ms); measure it, and if it matters, order the copy after
  `pose_done` at the cost of a later recorder handoff, or crop from the
  camera buffer and let the copy wait.
- **Conditional nodes.** Available from CUDA 12.4; the rig's toolkit
  version needs checking before relying on them. Run-and-mask needs
  nothing.
- **Two configurations per camera.** Switching configurations between recordings is
  easy; between frames it is not, and should not be a goal.
- **Per-configuration timing.** Event pairs inside the graph give per-stage GPU
  time for free; the `pose_pre_ms` / `pose_infer_ms` columns proposed in
  the review fall out of this rather than needing separate plumbing.

## Decision requested

Adopt the three-layer structure (stages, pipeline configurations, executors) and the
one-CPU-wait invariant as the target, keep the threaded executor as the
migration and fallback path, and take the migration steps in the order
above with each step's gate as written. Step 2 is the first thing to build;
it is independent of graphs and removes the CPU round trip that the pose
stage currently waits on.
