# Handoff: Pose As A Second Stage, Device ROI, And The 192 px Head Model

Written 2026-09-16 at the end of a long session so the next one can start
from files. Everything below is either in this branch, in a published page,
or in the memory notes under
`~/.claude/projects/-home-jeremy-orange-jeremy/memory/`.

## Where things are

| What | Where |
|---|---|
| Code branch (canonical from here on) | `/home/jeremy/orange-device-roi-20260912`, branch `agent/analytics/device-roi-20260912`, from shaman-v2 `5cf21a9`; HEAD `c8ae644` plus this handoff commit. Built: `targets/release/orange_client`, `targets/release/detect_roi_tests` |
| Shared worktree (another session's) | `/home/jeremy/orange-gop-split-a16`, branch `agent/acquisition/shaman-v2-authoritative-20260824`, moving (`ca817a0`, dirty). Treat as read-only; its `scripts/run_detect_latency_spec.sh` and release binary are what the sudo wrapper accepts |
| Review worktree (docs only, superseded) | `/home/jeremy/orange-pose-review-2026-09-04`, branch `review/pose-second-stage-2026-09-04` |
| Documents (all in this branch's `docs/`) | `pose_second_stage_review_2026_09_04.md` (sections A to F, measurements), `pose_stage_design_notes_2026_09_12.md` (SLEAP, fused graph, arbitrary engines, camera-buffer hold), `analytics_pipeline_configuration_design_2026_09_12.md` (the stages/configurations/executors design, two crops), `pose_head_192_inspection_2026_09_16.md` (the new model), this file |
| Published pages | review https://claude.ai/code/artifact/6ce8c5d0-60db-4468-a1d1-616de66ca584 · design notes https://claude.ai/code/artifact/652a4d0a-93e2-4a3a-9977-b21019e82db7 · pipeline configurations https://claude.ai/code/artifact/cd069bb3-207a-4185-8cfc-a21bb3b410bf · float rounding explainer https://claude.ai/code/artifact/58e17ca1-ae62-4055-8be5-530349626433 · inspection report (see below) |
| Page builder | scratchpad of the old session: `build_page.py` + `<name>.json` configs; rebuild from the markdown and republish to the same URL. If the scratchpad is gone, the markdown is the source of truth |
| New model bundle | `~/orange_data/pose/pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915/` (digests verified 2026-09-16) |
| Training request sent to the training agent | `~/pose_head_crop_training_request_2026_09_14.md` |
| Inspection brief received | `~/pancake0_pose_head_192_acquisition_inspection_prompt.md` |

## What was established (numbers you should not re-derive)

- Detect: yolo11n FP16 graph 2.00 ms flat on a GA107 die; acquisition-to-detect 2.20 / 2.29 with the recorder, 2.15 / 2.19 engine-only after lever 2d (copy after detection). Camera buffer held until the pool copy completes at 2.35 ms; that is by design and costs nothing at a ring of 100.
- Pose real on every frame on the detect die (2026-09-12, two cameras, synthetic centre box, 256 crop, fish_v1 yolov8n-pose): `infer_ms` unchanged (1.974 vs 1.976), acquisition-to-detect +0.13 ms mean / +0.15 p95, all host-side; pose stage 0.90 p50 / 1.28 p95; capture-to-pose-done 3.56 mean / 3.97 p95. Odd/even frame alternation of 0.05 ms in the YOLO thread's post-sync work, mechanism unknown.
- Pose engine alone: 0.720 ms on an idle A16 die, 0.433 ms on the A6000 under citrus (trtexec). One camera into the A6000: capture-to-pose-done 1.93 / 2.08 / 2.20; pose stage only 0.82 p50 because the worker's fixed per-frame overhead dominates once the graph is fast. A6000 for production stays closed (no peer access with the A16 dies, one NVENC; decided 2026-09-05).
- EfficientNMS is in the detect engine; boxes are on the device when the graph finishes; today's crop waits for the CPU un-letterbox and `max_element` anyway. The device ROI kernel removes that dependency and is bit-identical to the CPU path (1992 unit-test cases).
- The detector's boxes enclose head + swim bladder only (erosion trimmed the tail) and scale is constant (3 mm water, top-down, whole tank), so the head crop is the pose crop with a smaller constant: S. S came back from the training labels as **192**.
- Two crops per camera are needed: the video crop (whole fish, 384 in the crop-recording specs) and the pose crop (192). Today's code has one crop; the spec key `pose_worker.crop_size_px` and the kernel's second origin exist, the pose worker does not yet consume a separate crop.

## Design decisions taken

- Target architecture: stages with declared contracts, pipeline
  configurations built from the spec (`detect_only`, `detect_pose`,
  `pose_only`), a graph executor (one CUDA graph per configuration, one
  CPU wait per frame per camera) and a threaded executor for migration and
  fallback. The word is "pipeline configuration", not "program".
- Ordering rule against the next frame's detect graph: same-stream
  serialisation once the graph executor exists; until then priority plus a
  yield wait plus a budget drop, with `infer_ms` as the proof.
- Head crop: centred on the detection centroid, fixed size S, 1:1, no
  rotation, no coarse-to-fine stage.
- Migration order: (0) S from labels, done; (1) result record; (2) device
  ROI, done and wired behind a flag; (3) graph executor for `detect_pose`
  with preprocess outside the graph; (4) `pose_only`/`detect_only` as
  configurations; (5) swap in the 192 model and crop; (6) retire the
  threaded executor where covered.

## Code state on the branch

- `src/detect_roi.{h,cu}`: kernel and host reference; `DetectRoi` carries
  video and pose crop origins; `DetectRoiParams` carries both sizes.
- `tools/detect_roi_tests.cpp`: 1992 cases, independent oracle, CMake
  target `detect_roi_tests` under `BUILD_TESTING`.
- `src/yolo_worker.cpp/.h`: launches the kernel after the graph when
  `ORANGE_ANALYTICS_DEVICE_ROI` is on, mirrors to a pinned per-entry slot
  (`WORKER_ENTRY::d_detect_roi/h_detect_roi`, `src/video_capture.h`),
  compares after postprocess, counters + summary line, perf columns
  `device_roi_valid`, `device_roi_match` (1 match, 0 mismatch, 2 mask-trimmed,
  3 synthetic). The crop producer still uses the CPU path.
- `src/orange_headless_client.cpp`: spec keys `fixed.analytics_device_roi`
  and `pose_worker.crop_size_px` (env `ORANGE_ANALYTICS_DEVICE_ROI`,
  `ORANGE_POSE_CROP_SIZE_PX`); `src/yolo_runtime_flags.h` and
  `src/project.cpp` carry the flag in the snapshot.
- `scripts/run_detect_latency_spec.sh --orange-client <binary>`.
- Specs: `experiment_specs/{threecam,twocam}_device_roi_realfish_engine_only{,_off}.json`
  (real detections, device ROI on/off, provisional `crop_size_px 128` on the
  "on" specs), `threecam/twocam_detect_latency_pose_engine_only{,_synthetic}.json`,
  `onecam_a6000_pose_engine_only_synthetic.json`.
- Config folder used by those specs: `/tmp/orange_pose_engine_only_config_a16`
  (validated fourcam config + `crop_pipeline.crop_size_px 256`). `/tmp`, so
  recreate after a reboot (four camera JSONs with the `crop_pipeline` block).

## Rig facts and traps

- sudo works only for `/usr/local/bin/orange-*`; the benchmark wrapper
  accepts only four binary paths, none of them this worktree. Add
  `DEVICE_ROI_ORANGE_CLIENT="/home/jeremy/orange-device-roi-20260912/targets/release/orange_client"`
  after line 9 and to the `case` on line 124 (needs your password), or
  merge and build in the shared worktree.
- Every run since 2026-09-04 had zero detections (empty tank); synthetic
  boxes bypass the engine and cannot test the device ROI. Real fish required
  for the comparison and for any keypoint check.
- Camera link state changes; check `ip -br link` and `--list-cameras`
  before choosing the three- or two-camera spec.
- Before launching, `pgrep -f 'orange_clien[t] --mode local'` (the bracket
  keeps the guard from matching itself). A colliding launch fails at camera
  open with `GVCP ACK error`; do not force-reboot a camera without that
  check.
- The runner must be invoked by absolute path from the worktree whose binary
  you want, or with `--orange-client`.
- `ORANGE_INLINE_CROP_PRODUCER` has a lifetime hole (no source-release
  event with `release_source_entry=false`); leave it off.

## Next steps, in order

1. Wrapper path (you), then the real-fish comparison runs
   (`threecam_device_roi_realfish_engine_only` and `_off`) once the tank has
   a fish. Pass: summary `mismatch=0`, `device_roi_match == 1` on every
   detected frame, detect latency within 0.03 ms of the control.
2. Build the 192 engine per the inspection report section 6 on GPU 5,
   record the binding line, benchmark with trtexec, write the manifest.
3. Probe-digest harness (inspection report claim 3), then the spec copy
   with the new engine and `crop_pipeline.crop_size_px 192`, single crop,
   real detections. Compare the pose event log against the 256 model on the
   same frames.
4. Wire `pose_worker.crop_size_px` into the crop producer so crop video at
   384 and pose at 192 coexist.
5. Graph executor for `detect_pose` behind `analytics.executor`, gate
   `infer_ms` unchanged and capture-to-pose-done p95 under 3.3 ms.

## Open questions

- Parity tolerance for TensorRT versus the training runtime: unowned.
- The 0.05 ms odd/even alternation with pose on: one Nsight trace.
- Keypoint label naming (`bladder` vs `swim_bladder`) and threshold spec keys.
- Whether the shaman-v2 remote head has moved beyond what the shared
  worktree shows; fetch before rebasing.

## Addendum 2026-09-16 (evening): 192 engine built

Built on GPU 5 (NVIDIA A16, `GPU-840b0989…`, CC 8.6, TensorRT 10.0.1,
driver 535.183.06) with direct `trtexec` at the production tier
(`--fp16 --builderOptimizationLevel=5 --avgTiming=32
--profilingVerbosity=detailed`, plus `--timingCacheFile`, `--exportLayerInfo`,
`--exportProfile`). Build time 279.8 s, engine 7.95 MiB. Everything is under
`~/orange_data/pose/pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915/engines/`
with a `SHA256SUMS`; the active 256 engine in `~/` was not touched.

- Engine: `pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915_a16_gpu5_trt100_fp16_bo5_avg32.engine`
- Bindings: input `images` FP32 `1x3x192x192`; output `output0` FP32 `1x14x756`.
- Manifest: `<stem>.manifest.json`, written by
  `scripts/write_pose_tensorrt_engine_manifest.py` (new; takes the identity
  preprocessing from `pose_model_input_contract.json` instead of the detect
  writer's hardcoded letterbox/114 fields). Status `candidate`, parity pending,
  `selector_activation: false`. The Palette export manifest (gap 7) is still
  absent and is recorded as such.
- `scripts/build_tensorrt_detect_engine.sh` was NOT used: it requires the gap-7
  file and, fed the canonical manifest, would silently skip the SHA check and
  record `[1,3,640,640]` with detect preprocessing. Four small edits would make
  it pose-capable (`--task`, optional/adapted manifest, task profile in the
  writer, timing-cache and `--useCudaGraph` benchmark flags).

trtexec on the same idle die, `--warmUp=1000 --duration=10`, GPU compute time
mean / p95 / p99 in ms:

| Engine | plain enqueue | `--useCudaGraph` |
|---|---|---|
| 256 cedar (yolov8n-pose, active) | 0.840 / 0.849 / 0.852 | 0.722 / 0.726 / 0.727 |
| 192 head (yolo11n-pose, new) | 0.734 / 0.740 / 0.745 | 0.575 / 0.578 / 0.581 |

So 192 saves 0.11 ms plain and 0.15 ms graphed; graphing saves 0.12 to 0.16 ms
on either engine. The pose worker today does plain enqueue (no graph) and a
blocking stream sync. The remembered "0.720 ms on an idle die" for the 256
engine was the graphed number.

Two-stage speedup assessment (read-only, 2026-09-16): capture-to-pose-done
3.56 mean / 3.97 p95 today is 2.00 detect graph + ~0.72 pose + ~0.6 mean
(1.08 p95) of thread hops, CPU waits and the blocking sync. Estimates: device
ROI crop on the YOLO stream + pose graph + event completion with two threads
kept gives ~3.15-3.25 / 3.50-3.60; a single fused graph with one CPU wait
gives ~3.05-3.15 / 3.20-3.30 at 256 and ~2.90-2.95 / 3.05-3.15 at 192.
Conditional graph nodes need CUDA 12.4; the rig has 12.2, so pose runs every
frame and results are masked by ROI validity. The one missing piece for
capture is a crop kernel that reads its origin from the device `DetectRoi`
(today's `mono_roi_copy_kernel` takes host ints).

## Addendum 2026-09-16 (late): step 1 implemented, not yet run on the rig

Step 1 of the plan (device-origin crop cut and pose queued from the YOLO
thread right behind the detect graph, two threads kept, CPU results path
kept and reordered) is implemented and builds; the kernel tests pass
(`detect_roi_tests`, 2293 cases). It has not run on cameras: the sudo
benchmark wrapper refuses this worktree's binary. Add
`DEVICE_ROI_ORANGE_CLIENT="/home/jeremy/orange-device-roi-20260912/targets/release/orange_client"`
to `/usr/local/bin/orange-local-benchmark` (after line 9 and in the `case`
at line 124), then:

```
scripts/run_detect_latency_spec.sh twocam_device_crop_realfish_off   # control
scripts/run_detect_latency_spec.sh twocam_device_crop_realfish       # device crop
```

(absolute path from this worktree; `pgrep -f 'orange_clien[t] --mode local'`
first; the pair needs a fish for the ROI comparison, but the timing path
runs on an empty tank too: pose runs on a blank crop every frame.)

What changed:

- `src/detect_roi.{h,cu}`: `DetectRoiParams` gained the spatial-mask
  centroid gate (`centroid_gate`, `gate_cx/cy/radius`), enabled by the YOLO
  worker only when the mask policy enforces the centroid (audit leaves it
  off), reproducing `evaluate_box_centroid` operation for operation;
  `DetectRoi.num_gated` counts rejected boxes. Crop origins are pinned at 0
  when the crop is larger than the frame instead of clamping to a negative
  bound. The device-vs-CPU comparison now compares mask-trimmed frames when
  the gate is on (match code 2 only without the gate).
- `src/pose_crop_from_roi.{h,cu}`: crop kernel reading its origin from the
  device `DetectRoi`; zero fill outside the source and for an invalid ROI.
- `src/pose_worker.{h,cpp}`: device stage. `EnableDeviceStage(px)` allocates
  a ring of `ORANGE_POSE_DEVICE_SLOTS` (default 8) slots (mono crop, engine
  input, engine output, pinned output, pinned ROI mirror, two timing events).
  `EnqueueDeviceStage(entry, source, pitch, yolo_stream)` runs on the YOLO
  thread: crop + the existing preprocess + ROI mirror + event on the YOLO
  stream, then wait-event + `enqueueV3` with the slot's buffers + output copy
  + done event on the pose stream, then pushes the slot onto the worker
  queue. `process_device_slot` on the pose thread waits on the done event,
  decodes from the slot, fills the snapshot from the ROI mirror, logs and
  publishes. In this mode the TensorRT context is driven only by the YOLO
  thread. `Cam*_pose_perf.csv` gained six trailing `device_stage_gpu_*`
  columns (GPU time from input ready to output copied); on this path
  `pose_start_to_pose_done` spans detect + pose (pose_start is the YOLO
  thread's enqueue) and the crop-thread columns have no samples.
- `src/yolo_worker.{h,cpp}`: `SetPoseWorker`, `ORANGE_ANALYTICS_DEVICE_CROP`
  (`UseDeviceCrop`), the launch block after the ROI mirror and before the
  completion event, source selection per the late-owned-copy contract (late
  copy pending: camera buffer; early owned copy: behind its ready event;
  pool copy: the entry; detached camera buffer without an owned copy: skip,
  code 4), synthetic detections skipped (code 3). New perf columns
  `device_crop` and `cpu_pose_enqueue_ms`; a `[YOLO] device crop summary`
  line at teardown.
- `src/crop_producer_worker.{h,cpp}`: `SetPoseWorker(pose, crop_fanout)`;
  with fan-out off the producer keeps the pose worker for flush ticks only
  and no longer forces a crop on every frame.
- Plumbing: `fixed.analytics_device_crop` -> `ORANGE_ANALYTICS_DEVICE_CROP`
  (`src/orange_headless_client.cpp`, `src/yolo_runtime_flags.h`,
  `src/project.cpp`); the headless client enables the device stage when the
  flag is on, device ROI is on, the camera is mono and the pose mode is
  real, else logs why and falls back to the crop-producer fan-out.
- Specs: `experiment_specs/{twocam,threecam}_device_crop_realfish{,_off}.json`
  (pose crop 256 = video crop so the pose input matches the control).
- Tests: `tools/detect_roi_tests.cpp` gained the gate oracle (transcribed
  from `yolo_spatial_mask.h` and the YOLO worker's compaction loop),
  boundary and best-outside cases, and crop-kernel cases (interior, edges,
  invalid ROI, crop larger than the source, negative origin).

Known limits of step 1: one wake remains (the pose thread waking to decode);
the pose enqueue is still a CPU call on the YOLO thread (about 0.05 ms);
the video crop still runs when recording or preview need it; not yet
exercised under the GUI client (env-gated, `SetPoseWorker` is only wired in
the headless client).

## Addendum 2026-09-17: step 1 first rig run (empty tank, two cameras)

Runs `twocam_device_crop_realfish_off_20260917_085621` (control) and
`twocam_device_crop_realfish_20260917_085745` (device crop), cameras
2010094 (die 1) and 2010095 (die 7), 60 s at 100 fps, CPU results path on,
no recorder. The tank was empty: `device_roi_valid` 0 on every frame, so
the control produced no crops and no pose at all (the crop producer only
crops on a detection), while the device path posed a blank crop on every
frame. Device ROI comparison: 5901/5901 match on both cameras, 0 mismatch.
Device crop: 5900 enqueued, 0 slot-busy, 0 skipped, 0 failed, queue
high-water 1.

| Metric (ms, per camera, both alike) | Control | Device crop |
|---|---|---|
| infer_ms mean / p95 | 1.979 / 1.985 | 1.998 / 2.004 |
| acquisition to detect done mean / p95 / p99 | 2.262 / 2.311 / 2.367 | 2.312 / 2.356 / 2.42 |
| cpu_post_sync mean | 0.099 | 0.130 |
| cpu_pose_enqueue mean / p95 / p99 (YOLO thread) | n/a | 0.40 / 0.42 / 0.45 |
| capture to pose done mean / p50 / p95 / p99 | none (no pose) | 3.358 / 3.356 / 3.376 / 3.416 |
| device_stage_gpu (input ready to output copied) mean / p95 | n/a | 1.11 / 1.12 |

Against the 2026-09-12 measurement of today's path (3.56 mean / 3.97 p95
capture to pose done, real detection, synthetic box) the p95 collapsed as
predicted and the mean fell 0.2 ms, with no decode in this run (invalid
ROI skips it; add about 0.1 ms with a fish). Two costs stand out and both
were predicted:

1. **The pose enqueue costs 0.40 ms of CPU on the YOLO thread** (trtexec
   Enqueue Time for the 256 engine is 0.33 ms without a graph, 0.016 ms
   with `--useCudaGraph`). It overlaps the detect graph so it is not on the
   pose critical path, but the driver activity shows up as infer_ms +0.02
   and acquisition-to-detect +0.05 (gate is +0.03). Step 2 (capture the
   pose stage into a CUDA graph per slot) removes it.
2. **Pose GPU time is 1.11 ms, not 0.84**: the late owned pool copy (lever
   2d) now overlaps pose and steals about 0.27 ms, the figure the design
   notes estimated. Step 4 (issue the copy after pose done, or measure the
   alternatives) recovers it.

Gate status: device ROI match passes; slot ring passes; capture to pose
done p95 3.38 < 3.6 passes; detect latency +0.05 fails narrowly until step
2. The real-fish comparison is still owed.

## Addendum 2026-09-17: step 2, one pose CUDA graph per slot

Run `twocam_device_crop_realfish_20260917_090611` (same spec, same two
cameras, empty tank, 8/8 graphs captured per camera, 5900 frames each, no
slot-busy drops). `ORANGE_POSE_DEVICE_GRAPH` (default on) selects the
graph; off restores the step 1 plain enqueue for an A/B.

| Metric (ms, cam 2010094) | Control | Step 1 | Step 2 |
|---|---|---|---|
| cpu_pose_enqueue mean / p99 (YOLO thread) | n/a | 0.400 / 0.452 | 0.026 / 0.030 |
| capture to pose done mean / p95 / p99 | none | 3.358 / 3.376 / 3.416 | 3.237 / 3.255 / 3.282 |
| device_stage_gpu mean (input ready to output copied) | n/a | 1.11 | 1.00 |
| infer_ms mean / p95 | 1.979 / 1.985 | 1.998 / 2.004 | 1.994 / 2.001 |
| cpu_pre_sync mean | 0.100 | 0.507 | 0.134 |
| cpu_post_sync mean | 0.099 | 0.130 | 0.130 |
| acquisition to detect done mean / p95 | 2.262 / 2.311 | 2.312 / 2.356 | 2.300 / 2.342 |

Detect-latency delta against the control after step 2: +0.038 mean,
+0.031 p95, at the +0.03 gate's edge. Attribution from the per-phase
columns: infer_ms +0.015 (GPU, the graph runs while the YOLO thread's
device-stage launches hit the driver); cpu_pre_sync +0.034 (the crop,
preprocess, ROI mirror, event, wait-event and graph launch calls, all
before the completion event, so overlapped by the graph and not on the
pose path); cpu_post_sync +0.031, unattributed (post_ms and ipc_ms account
for 0.006; the rest is suspected driver contention with the pose graph
executing during the late owned copy issue and the timing-event reads).
Step 4 (copy placement) is the next lever for both the pose GPU time
(1.00 vs 0.72 for the graphed engine alone) and this residual.

### Why one graph per slot, and what "per slot" means

A CUDA graph is instantiated once and reused, and that is still true here:
each of the eight graphs is captured and instantiated at
`EnableDeviceStage` and launched every eighth frame with no per-frame
work. There are eight because a captured graph stores the device pointers
of every kernel argument as literal values. The TensorRT enqueue captured
for slot 3 reads slot 3's input buffer and writes slot 3's output buffer,
forever. The device stage keeps eight slots so that frame N+1's crop can
be written while the pose thread is still decoding frame N's output; each
slot therefore needs a graph with its own baked addresses. The alternative
(one graph, one input buffer, copy each slot's input into it before
launch) would also force the YOLO thread to wait until the pose thread has
finished reading the single output buffer, which is the hop step 1
removed. TensorRT does not expose its kernel nodes, so updating node
parameters per launch (`cudaGraphExecKernelNodeSetParams`) is not an
option either.

What must stay stable for a graph: the memory behind every captured
pointer, at the same address, for the graph's lifetime. Buffer contents
may change between launches; addresses may not. Hence the slot buffers
are allocated once in `EnableDeviceStage` and freed only after the graphs
(`free_device_slots`), the TensorRT context (whose scratch workspace the
captured kernels also address) outlives the graphs, and the pinned host
output buffer of the captured D2H copy is fixed. The detect graph obeys
the same rule by running preprocess outside the graph into a fixed input
buffer, which is how the varying camera-frame address stays out of it.

TensorRT rules the capture follows (`capture_slot_graph` in
`src/pose_worker.cpp`): one ordinary enqueue with the slot's addresses
before capture, because TensorRT's lazy first-use allocation would break
the capture; thread-local capture mode so the recorder's CUDA calls on
other threads cannot join the graph; one un-timed launch after
instantiation so the first real frame does not pay the graph upload. The
eight graphs share the context's workspace, so they must run serialised,
which the single pose stream guarantees. Per frame the YOLO thread now
does: crop kernel, preprocess kernel, ROI mirror copy, event record (YOLO
stream), then wait-event, `cudaGraphLaunch`, event record (pose stream):
about 0.03 ms of CPU in total.

## Addendum 2026-09-17: step 4, the late owned copy behind pose

Goal: stop the 20 MB late owned pool copy (lever 2d, issued on the
acquisition stream at detect done) from overlapping the pose graph on the
die, which cost pose 0.27 ms of GPU time (1.00 vs 0.72 for the engine
alone). `ORANGE_ANALYTICS_COPY_AFTER_POSE` (spec
`fixed.analytics_copy_after_pose`, default on) selects it; off restores
the copy at detect done for an A/B.

First attempt (run `…_094122`, and `…_094447` with the extra timing
columns): keep the copy on the acquisition stream but make it wait on the
slot's pose-done event with `cudaStreamWaitEvent`. Pose GPU fell to 0.74
and capture to pose done to 2.99 / 3.01, but acquisition-to-detect p95
jumped from 2.34 to 3.07: on about one frame in eight the host call that
issues the copy blocked for 0.78 ms, the pose time (new column
`cpu_late_copy_ms`: p50 0.027, p90 0.770). A cross-stream wait on an
incomplete event followed by the memcpy stalls the issuing host call on
some frames; the mechanism in the driver was not chased further because
the fix removes the wait.

Fix (run `…_094750`): queue the copy on the pose stream itself, right
behind the slot's graph launch, at enqueue time on the YOLO thread
(`issue_late_owned_copy_on_stream` in `late_owned_copy.h`;
`PoseWorker::device_stage_stream()`). Stream order gives the same GPU
ordering with no cross-stream dependency, and the issue cost moves off
the post-sync path (it is overlapped by the detect graph). The later
issue site is a no-op because the pending flag is already clear; if the
device stage did not enqueue, the copy goes out at detect done as before.

| Metric (ms, cam 2010094) | Control | Step 2 | Step 4 |
|---|---|---|---|
| capture to pose done mean / p50 / p95 / p99 | none | 3.237 / 3.235 / 3.255 / 3.282 | 2.982 / 2.979 / 3.003 / 3.056 |
| device_stage_gpu mean / p95 | n/a | 1.00 / 1.01 | 0.743 / 0.747 |
| cpu_late_copy (YOLO thread) mean / p99 | n/a | (0.03, inside post_sync) | 0.012 / 0.018 |
| cpu_pre_sync / cpu_post_sync mean | 0.100 / 0.099 | 0.134 / 0.130 | 0.148 / 0.128 |
| infer_ms mean / p95 | 1.979 / 1.985 | 1.994 / 2.001 | 1.994 / 2.001 |
| acquisition to detect done mean / p95 / p99 | 2.262 / 2.311 / 2.367 | 2.300 / 2.342 / 2.372 | 2.298 / 2.326 / 2.373 |

Gate status after step 4 (empty tank, two cameras): capture to pose done
2.98 mean / 3.00 p95, below the 3.3 target of step 3 already; detect
delta vs control +0.036 mean (gate +0.03, still 0.006 over) and +0.015
p95 (passes). The remaining mean excess sits in cpu_post_sync (+0.03,
unattributed; post_ms and ipc_ms account for 0.006) and infer_ms (+0.015).
The recorder and display now see the owned frame about 0.75 ms later;
that cost has not been measured with a recorder on. Real-fish comparison
still owed, and add about 0.1 ms of decode with a fish.

New perf columns this step: `cpu_late_copy_ms`, `cpu_timing_reads_ms`.

## Addendum 2026-09-17: real-fish gate, four cameras

Two traps explained the two weeks of zero detections, and both are now
recorded in the spec notes:

1. **Cam2010096 drives the IR lights** (its `GPO_0_Mode` is `Exposure`,
   the strobe output). Every latency spec since 2026-09-03 excluded it
   (its link was down that day), so the tank was dark in every run.
2. **The engine-only config folder** `/tmp/orange_pose_engine_only_config_a16`
   is a 2026-09-04 schema-3 copy: exposure 50 (standard 100), focus 0 on
   two cameras, iris 5 to 19, PTP off. The standard folder
   `~/orange_data/config/local/100_cam4_ptp_fourcam` moved to schema 4 on
   2026-09-14 and is re-saved by the GUI.

New specs `fourcam_device_crop_realfish{,_off}` use all four cameras
(dies 3, 1, 7, 5) and `/tmp/orange_device_crop_config_a16`, a copy of
the standard folder with `crop_pipeline.crop_size_px 256` on every
camera (regenerate after a reboot or after the GUI saves the configs).
The twocam/threecam specs are marked latency-only. Also: the GUI
validation session (`orange-gui-validation`, the shared worktree's
`orange` binary) holds all four cameras while it runs; a headless launch
then fails at open with `GVCP ACK error` on the first camera. Check
`ps -eo cmd | grep orange-gui-validation` as well as the `orange_clien[t]`
guard, and never force-reboot a camera the GUI holds.

Runs `fourcam_device_crop_realfish_off_20260917_111027` (control:
crop-producer path, real pose) and `fourcam_device_crop_realfish_20260917_111140`
(steps 1, 2, 4), 60 s at 100 fps, one fish, detections on 100% of frames
on all four cameras, CPU results path on, no recorder.

| Metric (ms, range over the four cameras) | Control | Device crop |
|---|---|---|
| capture to pose done mean | 3.81 to 3.88 | 3.12 to 3.14 |
| capture to pose done p95 | 3.95 to 4.24 | 3.15 to 3.17 |
| capture to pose done p99 | 4.24 to 4.31 | 3.24 to 3.27 |
| device ROI match | 5900/5900 per camera, 0 mismatch | 5900/5900 per camera, 0 mismatch |
| device_stage_gpu mean | n/a | 0.745 to 0.747 |
| infer_ms mean / p95 (cam 2010094) | 2.003 / 2.017 | 2.017 / 2.031 |
| acquisition to detect done mean / p95 (cam 2010094) | 2.458 / 2.514 | 2.545 / 2.657 |
| cpu_pre_sync / cpu_post_sync mean (cam 2010094) | 0.167 / 0.223 | 0.225 / 0.298 |

Pose results agree in distribution (frame-to-frame comparison is not
possible across two live runs): status `poses` on every frame in both,
pose confidence mean 0.88 to 0.94 (control) vs 0.89 to 0.93 (device),
keypoint confidence 0.99+ and all keypoints visible in both, and the
keypoint centroid sits at about (125, 125) of the 256 crop in both, i.e.
both paths centre the crop on the fish. Pose crop and pose input are the
same 256 crop through the same preprocess, and the ROI match proves the
origin is the same, so agreement is by construction.

Gate status with a fish: capture to pose done p95 3.16 vs 4.0 to 4.2 on
the control, a 0.7 ms mean and 1.0 ms p95 improvement; ROI match passes;
slot ring never full. Detect side: infer_ms +0.014, acquisition to
detect done +0.087 mean / +0.14 p95, which fails the +0.03 gate. The
per-column diff puts it in cpu_pre_sync +0.058 (the six CUDA calls of the
device-stage launch, before the completion event: crop, preprocess, ROI
mirror, event, wait-event, graph launch) and cpu_post_sync +0.075
(post_ms +0.009 with detections present, ipc +0.007, enet +0.006, timing
reads +0.025 with a p95 of 0.146, rest unattributed). Step 3 (one
capture for the whole frame) collapses the six launch calls into one and
is the designed answer; a cheaper interim is to record the completion
event before the device-stage launches, which needs a second event to
keep the entry alive until the crop has read the source in ring-copy
mode. Whether +0.09 ms on detect is acceptable against -0.7 ms on pose
depends on which signal closes the loop; both are now measured.

## Addendum 2026-09-17: step 3, one captured graph per slot for the whole frame

`ORANGE_ANALYTICS_FUSED_FRAME` (spec `fixed.analytics_fused_frame`,
default off; needs the device crop path). Per pose slot one CUDA graph:
[pre_start] preprocess (indirect) [pre_end][infer_start] detect enqueue +
four output copies [infer_end] -> ROI (indirect params) -> ROI mirror to
the slot -> detect_done node -> pose crop (indirect) -> pose preprocess ->
input_ready node -> pose enqueue -> pose output copy -> done node -> pool
copy (indirect kernel) -> graph_end node. Everything per frame (source
address, pool copy addresses and size, input mask, ROI parameters
including the centroid gate) is read from the slot's `FusedFrameArgs`
block in pinned mapped memory (`src/fused_frame_args.h`), written by the
YOLO thread before the launch. Per frame the YOLO thread does the graph
launch plus the entry's own event records (completion, analytics_ready,
input_ready, all recorded after the launch so they fire when the whole
graph is done), then waits on the slot's detect_done node; the host work
after the wait is unchanged. Graphs are captured in `Warmup` on the warm
source. The pose thread waits on the slot's done node, and frees the slot
only after the graph_end node so the trailing copy never reads a rewritten
argument block. Any capture failure disables the path for the run and the
device crop path (steps 1, 2, 4) stays in use.

Run `fourcam_fused_realfish_20260917_114351` (four cameras, fish detected
on every frame, CPU results path on, no recorder), against the same-day
controls `fourcam_device_crop_realfish_off_…_111027` (crop producer) and
`fourcam_device_crop_realfish_…_113152` (steps 1, 2, 4):

| Metric (ms, range over four cameras) | Crop producer | Steps 1+2+4 | Step 3 fused |
|---|---|---|---|
| acquisition to detect done mean | 2.455 to 2.469 | 2.541 to 2.556 | 2.473 to 2.486 |
| acquisition to detect done p95 | 2.511 to 2.525 | 2.644 to 2.660 | 2.527 to 2.536 |
| infer_ms mean | 1.999 to 2.008 | 2.016 to 2.026 | 2.005 to 2.014 |
| cpu_pre_sync mean | 0.166 to 0.173 | 0.263 to 0.313 | 0.140 to 0.150 |
| cpu_post_sync mean | 0.223 to 0.236 | 0.248 to 0.260 | 0.243 to 0.248 |
| launch CPU (cpu_pose_enqueue) mean | n/a | 0.028 to 0.040 | 0.057 to 0.065 (the whole frame) |
| capture to pose done mean | 3.81 to 3.88 | 3.18 to 3.19 | 3.13 to 3.14 |
| capture to pose done p95 | 3.95 to 4.24 | 3.28 to 3.30 | 3.18 to 3.21 |
| capture to pose done p99 | 4.24 to 4.31 | 3.40 to 3.42 | 3.29 to 3.33 |
| device_stage_gpu mean | n/a | 0.746 | 0.739 |
| device ROI match | 5900/5900, 0 mismatch | same | 5901/5901, 0 mismatch |
| pose status `poses` | every frame | every frame | every frame |

Gates: detect delta vs the crop-producer control +0.017 mean / +0.012 p95
(gate +0.03) passes; infer_ms within 0.006; capture to pose done p95 3.19
against the 3.3 target passes; ROI match and slot ring pass; pose results
agree in distribution (confidence 0.86 to 0.94, keypoint centroid at the
crop centre). Against the old path: -0.7 ms mean, -0.9 ms p95, -1.0 ms
p99 on capture to pose done, at a detect cost inside the noise.

Three capture traps, each cost one run:

1. **Events recorded inside a capture must use
   `cudaEventRecordWithFlags(ev, stream, cudaEventRecordExternal)`.** A
   plain `cudaEventRecord` during capture is only a capture dependency
   marker; waiting on that event afterwards fails with
   `cudaErrorInvalidValue` (every fused frame threw at the completion
   wait). Step 2 worked because its events were recorded outside the
   capture.
2. **The captured stream must be non-blocking.** The YOLO stream was
   legacy-blocking by default; other threads' null-stream work during the
   capture invalidated it (TensorRT "previous error during capture",
   `cudaErrorStreamCaptureInvalidated`, Cask and plugin execution errors,
   flaky per camera). `YOLOv8` now creates the stream non-blocking when
   the fused flag is set.
3. **Capture in Warmup, not on the first frame.** Eight captures with warm
   launches on the worker thread stalled the camera ring (dropped frames)
   and raced other threads.

Also learned: the done node must sit before the trailing pool copy, or the
pose thread waits for the copy too (measured +0.21 ms on capture to pose
done, 3.39 vs 3.18, in run `…_114057`); with graph_end as the slot-reuse
guard the copy trails the pose in the graph for free.

Perf: `device_crop == 5` marks fused frames; on them `cpu_pose_enqueue_ms`
is the whole launch, `pre_ms`/`gap_ms`/`infer_ms` come from the graph's
external timing nodes, and the entry's input-ready timestamp is the end of
the graph (about 1 ms later than on the other paths).

## Addendum 2026-09-17: step 6, the 192 px head engine on the fused path

Spec `fourcam_fused_realfish_192` = the fused spec with the candidate 192
engine (`…_a16_gpu5_trt100_fp16_bo5_avg32.engine`), `pose_worker.input_width/height 192`,
`pose_worker.crop_size_px 192` (video crop stays 256, so this is the first
run with the two crops split), `skeleton_id pose_schema:traditional_v1`.
Preprocess 192 -> 192 is the identity the input contract asks for (ratio
1, no padding, bilinear at integer coordinates reduces to the pixel, then
/255 into three planes). Run `fourcam_fused_realfish_192_20260917_115309`
against `fourcam_fused_realfish_20260917_114351` (256 cedar), same fish,
four cameras, CPU results path on, no recorder:

| Metric (ms, range over four cameras) | 256 cedar, fused | 192 head, fused |
|---|---|---|
| capture to pose done mean | 3.131 to 3.142 | 2.968 to 2.986 |
| capture to pose done p95 | 3.182 to 3.211 | 3.005 to 3.024 |
| capture to pose done p99 | 3.291 to 3.329 | 3.124 to 3.151 |
| device_stage_gpu mean / p95 | 0.739 / 0.75 | 0.586 / 0.59 |
| acquisition to detect done mean / p95 | 2.47 to 2.49 / 2.53 to 2.54 | 2.47 to 2.49 / 2.52 to 2.53 |
| device ROI match (pose crop 192, video crop 256) | 5901/5901 | 5901/5901, 0 mismatch |
| pose status `poses` | every frame | every frame |
| pose confidence mean / p10 | 0.86 to 0.94 / 0.83 to 0.92 | 0.96 to 0.97 / 0.95 to 0.97 |
| keypoints visible | all | all |

Latency gates pass: pose GPU 0.586 (gate < 0.65; trtexec predicted
0.575 vs 0.722), capture to pose done p95 3.02, detect unchanged. Against
the crop-producer path of this morning (3.84 / 4.11 / 4.28) the fused
192 path is 0.87 ms faster on the mean and 1.1 ms on the tails.

**Accuracy is not established, and the live data shows a systematic
difference that needs the offline check before activation.** The two
models were run minutes apart on the same fish, so frames cannot be
paired, but the within-run landmark geometry differs consistently across
cameras: inter-eye distance 13.9 to 14.5 px on the 256 model vs 17.1 to
17.6 px on the 192 model, and eye-midpoint-to-bladder 32.9 to 38.7 px vs
40.1 to 44.4 px (cameras 2010093 and 2010096, where the fish held still
within each run, keypoint sd 1 to 5 px). Both crops are 1:1 scale, so
pixel distances are comparable; a 20 % difference in both spans points at
different landmark conventions between the two label sets (the 192 set is
the recovered, reviewed head set), not at the crop or preprocess. The
higher confidence of the 192 model is not evidence either way. Claims 3, 4
and 7 of the inspection report (probe digest, fixed-crop parity against
the training runtime with a tolerance, reviewed overlays) remain the
gate; selector activation stays off. Label names are still `bladder,
eye_left, eye_right` from the K=3 default (the contract says
`swim_bladder`), a metadata-only difference noted in the inspection report.

## Addendum 2026-09-17: recorder-on measurement, first attempt (blocked mid-way)

Specs `fourcam_fused_recorder_realfish{,_off}`: the four-camera split-GOP
full-frame external recorder (registered source, detect priority, shards
[3,4] [1,2] [7,8] [5,6]) plus the external crop video recorder at 384 px,
GOP-parity interleaved, real fish, real pose (256 cedar, pose crop 256),
CPU results path on. Config folder `/tmp/orange_recorder_config_a16` =
the standard 100_cam4 folder with `crop_pipeline.crop_size_px 384`
(the `validated_split_gop_hevc_100fps_gop25_fourcam_a16` folder every
recording spec used is the same dark 2026-09-04 schema-3 copy: exposure
50, focus 0). The two crop sizes are separate spec keys:
`crop_recording.crop_size_px` (video crop) and `pose_worker.crop_size_px`
(inference crop, meant to equal the engine input; a mismatch now logs a
warning at startup and is letterbox-resized).

Needed to run at all: `external_recorder_ipc_probe` must be built in the
worktree whose client runs (`make external_recorder_ipc_probe`); the
supervisor otherwise reports "external recorder exited before socket
readiness".

Control run `fourcam_fused_recorder_realfish_off_20260917_120207`
(crop-producer path) recorded the full 60 s cleanly on every camera
(5901/5901 frames encoded per stream, 0 drops, 0 copy fallbacks, identity
proof passed, merged-output pending peak 3 GOPs; crops: 5901 rows, 0
dropped, queue depth 0) and then the client **segfaulted at teardown**
(after "Acquire frames thread finished", exit 139; core in
systemd-coredump, PID 3874037, root-owned). The perf CSVs are complete;
`runs.json` and `latency_phases.json` are missing. The crash left all
four cameras with stale control sessions (`GVCP ACK error`), which needs
`evt_force_reboot <serial> <ip>` (93 192.168.110.2, 94 192.168.120.2,
95 192.168.170.2, 96 192.168.180.2); this session's reboot attempt was
blocked by its permission policy. The fused run
(`…_120324`) therefore failed at camera open and has no data.

**The four-camera full-consumer configuration is much slower than the
three-camera numbers of 2026-09-04, on the crop-producer path, before any
of this branch's changes take effect** (cam 2010094; other cameras alike):

| Phase (ms) | no recorders (11:10 run) | full-frame + crop recorders |
|---|---|---|
| acquisition to worker start mean / p95 | 0.146 / 0.168 | 0.458 / 0.753 |
| cpu_pre_sync | 0.167 / 0.185 | 0.355 / 0.512 |
| pre_ms (GPU preprocess) | 0.076 / 0.078 | 0.148 / 0.183 |
| infer_ms (GPU detect graph) mean / p95 / p99 | 2.003 / 2.017 / 2.03 | 2.107 / 2.804 / 2.83 |
| cpu_post_sync | 0.223 / 0.287 | 0.458 / 0.631 |
| acquisition to detect done mean / p95 | 2.458 / 2.514 | 3.231 / 3.857 |
| capture to pose done mean / p95 | 3.86 / 4.23 | 5.21 / 6.08 |

Steady over the 60 s (per-10 s means 3.20 to 3.34), no affinity applied
(`yolo_affinity_configured 0`), no acquisition starvation (free entries
min 57 of 63, pending requeues max 2). infer_ms is bimodal: about 10 %
of frames (573 of 5901) run the detect graph at 2.8 ms instead of 2.0,
in short runs, with cross-camera correlation of acquisition-to-detect
0.2 to 0.7 by frame id, so a shared cause. Host side, the crop encode
submission costs 1.15 ms of CPU per crop on the crop thread
(`encode_submit_cpu_ms`), and the 16 recorder processes plus the crop
threads compete with the unpinned YOLO and pose threads (acquisition to
worker start 0.15 -> 0.46). Reference: the three-camera endurance of
2026-09-04 with the same consumer set minus real pose was 2.26 / 2.34.
Candidate decomposition runs once cameras are back: full-frame recorder
only; crop recorder only; YOLO/pose threads pinned
(`ORANGE_YOLO_AFFINITY_CAM_<serial>`); three cameras with this consumer
set; pose noop.

Still owed: the fused-path recorder run (same spec with the flags on),
the teardown backtrace (`sudo coredumpctl gdb 3874037` or the
`orange-gdb-headless-bt` wrapper, which only runs the shared worktree's
client, itself a useful bisect: if that binary also segfaults on this
spec the crash predates this branch), and the decomposition above.

## Addendum 2026-09-17: recorder-on measurement, fused path vs crop-producer path

Run `fourcam_fused_recorder_realfish_20260917_121251` (fused path, steps
1 to 4, 256 cedar engine, pose crop 256, video crop 384, both recorders
on, four cameras, fish) against the control `…_off_20260917_120207`
(crop-producer path, same recorders). The fused run passed the run policy
on every camera, all recorders acknowledged every frame, crops all
encoded, and the client exited cleanly (the control had segfaulted at
teardown; that crash is on the crop-producer path with both recorders and
real pose, and needs a fresh reproduction for a usable backtrace because
the binary was rebuilt after the core was taken).

| Metric (ms, mean over four cameras) | Crop producer + recorders | Fused + recorders |
|---|---|---|
| acquisition to worker start | 0.461 | 0.328 |
| pre_ms (GPU preprocess) | 0.146 | 0.090 |
| infer_ms mean / p95 | 2.120 / 2.814 | 2.163 / 2.822 |
| cpu_post_sync | 0.475 | 0.524 |
| acquisition to detect done mean / p95 / p99 | 3.272 / 3.896 / 4.412 | 3.229 / 3.747 / 4.104 |
| capture to pose done mean / p95 / p99 | 5.268 / 6.241 / 6.680 | 3.802 / 4.379 / 4.781 |
| device_stage_gpu mean | n/a | 0.780 |
| recorder enqueue age p95 | 20.6 | 18.0 |
| recorder detach total p95 | 0.427 | 0.270 |
| recorder peak pending GOPs / frontier age | 3 / 524 | 3 / 524 |
| recorder frames encoded / dropped / copy fallbacks | 5901 / 0 / 0 | 5900 / 0 / 0 |
| crops encoded / dropped, crop queue high-water | 5901 / 0, 0 | 5900 / 0, 0 |

Answers: (1) receiving the owned frame after pose costs the recorder
nothing measurable; its enqueue age and detach timing both improved, and
its pending depth did not move. (2) The fused path keeps its advantage
under full load: 1.5 ms off the pose mean and 1.9 ms off the p95, and
even the detect side is slightly better (fewer host calls to lose to the
recorder processes). (3) The 0.8 ms four-camera contention on detect is
common to both paths (infer_ms bimodal at 2.0 / 2.8 in both) and is not
touched by this branch.

**Pose quality on the crop-producer path collapses once the video crop is
384.** The old path poses on the video crop (384 letterboxed into the 256
engine), so the head is smaller than the model was trained on: pose
status `poses` on only 22 of 5901 frames on cam 2010093 and 4465 / 5899 /
5633 on the others, confidence 0.29 to 0.78. The device path poses on its
own 256 crop: `poses` on every frame, confidence 0.90 to 0.94. The
production GUI records 384 crops, so this is the production condition of
the old path. The two-crop split is therefore a correctness fix as well
as a latency one.

## Addendum 2026-09-17: decomposition of the four-camera recorder cost, and the heartbeat

Fused path, four cameras, fish, cam 2010094 (2010096 alike):

| Configuration | acq to worker | pre_ms | infer_ms mean / p95 | slow-frame fraction (infer > 2.3) | cpu_post_sync | acq to detect mean / p95 | capture to pose done mean / p95 |
|---|---|---|---|---|---|---|---|
| no recorders (`fourcam_fused_realfish_…_114351`) | 0.161 | 0.072 | 2.005 / 2.019 | 0.000 | 0.245 | 2.476 / 2.527 | 3.137 / 3.211 |
| full-frame recorder only (`…_fullframe_only_…_121644`) | 0.276 | 0.088 | 2.138 / 2.820 | 0.113 | 0.418 | 2.988 / 3.626 | (client aborted at teardown before the pose summary) |
| both recorders (`…_recorder_realfish_…_121251`) | 0.327 | 0.085 | 2.140 / 2.814 | 0.115 | 0.522 | 3.194 / 3.700 | 3.742 / 4.270 |

The full-frame split-GOP recorder alone accounts for the whole detect-graph
slowdown (infer_ms 2.00 -> 2.14 mean, p95 2.82, 11 to 17 % of frames at
about 2.6 to 2.8 ms) and most of the wake-time growth; the crop video
recorder adds about 0.2 ms on the mean (more host work: acq to worker
+0.05, post-sync +0.10) and nothing to infer_ms. The crop-only run could
not start (cameras stale after the previous abort).

**The split-GOP heartbeat is back with four cameras.** By frame phase
(frame id mod 50, two GOPs of 25): the slow detect frames are not spread
out, they come in three bursts per 50 frames, at phases 8 to 11, 17 to 21
and 26 to 30, where 40 to 78 % of frames run the graph slow, against under
5 % elsewhere; infer_ms by phase ranges 2.03 to 2.64 and acq-to-detect
2.71 to 3.72 (full-frame only) with the two GOP halves at 2.92 vs 3.05
(both recorders: 3.09 vs 3.30). Identical phase pattern on the
crop-producer control, so it is the recorders' doing, not this branch's.
On 2026-09-04 with three cameras and interleaved crops the square wave had
gone (halves met at 2.25, one boundary tick on 1 frame in 50). With the
fourth camera every die serves a detect graph plus a full-frame shard plus
a crop shard, and something GOP-periodic on the shard (I-frame, bitstream
lock, merged-output push) now steals SM or memory bandwidth from the
detect graph on the same die. Next levers to test, in order: full-frame
shards off the detect dies (contract `expected_shard_gpu_ids` on the
non-analytics dies only, if NVENC capacity allows two shards per die),
detect-priority gate settings, and pinning the analytics threads.

Teardown: the fused+both run exited cleanly; the fused full-frame-only run
aborted (SIGABRT) after its 60 s with the perf CSVs complete, and the
crop-producer control segfaulted. The abort's core matches the current
binary (no rebuild since); see the journal for the PID to dump.

Note: `fourcam_fused_recorder_crop_only` cannot run as written: the
client refuses `fixed.crop_recording mode=external_ipc` without the
supervised full-frame external recorder (`recording_sink_mode
external_ipc`). The crop recorder's share is therefore the difference
between the both-recorders and full-frame-only runs: about +0.2 ms on
the acquisition-to-detect mean, host-side, none on the detect graph. The
abort core that matches the current binary is PID 3878883
(2026-09-17 12:17:58, the full-frame-only fused run); dump with
`sudo coredumpctl dump 3878883 -o /tmp/orange_core_3878883 && sudo chown jeremy /tmp/orange_core_3878883`.

## Addendum 2026-09-17: the teardown abort, from the core

Core of PID 3878883 (fused full-frame-only run, current binary):
`munmap_chunk(): invalid pointer` raised from `__libc_free` inside
`RecordingIngress::ExternalIpcHandoffWorker::ThreadRunning()` (frame 9,
callee inlined; the Release build has no `-g`, so the line is not
recoverable). At that moment the main thread was in
`orange::session::wait_for_recording_run_drain`, three other ingress
handoff workers were in `ExternalIpcHandoffWorker::OnFlushTick`, one YOLO
worker was inside `WorkerFunction` and one pose worker inside
`process_device_slot`, every other worker idle in its queue wait. So the
abort is a teardown-ordering fault in the recording ingress drain while
analytics is still producing, in code this branch has not changed
(`git log 5cf21a9..HEAD -- src/recording_ingress.cpp` in the shared
worktree is empty). The crop-producer control's SIGSEGV happened with the
device stage off entirely, which points away from this branch's memory
too. What is new in these runs versus the 2026-09-04 endurance that exited
cleanly: four cameras, real pose instead of noop, both recorders. Next
step to localise it: rebuild with `-g` (no codegen change), rerun
`fourcam_fused_recorder_fullframe_only`, dump the core again.

## Addendum 2026-09-17: was the recording slowdown solved before?

For three cameras, yes; for four, it was never measured until today. The
2026-09-04 levers (registered source, detect-priority gate, late owned
copy, GOP-parity crop interleave) brought the three-camera registered
spec to 2.20 mean / 2.29 p95 and the three-camera endurance with crops to
2.26 / 2.34, and the review doc shows the square wave reduced to a
boundary tick (halves 2.254 vs 2.249). The four-camera runs were planned
for 2026-09-05 "once 2010096 is up"; its link stayed down until this week,
and `~/orange_data/exp/unsorted` holds no four-camera run before today.
The only earlier four-camera figure is the August profile in
`docs/a16_tensorrt_detect_engine_rebuild.md`, p95 3.81 to 3.93 before any
lever. Today's four-camera both-recorders run sits at p95 3.70 to 3.90,
i.e. with all levers on, the fourth camera brings detect latency back to
roughly the August level, and the heartbeat returns as GOP-locked bursts.
The three-camera gains therefore do not carry over; the fourth camera
fills the last two dies so every die carries a detect graph plus shards.

## Addendum 2026-09-17: the host is on a slow clocksource since 2026-09-11 (root cause of the host-side step change)

Every host-side segment of the YOLO perf row, including pure CPU
segments with no CUDA call in them, is uniformly 2 to 5 us slower on
every run since 2026-09-12 than on 2026-09-04, on the same binary and the
same two-camera engine-only spec; the excess scales with the number of
cameras and threads. Cam 2010094, means:

| Segment (ms) | 09-04 engine-only | 09-12 engine-only | today two-camera control | today four cameras, no recorders |
|---|---|---|---|---|
| acquisition to PTP done (a timestamp pair) | 0.0001 | 0.0026 | 0.0026 | 0.0067 |
| YOLO enqueue push | 0.0004 | 0.0027 | 0.0028 | 0.0059 |
| YOLO enqueue to dequeue (queue hop) | 0.0128 | 0.0375 | 0.0314 | 0.0628 |
| acquisition to worker start | 0.023 | 0.080 | 0.073 | 0.161 |
| cpu_wait_event (one CUDA call) | 0.0032 | 0.0127 | 0.0147 | 0.0296 |
| cpu_pre_sync | 0.056 | 0.111 | 0.099 | 0.138 |
| cpu_post_sync | 0.065 | 0.135 | 0.099 | 0.245 |
| infer_ms (GPU graph) | 1.976 | 1.974 | 1.979 | 2.005 |
| acquisition to detect done | 2.149 | 2.275 | 2.261 | 2.474 |

Cause, from the kernel log: on **2026-09-11 07:03:06** the clocksource
watchdog marked the TSC unstable ("skewed -485 ms over watchdog 'acpi_pm'
interval of 485 ms ... TSC found unstable after boot, most likely due to
broken BIOS") and the kernel **switched to clocksource acpi_pm**. A clock
read now costs about 1.95 us (measured; the TSC vDSO path is 20 to 40 ns)
and acpi_pm reads serialise on one I/O port across all CPUs, so the cost
grows with concurrent readers, which is why four cameras pay twice what
two do. The pipeline takes tens of timestamps per frame per thread, the
CUDA driver and the EVT SDK read the clock too, and every wake and every
queue hop is stretched. The machine has not rebooted since 2026-08-09.

Consequences: (1) every measurement since 2026-09-12, including all of
today's, carries this host-side inflation; comparisons between paths made
on the same day stay valid, absolute numbers do not, and the 09-04
figures are the true host baseline. (2) The "four-camera contention" is
two things: a GPU-side, content-dependent NVENC effect on the detect
graph (infer_ms p95 2.82 with lit frames vs 2.28 with the dark config,
none without recorders) and a host-side thread-count effect that is
mostly the clocksource. (3) The unexplained 09-12 odd/even post-sync
alternation appeared with the switch.

Fix: reboot (the TSC is re-validated at boot); to stop the watchdog from
switching again, add `tsc=reliable` (or `clocksource=tsc tsc=nowatchdog`)
to the kernel command line first. After the reboot, re-run
`fourcam_fused_realfish`, `fourcam_fused_recorder_realfish` and its
`_off` control before drawing any further conclusion about the fourth
camera.

## Addendum 2026-09-17: what the clocksource switch is, and how 2.15 becomes 3.2 to 3.5

**What happened, in plain terms.** Every "what time is it" call (this
pipeline makes tens per frame per thread; the CUDA driver and the EVT SDK
make their own) reads a hardware counter. Normally that is the TSC, a
cycle counter in each core, read in about 20 ns with no system call. The
kernel runs a watchdog that compares the TSC against a slower chipset
timer; if they ever disagree by too much it declares the TSC unusable and
switches to the slow timer, permanently, without telling anyone. That
happened on pancake0 on 2026-09-11 at 07:03 (kernel log: "Marking TSC
unstable due to clocksource watchdog ... most likely due to broken BIOS
... Switched to clocksource acpi_pm"). The skew was exactly one watchdog
interval, the signature of one glitchy comparison, a known occasional
event on Threadripper boards. Nobody ran anything to cause it. The slow
timer, acpi_pm, costs about 2 us per read (measured) through a system
call, and all cores read it through one port, so it gets slower the more
threads ask. A reboot re-validates the TSC; `tsc=reliable` on the kernel
command line stops the watchdog switching again.

**How much of the slowdown it is.** It is not most of the 2.15 -> 3.5. It
is the layer under the other two, and it makes them worse. Cam 2010094,
means in ms, fused path unless noted:

| Layer | Configuration | acq -> detect | What moved |
|---|---|---|---|
| true host floor (fast clock, 2026-09-04) | two cameras, engine-only | 2.149 | graph 1.976; every host segment tiny (worker start 0.023, post-sync 0.065) |
| + slow clock | two cameras, engine-only (today) | 2.261 | +0.11, all of it host segments; graph unchanged 1.979 |
| + two more cameras | four cameras, no recorders (today) | 2.474 | +0.21, again host segments (worker start 0.161, post-sync 0.245); graph 2.005 |
| + both recorders, GPU side | four cameras, recorders (today) | | graph 2.005 -> 2.140 mean, p95 2.02 -> 2.82 (content-dependent NVENC contention; 2.28 p95 on dark frames) |
| + both recorders, host side | same run | 3.194 | worker start 0.161 -> 0.327, pre-sync 0.138 -> 0.303, post-sync 0.245 -> 0.522 |
| p95 | same run | 3.700 | the GOP-locked slow-graph bursts (11 % of frames at 2.6 to 2.8) |

So: +0.11 clock at two cameras, +0.21 more at four (more threads sharing
the slow clock), +0.15 GPU-side from the encoder on lit frames, and about
+0.55 host-side under the recorders. That last, largest, term is the one
the clock is suspected of amplifying rather than causing outright: 16
recorder processes each timestamping and calling the driver all read the
same serialised clock. The evidence for that reading is the 2026-09-04
three-camera run with the same recorders on a fast clock, where the host
segments did not grow at all (worker start 0.021, post-sync 0.051 with
recorders and interleaved crops). It is inference until the reboot A/B;
the GPU-side term and the p95 bursts are independent of the clock and
will remain after it, and the 09-04 interleave result stands as
measured.

## Addendum 2026-09-17 (after the reboot): the fast-clock numbers

Rebooted 13:16 with `tsc=reliable`; kernel log "Switched to clocksource
tsc", a clock read 80 ns (was 1946). Four cameras, fish detected on every
frame, CPU results path on, dmon on. All four runs exited cleanly,
including the crop-producer control with both recorders that had
segfaulted at teardown on the slow clock.

| Run (means over cams 2010094 / 2010096) | acq to worker | cpu_post_sync | infer mean / p95 | acq to detect mean / p95 / p99 | capture to pose done mean / p95 / p99 |
|---|---|---|---|---|---|
| crop-producer path, no recorders (`…_off_133106`) | 0.021 | 0.065 | 2.004 / 2.017 | 2.18 / 2.23 / 2.26 | 3.29 / 3.33 / 3.51 |
| fused path, no recorders (`fused_realfish_133225`) | 0.018 | 0.030 | 2.005 / 2.018 | 2.15 / 2.18 / 2.20 | 2.90 / 2.92 / 2.94 |
| fused path + both recorders (`…_133343`) | 0.019 | 0.027 | 2.15 / 2.82 | 2.45 / 3.04 / 3.23 | 3.24 / 3.82 / 4.02 |
| crop-producer + both recorders (`…_off_133505`) | 0.019 | 0.050 | 2.14 / 2.81 | 2.43 / 3.06 / 3.15 | 3.70 / 4.46 / 4.74 |

What the reboot settled. (1) The host-side inflation was the clocksource:
every host segment is back at or below the 2026-09-04 values (worker
start 0.02, post-sync 0.03 on the fused path), with recorders on or off.
(2) The fused pose path with a fish on four cameras is **2.90 ms mean /
2.92 p95 / 2.94 p99** without recorders and 3.24 / 3.82 / 4.02 with both
recorders, against 3.29 / 3.33 and 3.70 / 4.46 on the crop-producer path;
the p99 spread on the fused path without recorders is 0.04 ms. (3) The
detect side with no recorders is 2.15 mean on the fused path, i.e. the
four-camera engine-only floor equals the three-camera one. (4) The whole
remaining recorder cost is GPU-side and unchanged by the clock: infer_ms
2.00 -> 2.15 mean with 10 to 16 % of frames at 2.6 to 2.8 ms, acquisition
to detect +0.25 mean / +0.85 p95. The recorder itself: 5901/5901 frames,
0 drops, enqueue age p95 11.9 (fused) vs 13.2 (crop producer), detach
0.003 ms, pending 3 GOPs, 1.50 Mbit per frame at the cap.

dmon (1 s samples) during the fused+recorders run: detect dies (1,3,5,7)
sm 44 to 50 %, memory controller 20 % mean / 28 % max, enc 51 to 58 %
mean (100 % during their GOP half), PCIe rx about 1.97 GB/s mean (the
camera inflow; max 2.6) and tx about 1.0 GB/s mean (max 2.6, the other
die's shard reading frames during its half); the other dies (2,4,6,8) sm
22 to 33 %, mem 7 %, rx about 1.0 GB/s mean (max 2.5), tx 0.1. Neither
the link (peaks about 40 % of a Gen4 x4 direction) nor the memory
controller is saturated on 1 s averages, so the mechanism is sub-second.
The slow detect frames are not at the GOP boundary: by frame phase they
recur every 7 to 9 frames (70 to 90 ms) in bursts of a few frames, on
every camera, with the exact phases differing per camera pair. Something
with that period on the encoder side is the next thing to identify (see
the per-frame encoder CSV correlation that follows).

## Addendum 2026-09-17: what the recorder's per-frame CSVs say about the bursts

From `Cam2010094_external_encode_shard{0,1}_gpu{1,2}.csv` of the
fused+recorders run (`…_133343`), joined to the YOLO perf rows by
`recording_frame_id`:

- **The local shard (die 1, the detect die) encodes a frame in about 4 ms,
  not 10.** `encode_total_ms` 4.04 (fast detect frames) / 4.19 (slow),
  almost all of it `lock_bitstream_ms` waiting for the engine; submit 0.04
  ms. So during its GOP half the local NVENC is busy about 40 % of each
  10 ms period, not 100 %. The earlier claim on this page and in the
  journal that the engine runs at full duty came from the 2026-09-05
  single-shard measurement (95.6 fps), which was the copy path with copy
  fallbacks and a pinned pending cap, i.e. bound by the recorder's copies,
  not by NVENC. With the registered source the engine itself appears more
  than twice as fast as that. This needs a direct test (below).
- **The other-die shard (die 2) spends 7 to 9 ms per frame in
  `prepare_ms`**: 9.29 on fast detect frames, 7.20 on slow ones, encode
  itself 0.05. Prepare is that shard pulling the 20 MB Y plane out of the
  detect die's memory across the card switch into its own NVENC input.
  At 20 MB per 7 to 9 ms that is 2.2 to 2.8 GB/s, the `txpci` peak dmon
  shows on the detect dies, and it runs for 7 to 9 of every 10 ms during
  the other-die half: a nearly continuous read of the detect die's memory
  over its PCIe link, concurrent with the camera's 2 GB/s inflow on the
  same link. A peer copy through the on-card switch was measured at about
  6 GB/s in September, so this copy runs at well under half that rate,
  which says it is contending (with the inflow, with the detect die's own
  memory traffic) or is not a bulk copy engine transfer.
- **Burst structure**: 623 slow detect frames in 346 bursts of median 2
  frames; autocorrelation of the slow indicator peaks at lag 50 (0.49, the
  two-GOP cycle) and lag 9 (0.37). Half A (local shard) infer 2.17 vs half
  B (other-die shard) 2.10; the bursts straddle both halves. The 9-frame
  sub-period is not explained yet; the varying prepare time (7 to 9 ms)
  drifting against the 10 ms frame period is the natural candidate.

Two consequences. First, the split-GOP design rests on "one NVENC cannot
do 100 fps of 20 MP"; if the registered-source engine really takes 4 ms
per frame, one shard per camera on the detect die would encode at 100 fps
with margin, and the other-die shard, its 20 MB per frame across the
switch, and the whole card-level contention would disappear. Test: the
single-shard capacity spec with registered source, lit frames, and real
detections. Second, if split-GOP must stay, the lever is the other-die
shard's pull: push the frame with a copy engine transfer scheduled after
pose, or find why the pull runs at a third of the link's rate.

## Addendum 2026-09-17: single shard per camera cannot keep up; the 4 ms was latency, not throughput

Run `fourcam_fused_single_shard_fullframe_20260917_134036`: four cameras,
fused path, one full-frame shard per camera on its detect die
(`routing_policy single_shard`), registered source, crop video off, lit
frames. Result per camera: about 4,940 to 5,140 frames encoded of 5,733
submitted, 760 to 960 dropped, copy fallbacks on nearly every frame
(5,609 to 5,659: the recorder could not hold registered buffers and fell
back to copies), enqueue age p95 370 to 390 ms, run policy fail on three
cameras. Effective encoder throughput about 85 to 88 fps per engine on
lit 20 MP frames, in line with the 2026-09-05 figure of 95.6 fps on dark
frames. The detect side got worse than with split GOP (infer p95 3.09,
acquisition to detect p95 3.36) because the saturated engine and the
fallback copies both sit on the detect die.

So the 4 ms `encode_total` of the local shard in the split-GOP run is the
pipelined per-frame latency of an engine fed one frame every 10 ms, not
its throughput; at 100 fps the engine needs about 11 ms of work per
frame and saturates. The earlier correction in this document ("busy
about 40 %") is withdrawn: during its GOP half the local engine is at
full duty, as first stated, and split GOP is required as the September
capacity note said. Spec kept for reference; it fails the verifier by
design.

What stands from the CSVs: the other-die shard's 7 to 9 ms per-frame
pull of 20 MB across the card switch at 2.2 to 2.8 GB/s is the largest
single traffic on the detect die's link and memory, and it runs almost
continuously during the other-die half. The next diagnostic is a peer
copy microbenchmark between the two dies of a pair (20 MB, copy engine,
idle and under the pipeline) to learn whether that pull is contended or
mis-implemented; if a bulk transfer reaches the 6 GB/s the switch
allows, pushing the frame from the analytics side after pose would halve
the time the link is busy.

## Addendum 2026-09-17: peer copy microbenchmark, what `prepare_ms` really is, and the beat

Microbenchmark (scratchpad `peer_copy_bench.cu`, 20,358,144 bytes, 40
iterations, GPU events): between the two dies of a pair (1->2, 3->4,
5->6) and across pairs (1->3, 1->5) the transfer is identical:
`cudaMemcpyPeerAsync` 3.18 ms pull / 3.09 ms push (6.4 to 6.6 GB/s), a
kernel reading the peer mapping 3.18 ms, and a pitched
`cudaMemcpy2DPeerAsync` (4512 -> 4608 or 4864 pitch) also 3.19 ms. Local
D2D on a die: 0.25 ms (80 GB/s). **While the four-camera fused pipeline
ran (no recorders) the peer copy still ran at 6.4 GB/s**, so the camera
inflow does not slow the copy; but that run then failed with camera
frame drops on every camera: back-to-back peer copies saturating a die's
link starve the GPUDirect RDMA writes. The copy wins the arbitration, the
camera loses. Never run link-saturating transfers on a detect die during
acquisition; a paced 3.2 ms burst per 10 ms is what the recorder already
does and it drops nothing.

`prepare_ms` (tools/external_recorder_ipc_probe.cpp:4377-4401) is not the
copy: it is `WaitForNextInputFrameAvailable` (waiting for NVENC to free
an input buffer, i.e. waiting for the engine's pace) plus the 2D peer
copy plus an event synchronize. On the other-die shard the copy is about
3.2 ms of it; the remaining 4 to 6 ms is the wait for the engine. The
earlier reading of "7 to 9 ms per frame across the switch at 2.2 to 2.8
GB/s" is withdrawn: the link carries one 3.2 ms burst at 6.4 GB/s per
frame during the other-die half, about a third of that half's time.

The beat, now with a mechanism. The other-die shard's copy of frame k
starts when NVENC frees a buffer, which is tied to the completion of an
earlier frame's encode, so the copy's start time drifts against the
frame period by (encode period - 10 ms) per frame and sweeps through the
period with a beat of about 9 frames; whenever the 3.2 ms burst sweeps
across the next frame's inference window (0 to 2.2 ms after arrival) the
graph runs slow for one or two frames. That is the lag-9 autocorrelation
and the two-frame bursts. The local shard's engine at full duty during
its half adds the smaller, steady penalty.

Lever this implies (a design change in the recorder handoff, not a
tuning): give the other-die shard its frame by a push from the analytics
side at a controlled time, right after detect, into a buffer resident on
that die (a per-die owned-copy pool: for other-die GOPs the late owned
copy targets the shard's die, peer copy by kernel or copy engine at 6.4
GB/s, then the shard reads locally and the detect die is never read across
the link at an uncontrolled phase). The transfer would then occupy 2.3
to 5.5 ms of each period, never the next frame's 0 to 2.2 ms window. The
fused graph's trailing copy node is the natural place: same kernel, peer
destination. Expected gain: the other-die half's slow bursts and most of
the +0.85 ms p95; the local half's engine read remains.

## Addendum 2026-09-17: crop recorder's share on the fast clock

`fourcam_fused_recorder_fullframe_only_20260917_135836` (fused path,
full-frame split-GOP recorder only, crop video off, four cameras, fish,
TSC clock), against the same-day both-recorders run (`…_133343`) and the
no-recorder run (`…_133225`), cams 2010094 / 2010096:

| Configuration | acq to worker | infer mean / p95 | slow fraction | acq to detect mean / p95 | capture to pose done mean / p95 / p99 |
|---|---|---|---|---|---|
| fused, no recorders | 0.018 | 2.005 / 2.018 | 0.00 | 2.15 / 2.18 | 2.90 / 2.92 / 2.94 |
| fused, full-frame recorder only | 0.019 | 2.145 to 2.172 / 2.82 | 0.12 to 0.18 | 2.40 to 2.44 / 3.03 | 3.18 to 3.26 / 3.79 to 3.80 / 3.87 to 3.92 |
| fused, both recorders | 0.019 | 2.131 to 2.165 / 2.82 | 0.11 to 0.16 | 2.41 to 2.49 / 3.04 | 3.18 to 3.30 / 3.80 to 3.85 / 3.93 to 4.11 |

The crop video recorder costs nothing measurable on the fast clock (the
0.2 ms host-side share seen on the slow clock was the clock). The whole
recorder cost is the full-frame split-GOP recorder, and it is GPU-side:
the detect graph itself, in bursts. Crop video cannot run without the
full-frame recorder (the client refuses `crop_recording external_ipc`
without it), so "crop video only" is not a configuration that exists; the
figure that answers it is the full-frame-only row, which equals both.
The journal's figures carry these five configurations.

## Addendum 2026-09-17: per-card split of the slow frames, and the paced-copy test (no cameras)

A second-opinion review (`~/pose_latency_regression_second_opinion_2026_09_17.md`,
section 7) joined the 13:33 fused+recorders run by shard GOP and found the
two mechanisms live on different cards. Re-joined here from the same run
with the recorder's `Cam*_external_meta.csv` (`assigned_gpu_id` per
`recording_frame_id`), slow = infer_ms > 2.3:

| Camera, detect die, card | slow fraction, local-shard GOPs | slow fraction, other-die GOPs | infer mean local / other |
|---|---|---|---|
| 2010093, die 3, A | 0.173 | 0.033 | 2.186 / 2.085 |
| 2010094, die 1, A | 0.161 | 0.049 | 2.172 / 2.091 |
| 2010095, die 7, B | 0.172 | 0.178 | 2.177 / 2.166 |
| 2010096, die 5, B | 0.165 | 0.154 | 2.170 / 2.160 |

So: the local-shard half is slow on every die (the engine encoding on the
detect die), and the other-die half is slow only on card B (dies 5 to 8).
The other-die pull sweep described in the previous addendum is a card B
effect; on card A the pull costs a few percent of slow frames. A push (or
a phase-controlled pull) can therefore remove at most card B's other-half
share, roughly a quarter of all slow frames, and none of the local-half
term. Gate any A/B per camera and per shard half, not pooled, plus the
other-die shard's `prepare_ms` falling by about 3.2 ms as the direct
proof it reads locally, plus `camera_frame_id_gaps` and
`get_frame_errors` at zero (a saturated detect-die link starves RDMA).

The review's no-camera test, run 14:07 (`tools/paced_peer_copy.cu`, one
20 MB copy engine transfer out of the die every 10 ms, 3.24 ms each;
`trtexec --loadEngine --useCudaGraph` of the detect engine on the same
die, 8 s):

| Die | graph alone mean / p95 / p99 / max | with the paced copy out of the die |
|---|---|---|
| 1 (card A) | 1.973 / 1.981 / 1.986 / 2.108 | 2.002 / 2.054 / 2.061 / 2.225 |
| 7 (card B) | 1.972 / 1.979 / 1.983 / 2.152 | 1.999 / 2.051 / 2.057 / 2.230 |

A paced peer read out of a detect die costs its graph +0.03 mean and
+0.07 p95 on both cards alike, with no card difference and nothing like
the 2.6 to 2.8 ms slow mode. So the pull by itself is not the slow mode;
on card B the slow other-half frames need something the test lacks, most
plausibly the camera's RDMA inflow sharing card B's root port with the
peer read (the review notes card B's dies are PHB to the NIC ports that
serve its cameras, card A is SYS to every NIC). And the local-half term
on every die is the engine's own work, which the review ties to the
bitstream size of the frame NVENC is on (a 1 MB I-frame and a ~10-frame
rate-control oscillation of the P-frames, 118 to 178 KB): encoder-side
levers (CBR or a sized VBV, a smaller I-frame, a longer GOP) are spec
matrix changes and should run as controls alongside any code A/B.

Telemetry from the review (50 ms samples on the 13:58 run): every detect
die at 1755 MHz on slow and fast frames alike, 42 W of the 62.5 W cap; no
clock or power lever exists, the contention is in the memory system.

Plan, revised. (1) The cheap change first, in the recorder only: the
other-die shard stages its copy at descriptor arrival (already
detect-gated) into a local NVENC-registered staging buffer, instead of at
NVENC pace; the implementation map (this session's mapping agent) puts it
at about 60 lines in `tools/external_recorder_ipc_probe.cpp` using the
existing `acquire_staging_buffer` / `use_staging` /
`SetNextInputRegisteredResource` machinery, no protocol change, no new
pool, and the pool entry is released about 3 ms earlier. Gate as above;
expected to move card B's other half only. (2) In the same session, one
encoder-side control (CBR or VBV) for the local-half term on all dies.
(3) The two-pool push (analytics copies to a pool on the partner die by
copy engine, hello and FRAME carry a per-shard source GPU) only if the
residual read still shows in `infer_ms` after (1); it is the larger
change (pool allocation on the partner die, peer access in the analytics
process, per-shard prewarm descriptors, IPC import per device) and by the
14:07 test its extra gain over (1) is at most the +0.03 / +0.07 the read
itself costs. Note from the map: the two shards are threads in one
recorder process per camera, the recorder assigns GOP routing itself
(`gop_index % shards`), and the analytics side does not know the shard
GPUs today.

## Addendum 2026-09-17 14:40: early peer staging in the recorder (plan step 1) — card B gate met

Commit `b0921f9`. The other-die shard of the external recorder now
copies the pool frame into a recorder-owned, NVENC-registered NV12
staging buffer on the intake thread at descriptor arrival (already
detect-gated), records a per-buffer event, and enqueues the work item
with the staging index; the encode thread waits for an input slot, waits
for the copy event (usually landed), sends RELEASE, points NVENC at the
staging buffer through the external input slots and encodes. Same-die
shards are untouched (registered source). Flag: spec key
`external_recorder_early_peer_stage` → env
`ORANGE_EXTERNAL_RECORDER_EARLY_PEER_STAGE=1` (`--early-peer-stage`);
opt-in until the endurance run below passes. New shard CSV columns
`stage_wait_ms` (encode-thread wait for the copy) and `stage_copy_ms`
(GPU-timed copy). Staging buffers are allocated on demand up to
max(encoder buffers, queue depth) + 2 (30.5 MB each; 5 to 7 were used),
registered lazily by the encode thread (the encoder may not exist yet at
the first frames), shared under `staging_mutex_`; the summary prints
`early_stage_frames` and `early_stage_exhausted`.

Trap found on the first run: the intake thread's current device is the
source GPU (it imports the pool handles there); the staging block
switched it to the shard GPU and did not switch back, so the next
`cudaIpcOpenMemHandle` landed in the shard die's context and the
same-die shard's `nvEncRegisterResource` failed with error 23 after 25
frames (both shards then stopped; the client reported acquisition
starvation because the ingress held the deferred entries). The block
now saves and restores the device.

A/B, four cameras, fused path, both recorders, real fish, TSC clock,
60 s each, back to back (`fourcam_fused_recorder_realfish_earlystage_20260917_142320`
vs `fourcam_fused_recorder_realfish_20260917_142450`), joined per
camera and per shard half via `Cam*_external_meta.csv`, slow =
infer_ms > 2.3:

| Camera, die, card | slow local-half: control → staging | slow other-half: control → staging | other-half infer p95 | capture→pose mean / p99 | device-stage GPU p95 | other shard prepare p50 |
|---|---|---|---|---|---|---|
| 2010093, 3, A | 0.166 → 0.169 | 0.092 → 0.031 | 2.39 → 2.09 | 3.23/3.95 → 3.16/3.88 | 0.97 → 0.75 | 10.5 → 6.4 |
| 2010094, 1, A | 0.165 → 0.144 | 0.076 → 0.036 | 2.37 → 2.09 | 3.22/3.90 → 3.16/3.91 | 0.95 → 0.75 | 10.3 → 6.6 |
| 2010095, 7, B | 0.182 → 0.174 | 0.156 → 0.040 | 2.42 → 2.27 | 3.30/4.13 → 3.26/4.06 | 1.05 → 0.75 | 10.9 → 6.0 |
| 2010096, 5, B | 0.180 → 0.171 | 0.161 → 0.042 | 2.42 → 2.27 | 3.30/4.07 → 3.25/3.86 | 1.04 → 0.75 | 10.8 → 6.0 |

Gates: card B other half 0.16 → 0.04 (target ≤ 0.05) — pass; local half
unchanged — as predicted; other-die prepare down 4.4 ms (more than the
copy: the copy now overlaps the NVENC input wait) — pass; camera drops,
`get_frame_errors`, `acq_starve` all 0, 2950 frames per shard in both
arms — pass; pose device-stage GPU p95 back to the no-recorder 0.75 ms.

Controls:

- `external_recorder_peer_access` (explicit `cudaDeviceEnablePeerAccess`
  shard → source; the probe only ever relied on
  `cudaIpcMemLazyEnablePeerAccess`, which acts for the importing device):
  no change in either arm (peer-only run `…_peer_20260917_143142` ≈
  control; `…_earlystage_peer_20260917_143020` ≈ staging). The copy was
  already a peer transfer. `nvidia-smi topo -p2p r` shows P2P OK between
  all A16 dies; card B's dies are PHB to NIC4–7, card A's are SYS to every NIC.
- GPU-timed copy under load (`stage_copy_ms`): p50 7.0 ms card A, 8.3 ms
  card B (p95 7.6 / 8.9), not the idle 3.2 ms of `tools/peer_copy_bench.cu`:
  the pull yields to the detect die's own traffic. Issued at ~2.5 ms after
  acquisition it lands at ~9.5 ms (card A, before the next detect) and
  ~10.8 ms (card B, 0.8 ms into the next detect) — the residual 4%.
- `external_recorder_early_stage_push` (the same copy issued from the
  source die's context with `cudaMemcpyPeerAsync` on a stream on the
  detect die; run `…_earlystage_push_20260917_143548`): copy 3.1 ms p50,
  other-half slow 0.1 to 0.7%, capture→pose mean 3.02 to 3.07 ms, but
  the run FAILED with camera drops: 2749 / 1876 / 664 / 1365 dropped
  frames, acquisition at 50 / 82 / 95 / 84 fps. The detect die's outbound
  copy starves camera intake (the second opinion's caution, confirmed);
  the detect numbers describe a lighter load and are not comparable.
  Negative as implemented; only a paced, chunked push remains untested.

Files: `tools/external_recorder_ipc_probe.cpp` (options
`early_peer_stage`, `peer_access`, `early_stage_push`;
`enqueue_direct_source` staging block; `encode_one_direct_source`
early-staged branch; `acquire_staging_buffer_locked`,
`ensure_staging_registered`, `register_pending_staging_buffers`,
`ensure_peer_access`, `ensure_push_peer_access`, `ensure_stage_stream`,
`release_staging_buffer`, `recycle_staged_frame`),
`src/orange_headless_client.cpp` (the three spec keys → env), specs
`experiment_specs/fourcam_fused_recorder_realfish_{earlystage,earlystage_peer,peer,earlystage_push,earlystage_endurance}.json`.
Analysis: `scratchpad/ab4.py` pattern — merge `Cam*_yolo_perf.csv` with
`Cam*_external_meta.csv` on `recording_frame_id`, half = assigned_gpu_id
== source_gpu_id, and read the shard CSVs skipping the first 50 rows.

Next: (1) endurance `fourcam_fused_recorder_realfish_earlystage_endurance`
(600 s, started 14:38) → flip the default if clean; (2) encoder-side
control for the local-half term (CBR / sized VBV / longer GOP) as a spec
matrix change; (3) paced push only if the 4% residual on card B matters.

## Addendum 2026-09-17 15:00: endurance passed, early peer staging is the default

`fourcam_fused_recorder_realfish_earlystage_endurance_20260917_143856`
(600 s, four cameras, both recorders, real fish): 59,900 frames per
camera, 29,950 encoded per shard, `camera_dropped_frames` /
`get_frame_errors` / `acq_starve` 0 on every camera,
`early_stage_exhausted` 0, staging buffers bounded at 6 to 7 per shard,
other-die-half slow fraction (infer > 2.3) steady at 0.03 to 0.04 in
every two-minute slice (cams 93/94/95/96: 0.038 / 0.038 / 0.033 / 0.031;
local half 0.147 / 0.154 / 0.165 / 0.158), capture-to-pose-done mean
3.17 / 3.17 / 3.24 / 3.24, p99 3.87 / 3.89 / 4.07 / 3.83, device-stage
p95 0.75 on all; GPU-timed copy p50 7.2 ms (card A) / 8.3 ms (card B),
max 9.4.

Default flipped: `ORANGE_EXTERNAL_RECORDER_EARLY_PEER_STAGE` defaults to
1 in the probe and `external_recorder_early_peer_stage` to true in the
headless spec parser (`=0` / `false` restores the in-loop copy). Verified
with the unmodified `fourcam_fused_recorder_realfish` spec
(`…_20260917_145056`, pass): every other-die shard reports
`early_stage_frames=2950`. `external_recorder_peer_access` and
`external_recorder_early_stage_push` stay off (no effect / camera drops).

## Addendum 2026-09-17 15:40: the GPUDirect ring cannot alternate dies on SDK 2.55.02 (tested)

Question answered locally instead of by Emergent, with a probe mode
added to `tools/evt_stream_smoke.cpp` (run as root; the sudo wrapper
only allows the gop-split worktree's binary, so the user ran
`sudo …/orange-device-roi-20260912/targets/release/evt_stream_smoke`):

- `--gpu-direct-switch 4` (camera 2010093 on die 3, 8 buffers, switch
  `gpuDirectDeviceId` to 4 before buffers 4 to 7): every zero-copy
  buffer has `imagePtr == 0` at allocation (they are descriptors), and
  every received frame is a device-3 pointer into one contiguous SDK
  ring, 20,375,040 bytes apart, in order, wrapping after 24 frames. The
  allocation-time device is ignored.
- `--user-ring device --user-ring-mb 512` (a `cudaMalloc` ring on die 3
  passed as `EVTStreamAttribute{ringBufferPtr, ringBufferSize}`):
  `EVT_CameraOpenStream` returns 0 but frames land in the SDK's own
  ring, `in_user_ring=0` on all 30 frames.
- `--user-ring vmm-split --user-ring-gpu2 4` (a 512 MB
  `cuMemAddressReserve` range, first half `cuMemCreate` on die 3, second
  on die 4, `cuMemSetAccess` for both): same result, ignored.

So on SDK 2.55.02.21104 the landing device is fixed at stream open and
the user ring attribute is not used under GPUDirect. The alternating
landing design (journal entry "Would alternating the landing buffer
between the dies help?") needs Emergent. The question is rewritten in
`~/emergent_sdk_question_gpudirect_ring_two_devices_2026_09_17.md` with
these results as context, the SDK version filled in, and five questions
(application-supplied GPU ring incl. a VMM range; SDK-side split;
registration limits; PHB vs SYS receive; multicast to two ports as a
fallback). The local-half term stays the encoder's, and the encoder-side
controls remain the next lever.

## Addendum 2026-09-17 15:46: clean-room proof that same-die NVENC slows the detect graph

For colleagues who doubt that encoding on the detect die matters. No
cameras, no recorder, no pipeline: `trtexec --loadEngine=<detect engine>
--device=1 --useCudaGraph --duration=8 --noDataTransfers` on die 1,
alone and with `targets/release/nvenc_stress_load --gpu-id <die> --fps
100 --pattern <host-noise|solid> --duration 30` (4512x4512 HEVC p1 ll,
GOP 25, 150 Mbit/s; it cannot hold 100 fps so it runs the engine at
full duty, harder than production's 50 %):

| Condition | trtexec GPU compute mean / p95 / p99 / max (ms) |
|---|---|
| graph alone, die 1 | 1.960 / 1.963 / 1.965 / 2.124 |
| + NVENC on die 2 (neighbour, noise content) | 1.968 / 1.982 / 1.985 / 2.189 |
| + NVENC on die 1 (solid content, no host copies) | 2.453 / 3.028 / 3.051 / 3.076 |
| + NVENC on die 1 (noise content) | 2.585 / 3.058 / 3.201 / 3.936 |

Neighbouring-die encode: no effect. Same-die encode: +0.5 to 0.6 mean,
+1.1 p95, and the solid-content run (device memset input, no H2D)
shows it is the encoder's own memory traffic. With the earlier telemetry
(no clock or power change) this is memory-system contention. In the
pipeline it is the local-half term: 0.16 of frames at 2.6 to 2.8 ms
during the detect die's own GOPs vs 0.03 to 0.04 during the other die's
GOPs, same camera, same run, alternating every 250 ms.

## Addendum 2026-09-17 17:20: one camera on the RTX A6000 (fused path, no recorders)

Engines built for the A6000 (GPU 0) at the production tier:
`~/orange_data/detect/…_a6000_gpu0_trt100_fp16_bo5_avg32.engine` (+ manifest,
via `scripts/build_tensorrt_detect_engine.sh --device 0
--target-hardware-class A6000`, 367 s, standalone 0.80 ms) and
`~/orange_data/pose/pose_head_192_…/engines/…_a6000_gpu0_trt100_fp16_bo5_avg32.engine`
(direct trtexec, 258 s). The 256 cedar pose model has no ONNX on disk, so
its A16-built engine ran on the A6000 (TensorRT warns; same as 2026-09-12).
Specs `onecam_{a6000,a16}_fused_realfish{,_192}` (camera 2010096, 60 s;
runs `…_171543`, `…_171656`, `…_171809`, `…_171922`), all pass, 5,900
frames each with a pose, zero drops:

| | A6000 256 | A16 die 5 256 | A6000 192 | A16 die 5 192 |
|---|---|---|---|---|
| infer_ms mean / p95 | 0.708 / 0.716 | 2.002 / 2.017 | 0.713 / 0.717 | 2.003 / 2.018 |
| acq→detect mean / p95 | 0.945 / 2.161 | 2.141 / 2.157 | 0.997 / 2.398 | 2.151 / 2.168 |
| device-stage GPU mean / p95 | 0.464 / 0.471 | 0.737 / 0.741 | 0.568 / 0.572 | 0.585 / 0.589 |
| capture→pose done mean / p95 / p99 | 1.502 / 3.737 / 4.529 | 2.899 / 2.915 / 2.923 | 1.674 / 3.977 / 4.712 | 2.746 / 2.764 / 2.770 |

A6000 tail: 6.9 % of frames have acq→detect > 1.5 ms; on them `sync_ms`
is 2.31 mean / 2.94 p95 vs 0.68 while `infer_ms` stays 0.707, i.e. the
graph launch waited for the GPU (time-sliced against Xorg, gnome-shell
and a Chrome GPU process on GPU 0). Slow frames come in bursts of 1 to 2
about every 90 ms and grow over the run (1 to 2 per 5 s early, 54 to 78
per 5 s late). A clean number needs the desktop off GPU 0. On the A6000
the 192 head model is slower than the 256 cedar model (launch-bound with
84 SMs; yolo11n-pose has more layers than yolov8n-pose). Production
decision of 2026-09-05 unchanged. Correction: an earlier chat remark that
the A6000 build was slow was wrong; the background watcher had matched
its own process name and never returned.

## Addendum 2026-09-17 evening: Emergent's answer on the GPUDirect ring; multicast route

Emergent (senior engineer, same day): the NIC must operate on one
physically contiguous ring buffer, so (2) a ring split across two GPUs
by the SDK is not possible; (1) `EVTStreamAttribute.ringBufferPtr` is
host memory only today, they could extend it to GPU memory and take a
two-device VMM range but flag NIC/Rivermax support of the layout, DMA
efficiency across the boundary, and the wrap-around frame being split
across GPUs; (3) registration limit ~8 GB, one ring; (4) same-root-
complex transfers efficient, otherwise latency expected; (5) multicast
would work ("the NIC transmits the data to two GPUs by duplicating it;
you pick the frames to process"), performance concerns, worth trying.
They asked how we move data between GPUs and suggested NVLink P2P.
Answer: cudaMemcpyAsync peer over the A16's on-card PCIe switch (6.4 GB/s
idle, 2.5 to 2.9 GB/s under load); the A16 has no NVLink
(`nvidia-smi nvlink -s`: all links inactive; only the A6000 has NVLink
and only to another A6000).

Multicast route (no SDK change): SDK streaming modes 2 (master multicast
with subscription) and 4 (slave multicast) in `EmergentCamera.h`,
examples `/opt/EVT/eSDK/Examples/EVT_Mcast/{EVT_Mcast_Master,EVT_Mcast_Slave}`.
Master opens the camera + stream to a multicast address/port; a slave
process opens only the stream (`ifaceAddress` = the NIC port IP,
`multicastAddress`, `portMulticast`) with its own `gpuDirectDeviceId`.
Both dies then hold every frame → the alternating design (die A infers
even GOPs from its ring while die B encodes the previous odd GOP from
its ring, then swap) with no peer copy and no die encoding while it
infers. Costs to measure: 2x RDMA inflow per camera, a second SDK
receiver per camera on the CPU, a second ring (~600 MB), inflow cost on
the detect graph.

First test: add a `--mcast-master <ip:port>` / `--mcast-slave <iface>
<ip:port>` mode to `tools/evt_stream_smoke.cpp`; master on die 1,
slave on die 2, camera 2010096 at 100 fps for 60 s, both counting frames
and drops; trtexec detect graph looping on die 1 during it. Pass: both
rings at 100 fps, zero drops, graph within +0.05 ms of alone.

## Addendum 2026-09-18 01:20: INT8 detect engine work (in progress) and PTP-gated raw capture

Tooling (commits d12325f, 6564b26, e20402f, 1305ada..dcd66ae):
`scripts/calibrate_tensorrt_int8.py` (production preprocessing mirrored
from `optimized_yolo_preprocess.cu`, entropy v2 or `--minmax`, cache +
JSON record; `--reuse-cache`, `--fp16-layers <regex>`, `--save-engine`,
`--opt-level` for experiments), `scripts/compare_tensorrt_engines.py`
(held-out parity: missed/extra, IoU, centre offset, confidence delta),
`scripts/build_tensorrt_detect_engine.sh --precision int8 --calib-cache`
(trtexec `--int8 --fp16 --calib=`; manifest records cache hash and
record), `scripts/extract_preenc_ref_frames.py` (NV12 dumps → PGM +
manifest), `pre_encoder_reference_capture.sample_every` and
`.synchronous`, spec `fourcam_fused_calibration_capture_raw`,
`scripts/evt_dump_frames.sh` (single-camera smoke dump; NOT for lit
multi-camera capture: only 2010096 lights the tanks and the others must
be PTP-gated to its exposure). TensorRT 10.0.1 Python wheel installed
into the juicebox env (cudart via ctypes; run with
`LD_LIBRARY_PATH=/usr/local/TensorRT-10.0.1.6/lib:/usr/local/cuda/lib64`
and `PYTHONDONTWRITEBYTECODE=1`, `scripts/__pycache__` is root-owned).

Calibration sets: provisional decoded-HEVC set from the 09-17 endurance
recordings (`calibration_hevc_20260917/{calib,holdout}`: 975 + 156 PGMs,
manifest with detection confidences); PTP-gated raw sleeping-fish set
(`calibration_raw_20260918_sleeping_preenc/` dumps, `…_sleeping/frames`
881 PGMs, luma means 160 to 205, cross-camera timestamps on the 10 ms
grid). Async capture ring fails on the 4th capture in this configuration
(stride or not) → `synchronous: true` in the spec.

INT8 results so far: engine `…_a16_gpu5_trt100_int8_bo5_avg32.engine`
builds (cache used, 178 layers, 77 INT8 convolutions); trtexec CUDA
graph on die 5: 1.572 mean / 1.545 median / 1.72 p95 vs FP16 1.962 /
1.959 / 1.964. Parity FAILS: 0 of 146 detections. Diagnosis: same with
the Python builder; all-layers-FP16 INT8-flagged build detects (0.715 =
FP16); backbone / neck / head+attention forced FP16 individually: still
0. Cache scales plausible. Single-layer INT8 and min-max tests pending
(`scratchpad/int8_bisect2.log`, `int8_minmax.log`). Do not use the INT8
engine until parity passes.

## Addendum 2026-09-18 01:30 (written 12:50): INT8 root cause and min-max engine gates

Root cause of the blind INT8 engine: entropy (KL) calibration clipped
the early feature activations on this background-dominated data
(model.0 act range 8 vs 64 true, model.1 act 24 vs 133, several neck
activations 3 to 4 vs 23 to 34; head ranges agree). Found by: cache
decode → same failure via trtexec and the Python builder → all-layers-
FP16 INT8 build detects → backbone/neck/head/attention FP16 one at a
time still blind → single-layer INT8 (model.1 alone kills it) → min-max
recalibration detects (0.728 vs FP16 0.715). Comparison script:
`scratchpad` python in the journal entry; caches
`calibration_hevc_20260917/int8_entropy_v2.cache` (do not use) and
`int8_minmax.cache` (+ `.trtexec` twin: trtexec only accepts a cache
whose header names EntropyCalibration2; the calibrator now writes the
twin automatically; the JSON record keeps the true calibrator).

Engine `…_a16_gpu5_trt100_int8mm_bo5_avg32.engine` (+ manifest; build
id passed with `--build-id`), 437 s. trtexec CUDA graph die 5: INT8
1.544 mean / 1.548 p95 / 1.707 max vs FP16 1.959 / 1.962 / 2.123.
Parity (`compare_tensorrt_engines.py`): holdout HEVC 156 frames: FP16
146 / INT8 150 detections, 0 missed, 4 extras at 0.33 to 0.50 (FP16 <
0.3), IoU median 0.944 p05 0.81, centre offset 1.7 px median / 6.5 p95,
conf delta mean -0.027 (p05 -0.09, p95 +0.04); raw sleeping set 881
frames: 660 / 660, 0 missed, IoU median 0.959, offset 1.8 / 7.1 px, conf
delta mean +0.014 (p05 -0.07, p95 +0.13). The 0.05 confidence gate
fails on both sets; the threshold-crossing gate passes. 31 frames on
cam 2010095 (no visible fish, debris specks): FP16 top detection a
speck at 0.47, INT8 a different speck at 0.69 (`parity_int8mm_vs_fp16.json`
rows with iou 0); empty-tank false positives exist in FP16 too, INT8 is
more confident on them on that camera. Status: candidate; rig run with
fish + raw fish calibration set decide. Reports:
`calibration_hevc_20260917/parity_int8mm_vs_fp16_holdout.json`,
`calibration_raw_20260918_sleeping/parity_int8mm_vs_fp16.json`.
Trap: `pkill -f`/`pgrep -f` patterns that appear in the calling shell's
own command line match that shell (a heredoc mentioning the script name
counts); kill by pid.

## Addendum 2026-09-18 15:30: INT8 on the A6000

`…_a6000_gpu0_trt100_int8mm_bo5_avg32.engine` (+ manifest) built from
the same `int8_minmax_trtexec.cache` (386 s). trtexec CUDA graph on GPU 0:
INT8 median 0.569 / p90 0.570 vs FP16 0.641 / 0.644 (means 0.59 vs 0.71
carry the desktop tail; compare medians): 11 % vs 21 % on a die (the
A6000 is launch-bound on this network). Parity A6000 INT8 vs A6000 FP16
on the 881 raw sleeping frames: 660/660, IoU median 0.959, offset p95
6.4 px, conf delta p95 0.13, same cam-95 debris disagreements
(`calibration_raw_20260918_sleeping/parity_a6000_int8mm_vs_fp16.json`).
Spec `onecam_a6000_fused_realfish_int8` (control `onecam_a6000_fused_realfish`).
Floor note for the 600 fps target: detect ≈ 0.57 ms on a big GPU is
kernel-count-bound; below that needs fewer kernels (smaller detector
input, tracking-driven crops with a watchdog detector, or one pose pass
on a megapixel ROI). Architecture check from engine strings: detect v004
= YOLO11n (head model.23, attention), pose 256 cedar = YOLOv8n-pose
(head model.22, no attention), pose 192 head = YOLO11n-pose.

## Addendum 2026-09-18 evening: offline replay tool (overlay plan step 3) and first replay results

`scripts/replay_pose_pipeline.py`: runs N pipeline configurations
(`name=detect.engine:pose.engine:pose_input:pose_crop[:detect|track]`)
over the same frames (PGM dir + manifest, or `--video <mp4>
--video-range a-b`), mirroring the pipeline arithmetic (letterbox
sampler, crop origin int(centre)-crop/2 clamped, pose letterbox from
crop to input, decode = best candidate by channel 4, keypoints 5+3k
mapped to crop px, thresholds 0.25). `track` mode: crop from the
previous pose centroid (`--velocity` = constant-velocity), detector
still run each frame as the trailing check, re-acquire on init / loss
(no pose, conf < `--min-pose-conf`, keypoint within `--edge-margin` of
the crop edge, detector centre > `--reacquire-px` away) / watchdog
(`--watchdog-every`, default 25). Outputs per-config JSONL, summary.json
with pairwise keypoint px (median/p95/max per label) and track stats,
side-by-side clips (`--clip`, native + slow) and a contact sheet. Env:
juicebox python with `LD_LIBRARY_PATH=/usr/local/TensorRT-10.0.1.6/lib:/usr/local/cuda/lib64`.

Results (camera 2010096, endurance recording, device 5):
- Fast-turn clip rec 35880-36280 (401 frames, jumps to 97 px/frame):
  int8_256 vs fp16_256 keypoints 0.75-1.0 px median, 2.5 p95, 5 max,
  conf -0.002; head_192 vs fp16_256 1.3-1.6 px median, 2.4-3.2 p95,
  8.6 max, conf +0.046, no missing poses; track_256 (velocity) vs
  fp16_256 1.1-1.4 px median, 2.7-3.3 p95, 4.9 max, poses on 401/401,
  0 near-edge frames, 384 tracked frames, 17 re-acquires (1 init + 16
  watchdog), tracked-vs-detected centre p50 6.6 px / p99 74 px.
- Loss clip rec 43194-43594 (fish along the air tube; detector has
  264/401): track_256 poses on 294/401 (+30 over detect-driven), 0
  near-edge, re-acquire reasons init 2 / watchdog 10 / no_pose 107,
  keypoints 1.0-1.9 px median vs the reference on the 264 shared frames.
Outputs under `<run>/pose_overlays/replay_2010096_{jump,loss}/`.

## Addendum 2026-09-18 17:10: 192 head model full-run replay, four cameras

`replay_pose_pipeline.py --every 5` over the endurance recordings
(11,980 frames per camera), fp16_256 (YOLOv8n-pose 256 cedar) vs
head_192 (YOLO11n-pose), same detector: keypoint median / p95 px and
span ratios (192 as % of 256, inter-eye / eyes-to-bladder): 2010093
3.3 / 12.5, 110 / 110, conf +0.14, poses 10,987 vs 11,144; 2010094
1.8 / 5.3, 103 / 104; 2010095 1.3 / 3.0, 101 / 101; 2010096 1.5 / 3.6,
100 / 101. Cam 93: disagreement in the bladder keypoint (median 3.9 px,
p95 12.5; eyes 2.7 to 3.4), 192 more confident on those frames (0.94 vs
0.79), smallest fish (ref inter-eye 16.4 px). Anatomical call owed from
the clips; gate open on that only. Outputs
`<run>/pose_overlays/replay_192_fullrun/Cam<serial>/{stats,clip_*}`
(stats summaries, six side-by-side clips per camera: 256 | 192 | track).
Trap: a python process inside `while read` consumes the rest of the
list on stdin; feed the loop from fd 3 and give the process `< /dev/null`.

## Addendum 2026-09-18 18:30: live fish runs, INT8 detect and 192 head, with and without recorders

Eight runs (`fourcam_fused_realfish{,_192,_int8,_int8_192}_20260918_18{1645,1803,1921,2039}`,
`fourcam_fused_recorder_realfish{,_192,_int8,_int8_192}_20260918_18{2318,2440,2602,2758}`),
all pass, ROI match 5901/5901, zero drops. Capture→pose mean / p95 / p99:
no recorders FP16+256 2.91/2.94/2.95, FP16+192 2.75/2.77/2.79, INT8+256
2.49/2.51/2.53, INT8+192 2.33/2.35/2.36; both recorders FP16+256
3.17-3.24/3.78/3.81-4.07, FP16+192 2.98-3.07/3.62/3.66-3.93, INT8+256
2.67-2.78/3.26-3.30/3.32-3.48, INT8+192 2.51-2.61/3.14/3.17-3.21. Detect
graph with recorders: FP16 p95 2.81 (+0.8 over mean, the local-half
term); INT8 p95 1.63 card A / 1.72-1.74 card B (+0.02 / +0.1): INT8
halves the activation traffic and so the encoder contention penalty.
192 head: 0 no-pose frames everywhere, pose conf 0.97 std 0.003-0.004;
256 cedar: 757 no-pose frames on cam 2010096 without recorders (132
episodes, longest 111, det conf 0.44 on those frames, box size normal),
203 with recorders, 8 / 64 with INT8 detect. INT8 det conf p05 on cam
96 0.69-0.73 vs FP16 0.78-0.79 (all detections still made). Raw fish
calibration capture `calibration_raw_20260918_fish_preenc` (220 frames
per camera, PTP-gated, synchronous capture) → `…_fish/frames`; raw
recalibration + `int8mmraw` engine + parity vs the HEVC-calibrated one
in `scratchpad/raw_recal.log`. Analysis script: `scratchpad/fish_analyze.py`
(pose_events + yolo_perf + pose_perf per camera).

## Addendum 2026-09-18 18:45: raw recalibration equivalence; live overlays; tail decomposition

Raw fish set `calibration_raw_20260918_fish/frames` (882 PGMs, luma
medians 168-217) + every 8th sleeping frame → `calibration_raw_20260918/`
(992 frames, `int8_minmax.cache` + `.trtexec` twin) → engine
`…_a16_gpu5_trt100_int8mmraw_bo5_avg32.engine` (452 s). On the raw fish
frames vs FP16 (879 detections): raw-cal 881 detections, 0 missed, IoU
median 0.950 p05 0.894, offset p95 4.4 px, conf delta p95 0.075; hevc-cal
882, 0 missed, 0.941 / 0.893, 3.8 px, 0.068; raw-cal vs hevc-cal IoU
0.95, conf delta p95 0.053, 1 missed. Decoded HEVC frames are an
acceptable calibration source for this detector; the hevc-cal engine
(`int8mm`) stays the candidate. Overlays from the live 192 recorder run:
`fourcam_fused_recorder_realfish_192_20260918_182440/run_0001*/pose_overlays/{2010096,2010093}`.

Recorder-on tail (INT8+192, cam 2010095): infer p95-mean 0.086,
device-stage p95-mean 0.006, sync_ms p95-mean 0.528, acq→detect p95 local-
die GOPs 2.556 vs other-die 2.267 → the graph starts late on local-die
GOPs. `external_recorder_registered_source: false` control made it worse
(c2p p95 3.51 / p99 3.83, spec `…_int8_192_copysource`). Hypothesis: the
in-graph 20 MB pool copy at the end of the fused graph delays the next
graph on the same stream while the encoder reads the pool. Fix under
test: `ORANGE_ANALYTICS_COPY_STREAM` / spec `analytics_copy_stream`
(copy issued on the device stage stream after graph_end_event via
issue_late_owned_copy_on_stream; spec `…_int8_192_copystream`).

## Addendum 2026-09-18 19:00: copy-stream and MPS tests, both negative; recorder-side GPU work per frame

Copy-stream (`analytics_copy_stream`, spec `…_int8_192_copystream_184344`):
sync p95 2.43-2.45 vs 2.41-2.42, c2p p95 3.18-3.21 vs 3.14, host issue
cost 0.02 ms → the in-graph pool copy was not delaying the next graph.
MPS (`nvidia-cuda-mps-control -d` as root, spec `…_int8_192_mps_185435`,
RDMA + NVENC fine under MPS): graph GPU time mean 1.86-1.89 / p95 2.40
(from 1.61-1.63 / 1.63-1.72), wait p95 2.38 (unchanged), c2p mean
2.75-2.82 / p95 3.24 / p99 3.27-3.48 (worse). Two contexts: graph flat,
starts late; one context: starts on time, runs slow. Conclusion: the
recorder process executes ~0.5 ms of real GPU work on the detect die per
locally encoded frame (candidate: driver-side surface conversion of the
registered pitch-linear NV12 buffer on map). Next: nsys trace of a
standalone registered-input encode loop (no rig) to see the per-frame
CUDA activity; then either an input layout NVENC consumes without a
copy, or encoding off the detect die. MPS is stopped.
