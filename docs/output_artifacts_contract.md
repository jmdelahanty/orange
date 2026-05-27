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
| Latest pointer (canonical) | `<canonical_pointer_root>/latest_recording.json` | Required when configured | Recording started |
| Latest pointer (shared) | `/run/orange/latest_recording.json` | Required (best-effort write) | Recording started |
| Main video | `<recording_folder>/Cam<serial>.mp4` | Typical | Per-camera HW encoding active |
| Main metadata CSV | `<recording_folder>/Cam<serial>_meta.csv` | Typical | Per-camera HW encoding active |
| Main keyframe sidecar | `<recording_folder>/Cam<serial>_keyframe.json` | Typical | Per-camera HW encoding active |
| Pipeline perf CSV | `<recording_folder>/Cam<serial>_pipeline_perf.csv` | Optional | Per-camera recording folder active |
| Acquisition cadence probe CSV | `<recording_folder>/Cam<serial>_acquisition_cadence_probe.csv` | Optional | Per-camera recording folder active; frames `80-160` only |
| Pre-encoder reference raw dump | `<recording_folder>/Cam<serial>_preenc_ref.bin` | Optional | `pre_encoder_reference_capture.enabled = true` |
| Pre-encoder reference index | `<recording_folder>/Cam<serial>_preenc_ref_index.csv` | Optional | `pre_encoder_reference_capture.enabled = true` |
| Pre-encoder reference metadata | `<recording_folder>/Cam<serial>_preenc_ref.json` | Optional | `pre_encoder_reference_capture.enabled = true` |
| GPU dmon output | `<recording_folder>/nvidia_smi_dmon.csv` | Optional | Headless recording session with best-effort GPU monitoring |
| GPU dmon stderr log | `<recording_folder>/nvidia_smi_dmon.stderr.log` | Optional | Headless recording session with best-effort GPU monitoring |
| Crop video | `<recording_folder>/Cam<serial>_crop.mp4` | Optional | Crop-and-encode active |
| Crop metadata CSV | `<recording_folder>/Cam<serial>_crop_meta.csv` | Optional | Crop-and-encode active |
| Crop keyframe sidecar | `<recording_folder>/Cam<serial>_crop_keyframe.json` | Optional | Crop-and-encode active |
| YOLO perf CSV | `<recording_folder>/Cam<serial>_yolo_perf.csv` | Optional | `ORANGE_YOLO_PERF_LOG != 0` |
| YOLO event JSONL | `<recording_folder>/Cam<serial>_yolo_events.jsonl` | Optional | GUI YOLO worker receives frames during recording |
| Pose perf CSV | `<recording_folder>/Cam<serial>_pose_perf.csv` | Optional | GUI pose worker active |
| Pose event JSONL | `<recording_folder>/Cam<serial>_pose_events.jsonl` | Optional | GUI pose worker receives crop frames during recording |
| YOLO debug PNG | `./debug_pre_yolo_<serial>_<frame_id>.png` | Optional | `Dump Input` action |

Diagnostic note:
- `ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME=1` starts a GUI recording session with
  `session.recording_sink_mode = "immediate_recycle"`. It creates the recording
  folder and timing artifacts, assigns `recording_frame_id`, and skips the main
  full-frame video pipeline. In that mode, main `Cam<serial>.mp4` and
  `Cam<serial>_meta.csv` artifacts are intentionally absent while YOLO, pose,
  crop, pipeline, and acquisition diagnostics may still be present.
- `ORANGE_GUI_RECORDING_SINK_MODE=preprocess_only` starts a GUI recording
  session that exercises full-frame preprocess routing but intentionally omits
  full-frame encode/output. Main `Cam<serial>.mp4`, main metadata, and main
  keyframe sidecars are absent by design; crop MP4s, YOLO perf, pose perf,
  crop perf, pipeline perf, and acquisition diagnostics may still be present
  when their normal producers are active.
- `ORANGE_CROP_PREVIEW_DISABLE=1` disables only the crop live-preview CUDA path.
  Full-frame `Cam<serial>.mp4`, crop `Cam<serial>_crop.mp4`, crop metadata,
  YOLO, pose, crop sidecar, pipeline, and acquisition diagnostics remain
  expected when their normal producers are active.
- `ORANGE_CROP_PREVIEW_MAX_FPS=<N>` overrides the persisted
  `crop_pipeline.preview_max_fps` setting for live crop preview only. `N=0`
  restores unlimited preview updates for diagnostics. Crop MP4s and crop
  metadata remain at crop/YOLO cadence.
- `ORANGE_CROP_RECORDING_SINK_MODE=external_ipc` is an experimental crop-video
  output sink. In that mode Orange keeps crop metadata/perf row writing in the
  recording folder but sends crop Mono8 CUDA buffers to an external recorder
  process for video encoding. The default remains in-process crop encoding
  (`real` and `in_process` are accepted aliases for the default). Treat this
  mode as diagnostic until live validation is complete. GUI supervision for
  this mode writes `external_crop_recorder_contract.json`,
  `external_crop_recorder_supervisor_plan.json`, and an
  `external_crop_recorder/` artifact root. The final
  `recording_outputs[serial].crop.video` path points at the external crop MP4,
  while crop metadata/perf paths remain Orange-written files in the recording
  folder. External crop recorder failures are sidecar failures: they should set
  `recording_outputs[serial].crop.status = "incomplete"` and
  `recording_backend.crop_recording.status = "incomplete"` without changing the
  full-frame `recording_outputs[serial].full` status.
- `ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=<N>` overrides the experimental
  external crop recorder encode queue depth. The default is currently `256`
  as a diagnostic shock absorber for four-camera validation, but that is high:
  at `100 fps` it permits about `2.56 s` of crop encode backlog per camera.
  Use lower values such as `32-64` once queue high-water and enqueue-age
  telemetry show enough margin.

Note: still-image save path is currently not a stable output contract (writer
handoff fields are underspecified at present).

## JSON Contracts

### Latest Recording Pointer JSON

Written to both:
- `<base_folder>/.orange/latest_recording.json`
- `<canonical_pointer_root>/latest_recording.json` when configured
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
- `<canonical_pointer_root>` currently defaults to `~/orange_data/.orange`.
- `/run/orange/latest_recording.json` is chmod `0644`.

### Recording Snapshot JSON

Path:
- `<recording_folder>/recording_snapshot.json`

Current emitted top-level fields:
- `schema_version: integer` (`2` for snapshots with unified output descriptors)
- `recording_id: string`
- `timestamp_utc: string` (UTC ISO8601)
- `producer_version: string` (currently `"unknown"`)
- `session: object` (optional in older artifacts)
- `recording_outputs: object` (schema-2 output descriptors keyed by camera
  serial, then output kind)
- `sync: object` (session-level synchronization provenance)
- `cameras: object`
- `camera_runtime: object` (resolved per-recording camera config keyed by camera id/serial)
- `gpu_inventory: object` (runtime GPU metadata keyed by GPU id string)
- `gpu_monitoring: object` (optional host-level GPU monitor sidecars keyed by monitor name)
- `encoders: object` (added later by encoder worker updates)
- `pipeline_metrics: object` (optional, added when acquisition worker finalizes per-camera pipeline summaries)

`session` object:
- `recording_sink_mode: string` (`real`, `immediate_recycle`,
  `preprocess_only`, `threaded_handoff_only`, or `external_ipc`)
