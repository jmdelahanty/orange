# Crop and Encode Reactivation TODO

Date: 2026-02-25
Scope: make `CropAndEncodeWorker` practically usable again in `orange-jeremy` with a safe enablement path and production-ready lifecycle behavior.

## Current State Snapshot

- Crop/encode worker code path is still wired into stream startup/shutdown:
  - creation/link/start: `src/orange.cpp:894`, `src/orange.cpp:906`, `src/orange.cpp:929`
  - stop/final flush: `src/orange.cpp:1063`, `src/orange.cpp:1012`
- YOLO dispatch into crop worker still exists, gated by active recording:
  - `src/yolo_worker.cpp:810`
- The runtime flag exists in camera selection state:
  - `src/video_capture.h:205`
- Camera control table does not expose a crop/encode toggle column, so the flag is not user-reachable in the current UI:
  - table/header/rows: `src/orange.cpp:377`, `src/orange.cpp:422`, `src/orange.cpp:464`
- Existing TODOs cover related risks but not full reactivation:
  - `docs/threading_review_todo.md`
  - `docs/recording_segment_rollover_todo.md`

## Gap Summary

`CropAndEncodeWorker` is present but effectively dormant from normal operator flow because there is no clear control path to enable `crop_and_encode` per camera.

## Audit Update (2026-03-16)

- Re-checked current runtime wiring:
  - worker creation/start/flush still exists,
  - worker creation is already gated by `crop_and_encode=true`,
  - crop file creation is therefore already gated once that flag is set.
- The practical blocker is still operator reachability:
  - the camera control table has no crop toggle,
  - the flag is not persisted or surfaced clearly.
- Crop dispatch is still gated by `camera_control_->record_video`, so this path is not yet usable for non-recording crop/pose consumers.
- No explicit crop observability, dynamic runtime toggle behavior, or lifecycle hardening has been added since this TODO was written.

## Implementation Plan Update (2026-04-22)

Recent GUI validation showed that one-camera `100 fps` full-frame recording with
YOLO can run cleanly with aligned video, metadata, YOLO event JSONL, and YOLO
perf CSV outputs. That makes lossless crop recording a practical next target,
but `CropAndEncodeWorker` should still be treated as dormant code until it is
hardened.

Architecture decision:

- Keep crop recording downstream of YOLO, not as a new direct acquisition
  subscriber.
- Full-frame recording remains the primary acquisition artifact.
- High-resolution crop generation becomes a reusable GPU stage derived from the
  YOLO result stream.
- Crop recording becomes one optional consumer of that crop stage.
- Pose TensorRT becomes another optional consumer of that crop stage and must
  receive the GPU crop before encode, not by decoding the crop video.
- Crop/pose overload must never break full-frame acquisition or full-frame
  recording.

Primary design constraint:

- Do not bake crop generation directly into the lossless encoder path.
- The next implementation should separate:
  - crop ROI selection and GPU crop production,
  - crop preview,
  - crop lossless encode,
  - future pose TensorRT inference.
- The first crop payload should preserve enough identity and geometry for all
  consumers: frame ids, timestamps, source crop rectangle, source detection
  rectangle, confidence, source dimensions, crop dimensions, GPU buffer pointer,
  and CUDA readiness event.
- Pose should consume a high-resolution crop tensor from GPU memory before any
  NVENC/NV12 conversion.

Headless decision:

- Do not make crop encode part of the first headless validation slice.
- Define the config and artifact contract so headless can use the same feature
  later.
- Validate through the GUI first because the current worker is GUI-wired, uses
  crop preview textures, and receives frames only through `YoloWorker`.
- Add headless support after lifecycle, metadata, and performance behavior are
  proven with one-camera GUI YOLO + full-frame record + crop record.

Rationale for GUI-first:

- The current headless runner intentionally disables `crop_and_encode`.
- Headless does not yet run the real GUI YOLO worker path that feeds
  `CropAndEncodeWorker`.
- Adding headless crop immediately would couple two unsettled changes:
  reactivating crop encode and introducing real headless YOLO/crop orchestration.
- A GUI-first slice gives a narrower failure surface and still exercises the
  real camera, real YOLO engine, real source-frame lifetime, and real NVENC path.

### Proposed Runtime Contract

Configuration should be app/session-level, not camera hardware config:

```json
{
  "crop_pipeline": {
    "enabled": true,
    "requires_yolo": true,
    "width": 512,
    "height": 512,
    "source": "best_yolo_detection",
    "output_layout": "mono8",
    "consumers": {
      "preview": true,
      "recording": true,
      "pose": false
    }
  },
  "crop_recording": {
    "enabled": true,
    "codec": "hevc",
    "tuning": "lossless",
    "no_detection_policy": "encode_blank",
    "write_metadata_for_every_frame": true
  },
  "pose_on_crop": {
    "enabled": false,
    "engine_path": "",
    "input_layout": "model_specific",
    "drop_when_busy": true
  }
}
```

Notes:

