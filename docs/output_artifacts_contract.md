# Orange Output Artifacts Contract (Current Runtime)

Purpose: define the concrete output contracts currently produced by Orange so
downstream analysis consumers can parse artifacts without guessing.

Date anchored: 2026-02-27.

Scope:
- Current runtime behavior from writer code paths in this repository.
- On-disk artifacts and runtime IPC payloads.

Non-scope:
- Planned/future contracts documented in TODO/plan files.
- Cross-repo consumer behavior details beyond minimal parse guidance.

## Contract Precedence

Use this precedence when contracts disagree:
1. Writer code paths in `src/` (current runtime behavior).
2. This document.
3. Other narrative docs/TODO plans.

## Recording Folder Resolution

Recording folder pattern:
- `<base_folder>/<recording_id>`
- `recording_id` format: `YYYY_MM_DD_HH_MM_SS`

Where `base_folder` comes from:
- UI/config save path (`input_folder` / encoder config folder).

Default configured base path in runtime:
- `.../orange_data/exp/unsorted`

## Artifact Inventory

| Artifact | Path pattern | Requiredness | Gate |
|---|---|---|---|
| Recording folder | `<base_folder>/<recording_id>/` | Required for recording sessions | Recording started |
| Snapshot JSON | `<recording_folder>/recording_snapshot.json` | Required | Recording started |
| PTP sync summary | `<recording_folder>/ptp_sync_summary.json` | Required | Recording started |
| Latest pointer (local) | `<base_folder>/.orange/latest_recording.json` | Required | Recording started |
| Latest pointer (shared) | `/run/orange/latest_recording.json` | Required (best-effort write) | Recording started |
| Main video | `<recording_folder>/Cam<serial>.mp4` | Typical | Per-camera HW encoding active |
| Main metadata CSV | `<recording_folder>/Cam<serial>_meta.csv` | Typical | Per-camera HW encoding active |
| Main keyframe sidecar | `<recording_folder>/Cam<serial>_keyframe.json` | Typical | Per-camera HW encoding active |
| Pipeline perf CSV | `<recording_folder>/Cam<serial>_pipeline_perf.csv` | Optional | Per-camera recording folder active |
| GPU dmon CSV | `<recording_folder>/nvidia_smi_dmon.csv` | Optional | Headless recording session with best-effort GPU monitoring |
| GPU dmon stderr log | `<recording_folder>/nvidia_smi_dmon.stderr.log` | Optional | Headless recording session with best-effort GPU monitoring |
| Crop video | `<recording_folder>/Cam<serial>_crop.mp4` | Optional | Crop-and-encode active |
| Crop metadata CSV | `<recording_folder>/Cam<serial>_crop_meta.csv` | Optional | Crop-and-encode active |
| Crop keyframe sidecar | `<recording_folder>/Cam<serial>_crop_keyframe.json` | Optional | Crop-and-encode active |
| YOLO perf CSV | `<recording_folder>/Cam<serial>_yolo_perf.csv` | Optional | `ORANGE_YOLO_PERF_LOG != 0` |
| YOLO debug PNG | `./debug_pre_yolo_<serial>_<frame_id>.png` | Optional | `Dump Input` action |

Note: still-image save path is currently not a stable output contract (writer
handoff fields are underspecified at present).

## JSON Contracts

### Latest Recording Pointer JSON

Written to both:
- `<base_folder>/.orange/latest_recording.json`
- `/run/orange/latest_recording.json`

Current emitted shape:

```json
{
  "recording_id": "YYYY_MM_DD_HH_MM_SS",
  "timestamp_utc": "YYYY-MM-DDTHH:MM:SSZ",
  "recording_folder": "/abs/path/to/base/recording_id",
  "snapshot_path": "/abs/path/to/base/recording_id/recording_snapshot.json"
}
```

Notes:
- Writes are atomic (`.tmp` + rename).
- `/run/orange/latest_recording.json` is chmod `0644`.

### Recording Snapshot JSON

Path:
- `<recording_folder>/recording_snapshot.json`