- `full_frame_video_enabled: boolean`
- `gui_display_frame_rate: object` (optional, GUI recordings after
  finalization)
  - `schema_version: integer`
  - `source: string` (currently `imgui_io_delta_time`)
  - `stream_downsample: integer` (GUI display preview downsample)
  - `display_preview_max_fps: integer` (main display preview cadence cap)
  - `yolo_speed_graphs_enabled: boolean` (whether per-camera ImPlot speed
    graphs were enabled at finalization)
  - `saw_crop_preview_enabled|hidden|visible: boolean`
  - `overall|crop_preview_hidden|crop_preview_visible: object`
    - `sample_count`, `min_fps`, `p05_fps`, `p50_fps`, `p95_fps`, `max_fps`,
      and `mean_fps`
  - `timings: object` (optional, GUI recordings with phase telemetry)
    - `frame_total_ms`, `main_texture_upload_ms`, `crop_texture_upload_ms`,
      `camera_window_draw_ms`, `crop_window_draw_ms`, `speed_graph_draw_ms`,
      `render_present_ms`
    - Each timing bucket reports `sample_count`, `min_ms`, `p05_ms`, `p50_ms`,
      `p95_ms`, `max_ms`, and `mean_ms`.
    - `main_texture_upload_count` and `crop_texture_upload_count` count total
      texture uploads sampled during active recording.

`recording_outputs` object:
- Key: camera identifier string (serial or camera_id string).
- Value: object keyed by output kind:
  - `full`: ingest-authoritative full-frame output descriptor.
  - `crop`: optional YOLO crop sidecar descriptor.
- Each descriptor carries:
  - `schema_version: integer`
  - `camera_serial: string`
  - `output_kind: string`
  - `role: string` (`ingest_authoritative` or `sidecar`)
  - `backend: string` (`in_process`, `external_ipc`, or diagnostic sink mode)
  - `status: string` (`pending`, `completed`, `incomplete`, or `disabled`)
  - Optional artifact paths: `video`, `metadata`, `keyframes`, `perf`,
    `sidecar_perf`, `summary`
  - Optional counts: `frame_count`, `first_recording_frame_id`,
    `last_recording_frame_id`, `recording_frame_id_gaps`, `packet_count`,
    `packet_count_source`
  - Optional media details: `width`, `height`, `frame_rate`, `codec`,
    `container`, `tuning`, `pixel_source_format`, `encoded_format`,
    `coordinate_space`

`cameras` object:
- Keys are camera identifiers (usually serial strings, fallback may use camera
  id string when serial not available).
- Values are full camera config JSON or `null`.

`camera_runtime` object:
- Keys are camera identifiers (usually serial strings, fallback may use camera
  id string when serial not available).
- Values are resolved runtime camera config snapshots:
  - `source.camera_config_path: string`
  - `source.configured_gpu_id: integer`
  - `source.gpu_id_runtime_overridden: boolean`
  - `runtime: object` (full resolved camera config JSON actually used for the run)

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

Camera config schema 4 promotes `recording.encode.aq` and
`recording.encode.temporal_aq` to persistent tri-state fields
(`auto|off|on`). The encoder snapshot records the resolved NVENC state in
`aq.*` and the effective request in `requested_overrides.*`.

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
  - `rc.qp_map_mode.value: integer`
  - `rc.qp_map_mode.name: string`
  - `aq.enable_aq: integer`
  - `aq.enable_temporal_aq: integer`
  - `requested_overrides.aq: integer` (`-1 auto`, `0 off`, `1 on`)
  - `requested_overrides.temporal_aq: integer` (`-1 auto`, `0 off`, `1 on`)
  - `requested_overrides.lookahead: integer` (`-1 auto`, `0 off`, `1 on`)
  - `requested_overrides.lookahead_depth: integer` (`-1 auto`)
  - `requested_overrides.target_bitrate_bps: integer` (`-1 auto`)
  - `requested_overrides.max_bitrate_bps: integer` (`-1 auto`)
  - `requested_overrides.vbv_buffer_size: integer` (`-1 auto`)
  - `requested_overrides.importance_map_mode: string` (`off|static_roi`)
  - `requested_overrides.importance_map_roi_size_px: integer`
  - `importance_map: object`
    - `requested_mode: string`
    - `active_mode: string`
    - `enabled: boolean`
    - `shape: string` (`square`)
    - `roi_size_px: integer`
    - `block_size: integer`
    - `grid_width: integer`
    - `grid_height: integer`
    - `inside_delta_qp: integer`
    - `outside_delta_qp: integer`
    - `qp_map_size_bytes: integer`
  - `lookahead.enable: integer`
  - `lookahead.depth: integer`
  - `rc.multi_pass.value: integer`
  - `rc.multi_pass.name: string`
  - `low_delay_keyframe_scale: integer`
  - `strict_gop_target: integer`
  - `enable_non_ref_p: integer`
  - `repeat_sps_pps: integer`
  - `enable_ptd: integer`
  - `resolved_config: object`
    - normalized resolved NVENC initialize/config snapshot captured after
      `CreateEncoder()` / `GetInitializeParams()`
    - includes:
      - `initialize: object`
      - `buffers: object`
      - `common: object`
      - `rc: object`
      - `codec: object`

  - `latency: object` (optional, full-frame encoder/output timing aggregates)
    - values are stats objects with `samples`, `mean_ms`, `max_ms`, and
      `last_ms`
    - may include `encoder_cuda_set_device`,
      `preprocess_complete_stream_wait_enqueue`,
      `source_to_helper_copy_sync_wait`,
      `source_to_helper_copy_elapsed_query`,
      `source_to_helper_copy`,
      `pre_encoder_reference_capture_enqueue`,
      `nvenc_get_next_input_frame`,
      `bitstream_fetch`,
      `nvenc_copy_to_input`,
      `nvenc_encode_frame_total`,
      `nvenc_map_input_resource`,
      `nvenc_map_reference_resource`,
      `nvenc_encode_picture`,
      `nvenc_completion_wait`,
      `nvenc_lock_bitstream`,
      `nvenc_bitstream_copy`,
      `nvenc_unlock_bitstream`,
      `nvenc_unmap_input_resource`,
      `nvenc_unmap_reference_resource`,
      `encoder_output_accounting`,
      `shared_submit_total`,
      `shared_submit_lock_wait`,
      `shared_gop_buffering`,
      `gop_hold_before_release`,
      `writer_push_packet_total`,
      `writer_packet_alloc_copy`,
      `writer_queue_push`,
      `writer_queue_wait`,
      `packet_mux_write`,
      and `gop_release_to_last_write`
  - Optional:
    - `pre_encoder_reference_capture: object`
      - `capture_mode: string` (currently `pre_encoder_reference`)
      - `enabled: boolean`
      - `max_frames: integer`
      - `max_seconds: integer`
      - `status: string`
      - `frames_captured: integer`
      - `bytes_written: integer`
      - `budget_reached: boolean`
      - `started_at_utc: string` (optional)
      - `stopped_at_utc: string` (optional)
      - `error: string` (optional)
      - `output_dir: string` (optional)
      - `width|height|pitch|frame_size: integer` (optional when capture opened)
      - `pixel_format: string` (optional, currently `nv12`)
      - `path_type: string` (optional, currently `copy` or `direct_input`)
      - `source_path_flavor: string` (optional, currently `color` or `mono`)
      - `resize_enabled: boolean` (optional)
      - `artifacts.raw_dump|index|metadata: string`