- The current code uses HEVC lossless, despite older notes referring to H.264
  lossless. H.264 can be added as a codec option, but the first slice should
  either keep HEVC lossless or explicitly change both encoder and writer
  together.
- The GUI now exposes a square crop size for the transitional crop encoder
  path. It defaults to `256x256`, is clamped to an even `32..2048` px value, is
  persisted as `crop_pipeline.crop_size_px` in camera JSON, and is fixed for the
  duration of a streaming session because GL textures and NVENC dimensions are
  allocated at stream start. For pose, the crop size may need to be larger than
  the encoded preview/crop artifact. If pose needs a `512x512` or
  model-specific tensor, generate that first and derive the encoded artifact
  from it, not the other way around.
- `write_metadata_for_every_frame=true` is important. If the crop video contains
  blank frames when YOLO finds no fish, the metadata should still have one row
  per encoded crop frame with `has_detection=false`.
- The artifact contract should distinguish the detection box from the actual
  crop rectangle. The current writer labels detection bbox fields as `crop_*`,
  which is misleading.

### Phase A: Split Crop Production From Encoding

- [ ] Introduce a crop payload type, for example `CropFrame`, that contains
      frame identity, timestamps, source geometry, crop geometry, detection
      geometry, confidence, GPU crop buffer, and CUDA ready event.
- [ ] Add a bounded crop buffer/event pool. Do not use a single reusable crop
      scratch buffer for asynchronous pose/encode consumers.
- [ ] Create one crop producer path that writes the high-resolution crop once
      per selected YOLO/tracker ROI.
- [ ] Allow independent bounded consumers:
      preview, lossless crop encoder, and future pose TensorRT worker.
- [ ] Make consumer overload best-effort: drop crop/pose work when queues or
      pools are full rather than blocking acquisition, full-frame recording, or
      YOLO indefinitely.
- [ ] Keep crop generation on GPU and pass readiness with CUDA events, not CPU
      synchronization.

### Phase B: Safety Patch Before Enabling

- [ ] Make `flush_and_close()` idempotent and safe when no writer was opened.
- [ ] Guard every `writer_.video->push_packet(...)` call.
- [ ] Add explicit crop producer/encode stream completion before releasing any
      source `WORKER_ENTRY` reference.
- [ ] Reject crop mode when frame dimensions are smaller than the configured
      crop size.
- [ ] Require YOLO for crop mode, or block stream start with clear GUI error
      text when `crop_and_encode=true` and `yolo=false`.
- [ ] Keep the current Mono8-only crop encode path explicit in logs/snapshots.
      Do not silently imply color/Bayer support.

### Phase C: Metadata and Artifact Contract

- [ ] Update `Cam<serial>_crop_meta.csv` to one row per crop video frame.
- [ ] Include `recording_frame_id`, `local_frame_id`, `camera_frame_id`,
      `timestamp`, `timestamp_sys`, and `has_detection`.
- [ ] Include actual crop rectangle fields:
      `crop_x`, `crop_y`, `crop_w`, `crop_h`.
- [ ] Include source detection fields separately:
      `detection_x`, `detection_y`, `detection_w`, `detection_h`,
      `detection_confidence`.
- [ ] Include `blank_frame` or `no_detection_policy` so consumers can interpret
      no-detection frames without guessing.
- [ ] Update `recording_snapshot.json` with crop output metadata under a
      multi-output encoder shape, or a documented transitional crop section.
- [ ] Reserve pose-on-crop metadata fields for the later pose worker:
      pose engine id/path, crop tensor geometry, input layout, keypoint schema,
      and full-frame coordinate transform.

### Phase D: Observability

- [ ] Add crop counters:
      frames enqueued, frames encoded, blank frames encoded, frames dropped,
      queue depth high-water mark, encode failures.
- [ ] Add crop timing:
      YOLO-to-crop enqueue delay, crop wait, crop copy/prepare, NVENC encode,
      writer push time, total crop worker time.
- [ ] Add pose-ready crop counters even before pose is implemented:
      crop frames produced, crop pool unavailable, consumer drops by consumer.
- [ ] Emit a `Cam<serial>_crop_perf.csv` during recording.
- [ ] Surface crop active/error state in the GUI near the recording/YOLO state.

### Phase E: GUI Enablement and Validation

- [ ] Add a visible per-camera `Crop` or `Crop+Encode` control.
- [ ] Start with a conservative operator policy:
      crop can be enabled before stream start, not toggled dynamically during
      active streaming.
- [ ] Run one-camera GUI smoke:
      stream + YOLO + full-frame record + crop record.
- [ ] Validate:
      main video frame count, main metadata rows, YOLO JSONL rows, crop video
      frame count, crop metadata rows, no frame-id gaps, no acquisition
      starvation, no unexpected `active_recorders` drain timeout.
- [ ] Only after one-camera success, try dual-camera GUI with crop enabled on
      one camera.

### Phase F: Pose TensorRT Integration After Crop Producer Is Stable

- [ ] Implement `PoseWorker` as a consumer of `CropFrame`, not as a consumer of
      encoded crop video.
