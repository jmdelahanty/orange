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