Important:
- Schema-2 snapshots mirror unified output descriptors into
  `encoders[serial].outputs`. Keep using legacy `encoders[serial]` fields for
  detailed full-frame encoder settings.
- Older artifacts may not have `encoders[serial].outputs`, `recording_outputs`,
  or `models`.

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
  - `totals.camera_dropped_frames: integer` (optional legacy name; now counts camera frame-ID gaps only)
  - `totals.camera_frame_id_gaps: integer` (optional alias for true camera frame-ID gaps)
  - `totals.get_frame_errors: integer` (optional SDK `EVT_CameraGetFrame` error count)
  - `totals.last_get_frame_error_code: integer` (optional last nonzero `EVT_CameraGetFrame` error code)
  - `totals.get_frame_errors_by_code: object` (optional map of SDK error code string to count)
  - `totals.display_preview_max_fps: integer` (optional configured display preview cap)
  - `totals.display_preview_eligible_frames: integer` (optional cumulative display-eligible frames)
  - `totals.display_preview_selected_frames: integer` (optional cumulative frames sent to display)
  - `totals.display_preview_skipped_frames: integer` (optional cumulative frames skipped by display cadence)
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
  - `frame_count` (legacy alias for
    `acquisition_frames_received_total`)
  - `frame_count_semantics`
  - `acquisition_frames_received_total`
  - `sync_observed_frames_total`
  - `sync_observed_frame_count_source`
  - `recording_frames_assigned_total`
  - `last_recording_frame_id`
  - `recording_ingress_submitted_frames`
  - `encoded_frames_total` (currently `null`; encoded counts are reported
    by the video/metadata/session artifacts)
  - `encoded_frame_count_source`
  - `encoded_frame_count_authoritative_artifacts`
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
- Its frame population is acquisition/sync-observed frames, not encoded MP4
  frames. Use `recording_session.json` camera `frame_count`,
  `Cam*_meta.csv` data rows, or `Cam*_keyframe.json.total_frames` for
  video-ingest row counts.

## CSV Contracts

### Headless Experiment Results (`runs.json` / `runs.csv`)

Paths:
- `<experiment_root>/runs.json`
- `<experiment_root>/runs.csv`

Current per-camera row fields include:
- `recording_folder: string`
- `importance_map_mode: string`
- `importance_map_roi_size_px: integer`
- `importance_map_enabled: boolean`
- `importance_map_active_mode: string`
- `importance_map_block_size: integer`
- `importance_map_grid_width: integer`
- `importance_map_grid_height: integer`
- `video_present: boolean`
- `video_path: string`
- `video_file_size_bytes: integer`
- `video_duration_s: number`
- `video_achieved_bitrate_bps: integer`
- `video_content_checked: boolean`
- `video_content_valid: boolean`
- `video_content_status: string`
- `video_first_frame_luma_mean: number`
- `video_first_frame_luma_stddev: number`
- `video_first_frame_black_fraction: number`
- `video_first_frame_decoded_bytes: integer`
- `external_ipc_frames_acked_final: integer`
- `external_ipc_failures_final: integer`
- `external_ipc_ack_timeouts_final: integer`
- `external_recorder_contract_mode: string`
- `external_recorder_contract_artifact_root: string`
- `external_recorder_summary_json_path: string`
- `external_recorder_video_sanity_json_path: string`
- `external_recorder_mp4_path: string`
- `external_recorder_gop_routing_csv_path: string`
- `external_recorder_routing_policy: string`
- `external_recorder_expected_shard_count: integer`

Field semantics:
- `importance_map_mode` is the requested headless importance-map mode for the
  run (`off` or `static_roi` today).
- `importance_map_roi_size_px` is the requested centered square ROI size in
  source/output pixels for the synthetic `static_roi` test path. It is ignored
  when the mode is `off`.
- `importance_map_active_mode` is what the encoder snapshot reported after
  initialization; it should match the requested mode for a valid importance-map
  smoke run.
- `importance_map_block_size`, `importance_map_grid_width`, and
  `importance_map_grid_height` describe the codec block grid used for the
  qp-delta map (`32x32` CTBs for current HEVC runs).
- `video_*` fields describe the main full-frame video artifact
  `<recording_folder>/Cam<serial>.mp4`.
- `video_file_size_bytes` is the on-disk MP4 size after the run finalizes.
- `video_duration_s` is the actual container duration reported by `ffprobe`.
- `video_achieved_bitrate_bps` is the actual written file bitrate reported by
  `ffprobe`, or a fallback `size * 8 / duration` calculation when `bit_rate`
  is not present.
- `video_content_*` fields are the decoded-frame sanity check for the main
  full-frame video. The current check decodes the first video frame to luma and
  rejects effectively black or near-zero-variance content.
- `video_content_status` is `pass`, `decode_failed`, `black_frame`,
  `low_luma_stddev`, or `not_checked`.
- `external_ipc_*_final` fields are post-warmup deltas from
  `Cam<serial>_pipeline_perf.csv` when `recording_sink_mode = "external_ipc"`.
  The mode is metrics-only, but nonzero failures/timeouts or fewer ACKed frames
  than submitted frames fail the experiment row.
- `external_recorder_*` fields are expected external artifact paths and routing
  settings copied from `fixed.external_recorder_contract`. They are validated
  after recorder finalization by `scripts/verify_external_recorder_session.py`.
  See `docs/external_recorder_contract.md`.

Important:
- `video_duration_s` includes warmup time in current headless experiment runs,
  because recording starts immediately and warmup is only excluded from scoring.
- `video_achieved_bitrate_bps` is an output metric and can differ from
  requested or resolved encoder bitrate settings.
- For real-recording experiment runs, policy field
  `require_valid_video_content` defaults to `true`. Set it to `false` only for
  explicit dark-frame or synthetic-output diagnostics.

### Main Metadata CSV (`Cam<serial>_meta.csv`)

Header (exact):

```text
frame_id,timestamp,timestamp_sys
```

Field semantics:
- `frame_id`: recording-frame counter (`recording_frame_id`, uint64), not the
  absolute camera frame counter.
- `timestamp`: camera/acquisition timestamp from the SDK (`uint64`). In current
  Orange terminology this is the `camera_timestamp_ns` domain and is distinct
  from any Orange SHM publish timestamp. Current Orange/Emergent deployments
  treat this timestamp domain as nanoseconds.
- `timestamp_sys`: system wall-clock timestamp from `CLOCK_REALTIME` in
  nanoseconds (`uint64`).

### Crop Metadata CSV (`Cam<serial>_crop_meta.csv`)

Header (exact):

```text
recording_frame_id,local_frame_id,camera_frame_id,timestamp,timestamp_sys,has_detection,blank_frame,detection_confidence,crop_x,crop_y,crop_w,crop_h,detection_x,detection_y,detection_w,detection_h
```

Field semantics:
- `recording_frame_id`: recording-frame counter (`uint64`).
- `local_frame_id`: Orange local acquisition frame counter (`uint64`).
- `camera_frame_id`: camera SDK frame id (`uint64`).
- `timestamp`: camera/acquisition timestamp from the SDK (`uint64`), in the
  `camera_timestamp_ns` domain used by Orange docs and diagnostics. Current
  Orange/Emergent deployments treat this timestamp domain as nanoseconds.