- [ ] Add model-specific crop tensor conversion after high-resolution crop
      production and before TensorRT enqueue.
- [ ] Return pose crop buffers to the pool after the pose worker records or
      observes completion.
- [ ] Publish pose results with both crop-local coordinates and transformed
      full-frame coordinates.
- [ ] Keep pose best-effort initially: if pose is slower than crop production,
      drop pose inputs and count drops instead of blocking full-frame recording.
- [ ] Decide whether pose outputs are live latest-state IPC, Orange-owned JSONL
      audit events, or both.

### Phase G: Headless Support Later

- [ ] Add experiment-spec fields only after GUI crop behavior is stable.
- [ ] Reuse the same app/session crop config schema.
- [ ] Decide whether headless crop uses real YOLO, synthetic detections, or both:
      synthetic detections are useful for CI/schema tests; real YOLO is needed
      for camera performance validation.
- [ ] Extend analyzer output with crop artifact presence, row counts, frame
      counts, and crop/frame-id consistency checks.
- [ ] Add a dedicated headless validation mode:
      full-frame record + YOLO event log + crop record.

### Defer Until After Reactivation

- Dynamic YOLO-derived QP maps should remain separate from crop reactivation.
- Pose-stage inference implementation should wait until the crop producer
  contract and buffer pool are stable, but crop reactivation must be designed to
  feed pose before encoding.
- Runtime crop toggle while streaming should be deferred until the fixed-start
  mode is reliable.
- Shared encoder-core refactoring is worthwhile, but should not block the first
  safe crop validation.

### Implementation Status (2026-04-22)

Transitional GUI crop reactivation has started, but the full `CropProducer`
split is not implemented yet.

Completed in the first code slice:

- GUI camera table exposes a per-camera `Crop` checkbox and `All` crop toggle.
- GUI recording preflight blocks crop when full-frame `Record` or `YOLO` is not
  enabled for the same camera.
- GUI recording preflight blocks crop when the source frame is smaller than the
  current crop dimensions.
- Crop preview/texture dimensions now use the GUI/session crop size instead of
  repeated literals.
- Crop size now loads from and saves to camera config as
  `crop_pipeline.crop_size_px`; the GUI uses a single session value and applies
  it to open camera configs before save.
- `flush_and_close()` is safer when no crop writer is open.
- Encoded packet writes are guarded behind an open crop writer.
- Crop metadata now writes one row per encoded crop frame, including
  no-detection blank frames.
- Crop metadata now separates actual crop rectangle from source detection
  rectangle.
- Crop worker snapshots frame/detection metadata before downstream crop work, so
  crop metadata/perf rows no longer need to hold the original `WORKER_ENTRY`.
- Crop worker records a CUDA source-safe event and defers source
  `WORKER_ENTRY` release behind that event. The original camera frame is no
  longer intentionally held until the full crop encode/preview tail completes.
- `camera_config_validation_tests` covers `crop_pipeline.crop_size_px` parse,
  save-shape, defaulting, clamp behavior, odd-value sanitization, and legacy
  `size_px` / square `width,height` aliases.

GUI validation result:

- Artifact folder:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_21_47_28`
- Camera `2010096`, `100 fps`, `4512x4512` full-frame recording with YOLO and
  crop enabled.
- Saved camera config:
  `/home/jeremy/orange_data/config/local/100_cam4/2010096.json` contained
  `crop_pipeline.crop_size_px = 328`.
- `recording_snapshot.json` captured `crop_pipeline.crop_size_px = 328` in the
  saved camera/runtime config snapshot.
- `Cam2010096.mp4`: `4512x4512`, `100 fps`, `475` frames, `4.75 s`.
- `Cam2010096_crop.mp4`: `328x328`, `100 fps`, `475` frames, `4.75 s`.
- `Cam2010096_meta.csv`: `475` data rows.
- `Cam2010096_crop_meta.csv`: `475` data rows with `crop_w,crop_h = 328,328`.
- `Cam2010096_yolo_events.jsonl`: `475` rows.
- `Cam2010096_yolo_perf.csv`: `475` data rows.
- Operator-visible video check was good, and stop/finalize completed without
  the earlier crop shutdown segfault.
- New code path now emits `Cam<serial>_crop_perf.csv` for crop-recorded frames
  and writes `recording_snapshot.json` `crop_outputs[serial]` metadata for GUI
  crop outputs.

Crop observability validation result:

- Artifact folder:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_22_53_43`
- Camera `2010096`, `100 fps`, `4512x4512` full-frame recording with YOLO and
  crop enabled.
- `recording_snapshot.json` contained `crop_outputs[2010096]` with
  `enabled=true`, `mode=yolo_centered_square`, `crop_size_px=256`, and file
  names matching the emitted crop artifacts.
- `Cam2010096.mp4`: `366` frames, matching `Cam2010096_meta.csv`.
- `Cam2010096_crop.mp4`: `256x256`, `366` frames, matching
  `Cam2010096_crop_meta.csv`.
