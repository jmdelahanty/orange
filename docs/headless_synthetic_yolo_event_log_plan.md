# Headless Synthetic YOLO Event Log Plan

Date: 2026-04-21
Status: plan only.

Purpose: make `Cam<serial>_yolo_events.jsonl` testable through headless
experiments without requiring TensorRT YOLO, GUI workers, or real detections.

## Goal

Add a deterministic headless mode that writes the same YOLO event JSONL artifact
as the GUI YOLO path, but with synthetic detection results generated from frame
metadata.

This is a contract and plumbing test. It is not a model-quality test.

## Why This Is Useful

The GUI YOLO path now emits `yolo_result` rows, but validating that path requires
a running GUI, a YOLO engine, and camera interaction. Headless synthetic YOLO
lets us verify:

- JSONL file creation in recording folders,
- schema stability,
- per-line JSON parsing,
- monotonic `event_sequence`,
- joins to `Cam<serial>_meta.csv` through `recording_frame_id`,
- zero-detection rows,
- detection rows,
- Citrus live IPC request-status fields,
- experiment result validation.

## Required Refactor

Move the event logger out of `src/yolo_worker.cpp` into a shared component:

- `src/yolo_event_log.h`
- `src/yolo_event_log.cpp`

The shared component should expose:

- `YoloEventLogger`
- `YoloResultRecord`
- helper conversion from detection objects to JSON

The GUI `YOLOv8Worker` and headless synthetic emitter should use the same
logger. That avoids creating two independent JSONL writers with subtly
different schemas.

## Experiment Spec Shape

Add an optional fixed-mode block:

```json
"fixed": {
  "yolo_event_log": {
    "mode": "synthetic",
    "every_n_frames": 10,
    "pattern": "alternating",
    "emit_zero_detections": true,
    "label": 0,
    "confidence": 0.9
  }
}
```

Initial fields:

- `mode`
  - `off`: default, no synthetic YOLO event log.
  - `synthetic`: emit deterministic synthetic `yolo_result` rows.
- `every_n_frames`
  - positive integer cadence for synthetic detection rows.
  - example: `10` means frames `10, 20, 30...` get one detection.
- `pattern`
  - initial accepted value: `alternating`.
  - future values can add multi-object or timeout patterns.
- `emit_zero_detections`
  - if true, emit `zero_detections` rows for frames that do not receive a
    synthetic detection.
  - if false, emit only synthetic detection rows.
- `label`
  - integer label for generated detections.
- `confidence`
  - floating-point confidence for generated detections.

## Synthetic Detection Rule

For each frame with a recording folder and a valid `recording_frame_id`:

- If `recording_frame_id % every_n_frames == 0`, emit `status=detections` with
  one box.
- Otherwise, emit `status=zero_detections` when `emit_zero_detections=true`.

Suggested deterministic box:

```json
{
  "x_px": 100.0 + (recording_frame_id % 50),
  "y_px": 200.0,
  "width_px": 40.0,
  "height_px": 30.0,
  "label": 0,
  "confidence": 0.9,
  "keypoints": []
}
```

All coordinates are source-frame pixels.

## Where To Emit

Headless currently does not create `YOLOv8Worker`. The synthetic emitter should
sit in the headless experiment/acquisition path, not in the GUI YOLO path.

Preferred implementation:

1. Add a lightweight synthetic writer owned by the headless run.
2. Feed it per-camera frame metadata when acquisition/recording assigns a
   `recording_frame_id`.
3. Use the same `YoloEventLogger` as the GUI path.

Potential integration points:

- acquisition loop, after `recording_frame_id` and `ipc_frame_id` are known,
- or a headless-only callback/hook passed into `acquire_frames(...)`.

The callback/hook approach is cleaner long term because it avoids embedding
headless experiment behavior directly in the generic acquisition loop.

## Citrus Live IPC Semantics

Synthetic headless YOLO should not change the current live IPC contract.

For the synthetic JSONL rows:

- If headless frame IPC is disabled:
  - `citrus_live_ipc.enabled = false`
  - `requested = false`
  - `request_status = "not_enabled"`
- If headless frame IPC is enabled:
  - detection rows may report `request_status = "queued"` only if the synthetic
    test also calls `FrameIPCManager::updateFrameWithDetections(...)`.
  - zero-detection rows should report
    `request_status = "not_requested_zero_detections"`.

The first implementation may keep synthetic YOLO event logging independent of
SHM updates. In that case, even detection rows should use a distinct status such
as `not_requested_synthetic` unless we add real synthetic IPC update emission.

## Validation

A headless run with synthetic YOLO enabled should validate:

- `Cam<serial>_yolo_events.jsonl` exists for each selected camera.
- Every line parses as JSON.
- Every line has `schema_id = "orange.yolo_event"` and `schema_version = 1`.
- `event_sequence` starts at `1` and increments by `1`.
- Required common fields are present.
- `recording_frame_id` values are positive.
- Detection rows occur at the configured cadence.
- Zero-detection rows occur on non-detection frames when enabled.
- Each detection row has `detection_count = 1` and one detection object.
- Each zero-detection row has `detection_count = 0` and an empty detection
  array.
- Rows join to `Cam<serial>_meta.csv` by `recording_frame_id`.

Add summary fields to `runs.csv` / `runs.json`:

- `yolo_event_log_present`
- `yolo_event_log_rows`
- `yolo_event_log_detection_rows`
- `yolo_event_log_zero_rows`
- `yolo_event_log_parse_errors`
- `yolo_event_log_status`

## Example Test Spec

Suggested checked-in spec:

```text
experiment_specs/2010096_synthetic_yolo_event_log_a16_gpu5.json
```

Target:

- single camera `2010096`
- normal short recording
- `fixed.yolo_event_log.mode = "synthetic"`
- `every_n_frames = 10`
- `emit_zero_detections = true`

Expected result for a 3 second 100 fps run:

- roughly 300 `yolo_result` rows,
- roughly 30 `detections` rows,
- roughly 270 `zero_detections` rows,
- no parse errors,
- `pass` status.

## Implementation Checklist

- [x] Extract shared logger from `src/yolo_worker.cpp`.
- [x] Add CMake entries for `src/yolo_event_log.cpp` to GUI and headless
      targets.
- [x] Keep GUI behavior unchanged after extraction.
- [ ] Parse `fixed.yolo_event_log` in `orange_headless_client.cpp`.
- [ ] Add deterministic synthetic generator.
- [ ] Wire generator to per-frame recording metadata.
- [ ] Add JSONL validation to headless result summarization.
- [ ] Add checked-in experiment spec.
- [ ] Build `orange` and `orange_client`.
- [ ] Run a short headless validation with the existing sudo wrapper.

## Non-Goals

- Do not run TensorRT YOLO in headless mode in this slice.
- Do not change Citrus live IPC semantics.
- Do not require Citrus to consume the JSONL file.
- Do not add final `citrus_live_ipc_decision` rows until
  `FrameIPCManager` exposes asynchronous publish/suppress outcomes.