- `timestamp_sys`: realtime nanoseconds (`uint64`).
- `has_detection`: integer boolean (`0|1`).
- `blank_frame`: integer boolean (`0|1`), true when the encoded crop frame is
  an explicit no-detection blank frame.
- `detection_confidence`: source detection confidence (`float`, `0` when no
  detection is present).
- `crop_x,crop_y,crop_w,crop_h`: actual crop rectangle in source-frame pixels.
- `detection_x,detection_y,detection_w,detection_h`: selected source detection
  rectangle in source-frame pixels.

Behavior note:
- Blank crop frames may still be encoded when no detection exists.
- Metadata is now intended to contain one row per encoded crop frame so crop
  video frames can be joined against crop metadata without inferring missing
  no-detection frames.

### YOLO Perf CSV (`Cam<serial>_yolo_perf.csv`)

Header (exact order):

```text
frame_id,recording_frame_id,timestamp,timestamp_sys,queue_depth,queue_depth_at_enqueue,queue_depth_at_worker_start,fps,ok,acquisition_to_worker_start_ms,yolo_queue_wait_ms,oldest_frame_age_at_worker_start_ms,oldest_queued_frame_age_at_worker_start_ms,ingress_event_record_to_worker_start_ms,acquisition_to_yolo_input_ready_ms,worker_start_to_yolo_input_ready_ms,acquisition_to_detect_done_ms,worker_start_to_detect_done_ms,service_sequence,camera_service_sequence,active_camera_count,same_camera_service_gap_ms,service_skew_latest_other_ms,service_skew_oldest_other_ms,service_count_skew_vs_min,service_count_skew_range,ingress_event_ready_before_wait,wait_ms,pre_ms,gap_ms,enqueue_ms,infer_ms,sync_ms,completion_event_ready_before_sync,cpu_wait_event_ms,cpu_ingress_event_query_ms,cpu_stream_wait_event_ms,cpu_npp_set_stream_ms,cpu_preprocess_ms,cpu_input_ready_event_record_ms,cpu_dump_ms,cpu_infer_call_ms,cpu_event_record_ms,cpu_pre_sync_ms,cpu_pre_sync_other_ms,cpu_post_sync_ms,queue_ms,post_ms,track_ms,ipc_ms,enet_ms,total_ms
```

Field semantics:
- ID/timestamp fields: integer counters/timestamps.
- `ok`: integer success flag.
- `queue_depth`: YOLO queue depth sampled at log emission, after processing.
- `queue_depth_at_enqueue`: YOLO input queue depth just before acquisition
  enqueued this frame.
- `queue_depth_at_worker_start`: YOLO input queue depth immediately after this
  worker popped the frame for service.
- `yolo_queue_wait_ms`: host time from YOLO enqueue to worker start.
- `oldest_frame_age_at_worker_start_ms`: age of the frame being serviced at
  worker start, measured from acquisition receive.
- `oldest_queued_frame_age_at_worker_start_ms`: age of the next queued frame at
  worker start, or `-1` when no frame is waiting behind the current one.
- `acquisition_to_yolo_input_ready_ms` and
  `worker_start_to_yolo_input_ready_ms`: host timing to the point where YOLO has
  enqueued its input-ready event. These fields are populated when
  `ORANGE_YOLO_DETACH_INPUT=1`, otherwise `-1`.
- `service_*`: cross-camera YOLO service-order instrumentation used to spot
  multi-camera fairness skew.
- `cpu_wait_event_ms`: aggregate host time spent checking and optionally
  enqueuing the ingress event dependency.
- `cpu_ingress_event_query_ms`: host time spent in `cudaEventQuery` for the
  ingress event.
- `cpu_stream_wait_event_ms`: host time spent in `cudaStreamWaitEvent` for the
  ingress event. This should be near zero when
  `ORANGE_YOLO_READY_EVENT_FASTPATH=1` and
  `ingress_event_ready_before_wait=1`.
- `cpu_input_ready_event_record_ms`: host time spent recording the
  YOLO-input-ready CUDA event when `ORANGE_YOLO_DETACH_INPUT=1`.
- `*_ms` and fps fields: floating-point timing/throughput metrics.

Gate:
- Disabled when `ORANGE_YOLO_PERF_LOG=0`.
- Sampling controlled by `ORANGE_YOLO_PERF_SAMPLE`.
- `ORANGE_YOLO_READY_EVENT_FASTPATH=1` skips `cudaStreamWaitEvent` when
  `cudaEventQuery` has already proven the ingress event is complete. It
  preserves the wait path when the event is not ready.
- `ORANGE_YOLO_DETACH_INPUT=1` records an event after YOLO has copied/preprocessed
  the source frame into its owned TensorRT input buffer. With
  `ORANGE_RECORDING_DETECT_PRIORITY=1`, source/primary recording gates on this
  event instead of full detection completion.

Behavior notes:
- The GUI YOLO perf file is opened against the active recording folder.
  Stream-only YOLO prints `[YOLO_TIME]` console rows when `YOLO_PROFILE=1`, but
  does not create `Cam<serial>_yolo_perf.csv` until a recording folder exists.
- Current GUI logging only writes sampled rows when `recording_frame_id > 0`,
  so the CSV should represent recorded frames only.

### YOLO Event JSONL (`Cam<serial>_yolo_events.jsonl`)

Path:
- `<recording_folder>/Cam<serial>_yolo_events.jsonl`

Gate:
- GUI YOLO worker receives frames that carry a non-empty recording folder.
- Or headless experiment spec sets `fixed.yolo_event_log.mode = "synthetic"`.

Current emitted row type:
- `yolo_result`

Current behavior:
- One JSON object per line.
- The file is append-only during recording.
- Rows include frame identity, timestamps, YOLO result status, detections, and
  Citrus live IPC request status.
- Runtime synthetic pose-plumbing detections are explicitly marked under
  `yolo` with `detection_source = "synthetic_center_box"`,
  `synthetic_runtime_detection = true`, and
  `production_detection_valid = false`. Those rows must never be used as
  validation of production detections or real detection-to-pose behavior.
- Current runtime records whether a live IPC update was requested/queued, but
  does not yet emit final asynchronous `citrus_live_ipc_decision` rows.
- Headless synthetic rows are audit-only and do not publish synthetic detection
  updates into the live Citrus shared-memory queue.
- Current GUI logging only writes rows for frames that have
  `frame.recording_frame_id > 0`, so rows should correspond to recorded frames
  rather than post-recording stream tail frames.

See [yolo_event_log_jsonl_contract.md](./yolo_event_log_jsonl_contract.md).

### Pose Event JSONL (`Cam<serial>_pose_events.jsonl`)

Path:
- `<recording_folder>/Cam<serial>_pose_events.jsonl`

Gate:
- Pose is enabled for that camera in the GUI or in a headless
  `fixed.pose_worker.mode = "noop"` or `fixed.pose_worker.mode = "real"`
  experiment spec.
- YOLO is enabled because the current pose worker consumes `CropFrame`
  payloads produced from the YOLO worker result path. With
  `roi_source = "yolo_top_detection"`, the crop comes from a real model
  detection. With `roi_source = "synthetic_center_box"`, the crop comes from a
  non-production synthetic centered box for plumbing diagnostics only. GUI crop
  output may be enabled as a sidecar consumer, but headless pose does not
  require crop-video encoding.
- The crop frame carries `recording_frame_id > 0`.