- `Cam2010096_crop_keyframe.json` existed and parsed as JSON.
- `Cam2010096_crop_perf.csv`: `366` rows, matching crop metadata frame IDs,
  `0` dropped crop frames, max queue depth `1`.
- Crop worker `total_ms`: mean `4.54 ms`, p95 `10.70 ms`, p99 `14.27 ms`,
  max `17.85 ms`.
- `Cam2010096_yolo_events.jsonl`: `366` rows, matching crop metadata frame IDs.
- `scripts/validate_recording_artifacts.py` passed on the folder.

Source-release split validation result:

- Artifact folder:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_23_20_41`
- Camera `2010096`, `100 fps`, `4512x4512` full-frame recording with YOLO and
  crop enabled.
- `scripts/validate_recording_artifacts.py` passed after updating the validator
  to accept documented blank crop frames (`blank_frame=1`, `has_detection=0`,
  `crop_w,crop_h=0,0`).
- `Cam2010096.mp4`: `748` frames, matching `Cam2010096_meta.csv`.
- `Cam2010096_crop.mp4`: `256x256`, `748` frames, matching
  `Cam2010096_crop_meta.csv`.
- `Cam2010096_yolo_events.jsonl`: `748` rows, with `713` detection frames and
  `35` zero-detection frames.
- `Cam2010096_crop_perf.csv`: `748` rows, `0` dropped crop frames, max queue
  depth `1`.
- Crop worker `stream_sync_ms` was nonzero on `0/748` rows, versus `366/366`
  rows in the previous transitional run. This indicates the CUDA source-release
  event path is active and the fallback source-stream synchronization is not
  being used.
- Crop worker `total_ms`: mean `4.55 ms`, p95 `13.40 ms`, p99 `18.50 ms`,
  max `30.85 ms`. The source-frame release path improved, but total crop worker
  tail latency remains because preview and crop video encoding still run inside
  the same combined worker.

### Design Update: Split Crop Production Before Pose

The `2026_04_22_22_53_43` crop perf data is healthy enough for a short GUI
smoke (`0` crop drops, max crop queue depth `1`), but the per-frame crop worker
`total_ms` had p95 around `10.70 ms` and p99 around `14.27 ms` at `100 fps`.
Because `100 fps` has a `10 ms` frame period, those tail latencies are a real
headroom warning even though they did not cause backlog in the short run.

The important nuance is that current `CropAndEncodeWorker::WorkerFunction`
measures more than the crop operation. It serializes:

```text
YOLO result
  -> source-frame event wait submission
  -> crop selection
  -> crop preview copy / possible cross-GPU host staging
  -> crop video encode submission
  -> metadata/perf row writes
  -> stream/display synchronization
  -> source WORKER_ENTRY release
```

So a high `total_ms` does not necessarily mean the crop kernel is slow. It means
the source frame lease can be held until preview/encode/sync work finishes. That
is the wrong long-term ownership model for pose, because pose would add another
consumer to the same serialized path.

Current first split slice:

- `CropAndEncodeWorker` copies frame identity, timestamps, crop geometry, and
  detection geometry into a local snapshot before any source-frame release.
- It owns a small CUDA source-release event pool and drains pending releases
  with `cudaEventQuery`, synchronizing only during teardown or rare event-pool
  exhaustion.
- Detection frames release the source `WORKER_ENTRY` after source-dependent
  crop/encode input work is queued and the source-safe CUDA event is recorded.
- No-detection crop frames release the source immediately because blank preview
  and blank crop video encoding do not need the source image.
- Preview and crop video encoding still run inside the same worker, so this is
  not yet the full `CropFrame` producer/consumer architecture.

Target ownership model:

```text
YOLO result
  -> CropProducer quickly copies source ROI into a bounded CropFrame pool
  -> records crop_ready_event
  -> releases source WORKER_ENTRY as soon as the crop copy is safely detached

CropFrame consumers:
  -> crop preview
  -> lossless crop encoder
  -> future pose TensorRT worker
