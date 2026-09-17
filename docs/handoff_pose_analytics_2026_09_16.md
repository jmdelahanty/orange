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
