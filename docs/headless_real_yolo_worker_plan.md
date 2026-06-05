# Headless Real YOLO Worker Plan

Date: 2026-04-22
Status: first audit-only real-YOLO slice implemented; live IPC and broader
validation remain future work.

Purpose: define what it would take to run the real `YoloWorker` from headless
experiments, while preserving the current stable recording path and the Citrus
live-control IPC contract. The current worker implementation is backed by the
TensorRT YOLOv8 detector, but the Orange worker name is model-version neutral.

## Current State

As of this plan:

- GUI real YOLO can emit `Cam<serial>_yolo_events.jsonl`.
- Headless synthetic YOLO can emit and validate the same JSONL schema without
  TensorRT inference.
- Headless synthetic YOLO is audit-only: no real YOLO worker, no live Citrus
  detection update IPC.
- Headless frame IPC can verify serial-named base-frame queues such as
  `/shm_cam_2010096`.
- Local headless experiments still reject legacy `fixed.yolo=true`.
- Local headless experiments now support explicit real inference through
  `fixed.yolo_worker.mode = "real"` or direct CLI `--yolo-engine`.

Interpretation:

- We have good headless coverage for recording, base-frame IPC, and the YOLO
  audit-log contract.
- We now have the first real model-inference path in headless. It is
  audit-only by default and uses the same `YoloWorker` as GUI.

## Recommendation

Do not make real headless YOLO the default next implementation unless headless
inference is needed soon. It touches TensorRT lifecycle, CUDA resources,
acquisition fanout, worker shutdown, and live IPC policy.

The implemented first slice is intentionally narrow and opt-in:

- one or more selected cameras, with single-camera recommended for first smoke
  tests,
- real TensorRT YOLO worker,
- JSONL audit output enabled,
- live Citrus detection IPC disabled,
- no crop/encode coupling,
- no display coupling,
- no multi-camera performance promises until the single-camera path is stable.

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
  - `real`: instantiate `YoloWorker` in headless mode.
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

Headless should use the same `YoloWorker` class as GUI for the first real
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
6. Construct `YoloWorker` for each selected camera.
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

## Implementation Status

- [x] Define `HeadlessYoloWorkerConfig`.
- [x] Parse `fixed.yolo_worker` in `orange_headless_client.cpp`.
- [x] Add direct local CLI flags:
  `--yolo-engine`, `--yolo-decimate`, `--yolo-publish-live-ipc`.
- [x] Add run config serialization for `yolo_worker`.
- [x] Add summary fields:
  `yolo_worker_mode`, `yolo_worker_status`, `yolo_worker_engine_path`.
- [x] Keep `fixed.yolo=true` rejected or map it explicitly to
  `fixed.yolo_worker.mode = "real"` only after the config is implemented.
- [x] Store YOLO engine paths in owned config strings that outlive workers.
- [x] Initialize `CameraResources` with YOLO support when real headless YOLO is
  enabled.
- [x] Initialize TensorRT plugins in the headless path before constructing
  `YoloWorker`.
- [x] Construct and start `YoloWorker` in headless local runs.
- [x] Pass the worker pointer into `acquire_frames(...)`.
- [x] Ensure shutdown stops YOLO workers before camera resources are freed.
- [x] Reuse existing JSONL summarization for real YOLO.
- [x] Extend JSONL summarization with timeout/failed row counts.
- [x] Add a checked-in single-camera real-YOLO smoke spec, if an engine path can
  be made host-local and configurable.
- [x] Build `orange_client`.
- [x] Run a short single-camera headless real-YOLO smoke.
- [ ] Only after audit-only real YOLO is stable, design and test
  `publish_live_ipc=true`.

## Baseline Validation

Validated on 2026-04-25 with the sudo wrapper and the checked-in spec shape.
The successful run used a temporary experiment id because the first failed
attempt left its output folder intact:

```text
/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_preprocessonly_a16_gpu5_run2
```

Initial failure:

- Headless real YOLO reached `YoloWorker` construction but TensorRT could not
  deserialize the engine because the `EfficientNMS_TRT` plugin creator was not
  registered.