```

This makes crop generation the short source-frame lifetime boundary. Encode,
preview, and pose become downstream consumers. If those consumers fall behind,
they should drop their own crop outputs or skip preview/pose work rather than
holding original camera frames or backpressuring full-frame recording.

The next implementation should optimize for this separation before tuning the
current combined crop worker. The goal is not to make the existing
`CropAndEncodeWorker` faster in isolation; the goal is to make the original
camera frame lifetime independent from crop-video encoding and preview.

Still pending before pose:

- Extract a true `CropProducer` with a bounded GPU crop buffer/event pool.
- Make pose a `CropFrame` consumer instead of extending crop-video encoding.
- Replace transitional per-frame stream sync with CUDA readiness events for
  downstream consumers.
- Add aggregate crop counters to the runtime/pipeline status once the crop
  producer split exists.

Implemented artifact validation tool:

- `scripts/validate_recording_artifacts.py`
- Current checks:
  - main video frame count equals `Cam<serial>_meta.csv` data rows,
  - optional crop video dimensions equal snapshot `crop_pipeline.crop_size_px`,
  - crop output snapshot metadata exists and matches artifact names/geometry,
  - crop keyframe sidecar exists as `Cam<serial>_crop_keyframe.json`,
  - crop video frame count equals `Cam<serial>_crop_meta.csv` data rows,
  - crop metadata `crop_w,crop_h` equal configured crop size,
  - crop perf rows match crop metadata rows and report no dropped crop frames,
  - crop and YOLO `recording_frame_id` sequences are positive, monotonic, and
    equal for current full-rate GUI YOLO mode.
- Use `--allow-yolo-decimation` for future runs where YOLO/crop cadence is
  intentionally less than recorded frame rate.

Next artifact validation checklist:

- Verify full-frame video and full-frame metadata still match, so crop
  validation cannot hide a regression in the primary recording artifact.
- Add a machine-readable output summary to downstream experiment analysis if
  we want crop validation to feed `runs.json` / `runs.csv`.

## TODO Plan

## Immediate Implementation Checklist: Crop Producer Split

Current implementation note (2026-04-23):

- The first crop-producer split is implemented inside
  `CropAndEncodeWorker`, not as a standalone `CropProducer` class yet.
- Detected frames now copy the source ROI once into a bounded crop-owned Mono8
  GPU buffer on a dedicated producer CUDA stream, record a `crop_ready_event`,
  then release the source `WORKER_ENTRY` from a CUDA source-safe event after
  that ROI copy completes.
- Crop preview and crop-video encoding now read the crop-owned buffer instead
  of rereading the original GPUDirect camera frame.
- `Cam<serial>_crop_perf.csv` now separates `crop_pool_wait_ms`,
  `crop_producer_cpu_ms`, crop-producer enqueue substeps,
  `crop_copy_gpu_ms`, `crop_preview_cpu_ms`, and `encode_submit_cpu_ms`.
- Crop-copy GPU timing is enabled by default and can be disabled with
  `ORANGE_CROP_COPY_TIMING=0` to test whether the timing events themselves are
  perturbing the producer hot path.
- Crop ROI copy can also be switched from `cudaMemcpy2DAsync` to a dedicated
  CUDA kernel with `ORANGE_CROP_COPY_KERNEL=1` so we can compare memcpy
  submission stalls against kernel launch overhead on the same workload.
- The GPUDirect source can also be staged first with
  `ORANGE_CROP_STAGE_SOURCE=1`, which copies the full source frame into
  ordinary device memory before crop extraction. That isolates GPUDirect-source
  access costs from the ROI crop path.
- Remaining architectural work is to make preview, crop recording, and pose
  independent `CropFrame` consumers with explicit leases/ref-counting.

Validation update (2026-04-23):

- One-camera direct-source crop runs showed the pathological tail on the ROI
  path itself. Representative detected-frame result:
  - direct source (`2026_04_23_12_01_18`):
    - `crop_producer_cpu_ms p95=6.6866 ms`
    - `crop_roi_copy_enqueue_cpu_ms p95=6.6594 ms`
- Replacing ROI `cudaMemcpy2DAsync` with a custom CUDA kernel did not remove
  that tail. The stall followed "touch GPUDirect source memory", not the ROI
  API.
- One-camera staged-source runs moved the cost out of ROI extraction and into a
  single explicit full-frame detach copy:
  - staged source (`2026_04_23_12_29_32`):
    - `crop_producer_cpu_ms p95=0.0098 ms`
    - `crop_roi_copy_enqueue_cpu_ms p95=0.0070 ms`
    - `source_stage_enqueue_cpu_ms p95=6.3012 ms`
- Two-camera staged-source validation also held at `100 fps` with real
  detections on both cameras and zero camera/crop drops:
  - run: `2026_04_23_12_55_27`
  - `Cam2010095` detected frames:
    - `crop_producer_cpu_ms p95=0.0379 ms`
    - `crop_roi_copy_enqueue_cpu_ms p95=0.0224 ms`
    - `source_stage_enqueue_cpu_ms p95=7.6983 ms`
  - `Cam2010096` detected frames:
    - `crop_producer_cpu_ms p95=0.0226 ms`
    - `crop_roi_copy_enqueue_cpu_ms p95=0.0136 ms`
    - `source_stage_enqueue_cpu_ms p95=6.1465 ms`
- Current conclusion:
  - The GPUDirect camera frame is a good ingress/lease buffer but a bad
    general-purpose workspace buffer for crop/pose fanout.
  - The defensible ownership model is:
    - GPUDirect source for ingress and current stable full-frame recording path.
    - One explicit detach into ordinary device memory for crop/pose-style
      downstream consumers.
  - The measured cost did not disappear; it moved into the explicit detach.
    That is still preferable because it is predictable, bounded, and no longer
    couples ROI consumers directly to GPUDirect-backed memory behavior.

Immediate next-step recommendation (2026-04-23):

- Treat the staged full-frame device buffer as the canonical source for future
  crop/pose consumers.
- Do not move the already-stable full-frame recording/preprocess path off the
  GPUDirect source unless new measurements show the same source-touch
  pathology there.
- Finish the producer/consumer split so preview, crop encode, and future pose
  become independent best-effort consumers of detached frame/crop payloads.
- Add aggregate counters so runtime status can report:
  - staged-source frames,
  - crop frames produced,
  - per-consumer accepted/dropped counts,
  - and detach/crop producer tail summaries.
- After the split is complete, validate the first pose-on-crop path against the
  same two-camera `100 fps` workload.

Step 1: Define crop payload and pool.

- [x] Add an internal `CropFrame` payload type with frame identity,
      timestamps, source-frame dimensions, crop rectangle, detection
      rectangle, confidence, crop dimensions, GPU crop pointer,
      `crop_ready_event`, and `recycle_event`.
- [x] Add a bounded `CropFrame` pool with device buffers and CUDA events,
      sized conservatively at first (`8` entries).
- [ ] Add complete counters for pool unavailable, frames produced, and crop
      production failures. Current code tracks pool misses; produced/failure
      counters still need explicit telemetry.

Step 2: Extract crop production from crop encode.

- [ ] Move ROI selection and source-to-crop GPU copy into a standalone
      `CropProducer` stage/class. Current code has the producer sub-path inside
      `CropAndEncodeWorker`.
- [x] Make the producer sub-path record a readiness event after the crop copy.
- [x] Run source-frame wait, source-to-crop copy, crop-ready event, and
      source-release event on a dedicated producer CUDA stream so preview and
      crop encode work cannot extend the source-frame lease.
- [x] Release the source `WORKER_ENTRY` after the crop copy is safely detached,
      not after crop preview/encode finishes.
- [x] Keep crop production best-effort: if no crop buffer/event is available,
      drop the crop for that frame and count it instead of blocking YOLO,
      acquisition, or full-frame recording.

Step 3: Convert crop video into a consumer.

- [x] Change crop video encoding to consume the internal `CropFrame` payload.
- [x] Preserve current crop artifact behavior:
      `Cam<serial>_crop.mp4`, `Cam<serial>_crop_meta.csv`,
      `Cam<serial>_crop_keyframe.json`, and `Cam<serial>_crop_perf.csv`.
- [x] Keep one crop metadata/perf row per encoded crop frame.
- [x] Return or release the crop consumer lease after the encoder no longer
      needs the crop buffer.

Step 4: Convert crop preview into a consumer.

- [x] Move preview copy/sync out of the source-frame lifetime. It still runs
      inside the same worker and should become an independent consumer later.
- [ ] Make preview droppable/rate-limited so GUI display cannot hold crop
      buffers or source frames.
- [x] Keep the preview cross-GPU host-staging path observable because it is a
      likely source of timing spikes.

Step 5: Add lease/ref-count semantics for crop payloads.

- [ ] Track which consumers accepted each `CropFrame`.
- [ ] Return a crop buffer to the pool only after all accepted consumers have
      released it.
- [ ] Treat consumer queue-full states as non-acceptance, not as blocking waits.
- [ ] Add debug counters for producer drops and per-consumer drops.

Step 6: Validate before adding pose.

- [ ] Re-run one-camera GUI `100 fps` YOLO + full-frame record + crop record.
- [ ] Validate artifact alignment with `scripts/validate_recording_artifacts.py`.
- [ ] Compare crop producer timing separately from crop encoder/preview timing.
- [ ] Use the crop-producer substep columns to determine whether remaining
      tails are CPU enqueue stalls, crop-copy GPU execution, or source-release
      event recording.
- [ ] Compare one run with default crop-copy timing against one run with
      `ORANGE_CROP_COPY_TIMING=0`; if the producer tails disappear when timing
      is disabled, the measurement path is perturbing the crop producer.
- [ ] Compare one run with default `cudaMemcpy2DAsync` crop copy against one
      run with `ORANGE_CROP_COPY_KERNEL=1`; if `crop_roi_copy_enqueue_cpu_ms`
      drops materially, the memcpy submission path is the bottleneck.
- [ ] Compare one run with direct GPUDirect source access against one run with
      `ORANGE_CROP_STAGE_SOURCE=1`; if `crop_roi_copy_enqueue_cpu_ms` drops but
      `source_stage_enqueue_cpu_ms` rises, the issue is the GPUDirect-backed
      source path rather than the crop extraction API.
- [ ] Confirm source-frame release timing no longer includes crop encode or
      preview synchronization.
- [ ] Confirm full-frame recording still has no frame gaps, receive errors,
      encode failures, or acquisition starvation.

Step 7: Add pose only after the producer split is stable.

- [ ] Implement pose as a `CropFrame` consumer, not as part of crop-video
      encoding.
- [ ] Keep pose best-effort initially, with drop counters and no full-frame
      backpressure.
- [ ] Preserve both crop-local and full-frame coordinate mappings in pose
      outputs.

## Phase 1: Re-expose Enablement Path

- [ ] Add a per-camera `Crop+Encode` checkbox in the camera control table.
- [ ] Add an optional `All` toggle for batch enable/disable.
- [ ] Gate crop worker creation on explicit user enablement:
  - only construct/start `CropAndEncodeWorker` for cameras with `crop_and_encode=true`,
  - do not spawn idle crop workers for cameras where crop recording is off.
- [ ] Gate crop artifact writing on explicit user enablement:
  - only create `Cam<serial>_crop.*` outputs when crop mode is enabled,
  - disabling crop mode must prevent new crop files from being created.
- [ ] Enforce or auto-manage dependency on YOLO:
  - if crop+encode requires detections, either auto-enable YOLO or block with a clear warning.
- [ ] Decide and implement how this setting is persisted across runs (if desired):
  - runtime-only,
  - or stored in a dedicated local session file (not camera hardware config JSON).

## Phase 2: Lifecycle and Shutdown Hardening

- [ ] Make `flush_and_close()` idempotent and safe when writer/encoder were never fully initialized.
  - guard `writer_.video` before `push_packet`.
  - refs: `src/crop_and_encode_worker.cpp:129`, `src/crop_and_encode_worker.cpp:134`
- [ ] Keep producer/consumer shutdown ordering safe for YOLO -> crop queue handoff.
  - refs: `src/orange.cpp:1063`, `src/yolo_worker.cpp:810`, `docs/threading_review_todo.md`
- [ ] Add explicit queue-close/stop-token semantics (or equivalent) to avoid enqueue-after-stop spin waits.
- [ ] Define runtime toggle behavior while streaming:
  - enabling crop mode should spawn/start the crop worker on demand,
  - disabling crop mode should drain/finalize and tear down that crop worker safely.

## Phase 3: Crop Geometry and Data Safety

- [ ] Handle small frame sizes safely (`width < 256` or `height < 256`):
  - reject crop mode with clear error, or
  - dynamically choose a smaller crop size.
  - refs: `src/crop_and_encode_worker.cpp:273`, `src/crop_and_encode_worker.cpp:277`
- [ ] Verify copy/stride assumptions in crop encode path under all supported input modes.

## Phase 4: Observability

- [x] Add per-recording crop perf logging for:
  - crop frames enqueued,
  - crop frames encoded,
  - dropped/skipped crop frames (and reason),
  - crop queue depth / backpressure events.
- [x] Persist crop output metadata in `recording_snapshot.json`.
- [ ] Add aggregate crop counters to runtime/pipeline status.
- [ ] Surface crop+encode active state in UI/runtime status.

## Phase 5: Validation

- [x] GUI smoke: enable crop+encode, start streaming, start/stop recording,
      confirm output files are produced.
- [x] GUI smoke: stop streaming after crop recording and confirm clean finalize.
- [ ] Integration test: YOLO off + crop enabled policy behaves as specified (auto-enable or explicit block).
- [x] Automated artifact check: crop video dimensions match
      `crop_pipeline.crop_size_px`.
- [x] Automated artifact check: crop metadata row count, crop video frame
      count, and YOLO event count match for current full-rate GUI YOLO mode.
- [x] Automated artifact check: crop output snapshot and crop perf CSV align
      with crop artifacts.
- [ ] Stress test: long recording run with crop enabled; verify no deadlock and stable frame-id continuity.

## Phase 6: Pose TRT Readiness (Crop Consumer)

- [ ] Decouple crop dispatch from global `record_video` gating if pose should run during non-recording sessions.
  - today dispatch is gated by `camera_control_->record_video`.
  - refs: `src/yolo_worker.cpp:810`
- [ ] Keep pose as best-effort and non-blocking:
  - avoid blocking YOLO when downstream crop/pose queues are full,
  - add try-enqueue/drop-path + counters for pose/crop overload,
  - avoid unbounded spin-wait coupling in hot path.
  - refs: `src/threadworker.h:143`, `src/yolo_worker.cpp:814`
- [ ] Replace single crop scratch buffer with a small pool when feeding pose asynchronously.
  - current `d_cropped_rgba_` is single-buffer and can be overwritten by next frame.
  - refs: `src/crop_and_encode_worker.h:42`, `src/crop_and_encode_worker.cpp:37`
- [ ] Define explicit crop->pose event contract:
  - crop records readiness event on crop stream,
  - pose worker waits in its own stream,
  - no CPU sync in crop hot path for pose handoff.
- [ ] Revisit per-frame sync in crop preview path to preserve GPU headroom for pose.
  - current `cudaStreamSynchronize(m_display_stream)` can serialize work.
  - refs: `src/crop_and_encode_worker.cpp:346`
- [ ] Add CUDA Graph capture path for pose TensorRT inference (default-on, opt-out):
  - add flag (for example `ORANGE_POSE_CUDA_GRAPH=1` default),
  - perform warmup + graph capture once per pose worker/model instance,
  - execute via graph launch in steady state to reduce per-frame enqueue CPU overhead.
- [ ] Add safe fallback when graph capture is unavailable/fails:
  - fall back to normal TRT enqueue path without interrupting stream/recording,
  - emit one-time warning + counter for capture failure/fallback usage.
- [ ] Define recapture policy:
  - recapture when pose engine changes or input tensor shape/batch changes,
  - avoid per-frame recapture attempts.
- [ ] Add pose graph telemetry:
  - graph enabled/captured status,
  - pose enqueue/capture/launch timing,
  - capture failures and fallback count.
- [ ] Lock in frame identity policy for pose outputs:
  - include `frame_id`, `recording_frame_id`, timestamps, and camera id for downstream joins.

## Phase 7: Startup-Specialized Color/Mono Paths (Branchless Hot Path)

- [ ] Define a single per-camera input mode at setup/open time from config + pixel format:
  - examples: `Mono8`, `BayerRG8`, `BayerGB8`, `Unsupported`.
  - refs: `src/project.cpp:148`, `src/project.cpp:153`, `src/camera.cpp:799`
- [ ] Add consistency validation at startup:
  - fail fast or warn if `color` flag and `pixel_format` imply conflicting pipelines.
- [ ] Pre-bind camera-specific preprocess function objects once at worker construction:
  - YOLO preprocess strategy (mono vs bayer),
  - encoder preprocess strategy (mono vs color),
  - crop->pose preprocess strategy (canonical grayscale for pose model).
  - refs: `src/yolo_worker.cpp:643`, `src/encoder_preprocess_worker.cpp:270`
- [ ] Remove/avoid per-frame mode branching in worker hot paths by using the bound strategy function.
- [ ] For pose grayscale model, define one canonical inference input contract:
  - mono cameras: direct mono crop path,
  - color/bayer cameras: explicit deterministic conversion to grayscale before pose inference.
- [ ] Add mode visibility in logs/telemetry:
  - print selected mode + strategy per camera at stream start.
- [ ] Add tests for both modes:
  - startup mode selection,
  - runtime equivalence and expected output shape/range,
  - no regressions in FPS/latency under mixed camera modes.

## Phase 8: Split Crop and Encode, Reuse Encoder Core

- [ ] Split `CropAndEncodeWorker` responsibilities into:
  - crop producer path (crop preview + pose handoff),
  - crop encoder preprocess path (prepare resolved square-crop NV12 + metadata payload),
  - crop hardware encode path (NVENC + writer thread).
- [ ] Reuse `EncoderHwWorker` logic as shared encoder core (or extract common base) instead of maintaining two separate NVENC implementations.
  - current duplicate encode stack in crop worker:
    - `src/crop_and_encode_worker.cpp:43`
    - `src/crop_and_encode_worker.cpp:219`
    - `src/crop_and_encode_worker.cpp:303`
  - existing HW encode pipeline:
    - `src/encoder_hw_worker.cpp:157`
    - `src/encoder_hw_worker.cpp:435`
- [ ] Generalize `EncoderHwWorker` recycle contract so it is not hard-bound to `EncoderPreprocessWorker`.
  - today it returns buffers/events via `m_prep_worker_` typed as `EncoderPreprocessWorker*`.
  - refs: `src/encoder_hw_worker.h:14`, `src/encoder_hw_worker.cpp:440`
- [ ] Reuse `EncoderPreprocessWorker` concepts by extracting a shared NV12 preprocess core, then run separate instances for:
  - full-frame recording preprocess,
  - crop recording preprocess.
  - Keep queues independent so crop overload never backpressures full-frame recording.
- [ ] Make crop NV12 conversion explicit per input mode (mono vs color/bayer), not implicit raw ROI copy.
  - current crop encode path writes ROI bytes directly into NV12 Y and fills UV=128.
  - refs: `src/crop_and_encode_worker.cpp:296`, `src/crop_and_encode_worker.cpp:300`, `src/encoder_preprocess_worker.cpp:270`
- [ ] Define crop encoder config source separate from full-frame `CameraParams` dimensions:
  - square encode geometry resolved from app/camera/session configuration,
  - preserve camera id/serial/fps and recording frame id mapping.
- [ ] Extend recording snapshot encoder updates to support multi-output entries:
  - add output-scoped upsert API (for example `update_recording_snapshot_encoder_output(..., output_key, encoder_info)`),
  - write `encoders[camera].outputs.full` from full-frame HW encoder,
  - write `encoders[camera].outputs.crop` from crop encoder path.
- [ ] Keep legacy compatibility during migration:
  - continue writing legacy `encoders[camera] = <encoder_info>` as `full` until all consumers read `outputs`.
- [ ] Decide metadata ownership after split:
  - preserve crop-specific metadata fields (`detection_confidence`, crop bbox) currently written by crop worker,
  - integrate with shared writer path without losing fields.
  - refs: `src/crop_and_encode_worker.cpp:313`
- [ ] Keep stop/drain semantics unified across full-frame and crop encoders:
  - same lifecycle rules, idempotent finalize, same timeout/recovery behavior.

## Definition of Done

- [ ] Operator can intentionally enable crop+encode from the UI (or documented config path).
- [ ] Crop recordings are created reliably without lifecycle races/crashes during start/stop.
- [ ] Behavior is explicit when YOLO is disabled or frame size is below crop requirements.
- [ ] Basic integration + stress tests pass and are documented.
