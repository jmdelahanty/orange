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

## TODO Plan

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

- [ ] Add counters/logging for:
  - crop frames enqueued,
  - crop frames encoded,
  - dropped/skipped crop frames (and reason),
  - crop queue depth / backpressure events.
- [ ] Surface crop+encode active state in UI/runtime status.

## Phase 5: Validation

- [ ] Integration test: enable crop+encode, start streaming, start/stop recording, confirm output files are produced.
- [ ] Integration test: stop streaming while recording drain is active; confirm clean finalize.
- [ ] Integration test: YOLO off + crop enabled policy behaves as specified (auto-enable or explicit block).
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
  - crop encoder preprocess path (prepare 256x256 NV12 + metadata payload),
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
  - fixed 256x256 encode geometry,
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