Current emitted top-level fields:
- `recording_id: string`
- `timestamp_utc: string` (UTC ISO8601)
- `producer_version: string` (currently `"unknown"`)
- `sync: object` (session-level synchronization provenance)
- `cameras: object`
- `gpu_inventory: object` (runtime GPU metadata keyed by GPU id string)
- `gpu_monitoring: object` (optional host-level GPU monitor sidecars keyed by monitor name)
- `encoders: object` (added later by encoder worker updates)
- `pipeline_metrics: object` (optional, added when acquisition worker finalizes per-camera pipeline summaries)

`cameras` object:
- Keys are camera identifiers (usually serial strings, fallback may use camera
  id string when serial not available).
- Values are full camera config JSON or `null`.

`gpu_inventory` object:
- Keys are GPU id strings such as `"0"`.
- Values are runtime GPU metadata objects:
  - `id: integer`
  - `name: string` (when lookup succeeds)
  - `pci_bus_id: string` (when lookup succeeds)
  - `compute_capability.major: integer` (when lookup succeeds)
  - `compute_capability.minor: integer` (when lookup succeeds)
  - `total_global_mem_bytes: integer` (when lookup succeeds)
  - `lookup_error: string` (optional, when lookup fails)
  - `pci_bus_id_lookup_error: string` (optional, when bus-id lookup fails)

`encoders` object (current shape):
- Key: camera identifier string (serial or camera_id string).
- Value: encoder info object:
  - `backend: string`
  - `path: string`
  - `codec: string`
  - `preset: string`
  - `tuning: string`
  - `gpu_id: integer`
  - `gpu: object` (optional runtime GPU metadata matching `gpu_inventory[gpu_id]`)
  - `color: boolean`
  - `resolution.width: integer`
  - `resolution.height: integer`
  - `fps: integer`
  - `gop_length: integer`
  - `frame_interval_p: integer`
  - `idr_period: integer`
  - Optional:
    - `refs.max_num_ref_frames: integer` (if > 0)
    - `refs.max_num_ref_frames_in_dpb: integer` (if > 0)
  - `rc.mode: string`
  - `rc.mode_value: integer`
  - `rc.average_bitrate: integer`
  - `rc.max_bitrate: integer`
  - `rc.vbv_buffer_size: integer`
  - `aq.enable_aq: integer`
  - `aq.enable_temporal_aq: integer`
  - `lookahead.enable: integer`
  - `low_delay_keyframe_scale: integer`
  - `strict_gop_target: integer`
  - `enable_non_ref_p: integer`
  - `repeat_sps_pps: integer`
  - `enable_ptd: integer`

Important:
- Current runtime snapshot shape is legacy/single-level encoder info.
- Do not assume `encoders[serial].outputs` or `models` exists.

`gpu_monitoring` object (current headless shape):
- Key: monitor name string (currently `nvidia_smi_dmon`)
- Value: monitor info object:
  - `schema_version: integer`
  - `tool: string`
  - `status: string`
  - `sample_period_seconds: integer`
  - `gpu_ids: integer[]`
  - `artifact_path: string`
  - `stderr_path: string`
  - Optional:
    - `started_at_utc: string`
    - `stopped_at_utc: string`
    - `pid: integer` (while running)
    - `exit_code: integer`
    - `signal: integer`
    - `error: string`
  - `command: string[]`