- Cause: GUI called `YOLOv8::initialize_plugins()` at startup, but the headless
  entrypoint did not.
- Fix: call `YOLOv8::initialize_plugins()` in the headless real-YOLO path before
  constructing workers.

Successful run summary:

- `status=completed`, `pass_fail=pass`.
- Acquisition held `~100 fps` with no camera drops, no frame-id gaps, and no
  get-frame errors.
- Real YOLO wrote `3203` event rows and `3203` perf rows.
- All event rows had zero detections, which is valid for this smoke because no
  subject was present.
- `recording_sink_mode=preprocess_only`, so no full-frame video and `enc_fps=0`
  are expected.

Steady-state YOLO metrics after frame 200:

| Metric | p95 |
| --- | ---: |
| `acquisition_to_worker_start_ms` | `0.0388 ms` |
| `yolo_queue_wait_ms` | `0.0181 ms` |
| `worker_start_to_detect_done_ms` | `3.4549 ms` |
| `acquisition_to_detect_done_ms` | `3.4894 ms` |
| `cpu_preprocess_ms` | `0.0165 ms` |
| `cpu_pre_sync_ms` | `0.0798 ms` |
| `cpu_infer_call_ms` | `0.0486 ms` |
| `total_ms` | `3.4606 ms` |

Interpretation:

- This is a clean Process A baseline for later process-isolation tests.
- It confirms that camera acquisition plus real TensorRT YOLO can sustain
  `100 fps` without GUI and without in-process full-frame NVENC.
- The YOLO hot path is not intrinsically slow in this setup. The large GUI
  detect-latency regressions are still most consistent with contention
  introduced by full-frame encode/output in the same process.
- Shutdown printed one source-release drain timeout. The run summary still
  passed and steady-state had no drops or starve, so treat this as a shutdown
  drain issue rather than a hot-path latency result.

## Current Invocation Shape

Direct local smoke example:

```bash
ORANGE_YOLO_PERF_LOG=1 ORANGE_YOLO_PERF_SAMPLE=1 \
./targets/release/orange_client \
  --mode local \
  --camera 2010096 \
  --record-folder /home/jeremy/orange_data/exp/headless_real_yolo_smoke \
  --recording-sink-mode preprocess_only \
  --duration 30 \
  --yolo-engine /home/jeremy/orange_data/detect/detect_all_available_detect_training_v004_yolo11n_trt_20260520_a16_gpu5_trt100_fp16_bo5_avg32.engine
```

Experiment spec smoke shape:

Checked-in spec:
`experiment_specs/2010096_headless_real_yolo_preprocessonly_a16_gpu5.json`

Sudo-wrapper invocation with explicit YOLO perf artifacts:

```bash
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  /home/jeremy/orange-gop-split-a16/experiment_specs/2010096_headless_real_yolo_preprocessonly_a16_gpu5.json
```

```json
"fixed": {
  "stream_only": false,
  "recording_sink_mode": "preprocess_only",
  "yolo_worker": {
    "mode": "real",
    "engine_path": "/home/jeremy/orange_data/detect/detect_all_available_detect_training_v004_yolo11n_trt_20260520_a16_gpu5_trt100_fp16_bo5_avg32.engine",
    "decimate": 1,
    "publish_live_ipc": false
  }
}
```

Local engine inventory checked on 2026-06-05:

- Current default/spec engine:
  `/home/jeremy/orange_data/detect/detect_all_available_detect_training_v004_yolo11n_trt_20260520_a16_gpu5_trt100_fp16_bo5_avg32.engine`
- Camera-config historical engine:
  `/home/jeremy/orange_data/detect/cam2010096_detect_v12_fp16.engine`
- Additional historical engine:
  `/home/jeremy/orange_data/detect/b.engine`

## Non-Goals For First Slice

- No multi-machine orchestration.
- No multi-camera real-YOLO requirement.
- No live Citrus detection IPC by default.
- No crop/encode integration.
- No GUI refactor.
- No model-quality assertions.