Current emitted row type:
- `pose_result`

Current behavior:
- One JSON object per accepted pose crop.
- Rows include frame identity, source-frame dimensions, crop geometry, selected
  detection geometry, pose backend/model metadata, per-stage latency fields, and
  an explicit `poses` array.
- Noop pose writes `pose.backend = "noop"`, `pose.status = "no_result"`, and
  `poses = []`. This remains useful for artifact contract validation without
  pretending to have keypoints.
- Real pose writes `pose.backend = "tensorrt"`. Rows can be `poses`,
  `no_result`, or `failed`; the first TensorRT backend supports FP32/NCHW
  `1x3x256x256` input and FP32 YOLO-pose-style output.
- The file is audit-only today. Pose results are not published to Citrus IPC or
  drawn as GUI overlays yet.

Validation note, `2026-04-22` GUI YOLO smoke:

- Artifact folder:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_15_07_45`
- Camera `2010096` produced HEVC video at `4512x4512`, `100 fps`, `23.51 s`,
  `2351` frames.
- `Cam2010096_meta.csv` had `2351` data rows, matching the video.
- `Cam2010096_yolo_events.jsonl` had `2356` rows, consecutive sequence ids,
  and active recording ids `1..2351` with no gaps.
- `Cam2010096_yolo_perf.csv` had `2356` rows, all `ok=1`; `total_ms` mean was
  about `3.51 ms`, p95 about `4.02 ms`, and p99 about `5.05 ms`.
- The run confirmed `models[2010096].detect` in `recording_snapshot.json`
  captured the TensorRT engine path, model id, worker, backend, and GPU id.
- This validation was captured before the strict recorded-frame logging gate;
  the extra `5` rows had `recording_frame_id=0` and should be omitted by the
  current runtime.

Validation note, `2026-04-22` strict recorded-frame GUI YOLO smoke:

- Artifact folder:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_17_41_15`
- Camera `2010096` produced HEVC video at `4512x4512`, `100 fps`, `11.51 s`,
  `1151` frames.
- `Cam2010096_meta.csv`, `Cam2010096_yolo_events.jsonl`, and
  `Cam2010096_yolo_perf.csv` each had `1151` data rows.
- `recording_frame_id` covered `1..1151` with no gaps and no zero ids.
- `Cam2010096_yolo_perf.csv` had all `ok=1`; `total_ms` mean was about
  `3.45 ms`, p95 about `4.06 ms`, and p99 about `4.12 ms`.

Validation note, `2026-04-22` GUI YOLO + crop smoke:

- Artifact folder:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_21_47_28`
- Active camera config:
  `/home/jeremy/orange_data/config/local/100_cam4/2010096.json`
  persisted `crop_pipeline.crop_size_px = 328`.
- `recording_snapshot.json` captured `crop_pipeline.crop_size_px = 328` for
  the runtime camera config.
- Camera `2010096` produced the main HEVC video at `4512x4512`, `100 fps`,
  `4.75 s`, `475` frames.
- `Cam2010096.mp4` and `Cam2010096_meta.csv` both represented `475` recorded
  frames.
- `Cam2010096_crop.mp4` was `328x328`, `100 fps`, `4.75 s`, `475` frames.
- `Cam2010096_crop_meta.csv` had `475` data rows, and the sampled rows used
  `crop_w,crop_h = 328,328`.
- `Cam2010096_yolo_events.jsonl` had `475` rows.
- `Cam2010096_yolo_perf.csv` had `475` data rows.
- This confirms the current full-rate GUI YOLO + crop path can produce aligned
  main video, main metadata, crop video, crop metadata, YOLO audit events, and
  YOLO perf rows for a one-camera recording.

Validation note, `2026-04-22` GUI YOLO + crop observability smoke:

- Artifact folder:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_22_53_43`
- `recording_snapshot.json` included `crop_outputs[2010096]` with
  `enabled=true`, `mode=yolo_centered_square`, `crop_size_px=256`, and expected
  file names for `Cam2010096_crop.mp4`, `Cam2010096_crop_meta.csv`,
  `Cam2010096_crop_keyframe.json`, and `Cam2010096_crop_perf.csv`. Newer
  snapshots also include the effective crop preview cap as
  `runtime.preview_max_fps`.
- `Cam2010096.mp4` and `Cam2010096_meta.csv` both represented `366` recorded
  frames.
- `Cam2010096_crop.mp4` was `256x256` and had `366` frames.
- `Cam2010096_crop_meta.csv`, `Cam2010096_crop_perf.csv`, and
  `Cam2010096_yolo_events.jsonl` each had `366` rows with matching
  `recording_frame_id` sequences.
- `Cam2010096_crop_perf.csv` reported `0` dropped crop frames and max queue
  depth `1`; crop worker `total_ms` mean was about `4.54 ms`, p95 about
  `10.70 ms`, p99 about `14.27 ms`, and max about `17.85 ms`.
- `scripts/validate_recording_artifacts.py` passed with the stricter crop
  snapshot, keyframe-sidecar, crop-perf, and YOLO alignment checks.

Artifact checker behavior:

- Crop artifacts are optional unless crop mode was enabled for that camera.
- When crop artifacts are present, `recording_snapshot.json` should contain
  `crop_outputs[serial]` with enabled state, geometry, and expected artifact
  file names.
- `crop_outputs[serial].runtime.preview_max_fps` is live-preview telemetry
  configuration only and must not be used as the crop-video frame rate.
- `crop_outputs[serial].runtime.files.keyframes` should name the emitted
  `Cam<serial>_crop_keyframe.json` sidecar.
- When crop artifacts are present, `ffprobe` crop video dimensions should match
  the effective `crop_pipeline.crop_size_px` captured in
  `recording_snapshot.json`.
- Crop metadata data rows should match crop video frame count.
- `crop_w,crop_h` in crop metadata should match the configured crop size.
- `Cam<serial>_crop_perf.csv` rows should match crop metadata rows and report
  no drops for the current strict full-rate path.
- For the current full-rate GUI YOLO path, YOLO event rows should match crop
  metadata rows. If YOLO decimation becomes configurable, validation should use
  the configured cadence instead of assuming one YOLO/crop event per recorded
  frame.

Validation tool:

```bash
python3 scripts/validate_recording_artifacts.py /path/to/recording_folder
```

The validator checks main video/metadata alignment, optional crop video
dimensions against `recording_snapshot.json` crop metadata, crop output snapshot
metadata, crop keyframe JSON sidecar presence, crop metadata row count, crop
geometry, crop perf row alignment, and current full-rate YOLO event alignment.
Use `--allow-yolo-decimation` for future intentionally decimated YOLO/crop runs.

For GUI PTP validation runs that specifically need to prove crop recording
stayed active while crop preview was sampled or hidden, use:

```bash
scripts/validate_gui_ptp_recording.py --latest-complete \
  --require-crop-recording-artifacts \
  --require-crop-preview-counters \
  --require-crop-preview-sampling \
  --expect-crop-preview-max-fps 15 \
  --expect-crop-preview-disabled 0 \
  --expect-crop-preview-display-enabled 1 \
  --min-crop-frame-pool-size 32 \
  --expect-gui-stream-downsample 4 \
  --expect-display-preview-max-fps 15 \
  --expect-yolo-speed-graphs-enabled 0 \
  --require-gui-timing-telemetry \
  --min-gui-crop-preview-visible-fps-p05 45
```