`pipeline_metrics` object (current shape):
- Key: camera identifier string (serial or camera_id string).
- Value: pipeline summary object:
  - `schema_version: integer`
  - `camera_serial: string`
  - `camera_id: integer`
  - `gpu_id: integer`
  - `gpu: object` (optional runtime GPU metadata matching `gpu_inventory[gpu_id]`)
  - `updated_at_utc: string`
  - `artifact_path: string`
  - `period_seconds: integer` (currently `1`)
  - `samples: integer`
  - `finalized: boolean`
  - Optional:
    - `last_sample_at_utc: string`
    - `last_frame_id: integer`
    - `last_recording_frame_id: integer`
  - `fps.acquisition|preprocess|encode: object`
  - `queue_depth.display|yolo|preprocess|encode|pending_requeues: object`
  - `resource_availability.acquire_entries|acquire_entries_low_watermark|acquire_events|acquire_events_low_watermark|yolo_events|yolo_events_low_watermark|preprocess_buffers|preprocess_events: object`
  - `totals.acquisition_resource_starvations: integer` (optional)
  - `totals.preprocess_resource_waits: integer` (optional)
  - `totals.preprocess_frames_dropped: integer` (optional)
  - `totals.encode_failures: integer` (optional)
  - `totals.encode_slow_frames: integer` (optional)
  - `totals.gpu_direct_frames: integer` (optional)
  - `totals.gpu_ring_copy_frames: integer` (optional)
  - `totals.gpu_copy_frames: integer` (optional)

Stats object shape used above:
- `samples: integer`
- `min: number` (omitted when `samples == 0`)
- `max: number` (omitted when `samples == 0`)
- `last: number` (omitted when `samples == 0`)
- `mean: number` (omitted when `samples == 0`)

`sync` object (current shape):
- `schema_version: integer`
- `captured_at_utc: string`
- `camera_sync_enabled: boolean`
- `mode: string` in `none|ptp_local|ptp_network`
- `network_sync: boolean`
- `num_cameras_expected: integer`
- `gate_times.start_ns: integer` (optional)
- `gate_times.stop_ns: integer` (optional)
- `barriers.start.participants_reached: integer`
- `barriers.start.all_reached: boolean`
- `barriers.stop.participants_reached: integer`
- `barriers.stop.all_reached: boolean`
- `signals.start_observed: boolean`
- `signals.stop_observed: boolean`

Contract note:
- `sync` is a snapshot of synchronization state at recording start.
- It is intended for session provenance and debugging, not as a full event log
  or per-frame timing stream.

### PTP Sync Summary JSON

Path:
- `<recording_folder>/ptp_sync_summary.json`

Current emitted top-level fields:
- `schema_version: integer`
- `recording_id: string`
- `recording_folder: string`
- `created_at_utc: string`
- `updated_at_utc: string`
- `sync: object` (same session-level sync snapshot shape as `recording_snapshot.json`)
- `cameras: object`

`cameras` object:
- Key: camera serial string
- Value: per-camera low-rate timing summary with fields including:
  - `camera_serial`
  - `camera_id`
  - `gpu_id`
  - `sync_camera_enabled`
  - `finalized`
  - `updated_at_utc`
  - `frame_count`
  - `frames_received`
  - `dropped_frames` (camera-side frame-ID gaps and `EVT_CameraGetFrame` errors)
  - `last_frame_timestamp_ns`
  - `last_latched_ptp_time_ns`
  - `ptp_offset_ns.{samples,min,max,last,mean}`
  - `latch_minus_frame_ns.{samples,min,max,last,mean}`
  - `frame_delta_ns.{samples,min,max,last,mean}`
  - `latch_delta_ns.{samples,min,max,last,mean}`
  - `delta_samples`
  - `avg_frame_delta_ns_running`
  - `avg_latch_delta_ns_running`

Contract notes:
- This sidecar is updated during acquisition and finalized when the active
  recording folder lifetime ends for that camera thread.
- It is a summarized diagnostic artifact, not a per-frame timing trace.

## CSV Contracts

### Main Metadata CSV (`Cam<serial>_meta.csv`)

Header (exact):

```text
frame_id,timestamp,timestamp_sys
```

Field semantics:
- `frame_id`: recording-frame counter (`recording_frame_id`, uint64), not the
  absolute camera frame counter.
- `timestamp`: camera SDK timestamp (`uint64`, unit not explicitly documented
  in code contract).
- `timestamp_sys`: system wall-clock timestamp from `CLOCK_REALTIME` in
  nanoseconds (`uint64`).

### Crop Metadata CSV (`Cam<serial>_crop_meta.csv`)

Header (exact):

```text
frame_id,timestamp,timestamp_sys,detection_confidence,crop_x,crop_y,crop_w,crop_h
```

