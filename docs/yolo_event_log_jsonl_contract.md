# YOLO Event Log JSONL Contract

Date: 2026-04-21
Status: v1 contract. Current GUI YOLO runtime emits `yolo_result` rows.
`citrus_live_ipc_decision` and `yolo_frame_decision` rows are planned.

Purpose: define the Orange-owned recording/audit artifact for YOLO semantic
history. This file is separate from the Citrus live-control shared-memory queue.
Citrus IPC remains a latest-state stream; this JSONL file is where Orange should
preserve complete YOLO results and Citrus live-IPC publish/suppress decisions.

See also:
[headless_synthetic_yolo_event_log_plan.md](./headless_synthetic_yolo_event_log_plan.md)
for the planned deterministic headless test mode.

## Path

Per camera, per recording folder:

```text
<recording_folder>/Cam<serial>_yolo_events.jsonl
```

Example:

```text
/home/jeremy/orange_data/exp/unsorted/2026_04_21_12_48_36/Cam2010096_yolo_events.jsonl
```

## File Encoding

- UTF-8 JSON Lines.
- One complete JSON object per line.
- No enclosing array.
- Lines are append-only during the recording.
- Each line must be independently parseable.
- No `NaN`, `Infinity`, or `-Infinity` values.
- Numeric timestamps and frame ids are decimal JSON numbers.

## Common Fields

Every line must contain these fields:

```json
{
  "schema_id": "orange.yolo_event",
  "schema_version": 1,
  "event_sequence": 1,
  "event_kind": "yolo_result",
  "recording_id": "2026_04_21_12_48_36",
  "camera_serial": "2010096",
  "camera_id": 3,
  "frame": {
    "local_frame_id": 987,
    "camera_frame_id": 456789,
    "recording_frame_id": 123,
    "ipc_frame_id": 123,
    "record_active": true
  },
  "timestamps": {
    "camera_timestamp": 1234567890,
    "timestamp_sys_ns": 1776800000000000000,
    "event_epoch_us": 1776800000012345,
    "event_monotonic_us": 123456789
  }
}
```

Field semantics:

- `schema_id`: always `orange.yolo_event` for this file.
- `schema_version`: integer schema version, initially `1`.
- `event_sequence`: per-file monotonic sequence number starting at `1`.
- `event_kind`: one of the row types below.
- `recording_id`: recording folder id, usually `YYYY_MM_DD_HH_MM_SS`.
- `camera_serial`: camera serial string used in filenames and SHM queue names.
- `camera_id`: Orange runtime camera id/index.
- `frame.local_frame_id`: Orange acquisition-thread local frame counter.
- `frame.camera_frame_id`: absolute SDK/acquisition frame id.
- `frame.recording_frame_id`: recording-local frame id, or `0` when no
  recording-local id exists.
- `frame.ipc_frame_id`: frame id Orange would use for the Citrus live IPC queue.
- `frame.record_active`: whether Orange considered recording active for this
  frame.
- `timestamps.camera_timestamp`: camera SDK timestamp from the frame.
- `timestamps.timestamp_sys_ns`: Orange realtime/system timestamp captured for
  the frame.
- `timestamps.event_epoch_us`: wall-clock microseconds when this JSONL event was
  emitted.
- `timestamps.event_monotonic_us`: Orange monotonic microseconds when this JSONL
  event was emitted.

Consumer rule:

- Use `frame.record_active == true` and `frame.recording_frame_id > 0` when
  joining YOLO rows one-to-one with recorded video frames or `Cam*_meta.csv`.
  The current GUI runtime only writes YOLO JSONL rows for frames that have a
  positive recording-local frame id.

## Row Types

### `yolo_result`

Required for each frame that reaches the YOLO worker and produces a terminal
result.

