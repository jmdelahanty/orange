# Inspection Report: The 192 px Fish-Head Pose Model On pancake0

Date: 2026-09-16. Read-only inspection requested by
`~/pancake0_pose_head_192_acquisition_inspection_prompt.md`. Nothing was
built, edited, installed, registered or activated. This report lives on the
`agent/analytics/device-roi-20260912` branch alongside the code it refers
to.

## 1. Artifact verification

Bundle: `~/orange_data/pose/pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915/`.

| File | Bytes | SHA-256 | Matches the brief |
|---|---|---|---|
| `pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915.onnx` | 10,717,230 | `db5c8c33…e84f12f` | yes |
| `…canonical.manifest.json` | 1,238 | `72a8c2a8…c3c6ab` | yes |
| `pose_model_input_contract.json` | 4,133 | `64cf20a2…3d1ee83` | yes |

Model identity from the manifest: `set_id pose_head_192_recovered_reviewed_v001`,
`run_id …_yolo11n_100e_20260915`, task pose, status complete. The
canonical manifest references two files that are not in the bundle: the
source export manifest (`exports/onnx/….onnx.manifest.json`, digest
`6b1e5288…`) and `weights/best.pt`. Neither is needed to build or run the
engine; the export manifest matters only for the existing build wrapper
(section 6).

ONNX interface, confirmed by string inspection (no `onnx` Python module on
this host): tensors `images` and `output0`, producer PyTorch 2.5.1, head
module `model.23` (YOLO11 layout). The brief's declared interface,
`images FLOAT [1,3,192,192]`, `output0 FLOAT [1,14,756]`, opset 13, batch 1
static, no NMS, is consistent with the file: 756 is the anchor count of a
192 input at strides 8, 16 and 32 (576 + 144 + 36), and 14 channels is
4 box + 1 score + 3 × (x, y, confidence). The engine build (section 6)
prints the bindings and is the authoritative confirmation.

Input contract (`pose_model_input_contract.json`, schema
`palette.pose_model_input_contract` v3): a 192×192 uint8 luma crop,
identity spatial transform, no resize, no letterbox, luma repeated into
three channels, `uint8 / 255` to float32, NCHW, results in submitted crop
pixel coordinates. Keypoint order `swim_bladder, eye_left, eye_right`,
skeleton `pose_schema:traditional_v1`. A preprocessing probe is defined:
pattern `uint8_luma_mod_251_x3_y5_repeated_three_channels`, whose
float32 little-endian `[1,3,192,192]` tensor must hash to
`394dbaf8…0cffb0`. Postprocessing settings of the reference canary:
`conf 0.25`, `iou 0.5`, `max_det 1` (not embedded in the ONNX).

## 2. Host and runtime identities

| Item | Value |
|---|---|
| GPUs | GPU 0 RTX A6000 (49 GB); GPUs 1 to 8 NVIDIA A16 dies, 15.3 GB each, compute capability 8.6 (GA107, 10 SMs per die) |
| Driver | 535.183.06 |
| CUDA | 12.2 (nvcc V12.2.140, `/usr/local/cuda`) |
| TensorRT | 10.0.1.6 at `/usr/local/TensorRT-10.0.1.6`; `trtexec` reports `TensorRT v100001` |
| Orange runtime linkage | `orange_client` links `libnvinfer.so.10` and `libnvinfer_plugin.so.10` from the same 10.0.1.6 tree, so engine and runtime share a TensorRT major |
| Build device | any A16 die; GPU 5 (`GPU-840b0989…`) is the die the existing detect and pose engines were built on and is not one of the three detect dies used in the latency runs (1, 3, 7). Engines built on one GA107 die load on the others; the same plan also loads on the A6000 with a cross-device warning (measured 2026-09-12) |
| Camera links on 2026-09-12 | 2010094 and 2010095 had carrier; 2010093 and 2010096 did not. Rig recommissioning was in progress on 2026-09-14 |

## 3. Repository identities