Field semantics:
- `frame_id`: recording-frame counter (`uint64`).
- `timestamp`: camera SDK timestamp (`uint64`, unit not explicitly documented).
- `timestamp_sys`: realtime nanoseconds (`uint64`).
- `detection_confidence`: detection confidence (`float`).
- `crop_x,crop_y,crop_w,crop_h`: crop rectangle in source-frame pixels.

Behavior note:
- Blank crop frames may still be encoded when no detection exists.
- Metadata row is only appended when detection exists.

### YOLO Perf CSV (`Cam<serial>_yolo_perf.csv`)

Header (exact order):

```text
frame_id,recording_frame_id,timestamp,timestamp_sys,queue_depth,fps,ok,wait_ms,pre_ms,gap_ms,enqueue_ms,infer_ms,sync_ms,cpu_wait_event_ms,cpu_npp_set_stream_ms,cpu_preprocess_ms,cpu_dump_ms,cpu_infer_call_ms,cpu_event_record_ms,cpu_pre_sync_ms,cpu_pre_sync_other_ms,cpu_post_sync_ms,queue_ms,post_ms,track_ms,ipc_ms,enet_ms,total_ms
```

Field semantics:
- ID/timestamp fields: integer counters/timestamps.
- `ok`: integer success flag.
- `*_ms` and fps fields: floating-point timing/throughput metrics.

Gate:
- Disabled when `ORANGE_YOLO_PERF_LOG=0`.
- Sampling controlled by `ORANGE_YOLO_PERF_SAMPLE`.

### Pipeline Perf CSV (`Cam<serial>_pipeline_perf.csv`)

Header (exact order):

```text
timestamp_utc,frame_id,recording_frame_id,acq_fps,pre_fps,enc_fps,display_q,yolo_q,pre_q,enc_q,acq_free_entries,acq_free_entries_low,acq_free_events,acq_free_events_low,yolo_events,yolo_events_low,pending_requeues,acq_starve,pre_buffers,pre_events,pre_waits,pre_drops,enc_fail,enc_slow,gpu_direct,gpu_ring,gpu_copy
```

Field semantics:
- `timestamp_utc`: UTC ISO8601 timestamp for the sample row.
- `frame_id`: absolute camera frame counter at sample time.
- `recording_frame_id`: last seen recording-frame counter for the active recording folder.
- `acq_fps`, `pre_fps`, `enc_fps`: current acquisition, preprocess, and encode FPS estimates.
- `display_q`, `yolo_q`, `pre_q`, `enc_q`: instantaneous queue depths for display, YOLO, preprocess, and HW encode stages.
- `acq_free_entries`, `acq_free_entries_low`: available acquire work entries and the interval low-water mark.
- `acq_free_events`, `acq_free_events_low`: available acquire completion events and the interval low-water mark.
- `yolo_events`, `yolo_events_low`: available YOLO completion events and the interval low-water mark.
- `pending_requeues`: camera buffers still waiting for safe requeue after GPU work.
- `acq_starve`: cumulative count of acquisition-loop iterations that could not reserve the required work entry or events before attempting to fetch another camera frame.
- `pre_buffers`, `pre_events`: available preprocess NV12 buffers and CUDA events.
- `pre_waits`, `pre_drops`: cumulative preprocess resource-wait and drop counters.
- `enc_fail`, `enc_slow`: cumulative encode-failure and slow-frame counters.
- `gpu_direct`, `gpu_ring`, `gpu_copy`: cumulative counts since the current recording folder opened for direct camera-pointer use, direct-pointer ring-copy fallback, and ordinary device copies.

Behavior notes:
- Emitted at approximately one row per second while the camera has a non-empty recording folder.
- Short recordings may create the file with only the header if no one-second sample boundary is crossed.

## Keyframe Sidecar JSON Contract

Path:
- Derived from configured keyframe sidecar path; normalized to `.json`.

Current emitted shape:

```json
{
  "codec": "h264|hevc|unknown",
  "fps": 120,
  "total_frames": 12345,
  "keyframe_frames": [0, 30, 60]
}
```

