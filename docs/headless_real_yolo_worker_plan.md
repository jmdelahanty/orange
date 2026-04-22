# Headless Real YOLO Worker Plan

Date: 2026-04-22
Status: design and implementation checklist.

Purpose: define what it would take to run the real TensorRT `YOLOv8Worker`
from headless experiments, while preserving the current stable recording path
and the Citrus live-control IPC contract.

## Current State

As of this plan:

- GUI real YOLO can emit `Cam<serial>_yolo_events.jsonl`.
- Headless synthetic YOLO can emit and validate the same JSONL schema without
  TensorRT inference.
- Headless synthetic YOLO is audit-only: no real YOLO worker, no live Citrus
  detection update IPC.
- Headless frame IPC can verify serial-named base-frame queues such as
  `/shm_cam_2010096`.
- Local headless experiments intentionally reject `fixed.yolo=true`; the
  current runner only supports `yolo=false`.

Interpretation:

- We have good headless coverage for recording, base-frame IPC, and the YOLO
  audit-log contract.
- We do not yet have headless coverage for real model inference.

## Recommendation

Do not make real headless YOLO the default next implementation unless headless
inference is needed soon. It touches TensorRT lifecycle, CUDA resources,
acquisition fanout, worker shutdown, and live IPC policy.

If we choose to start it, use a narrow opt-in first slice:

- single camera,
- real TensorRT YOLO worker,
- JSONL audit output enabled,
- live Citrus detection IPC disabled,
- no crop/encode coupling,
- no display coupling,
- no multi-camera promises until the single-camera path is stable.

This keeps the change useful without mixing model inference with Citrus
live-control semantics too early.

## Proposed Experiment Spec Shape

Keep synthetic audit mode under `fixed.yolo_event_log`. Add a separate block for
real headless inference:

```json
"fixed": {
  "yolo_worker": {
    "mode": "real",
    "engine_path": "/path/to/model.engine",
    "decimate": 1,
    "publish_live_ipc": false,
    "timeout_ms": 500,
    "fail_on_init_error": true
  }
}
```

Initial fields:

- `mode`
  - `off`: default.
  - `real`: instantiate the TensorRT `YOLOv8Worker` in headless mode.
- `engine_path`
  - Required for `mode=real`.
  - Should map to `CameraEachSelect::yolo_model` before worker construction.
- `decimate`
  - Optional positive integer.
  - Can reuse the existing `ORANGE_YOLO_DECIMATE` behavior initially, but a
    config field is better for reproducible experiment specs.
- `publish_live_ipc`
  - Default `false`.
  - When false, real headless YOLO writes JSONL only.
  - When true, Orange may publish non-empty detections to Citrus IPC only under
    the stale-update policy.
- `timeout_ms`
  - Future worker timeout policy. The current GUI worker path has timeout-like
    telemetry, but this should become explicit for headless validation.
- `fail_on_init_error`
  - Default `true` for experiments. A failed model load should fail the run
    rather than silently behaving like YOLO-off.

## Architecture

Headless should use the same `YOLOv8Worker` class as GUI for the first real
slice. Avoid creating a second inference implementation unless the GUI worker is
too coupled to display/crop behavior.

Expected headless wiring:

1. Parse `fixed.yolo_worker`.
2. Resolve selected cameras.
3. Set `cameras_select[idx].yolo = true` only for selected headless YOLO
   cameras.
4. Set `cameras_select[idx].yolo_model = engine_path.c_str()` from stable
   storage that outlives worker construction.
5. Initialize `CameraResources` with YOLO support enabled so YOLO completion
   events and queues exist.
6. Construct `YOLOv8Worker` for each selected camera.
7. Pass the worker pointer into `acquire_frames(...)`.
8. Start acquisition and worker threads.
9. Drain/stop/join workers before freeing camera resources.
10. Summarize `Cam<serial>_yolo_events.jsonl` in `runs.json` / `runs.csv`.

Important lifecycle point:

- The engine path string must not be a temporary. The GUI currently assigns
  `cameras_select[i].yolo_model = yolo_model.c_str()`. Headless should keep
  per-camera engine paths in stable owned strings for the full worker lifetime.