Set `--expect-crop-preview-display-enabled 0` for a hidden-preview validation
run. Hidden-preview validation should also use
`--min-gui-crop-preview-hidden-fps-p05`.
`--require-crop-recording-artifacts` validates the crop MP4, metadata, keyframe
`total_frames`, crop perf row alignment, matching `recording_frame_id`
sequences, zero crop-worker drops, and crop/Yolo row count alignment when YOLO
rows are present. For experimental external crop recording, the validator also
checks `recording_outputs[serial].crop.summary` and requires
`frames_received == frames_encoded == crop metadata rows`, with
`external_encode.frames_dropped = 0` and `encode_dropped = 0`.
Use `--expect-external-crop-encode-queue-depth <N>` to confirm the intended
external crop queue depth reached the recorder summary. Use
`--max-external-crop-encode-queue-high-water <N>` and
`--max-external-crop-enqueue-age-p95-ms <ms>` when tuning the diagnostic queue
down from `256` toward `32-64`.
`--require-crop-preview-sampling` is for visible bounded-preview runs; it fails
unless the sidecar shows cadence skips and fewer preview updates than offers.
GUI runs also write `recording_snapshot.json`
`session.gui_display_frame_rate`, with overall, crop-preview-visible, and
crop-preview-hidden ImGui delta-time FPS buckets plus
`stream_downsample`, `display_preview_max_fps`, and
`yolo_speed_graphs_enabled`. The `--min-gui-*fps-p05`,
`--expect-gui-stream-downsample`, and `--expect-display-preview-max-fps`
validator thresholds use those fields to reject UI-refresh collapses and stale
display configuration. Use `--expect-yolo-speed-graphs-enabled 0` to prove the
recording-time ImPlot speed graphs were disabled. Keep
`ORANGE_GUI_SHOW_SPEED_GRAPHS=0` for performance validation unless the run
specifically needs live speed plots.

Newer GUI runs also include `session.gui_display_frame_rate.timings` so slow
GUI refresh can be attributed to texture upload, camera/crop window drawing,
speed-graph drawing, or render/present. Use `--require-gui-timing-telemetry`
when a run is intended to diagnose GUI refresh performance.

### Crop Perf CSV (`Cam<serial>_crop_perf.csv`)

Header (exact order):

```text
recording_frame_id,local_frame_id,camera_frame_id,worker_start_steady_ns,queue_depth_start,encode_active,has_detection,blank_frame,dropped,drop_reason,crop_x,crop_y,crop_w,crop_h,packet_count,encoded_bytes,event_wait_cpu_ms,crop_pool_wait_ms,crop_producer_cpu_ms,crop_source_wait_enqueue_cpu_ms,analytics_owned_wait_cpu_ms,source_stage_enqueue_cpu_ms,crop_copy_start_event_record_cpu_ms,crop_roi_copy_enqueue_cpu_ms,crop_ready_event_record_cpu_ms,source_release_event_record_cpu_ms,crop_copy_gpu_ms,crop_preview_cpu_ms,encode_submit_cpu_ms,metadata_cpu_ms,stream_sync_ms,display_sync_ms,total_ms
```

Field semantics:
- `recording_frame_id`, `local_frame_id`, `camera_frame_id`: recording, local
  acquisition, and SDK frame identities for the crop-recorded frame.
- `worker_start_steady_ns`: host steady-clock timestamp for worker processing
  start. This is for duration ordering within one process, not wall-clock time.
- `queue_depth_start`: crop worker input queue depth immediately after this
  frame was dequeued.
- `encode_active`: whether this row was part of a recording artifact.
- `has_detection`, `blank_frame`: whether YOLO provided at least one detection
  and whether the crop encoder wrote a black placeholder frame.
- `dropped`, `drop_reason`: crop-worker drop diagnostics. Current strict GUI
  validation expects zero dropped crop frames.
- `crop_x,crop_y,crop_w,crop_h`: full-frame pixel crop rectangle used for the
  encoded crop frame. Blank frames use zero geometry.
- `packet_count`, `encoded_bytes`: immediate NVENC packet output returned for
  the frame. In experimental external crop recording mode these can be `0`
  because the external recorder owns packet production and Orange only records
  IPC submit/ACK timing in this CSV.
- `event_wait_cpu_ms`: CPU time spent enqueuing the wait on source-frame GPU
  readiness. This does not mean the GPU wait itself completed.
- `crop_pool_wait_ms`: CPU-side time spent acquiring a crop-owned GPU buffer.
  This should stay near zero; nonzero tails indicate crop consumer pressure.
- `crop_producer_cpu_ms`: CPU-side time to enqueue the source-to-crop GPU copy
  and crop-ready event on the crop producer CUDA stream. The source camera
  buffer is released from that producer stream after this copy is safe, not
  after crop preview or crop-video encoding.
- `crop_source_wait_enqueue_cpu_ms`: CPU-side time to enqueue the wait on the
  source-frame readiness event on the crop producer stream.
- `analytics_owned_wait_cpu_ms`: CPU-side time spent waiting for the
  analytics-owned frame to become available when the crop path uses detached
  analytics input.
- `source_stage_enqueue_cpu_ms`: CPU-side time to stage the full source frame
  into ordinary device memory when `ORANGE_CROP_STAGE_SOURCE=1`. This is `0`
  when staged-source mode is disabled.
- `crop_copy_start_event_record_cpu_ms`: CPU-side time to record the optional
  crop-copy timing start event immediately before the ROI copy submission.
- `crop_roi_copy_enqueue_cpu_ms`: CPU-side time to submit the source ROI
  device-to-device copy into the crop-owned GPU buffer.
- `crop_ready_event_record_cpu_ms`: CPU-side time to record the crop copy
  timing/ready events after the ROI copy submission.
- `source_release_event_record_cpu_ms`: CPU-side time to record the source-safe
  release event after crop production has detached from the original frame. In
  staged-source mode this release is recorded after the stage copy; otherwise
  it is recorded after the ROI copy.
- `crop_copy_gpu_ms`: CUDA event elapsed time for the source ROI copy itself,
  or `-1` when copy timing is disabled or the nonblocking timing query has not
  completed by row write. Set `ORANGE_CROP_COPY_TIMING=0` before launching
  Orange to disable the timing events and test whether they perturb the crop
  producer hot path.
- `ORANGE_CROP_STAGE_SOURCE=1` stages the GPUDirect source frame into ordinary
  device memory before crop extraction so the ROI path can be compared against
  the direct GPUDirect-backed source path.
- `crop_preview_cpu_ms`: legacy in-worker preview timing. New GUI builds move
  live crop preview into `CropPreviewWorker`, so this is expected to remain `0`
  for decoupled-preview runs.
- `encode_submit_cpu_ms`: CPU-side time for NVENC input copy/submission and
  packet handoff. In experimental external crop recording mode this measures
  CUDA stream source synchronization plus descriptor submit/ACK time, not local
  NVENC packet production.
- `metadata_cpu_ms`: CPU time to append the crop metadata row.
- `stream_sync_ms`: fallback synchronization time if the crop source-release
  event pool is exhausted. This should normally be `0`.
- `display_sync_ms`: legacy in-worker GUI preview synchronization time. New
  decoupled-preview runs are expected to leave this at `0`.
- `total_ms`: total crop worker service time for this frame.