Field semantics:
- `codec`: encoder codec label.
- `fps`: configured frame rate integer.
- `total_frames`: total packets written.
- `keyframe_frames`: append-order packet indices flagged as keyframes.

Detection notes:
- H.264 keyframe detection uses IDR NAL type `5`.
- HEVC keyframe detection uses IRAP NAL types `19`/`20`.
- Parser handles Annex-B and length-prefixed bitstreams.

Important:
- Keyframe indices are sequential packet indices and can differ from metadata
  CSV `frame_id` values.

## Shared-Memory IPC Contract

Queue name:
- `/shm_cam_<camera_serial>`

Queue characteristics:
- Ring slots: `8`
- Max objects per slot: `100`
- Max keypoint floats/object: `32`

Slot payload (`VectorSlot`) fields:
- `count: size_t`
- `objects[count]` where each object has:
  - `rect.x: float`
  - `rect.y: float`
  - `rect.width: float`
  - `rect.height: float`
  - `label: int`
  - `prob: float`
  - `kps[32]: float`
  - `num_kps: size_t`
- `timestamp_us_epoch: uint64` (producer wall-clock `system_clock` microseconds at push)
- `timestamp_us_monotonic: uint64` (producer local `steady_clock` microseconds at push)
- `frame_id: uint64`
- `camera_id: uint32` (camera index, not serial)
- `yolo_enabled: bool`

Emission behavior:
- Base frame event may be emitted with empty detections.
- Detection update emitted when YOLO produces detections.
- During recording, IPC `frame_id` uses `recording_frame_id`.
- Outside recording, IPC `frame_id` uses absolute camera `frame_id`.

## ENet / FlatBuffer Payload Contract

### Control/State Payloads (currently emitted)

Current emitters send `FetchGame.Server` root payloads with fields such as:
- `signal_type`
- `server_state`
- `control`
- `record_folder`
- `config_folder`
- `ptp_global_time`

### Wrapped ORNG Payloads

Schema for wrapped payloads exists via:
- `message_wrapper.fbs` (`file_identifier "ORNG"`)
- `yolo_payload.fbs` (`YoloFrameDetections`)

Current runtime note:
- Receive path validates wrapped ORNG payloads.
- YOLO ENet send block is presently inactive (schema exists; emitter is not
  actively producing wrapped YOLO packets).
- GUI ENet startup now initializes the ENet runtime explicitly and only starts
  the ENet service thread if host initialization succeeds. See
  [enet_startup_troubleshooting.md](./enet_startup_troubleshooting.md).

## Timestamp and Frame Identity Rules

- `frame_id` (camera-local absolute) and `recording_frame_id` (session-local)
  are distinct.
- Main/crop metadata CSV `frame_id` currently means `recording_frame_id`.
- `timestamp_sys` in metadata rows is realtime nanoseconds.
- IPC `timestamp_us_epoch` is enqueue-time epoch microseconds.
- IPC `timestamp_us_monotonic` is enqueue-time steady-clock microseconds.
- Neither IPC timestamp field is the camera SDK timestamp.
- Camera SDK `timestamp` units are not explicitly specified by the runtime
  contract and should be treated as source-native ticks.

## Known Inconsistencies and Caveats

1. Queue naming differs across code paths/docs in places (`serial` vs camera
   index formatting). Treat `/shm_cam_<camera_serial>` as producer contract.
2. Snapshot docs may describe richer target structures not currently emitted.
3. Still-image save path is not a reliable consumer contract at present.
4. Keyframe sidecar index space is packet-index based and may not align with
   metadata CSV `frame_id`.

## Parse Guidance for Downstream Consumers

- Prefer defensive parsing:
  - tolerate absent optional artifacts
  - treat snapshot extra fields as forward-compatible
  - do not assume target/TODO-only fields are present
- Join timing streams using explicit key choice:
  - use `recording_frame_id` semantics for metadata CSV `frame_id`
  - do not directly equate keyframe sidecar index with metadata `frame_id`