## Live Citrus IPC Policy

The first real headless YOLO slice should default to audit-only:

- base-frame IPC can still run,
- `yolo_result` JSONL rows are emitted,
- `FrameIPCManager::updateFrameWithDetections(...)` is not called for YOLO
  results.

Reason:

- Citrus currently treats the shared-memory queue as latest usable state.
- Delayed older-frame YOLO detections can regress Citrus state unless Orange
  suppresses stale updates.
- The current `FrameIPCManager` stale-update suppression is the right live
  policy, but it should be tested deliberately before we enable it from
  headless real YOLO.

Later `publish_live_ipc=true` requirements:

- non-empty detections only,
- monotonic state from Citrus's perspective,
- stale update suppression counted and visible in summary artifacts,
- no zero-detection update publishing until Citrus can interpret authoritative
  zero-detection clears safely,
- `citrus_live_ipc_decision` JSONL rows once final publish/suppress outcomes
  are observable.

## Validation

Initial single-camera real-YOLO validation should require:

- run exits cleanly,
- `Cam<serial>_yolo_events.jsonl` exists,
- JSONL parser passes,
- `yolo_event_log_status = pass`,
- `yolo=false` should be replaced or augmented by a clearer real worker field,
  such as `yolo_worker_mode = real`,
- counts are reported:
  - `yolo_event_log_rows`,
  - `yolo_event_log_detection_rows`,
  - `yolo_event_log_zero_rows`,
  - `yolo_event_log_timeout_rows`,
  - `yolo_event_log_failed_rows`.
- recording health counters still pass:
  - zero acquisition starvation,
  - zero preprocess drops,
  - zero encode failures,
  - zero camera frame-id gaps.

Optional performance counters:

- YOLO queue depth,
- YOLO FPS,
- inference latency percentiles,
- frames considered vs frames submitted to YOLO,
- frames skipped by decimation or resource pressure.

## Risks

- Worker construction may fail if engine path, CUDA context, or TensorRT plugin
  state differs between GUI and headless.
- Real YOLO adds a new acquisition fanout subscriber and can force ring-copy
  behavior, increasing GPU memory traffic.
- Long inference latency can hold frame resources and affect recording.
- Multi-camera real YOLO can become GPU-bound quickly.
- Live detection IPC can corrupt Citrus live-control semantics if stale updates
  are not suppressed correctly.

## Implementation Checklist

- [ ] Define `HeadlessYoloWorkerConfig`.
- [ ] Parse `fixed.yolo_worker` in `orange_headless_client.cpp`.
- [ ] Add run config serialization for `yolo_worker`.
- [ ] Add summary fields:
  `yolo_worker_mode`, `yolo_worker_status`, `yolo_worker_engine_path`.
- [ ] Keep `fixed.yolo=true` rejected or map it explicitly to
  `fixed.yolo_worker.mode = "real"` only after the config is implemented.
- [ ] Store per-camera YOLO engine paths in owned strings that outlive workers.
- [ ] Initialize `CameraResources` with YOLO support when real headless YOLO is
  enabled.
- [ ] Construct and start `YOLOv8Worker` in headless local runs.
- [ ] Pass the worker pointer into `acquire_frames(...)`.
- [ ] Ensure shutdown stops YOLO workers before camera resources are freed.
- [ ] Reuse existing JSONL summarization for real YOLO.
- [ ] Extend JSONL summarization with timeout/failed row counts.
- [ ] Add a checked-in single-camera real-YOLO smoke spec, if an engine path can
  be made host-local and configurable.
- [ ] Build `orange_client`.
- [ ] Run a short single-camera headless real-YOLO smoke.
- [ ] Only after audit-only real YOLO is stable, design and test
  `publish_live_ipc=true`.

## Non-Goals For First Slice

- No multi-machine orchestration.
- No multi-camera real-YOLO requirement.
- No live Citrus detection IPC by default.
- No crop/encode integration.
- No GUI refactor.
- No model-quality assertions.