```json
{
  "schema_id": "orange.yolo_event",
  "schema_version": 1,
  "event_sequence": 42,
  "event_kind": "yolo_result",
  "recording_id": "2026_04_21_12_48_36",
  "camera_serial": "2010096",
  "camera_id": 3,
  "frame": {
    "local_frame_id": 987,
    "camera_frame_id": 456789,
    "recording_frame_id": 123,
    "ipc_frame_id": 123,
    "record_active": true
  },
  "timestamps": {
    "camera_timestamp": 1234567890,
    "timestamp_sys_ns": 1776800000000000000,
    "event_epoch_us": 1776800000012345,
    "event_monotonic_us": 123456789
  },
  "yolo": {
    "status": "detections",
    "detection_count": 1,
    "coordinate_space": "source_frame_pixels",
    "model_id": "fish_jinyao",
    "engine_path": "/abs/path/models/fish_jinyao.engine",
    "gpu_id": 5
  },
  "detections": [
    {
      "index": 0,
      "x_px": 100.0,
      "y_px": 200.0,
      "width_px": 40.0,
      "height_px": 30.0,
      "label": 0,
      "confidence": 0.92,
      "keypoints": []
    }
  ],
  "citrus_live_ipc": {
    "queue_name": "/shm_cam_2010096",
    "enabled": true,
    "requested": true,
    "request_status": "queued"
  }
}
```

`yolo.status` values:

- `detections`: YOLO completed and produced one or more detections.
- `zero_detections`: YOLO completed successfully with no detections.
- `timeout`: YOLO did not produce a valid result before its timeout.
- `failed`: YOLO failed for a non-timeout reason.

Optional `yolo.error` values describe the failure or timeout reason. Current
runtime examples include:

- `inference_timeout`
- `cpu_results_skipped`

Detection semantics:

- `detections` must be present on every `yolo_result` row.
- `detections` is an empty array for `zero_detections`, `timeout`, and `failed`.
- Coordinates are full source-frame pixel coordinates with origin at top-left.
- Boxes use `x_px`, `y_px`, `width_px`, and `height_px`.
- `confidence` is the model/object probability.
- `keypoints` is reserved for pose-style models and is an empty array for
  current box-only YOLO.

`citrus_live_ipc.request_status` values:

- `queued`: Orange requested a live IPC update; final publish/suppress outcome
  may be represented by a later `citrus_live_ipc_decision` row.
- `not_enabled`: frame IPC was not enabled for this camera.
- `not_requested_synthetic`: headless synthetic YOLO emitted an audit row but
  intentionally did not publish a detection update into the live Citrus queue.
- `not_requested_zero_detections`: zero-detection results are not published to
  the current Citrus live queue.
- `not_requested_failed`: failed/timeout results are not published to the
  current Citrus live queue.

## Validation Notes

### GUI YOLO Recording Smoke, 2026-04-22

Artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2026_04_22_15_07_45
```

Observed behavior:

- Camera `2010096` produced `Cam2010096.mp4`, `Cam2010096_meta.csv`,
  `Cam2010096_yolo_events.jsonl`, and `Cam2010096_yolo_perf.csv`.
- Video and metadata matched at `2351` recorded frames.
- YOLO JSONL emitted `2356` `yolo_result` rows with consecutive
  `event_sequence` values.
- The active recording subset was clean: `recording_frame_id` covered
  `1..2351` with no gaps.
- The final `5` YOLO rows had `record_active=false` and
  `recording_frame_id=0` because streaming continued briefly after recording
  stopped. This was observed before the strict recorded-frame logging gate was
  added; current GUI logging should omit that post-recording tail.
- Status counts were `1720` `detections` and `636` `zero_detections`.
- Citrus live IPC was requested for all non-empty detection rows and was not
  requested for zero-detection rows, matching the current contract.

### Strict Recorded-Frame GUI YOLO Smoke, 2026-04-22

Artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2026_04_22_17_41_15
```

Observed behavior after the strict recorded-frame logging gate:

- Camera `2010096` produced HEVC video at `4512x4512`, `100 fps`,
  `11.51 s`, with `1151` encoded frames.
- `Cam2010096_meta.csv` had `1151` data rows.
- `Cam2010096_yolo_events.jsonl` had `1151` `yolo_result` rows.
- `Cam2010096_yolo_perf.csv` had `1151` data rows.
- `event_sequence` and `recording_frame_id` both covered `1..1151` with no
  gaps and no zero recording-frame ids.
- All YOLO rows were `record_active=true`; status count was `1151`
  `detections`.
- Citrus live IPC was requested for every detection row.

### `citrus_live_ipc_decision`

Planned row. Required once Orange can observe the final live-IPC handling
outcome for a non-empty YOLO result. This row is intentionally separate from
`yolo_result` because the live queue writer is asynchronous.