### Crop Sidecar Perf CSV (`Cam<serial>_crop_sidecar_perf.csv`)

Header (exact order):

```text
camera_serial,gpu_id,worker,queue_size,producer_jobs_offered,producer_jobs_enqueued,producer_queue_full_drops,producer_blank_jobs_offered,producer_blank_jobs_enqueued,producer_dropped_jobs_offered,producer_dropped_jobs_enqueued,consumer_jobs_enqueued,consumer_queue_full_drops,consumer_queue_high_water,crop_frame_pool_size,producer_recording_crop_frame_offered,producer_recording_crop_frame_accepted,producer_recording_crop_frame_dropped,producer_preview_crop_frame_offered,producer_preview_crop_frame_accepted,producer_preview_crop_frame_dropped,producer_pose_crop_frame_offered,producer_pose_crop_frame_accepted,producer_pose_crop_frame_dropped,producer_frames_produced_total,producer_frames_recycled_total,producer_crop_frame_release_total,producer_crop_frame_pool_misses_total,producer_source_release_event_misses_total,producer_pending_source_releases,producer_pending_crop_frame_recycles,preview_max_fps,preview_disabled,preview_display_enabled_final,preview_frames_offered,preview_frames_updated,preview_frames_skipped_by_cadence,preview_clears_updated,preview_queue_full_drops,preview_queue_high_water,preview_serial_final
```

Preview fields are GUI display telemetry. `preview_frames_skipped_by_cadence`
does not indicate crop recording loss; crop recording drops remain reported by
`Cam<serial>_crop_perf.csv` `dropped` and `drop_reason`.
`preview_disabled` means the preview CUDA/PBO path was unavailable or explicitly
disabled. `preview_display_enabled_final` records whether the GUI crop preview
windows were enabled at summary time.
Newer sidecars include `crop_frame_pool_size` between
`consumer_queue_high_water` and `preview_max_fps`; validators should use this
to confirm the effective crop-frame pool used for a run.
Newer sidecars also include explicit crop-frame fanout counters. The
`producer_recording_crop_frame_*`, `producer_preview_crop_frame_*`, and
`producer_pose_crop_frame_*` fields count owned `CropFrame` leases offered to
each downstream consumer. These are not the same as crop metadata row counts
because no-detection rows can produce blank crop output without allocating a
detected-object crop frame.
For current GUI validation, `producer_recording_crop_frame_accepted` is expected
to match the number of `Cam<serial>_crop_meta.csv` rows with `has_detection=1`,
while total crop metadata rows can be larger because blank/no-detection rows do
not use a crop-frame lease.
`preview_queue_full_drops` and `preview_queue_high_water` describe only the
best-effort live crop preview worker; they do not indicate crop recording loss.

### Pipeline Perf CSV (`Cam<serial>_pipeline_perf.csv`)

Header (exact order):

```text
timestamp_utc,frame_id,recording_frame_id,acq_fps,pre_fps,pre_fps_primary,pre_fps_helpers,enc_fps,enc_fps_primary,enc_fps_helpers,display_q,display_preview_max_fps,display_preview_eligible,display_preview_selected,display_preview_skipped,yolo_q,pre_q,enc_q,acq_free_entries,acq_free_entries_low,acq_free_events,acq_free_events_low,yolo_events,yolo_events_low,pending_requeues,acq_starve,pre_buffers,pre_events,pre_waits,pre_drops,detect_priority_gated_frames,detect_priority_waited_frames,detect_priority_wait_timeouts,detect_priority_wait_total_ns,detect_priority_wait_max_ns,enc_fail,enc_slow,submitted_frames,primary_routed_frames,helper_requested_frames,helper_fallback_frames,helper_dispatched_frames,last_target_gpu_id,last_route_mode,camera_dropped_frames,get_frame_errors,last_get_frame_error_code,gpu_direct,gpu_ring,gpu_copy
```

Field semantics:
- `timestamp_utc`: UTC ISO8601 timestamp for the sample row.
- `frame_id`: absolute camera frame counter at sample time.
- `recording_frame_id`: last seen recording-frame counter for the active recording folder.
- `acq_fps`, `pre_fps`, `enc_fps`: current acquisition, aggregate preprocess, and aggregate encode FPS estimates.
- `pre_fps_primary`, `pre_fps_helpers`, `enc_fps_primary`, `enc_fps_helpers`: split-GOP primary/helper lane throughput estimates.
- `display_q`, `yolo_q`, `pre_q`, `enc_q`: instantaneous queue depths for display, YOLO, preprocess, and HW encode stages.
- `display_preview_max_fps`: configured GUI preview cap for this camera. `0` means every eligible frame may be displayed.
- `display_preview_eligible`: cumulative frames that had display enabled and a display worker available.
- `display_preview_selected`: cumulative display-eligible frames actually offered to the display queue.
- `display_preview_skipped`: cumulative display-eligible frames skipped by preview cadence.
- `acq_free_entries`, `acq_free_entries_low`: available acquire work entries and the interval low-water mark.
- `acq_free_events`, `acq_free_events_low`: available acquire completion events and the interval low-water mark.
- `yolo_events`, `yolo_events_low`: available YOLO completion events and the interval low-water mark.
- `pending_requeues`: camera buffers still waiting for safe requeue after GPU work.
- `acq_starve`: cumulative count of acquisition-loop iterations that could not reserve the required work entry or events before attempting to fetch another camera frame.
- `pre_buffers`, `pre_events`: available preprocess NV12 buffers and CUDA events.
- `pre_waits`, `pre_drops`: cumulative preprocess resource-wait and drop counters.
- `detect_priority_gated_frames`, `detect_priority_waited_frames`, `detect_priority_wait_timeouts`, `detect_priority_wait_total_ns`, `detect_priority_wait_max_ns`: cumulative source-route recording gates and wait timing for `ORANGE_RECORDING_DETECT_PRIORITY=1`; zero when the experiment is disabled or no source-route frame needed to wait.
- `enc_fail`, `enc_slow`: cumulative encode-failure and slow-frame counters.
- `submitted_frames`, `primary_routed_frames`, `helper_requested_frames`, `helper_fallback_frames`, `helper_dispatched_frames`, `last_target_gpu_id`, `last_route_mode`: recording ingress routing counters and latest route state.
- `camera_dropped_frames`: cumulative camera frame-ID gaps.
- `get_frame_errors`, `last_get_frame_error_code`: cumulative SDK receive errors and latest nonzero SDK error code.
- `gpu_direct`, `gpu_ring`, `gpu_copy`: cumulative counts at sample time for direct camera-pointer use, direct-pointer ring-copy fallback, and ordinary device copies.

Behavior notes:
- Emitted at approximately one row per second while the camera has a non-empty recording folder.
- Short recordings may create the file with only the header if no one-second sample boundary is crossed.
- In `ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME=1` runs, `submitted_frames` and route
  counters come from the lightweight `immediate_recycle` sink. Full-frame
  `pre_*` and `enc_*` fields should remain zero or unavailable because no
  full-frame preprocess/encode workers are started.
- In `ORANGE_GUI_RECORDING_SINK_MODE=preprocess_only` runs, full-frame
  preprocess routing counters may be active while full-frame `enc_*` throughput
  remains zero because main full-frame encode/output is intentionally skipped.

### Acquisition Cadence Probe CSV (`Cam<serial>_acquisition_cadence_probe.csv`)

Header (exact order):