| Worktree | Branch | Commit | State |
|---|---|---|---|
| `/home/jeremy/orange-gop-split-a16` (shared, another session's) | `agent/acquisition/shaman-v2-authoritative-20260824` | `ca817a0` 2026-09-15 | 10 dirty files, active |
| `/home/jeremy/orange-device-roi-20260912` (this report) | `agent/analytics/device-roi-20260912`, branched from shaman-v2 at `5cf21a9` | `c8ae644` | clean |
| `/home/jeremy/orange-pose-review-2026-09-04` | `review/pose-second-stage-2026-09-04` | `eb1deb2` | docs only, superseded by the device-ROI branch |

Remote `git@github.com:jmdelahanty/orange.git`. Local `origin/main` is
`6b2d7fd`. `AGENTS.md` describes the shared worktree's headless PTP run
and the performance target; it is written for the `exp/gop-split-a16`
branch name and predates the shaman-v2 branch.

**Where implementation should start.** Not from `origin/main`. The pose
backend, the copy-after-detection lifecycle, the crop pipeline, the
headless pose spec keys and the device-side ROI exist only on the shaman-v2
line. Start from `agent/analytics/device-roi-20260912` (or the shaman-v2
head plus a cherry-pick of its commits), in a fresh worktree if the other
session is still active in the shared one. The brief's premise that Orange
has only the detector contract holds for `origin/main` and does not hold
here.

## 4. Current inference architecture, with references

Line numbers are in `agent/analytics/device-roi-20260912` at `c8ae644`.

1. **Full-frame detection.** `YoloWorker::WorkerFunction`
   (`src/yolo_worker.cpp`): preprocess reads the camera buffer into the
   fixed 640 input (`preprocess_gpu`, `src/yolov8_det.cpp:213-254`,
   letterbox with `pparam.ratio/dw/dh`), then `YOLOv8::infer`
   (`:346`) launches the captured CUDA graph of `enqueueV3` plus four
   device-to-host copies (`capture_infer_graph`, `:257-317`). The engine
   ends in EfficientNMS with outputs `num_dets, boxes, scores, labels`
   (`postprocess`, `:395-401`). The detector contract the brief describes
   is exactly this and it is untouched.
2. **Detection-centroid selection.** CPU path: `postprocess` un-letterboxes
   and clamps; `CropProducerWorker::ProcessEntryImpl`
   (`src/crop_producer_worker.cpp:508-520`) takes the highest-score box and
   centres a fixed square on it, clamped to the frame. Device path (new,
   behind `ORANGE_ANALYTICS_DEVICE_ROI`): `launch_detect_roi_kernel`
   (`src/detect_roi.cu`) reads the NMS buffers on the device after the
   graph and writes both the video-crop and the pose-crop origin
   (`src/detect_roi.h`); the worker compares them with the CPU result per
   frame (`src/yolo_worker.cpp`, `device_roi_*` counters).
3. **Crop extraction.** `CropProducer::Produce` (`src/crop_producer.cpp:637`)
   waits on the pool copy's ready event (`:718-719`) and copies a
   `crop_w × crop_h` window from the pool copy into a pooled `CropFrame`
   (`:788-795`), records `crop_ready_event` (`:807`), and leases it to the
   pose worker (`:816-829`). The crop size is one value per camera:
   `crop_recording.crop_size_px` when crop recording is on, else
   `crop_pipeline.crop_size_px` (`src/orange_headless_client.cpp:4743`).
   A separate pose crop size, `pose_worker.crop_size_px`, exists on this
   branch as a spec key and in the device kernel, but the pose worker does
   not yet consume a separate crop (section 5).
4. **Channel conversion and normalisation.** `TensorRtPoseBackend::infer`
   (`src/pose_worker.cpp:291-335`) calls `launch_optimized_yolo_preprocess`
   (`:310`, kernel in `src/optimized_yolo_preprocess.cu`): bilinear
   letterbox resize from the crop to the engine input, luma replicated to
   three planes, divide by 255, NCHW float32. With a 192 crop and a 192
   input the scale is 1 and the padding is 0, so every output pixel samples
   one source pixel exactly: the contract's identity transform.
5. **TensorRT bindings and enqueue.** `bind_metadata` (`:342-416`) reads
   the engine's IO tensors, requires FP32 input and output (`:366`), an
   input of `1×3×H×W` (`:385`), and an output of `1×C×N` with `C ≥ 8` and
   `(C − 5) % 3 == 0` (`:410`); `K = (C − 5) / 3` (`:413`).
   `allocate_buffers` and `bind_tensors` fix the addresses; `infer`
   calls `enqueueV3` (`:320`), copies the output to pinned host memory and
   synchronises the stream (`:332`). No CUDA graph on the pose engine yet.
6. **Decoding and top-1.** `decode_best_pose` (`:444-498`): scans channel 4
   over N candidates for the best score above `confidence_threshold_ =
   0.25`, undoes the letterbox (identity here), clamps keypoints to the
   crop, marks a keypoint visible when its confidence is at least 0.25,
   and emits one instance. No IoU NMS, which is equivalent to
   `max_det = 1`. Labels for K = 3 are `bladder, eye_left, eye_right`
   (`default_pose_keypoint_labels`, `:152-163`).
7. **Mapping to sensor coordinates.** `publish_pose_result_v2`
   (`:892-975`) adds `frame.crop_x`, `frame.crop_y` to each keypoint before
   writing the shaman v2 slot; the event log record keeps crop-local
   coordinates plus the crop origin (`build_pose_event_record`, `:827-890`).
8. **Publication.** Per-frame JSONL (`Cam*_pose_events.jsonl`, schema in
   `src/pose_event_log.h`), the per-recording `Cam*_pose_perf.csv` summary,
   and the shaman v2 IPC slot consumed by citrus. The verifier reads the
   event log through `pose_event_log_validation.h`.

The historical `origin/pose` implementation (`postprocess_kp`, two class
scores and four keypoints) is not present on this branch and nothing from
it is needed; the branch's decoder is already parameterised by K.

## 5. What is reusable, and every incompatibility

**Reusable as is:** the pose backend and its contract checks, the decoder,
the crop-origin arithmetic, the event log and IPC path, the headless pose
spec block, the verifier, the measurement runner and the perf columns, the
engine build wrapper's `trtexec` settings, and the device-side ROI.

**Gaps against this model and contract:**

| # | Gap | Severity | Fix |
|---|---|---|---|
| 1 | The pose worker consumes the single per-camera crop. With crop video at 384 the pose stage would receive a 384 crop and letterbox it down to 192, which is not the identity contract. With no crop recording and `crop_pipeline.crop_size_px = 192`, the single crop is 192 and the contract holds. | blocks the production configuration, not the first canary | wire `pose_worker.crop_size_px` into the crop producer (a second, pose-sized `CropFrame` or a crop from the pool copy into the pose input), per the design doc's "two crops" |
| 2 | `pose_worker.input_width/height` in specs are metadata (validated `> 0`); the backend takes dims from the engine. Specs must say 192 for the snapshot to be truthful. | cosmetic | set in the spec |
| 3 | Keypoint label `bladder` versus the contract's `swim_bladder`; `skeleton_id` free text. Order is identical. | metadata only, but downstream joins by name would break | take labels from `skeleton_path` or a spec list; record `pose_schema:traditional_v1` |
| 4 | Visibility threshold 0.25 and score threshold 0.25 are compiled constants; the contract states `conf 0.25` and leaves visibility unspecified. | decision needed | expose both as spec keys with the current defaults |
| 5 | The preprocessing-probe digest has not been checked against Orange's kernel. Identity is expected by construction, but the contract provides the test and it has not been run. | must pass before parity | a small harness (section 8) |
| 6 | No ONNX Runtime or PyTorch on this host, so a TensorRT-versus-ONNX parity check cannot run locally. | parity needs a reference | reference outputs on fixed crops produced on the training machine; Orange dumps its TensorRT outputs for the same crops |
| 7 | The build wrapper `scripts/build_tensorrt_detect_engine.sh` expects the Palette export manifest (`….onnx.manifest.json`), which the bundle does not include, and its defaults name detect directories. | tooling | either obtain the export manifest or run `trtexec` directly and write the manifest with `scripts/write_tensorrt_engine_manifest.py` |
| 8 | The engine plan will be GA107-specific; it loads on the A6000 with a warning but is not a supported configuration there. | none for A16 | build one plan per hardware class if the A6000 is ever a target |

Nothing in the detector path needs to change, and nothing here weakens
it: the pose contract is a separate backend with its own checks.

## 6. Engine build command and artifact layout

Direct command (the brief's shape, with paths filled in; do not run until
approved):

```bash
/usr/local/TensorRT-10.0.1.6/bin/trtexec \
  --device=5 \
  --onnx=/home/jeremy/orange_data/pose/pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915/pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915.onnx \
  --saveEngine=/home/jeremy/orange_data/pose/pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915/engines/pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915_a16_gpu5_trt100_fp16_bo5_avg32.engine \
  --fp16 \
  --builderOptimizationLevel=5 \
  --avgTiming=32 \
  --profilingVerbosity=detailed \
  > .../engines/build_a16_gpu5_trt100_fp16.log 2>&1
```

No shape profile: the ONNX is static batch 1. The wrapper
`scripts/build_tensorrt_detect_engine.sh` runs the same `trtexec` settings
and also benchmarks and writes a manifest, but wants the export manifest
(gap 7); if that file is obtained, prefer the wrapper with
`--output-engine-dir …/pose/<run_id>/engines --staging-root
…/trt_builds/pose --target-hardware-class A16 --status candidate`.

Artifact layout, versioned, never overwriting the active engine
(`/home/jeremy/pose_cedar_shadow_filtered_gray_latest_traditional_a4c30ae1_v001_r001_fp16.engine`):

```
orange_data/pose/<run_id>/
  <run_id>.onnx, <run_id>.canonical.manifest.json, pose_model_input_contract.json   (as delivered)
  engines/<run_id>_a16_gpu5_trt100_fp16_bo5_avg32.engine
  engines/build_a16_gpu5_trt100_fp16.log
  engines/benchmark_a16_gpu5_trt100_fp16.log            (trtexec --loadEngine --useCudaGraph)
  engines/<run_id>_a16_gpu5_trt100_fp16_bo5_avg32.manifest.json
  parity/<date>/  probe_digest.txt, fixed_crops/, trt_outputs/, reference_outputs/, report.json
  canary/<experiment_id>/ -> pointer to orange_data/exp/unsorted/<experiment_id>
```

Manifest fields (the set the brief lists, in `write_tensorrt_engine_manifest.py`'s
vocabulary): source ONNX path and SHA-256; canonical manifest and input
contract SHA-256s; engine path, size, SHA-256; bindings as reported by the
engine (name, dtype, shape) for input and output; precision and the exact
`trtexec` command line; TensorRT 10.0.1.6, CUDA 12.2, driver 535.183.06,
GPU UUID and compute capability of the build device; Orange repo, branch,
commit and dirty state; preprocessing contract digest (`394dbaf8…`) and
whether Orange reproduced it; keypoint schema, order and skeleton id;
benchmark result and parity status; lifecycle `candidate`;
`selector_activation: false`; Palette target `runtime orange, hardware
class A16`, distinct from the portable ONNX registration.

## 7. Proposed runtime interface and ownership

Keep the detector contract (`YOLOv8`) and the pose contract
(`TensorRtPoseBackend`) as separate backends, which they already are.
Ownership: the crop producer owns crop geometry and the two crop sizes;
the pose backend owns the engine contract, preprocessing to the engine
input, and decoding to crop-local keypoints; the pose worker owns the
mapping to sensor coordinates and publication. The design document
(`docs/analytics_pipeline_configuration_design_2026_09_12.md`) generalises
this into stages with declared contracts; nothing in this model requires
that refactor first.

## 8. Validation plan, kept as separate claims

| # | Claim | How | Existing support | Missing |
|---|---|---|---|---|
| 1 | Engine builds and reloads on A16 | `trtexec` build; `trtexec --loadEngine` on GPU 5 and on a detect die (1) | build wrapper; `docs/a16_tensorrt_detect_engine_rebuild.md` | none |
| 2 | Bindings match the declared interface | the backend's load log line (`[PoseWorker] TensorRT pose backend loaded … input=images:1x3x192x192 output=output0:1x14x756 keypoints=3`) and the manifest's binding dump | `bind_metadata` checks | none |
| 3 | Orange preprocessing reproduces the probe digest | generate the probe pattern, run `launch_optimized_yolo_preprocess` 192→192, hash the tensor | the kernel | a small harness (`tools/pose_preprocess_probe.cpp`); needs the exact probe pattern definition from Palette |
| 4 | TensorRT output and decoded pose match the reference on fixed crops | dump `output0` and decoded keypoints for N fixed crops; compare with reference outputs produced by the training runtime | none | reference outputs from the training machine; a dump mode on the backend; **a numerical tolerance, which Orange does not have and must be decided** |
| 5 | Acquisition canary with detector-centred 192 crops maps keypoints to full-frame coordinates | `threecam_device_roi_realfish_engine_only` with the new engine and `crop_pipeline.crop_size_px = 192`, real detections; check event-log keypoints plus crop origin against overlays | specs, event log, verifier | overlay renderer for review (claim 7) |
| 6 | Timing, queueing, drops, gaps acceptable | the same runs through `scripts/run_detect_latency_spec.sh`; gates `infer_ms` unchanged, pose queue high-water ≤ 2, zero drops, `camera_frame_id_gaps = 0` | runner, perf CSVs, verifier | none |
| 7 | Representative overlays reviewed before activation | render keypoints on saved crops or frames | none in headless | a small script over the event log and a frame dump |

Selector activation stays off through all seven.

## 9. Risks and blockers

- Gap 1 is the only functional blocker for the production configuration
  with crop video on; the first canary can avoid it with a single 192 crop.
- Parity tolerance is a decision, not a measurement, and it is unowned.
- No reference runtime on this host for claim 4.
- The real-detection runs depend on the rig having a fish and cameras with
  carrier; every run since 2026-09-04 has had zero detections.
- 192 sits at the top of the crop-size range the design anticipated (96 to
  192). Fine for the model; the pose graph will be closer to today's 0.72 ms
  than to the 0.5 ms hoped for at 128.

## 10. Recommended sequence of small changes

1. Build the engine (section 6) and run claims 1 and 2. No Orange change.
2. Probe harness for claim 3. New tool, no runtime change.
3. Spec: a copy of the pose engine-only spec with the new engine path,
   `input_width/height 192`, `crop_pipeline.crop_size_px 192`,
   `roi_source yolo_top_detection`, `analytics_device_roi true`. No code.
4. Labels from the skeleton file; `pose_schema:traditional_v1` recorded;
   thresholds as spec keys with today's defaults. Small runtime change.
5. Wire `pose_worker.crop_size_px` into the crop producer (second crop or
   pool-copy crop into the pose input), then the crop-video configuration
   can run with pose at 192. This is also the first piece of the graph
   executor's `Warp` stage.
6. Claims 4 to 7 with the fish, then decide activation.

## 11. Branch and prerequisites

Fresh worktree from `agent/analytics/device-roi-20260912` (which contains
the shaman-v2 head `5cf21a9` plus the device-ROI and design commits), not
from `origin/main`. Fetch first only to confirm the shaman-v2 remote head
has not moved past what the shared worktree holds; the shared worktree is
at `ca817a0` and dirty, so coordinate before rebasing onto it.

## 12. Confirmation

During this inspection no code, data, engine, registry entry, selector or
model file was created, modified or activated. Actions taken: SHA-256 of
the three bundle files, reading the two JSON files, string inspection of
the ONNX, `nvidia-smi`, `nvcc --version`, `trtexec --version`, `ldd` on the
existing client binary, and `git` status queries.