```json
{
  "schema_id": "orange.yolo_event",
  "schema_version": 1,
  "event_sequence": 43,
  "event_kind": "citrus_live_ipc_decision",
  "recording_id": "2026_04_21_12_48_36",
  "camera_serial": "2010096",
  "camera_id": 3,
  "frame": {
    "local_frame_id": 987,
    "camera_frame_id": 456789,
    "recording_frame_id": 123,
    "ipc_frame_id": 123,
    "record_active": true
  },
  "timestamps": {
    "camera_timestamp": 1234567890,
    "timestamp_sys_ns": 1776800000000000000,
    "event_epoch_us": 1776800000012400,
    "event_monotonic_us": 123456844
  },
  "citrus_live_ipc": {
    "queue_name": "/shm_cam_2010096",
    "enabled": true,
    "decision": "published",
    "reason": "",
    "detection_count": 1,
    "latest_base_frame_id_at_decision": 123,
    "updates_sent_after": 10,
    "update_stale_drops_after": 2,
    "push_failures_after": 0
  }
}
```

`citrus_live_ipc.decision` values:

- `published`: update was pushed to the Citrus live-control queue.
- `suppressed_stale`: update was not pushed because its `ipc_frame_id` was
  older than the latest emitted base frame.
- `push_failed`: update was eligible but the SHM push failed.
- `dropped_before_publish`: update was dropped from Orange's bounded internal
  update queue before the writer could process it.
- `not_enabled`: frame IPC was disabled or unavailable.

`reason` should be empty when `decision == "published"`. Otherwise it should be
a stable lowercase string such as:

- `older_than_latest_base_frame`
- `frame_ipc_disabled`
- `frame_ipc_init_failed`
- `shm_queue_full`
- `bounded_update_queue_overflow`

### `yolo_frame_decision`

Planned optional v1 row for frames that are considered for YOLO before the YOLO
worker receives them. This is useful when auditing skipped frames due to
decimation, disabled YOLO, resource pressure, or recording/session gates.

```json
{
  "schema_id": "orange.yolo_event",
  "schema_version": 1,
  "event_sequence": 44,
  "event_kind": "yolo_frame_decision",
  "recording_id": "2026_04_21_12_48_36",
  "camera_serial": "2010096",
  "camera_id": 3,
  "frame": {
    "local_frame_id": 988,
    "camera_frame_id": 456790,
    "recording_frame_id": 124,
    "ipc_frame_id": 124,
    "record_active": true
  },
  "timestamps": {
    "camera_timestamp": 1234567990,
    "timestamp_sys_ns": 1776800000010000000,
    "event_epoch_us": 1776800000012346,
    "event_monotonic_us": 123466789
  },
  "yolo_decision": {
    "decision": "skipped",
    "reason": "decimation"
  }
}
```

`yolo_decision.decision` values:

- `scheduled`
- `skipped`

Suggested `reason` values:

- `disabled`
- `decimation`
- `resource_unavailable`
- `worker_unavailable`
- `recording_inactive`
- `session_gate`

## Ordering And Joins

- `event_sequence` is the authoritative order within one file.
- Consumers should join rows by
  `(recording_id, camera_serial, camera_frame_id, recording_frame_id)`.
- `recording_frame_id` is preferred for joining to video metadata while
  recording is active.
- `camera_frame_id` is preferred when auditing behavior outside active
  recording windows.
- The JSONL file is append-only; consumers should tolerate partially written
  final lines if reading while Orange is still running.

## Implementation Phasing

Current implementation emits:

- `yolo_result` for every frame that reaches YOLO while the frame has a
  recording folder.
- deterministic headless synthetic `yolo_result` rows when
  `fixed.yolo_event_log.mode = "synthetic"` is enabled in an experiment spec.

Phase 2 should add:

- `citrus_live_ipc_decision` rows for every non-empty result that is published
  or suppressed by `FrameIPCManager`,
- `yolo_frame_decision` rows for skipped/scheduled audit coverage,
- model provenance fields in `yolo`,
- optional keypoint payloads if pose-like outputs are added.

## Non-Goals

- This file is not the Citrus live-control transport.
- This file does not require Citrus to consume every line.
- This file does not change the current Shaman shared-memory slot layout.
- This file does not make Citrus H5 the authoritative Orange event log.