```text
timestamp_utc,local_frame_id,recording_frame_id,camera_frame_id,camera_timestamp_ns,camera_timestamp_delta_ns,receive_host_ns,receive_delta_ns,get_frame_wait_ns,ptp_active,latched_ptp_time_ns,latch_minus_frame_ns,latch_delta_ns,record_active,dispatch_count,will_display,display_preview_max_fps,display_preview_eligible,display_preview_selected,display_preview_skipped,will_record,will_yolo,direct,ring_copy,free_entries,free_entries_low,free_events,free_events_low,yolo_events,yolo_events_low,pending_requeues,acq_starve,camera_dropped_frames,get_frame_errors,last_get_frame_error_code,recording_submit_host_ns,receive_to_submit_ns,recording_target_gpu_id,recording_helper_requested,recording_route_helper,helper_enqueue_q,helper_enqueue_buffers,helper_enqueue_events,helper_enqueue_delay_ns,submitted_frames,primary_routed_frames,helper_requested_frames,helper_fallback_frames,helper_dispatched_frames,last_target_gpu_id,last_route_mode,pre_q,enc_q,pre_buffers,pre_events,pre_waits,pre_drops,enc_fail,enc_slow
```

Field semantics:
- `local_frame_id`, `recording_frame_id`, `camera_frame_id`: acquisition-thread, recording, and SDK frame counters.
- `camera_timestamp_ns`, `camera_timestamp_delta_ns`, `receive_host_ns`, `receive_delta_ns`, `get_frame_wait_ns`: per-frame receive timing diagnostics.
- `ptp_active`, `latched_ptp_time_ns`, `latch_minus_frame_ns`, `latch_delta_ns`: PTP timing diagnostics; zeroed when PTP is not active.
- `record_active`, `dispatch_count`, `will_display`, `will_record`, `will_yolo`, `direct`, `ring_copy`: per-frame routing/lifetime decisions.
- `display_preview_*`: same display cadence counters as `Cam<serial>_pipeline_perf.csv`, sampled on the current probe row.
- Remaining resource, error, and routing fields mirror the pipeline perf counters at the frame where the probe row is emitted.

Behavior notes:
- Emitted only for probe frames `80-160` using `recording_frame_id` when present, otherwise `local_frame_id`.
- This artifact is intended for short-window fanout and timing debugging, not full-run per-frame logging.

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
- `timestamp_us_epoch: uint64` (current ABI field name; semantically
  `orange_shm_publish_timestamp_us_epoch`, producer wall-clock
  `system_clock` microseconds at SHM enqueue/publish)
- `timestamp_us_monotonic: uint64` (current ABI field name; semantically
  `orange_shm_publish_timestamp_us_monotonic`, producer local
  `steady_clock` microseconds at SHM enqueue/publish)
- `frame_id: uint64`
- `camera_id: uint32` (camera index, not serial)
- `yolo_enabled: bool`

Emission behavior:
- Base frame event may be emitted with empty detections.
- Detection update may be emitted when YOLO produces non-empty detections.
- Detection updates older than the latest emitted base frame may be suppressed
  from the live queue and counted as stale update drops.
- During recording, IPC `frame_id` uses `recording_frame_id`.
- Outside recording, IPC `frame_id` uses absolute camera `frame_id`.
- The current queue does not carry `camera_timestamp_ns`; the SHM timestamp
  fields above are Orange publish-time timestamps, not original frame
  acquisition timestamps.
- The current Citrus consumer treats this queue as latest live-control state,
  not as a complete Orange semantic event log. Do not rely on this queue to
  preserve every base frame, delayed YOLO update, or zero-detection completion.
  See [yolo_ipc_citrus_contract_plan.md](./yolo_ipc_citrus_contract_plan.md).

### Headless `frame_ipc_summary.json`

Headless local runs emit this file only when explicit frame IPC is enabled and a
run folder exists.

Top-level fields:
- `schema_id = "orange.headless.frame_ipc_summary"`
- `schema_version = 1`
- `mode = "producer_only" | "verify_drain" | "verify_drain_v2"`
- `queue_version = 1 | 2`
- `queue_naming = "serial"`
- `require_v2_pose_results`: true when the headless run had pose enabled and
  the v2 verifier should fail if no pose-result states are drained
- `cameras`: object keyed by camera serial

Per-camera fields include:
- `queue_name`: active verifier queue, either `/shm_cam_<camera_serial>` or
  `/shm_cam_<camera_serial>_v2`
- `v1_queue_name`, `v2_queue_name`
- `frames_sent`, `updates_sent`: active queue publish counts for the selected
  mode
- `v1_frames_sent`, `v1_updates_sent`
- `base_queue_drops`, `update_queue_drops`, `pose_update_queue_drops`,
  `update_stale_drops`
- `update_stale_drops` means delayed older-frame detection updates were
  intentionally suppressed from the Citrus live queue
- `ipc_push_failures`: active queue push failures for the selected mode
- `v1_ipc_push_failures`, `v2_ipc_push_failures`
- `v2_frames_published`, `v2_yolo_updates_published`,
  `v2_pose_updates_published`
- `v2_yolo_stale_suppressed`, `v2_pose_stale_suppressed`
- `v2_pending_drops`, `v2_queue_drops`
- `reader_messages_popped`, `reader_base_messages`
- `reader_v2_latest_state_messages`, `reader_v2_detection_pending_messages`,
  `reader_v2_detection_result_messages`, `reader_v2_pose_result_messages`
- `reader_frame_id_gaps`, `reader_camera_id_mismatches`
- `reader_sequence_id_gaps`, `reader_non_monotonic_sequence_ids`
- `status = "pass" | "fail"`

`verify_drain` and `verify_drain_v2` start internal single-consumer readers, so
they are test modes and should not be used while another consumer is expected to
receive every slot.

In `verify_drain_v2`, the transitional v1 queue is still created but the
internal verifier drains only the v2 queue. Use `ipc_push_failures` or
`v2_ipc_push_failures` for active-mode validation; `v1_ipc_push_failures` may
rise when no v1 consumer is attached.

When pose is enabled, `verify_drain_v2` requires at least one drained v2 pose
state. Pose-result states may be fewer than pose event JSONL rows because late
older-frame pose results are stale for live Citrus control and are deliberately
suppressed from the v2 queue.

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
- Main/crop metadata `timestamp` is the camera/acquisition timestamp
  (`camera_timestamp_ns` terminology in current Orange docs/diagnostics). In the
  current Orange/Emergent path this is treated as nanoseconds.
- `timestamp_sys` in metadata rows is Orange realtime nanoseconds captured at
  frame receive.
- IPC `timestamp_us_epoch` is the current ABI field name for
  `orange_shm_publish_timestamp_us_epoch`.
- IPC `timestamp_us_monotonic` is the current ABI field name for
  `orange_shm_publish_timestamp_us_monotonic`.
- Neither IPC timestamp field is the original camera/acquisition timestamp.
- Citrus consumer-side timestamps should be described separately as
  `citrus_ipc_receive_timestamp_*` and `citrus_stimulus_output_timestamp_*`.
- If Citrus later needs live `camera_timestamp_ns`, keep the current queue
  unchanged and add a versioned `/shm_cam_<camera_serial>_v2` contract with
  explicit schema/version fields and the same stale-update suppression rules.
  See [shaman_v2_live_state_contract.md](./shaman_v2_live_state_contract.md).

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
