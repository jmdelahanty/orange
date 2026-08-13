# Orange Output Artifacts Contract (Current Runtime)

Purpose: define the concrete output contracts currently produced by Orange so
downstream analysis consumers can parse artifacts without guessing.

A possible future finalization-time consolidation is documented in
[unified_recording_metadata_dataset_option.md](./unified_recording_metadata_dataset_option.md).
That design is explicitly deferred until multi-arena-per-camera geometry and
one-to-many detection/crop/pose relationships are stable library contracts; it
does not describe current runtime output.

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
| Recording session manifest | `<recording_folder>/recording_session.json` | Required for current GUI/headless session finalization | Recording finalization |
| PTP sync summary | `<recording_folder>/ptp_sync_summary.json` | Required | Recording started |
| Local-control event log | `<recording_folder>/orange_local_control.events.jsonl` | Optional | Local-control/orchestrated GUI recording |
| Latest pointer (local) | `<base_folder>/.orange/latest_recording.json` | Required | Recording started |
| Latest pointer (canonical) | `<canonical_pointer_root>/latest_recording.json` | Required when configured | Recording started |
| Latest pointer (shared) | `/run/orange/latest_recording.json` | Required (best-effort write) | Recording started |
| Main video | `<recording_folder>/Cam<serial>.mp4` | Typical | Per-camera HW encoding active |
| Main video finalization | `<video-path>.finalization.json` | Typical for newly written MP4s | MP4 writer opened; terminal at clean close |
| Main metadata CSV | `<recording_folder>/Cam<serial>_meta.csv` | Typical | Per-camera HW encoding active |
| Main keyframe sidecar | `<recording_folder>/Cam<serial>_keyframe.json` | Typical | Per-camera HW encoding active |
| Pipeline perf CSV | `<recording_folder>/Cam<serial>_pipeline_perf.csv` | Optional | Per-camera recording folder active |
| Acquisition cadence probe CSV | `<recording_folder>/Cam<serial>_acquisition_cadence_probe.csv` | Optional | Per-camera recording folder active; frames `80-160` only |
| Pre-encoder reference raw dump | `<recording_folder>/Cam<serial>_preenc_ref.bin` | Optional | `pre_encoder_reference_capture.enabled = true` |
| Pre-encoder reference index | `<recording_folder>/Cam<serial>_preenc_ref_index.csv` | Optional | `pre_encoder_reference_capture.enabled = true` |
| Pre-encoder reference metadata | `<recording_folder>/Cam<serial>_preenc_ref.json` | Optional | `pre_encoder_reference_capture.enabled = true` |
| GPU dmon output | `<recording_folder>/nvidia_smi_dmon.csv` | Optional | Headless recording session with best-effort GPU monitoring |
| GPU dmon stderr log | `<recording_folder>/nvidia_smi_dmon.stderr.log` | Optional | Headless recording session with best-effort GPU monitoring |
| NIC thermal samples | `<recording_folder>/nic_thermal_monitor.csv` | Optional | GUI or headless recording with the recording-scoped NIC thermal helper enabled |
| NIC thermal summary | `<recording_folder>/nic_thermal_monitor_summary.json` | Optional | Same; atomically refreshed while running and terminal at helper shutdown |
| NIC thermal stderr log | `<recording_folder>/nic_thermal_monitor.stderr.log` | Optional | Same; helper startup/runtime diagnostics |
| Crop video | `<recording_folder>/Cam<serial>_crop.mp4` | Optional | Crop-and-encode active |
| Crop video finalization | `<crop-video-path>.finalization.json` | Optional for newly written crop MP4s | Crop MP4 writer opened; terminal at clean close |
| Crop metadata CSV | `<recording_folder>/Cam<serial>_crop_meta.csv` | Optional | Crop-and-encode active |
| Crop keyframe sidecar | `<recording_folder>/Cam<serial>_crop_keyframe.json` | Optional | Crop-and-encode active |
| Crop perf CSV | `<recording_folder>/Cam<serial>_crop_perf.csv` | Optional | Crop-and-encode active |
| Crop sidecar perf CSV | `<recording_folder>/Cam<serial>_crop_sidecar_perf.csv` | Optional | Crop pipeline active |
| YOLO perf CSV | `<recording_folder>/Cam<serial>_yolo_perf.csv` | Optional | `ORANGE_YOLO_PERF_LOG != 0` |
| YOLO event JSONL | `<recording_folder>/Cam<serial>_yolo_events.jsonl` | Optional | GUI YOLO worker receives frames during recording |
| Pose perf CSV | `<recording_folder>/Cam<serial>_pose_perf.csv` | Optional | GUI pose worker active |
| Pose event JSONL | `<recording_folder>/Cam<serial>_pose_events.jsonl` | Optional | GUI pose worker receives crop frames during recording |
| YOLO debug PNG | `./debug_pre_yolo_<serial>_<frame_id>.png` | Optional | `Dump Input` action |
| Full external recorder contract | `<recording_folder>/external_recorder_contract.json` | Optional | Full-frame `recording_sink_mode = external_ipc` |
| Full external recorder supervisor plan | `<recording_folder>/external_recorder_supervisor_plan.json` | Optional | Supervised full-frame external IPC |
| Full external recorder artifact root | `<recording_folder>/external_recorder/` | Optional | Supervised full-frame external IPC |
| Full external single-clip frame metadata | `<recording_folder>/external_recorder/Cam<serial>_external_meta.csv` | Required for completed external IPC single clips | One row per encoded video frame; carries `recording_frame_id`, camera `timestamp`, and host `timestamp_sys` |
| Crop external recorder contract | `<recording_folder>/external_crop_recorder_contract.json` | Optional | Crop `recording_sink_mode = external_ipc` |
| Crop external recorder supervisor plan | `<recording_folder>/external_crop_recorder_supervisor_plan.json` | Optional | Supervised crop external IPC |
| Crop external recorder artifact root | `<recording_folder>/external_crop_recorder/` | Optional | Supervised crop external IPC |

## Recording Payload Summary

A recording payload has four classes of files:

1. Session manifests and provenance.
   These describe what was attempted, what finished, and where the authoritative
   media lives. Consumers should start with `recording_session.json` and
   `recording_snapshot.json`, then follow `recording_outputs`.
2. Durable media.
   These are the videos consumers should load for analysis. In in-process
   full-frame mode this is root `Cam<serial>.mp4`; in continuous full-frame
   external IPC mode it is `external_recorder/Cam<serial>_external.mp4`; and in
   rolling external IPC mode it is the ordered clip set referenced by
   `recording_session.json` and `recording_clip_index.json`. Crop videos are
   sidecar media and may be root `Cam<serial>_crop.mp4` or external
   `external_crop_recorder/Cam<serial>_crop_external.mp4`.
   A continuous external IPC full-frame video is paired with
   `external_recorder/Cam<serial>_external_meta.csv`; consumers should follow
   the manifest path rather than infer a root-level `Cam*_meta.csv` name.
3. Telemetry and sidecars.
   CSV/JSON/JSONL files record timing, routing, keyframes, queue pressure,
   event rows, local-control state, PTP state, and validation summaries. These
   are durable diagnostics but are not alternate copies of the recorded video.
4. Temporary/opt-in diagnostics.
   Full-frame split-GOP shard MP4s are not part of the durable payload by
   default and their writers are not opened. The recorder creates
   `Cam<serial>_external_shard*_gpu*.mp4` only when
   `preserve_shard_mp4s = true` explicitly requests diagnostic copies.

For the current production-like GUI shape with both full-frame external IPC and
external crop recording enabled, the high-level payload looks like:

```text
<recording_folder>/
  recording_snapshot.json
  recording_session.json
  ptp_sync_summary.json
  orange_local_control.events.jsonl

  external_recorder_contract.json
  external_recorder_supervisor_plan.json
  external_crop_recorder_contract.json
  external_crop_recorder_supervisor_plan.json

  Cam<serial>_pipeline_perf.csv
  Cam<serial>_acquisition_cadence_probe.csv
  Cam<serial>_yolo_perf.csv
  Cam<serial>_yolo_events.jsonl
  Cam<serial>_crop_meta.csv
  Cam<serial>_crop_perf.csv
  Cam<serial>_crop_sidecar_perf.csv

  external_recorder/
    Cam<serial>_external.mp4                 # durable continuous-mode media
    Cam<serial>_external_keyframes.json
    Cam<serial>_external_summary.json
    Cam<serial>_external_status.json
    Cam<serial>_external_gop_routing.csv
    Cam<serial>_external_detach.csv
    Cam<serial>_external_encode_shard*_gpu*.csv
    Cam<serial>_external_recorder.log
    clips/clip_<index>/                      # authoritative rolling-mode media
    Cam<serial>_external_shard*_gpu*.mp4     # explicit diagnostic opt-in only
    external_recorder_session.json
    external_recorder_supervisor_runtime.json
    external_recorder_verifier_handoff.json
    external_recorder_finalization.json

  external_crop_recorder/
    Cam<serial>_crop_external.mp4            # durable crop sidecar media
    Cam<serial>_crop_external_keyframe.json
    Cam<serial>_crop_external_summary.json
    Cam<serial>_crop_external_status.json
    Cam<serial>_crop_external_gop_routing.csv
    Cam<serial>_crop_external_detach.csv
    Cam<serial>_crop_external_encode.csv
    Cam<serial>_crop_external_recorder.log
    external_recorder_session.json
    external_recorder_supervisor_runtime.json
    external_recorder_verifier_handoff.json
    external_recorder_finalization.json
```

`<serial>` entries are per selected/recorded camera. In external full-frame IPC
mode, consumers should not require root-level `Cam<serial>.mp4`; the
authoritative full-frame video is the path recorded in
`recording_outputs[serial].full.video` and
`recording_session.json camera_artifacts[serial].video`.

With the current default, `external_encode_shards[].mp4` is empty and
`external_encode_shards[].mp4_retention.status` is
`not_applicable_no_mp4_path`. Older summaries or explicit diagnostic runs can
contain the legacy retention states:

- `deleted_after_merged_finalization` or
  `already_absent_after_merged_finalization`: expected default for a clean
  merged run.
- `preserved_by_request`: the run explicitly retained shard MP4s.
- `preserved_merged_incomplete`, `preserved_shard_incomplete`, or
  `preserved_descriptor_intake_incomplete`: retained for recovery/debugging.
- `delete_failed`: cleanup was attempted but failed; treat as a diagnostic
  failure or storage-cleanup issue.

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
  external crop recorder encode queue depth. The default is currently `64`.
  At `100 fps`, a 64-deep queue permits about `0.64 s` of crop encode backlog
  per camera. Use `256` only as a diagnostic shock absorber; it permits about
  `2.56 s` of backlog per camera and can hide latency that should be visible
  in production validation.
- `ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID=<N>` overrides the external crop
  recorder GPU for every crop stream. Per-camera overrides such as
  `ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095=<N>` take precedence.
  Defaults keep each crop recorder on that camera's analytics/source GPU.
  Use these only for placement diagnostics or deliberate NVENC load routing,
  and confirm the resulting `analytics_gpu_id -> recorder_gpu_id` mapping in
  validation JSON before comparing runs. The GUI validation launcher rejects
  non-integer or negative placement values before starting Orange.
- `ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=1` is a placement diagnostic guard.
  The GUI validation launcher preflights the local camera config before Orange
  starts, and the GUI recording-session runtime also refuses to start
  supervised external crop recorders when any crop stream resolves
  `recorder_gpu_id == analytics_gpu_id`. That condition means the crop recorder
  is still on the same CUDA device as crop production. It does not infer
  physical encoder topology across different GPU ids.
- `ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER=<N>` and
  `ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS=<ms>` are launcher validation
  helpers. They do not change runtime behavior; `scripts/run_gui_aq_off_validation.sh`
  uses them only to print matching `validate_gui_ptp_recording.py` queue-pressure
  gates for external crop recorder runs.

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
- `producer_version: string` (short git commit when available, otherwise
  `"unknown"`)
- `source_version: object` (schema-1 git provenance captured at recording
  start; includes `worktree`, `branch`, `commit`, `commit_short`, `describe`,
  `git_command_user`, `dirty_tracked_available`, `dirty_tracked`, and
  `status_porcelain_tracked` when the source worktree can be resolved; sudo/root
  GUI runs execute git in a child dropped to `SUDO_UID`/`SUDO_GID`)
- `session: object` (optional in older artifacts)
- `recording_outputs: object` (schema-2 output descriptors keyed by camera
  serial, then output kind)
- `sync: object` (session-level synchronization provenance)
- `cameras: object`
- `camera_runtime: object` (resolved per-recording camera config and raster
  coordinate-frame contract keyed by camera id/serial)
- `gpu_inventory: object` (runtime GPU metadata keyed by GPU id string)
- `gpu_monitoring: object` (optional host-level GPU monitor sidecars keyed by monitor name)
- `system_monitoring: object` (optional recording-scoped host hardware monitors keyed by monitor name)
- `encoders: object` (added later by encoder worker updates)
- `pipeline_metrics: object` (optional, added when acquisition worker finalizes per-camera pipeline summaries)

`session` object:
- `recording_sink_mode: string` (`real`, `immediate_recycle`,
  `preprocess_only`, `threaded_handoff_only`, or `external_ipc`)
- `full_frame_video_enabled: boolean`
- `system_cpu: object` (optional in older artifacts)
  - `schema_version: integer`
  - `isolated_cpus.source: string` (currently
    `/sys/devices/system/cpu/isolated`)
  - `isolated_cpus.available: boolean`
  - `isolated_cpus.raw: string`
  - `isolated_cpus.cpus: integer[]`
  - `isolated_cpus.parse_ok: boolean`
  - `kernel_cmdline.source: string` (currently `/proc/cmdline`)
  - `kernel_cmdline.available: boolean`
  - `kernel_cmdline.raw: string`
  - `kernel_cmdline.options.isolcpus|nohz_full|rcu_nocbs: string` when present
- `system_cpu_kernel_cmdline_cpu_option_values: string[]` in
  `scripts/validate_gui_ptp_recording.py --json-out` and
  `scripts/summarize_gui_validation.py --json` output only; this is a derived,
  normalized view of the recorded boot CPU-list options for human inspection
  and comparison debugging, not a persisted Orange snapshot field
- `yolo_worker: object` (optional in older artifacts)
  - `schema_version: integer`
  - `affinity.source: string` (currently `environment`)
  - `affinity.per_camera[serial].configured: boolean`
  - `affinity.per_camera[serial].source: string` (`per_camera_environment`,
    `global_environment`, or `none`)
  - `affinity.per_camera[serial].env_key: string|null`
  - `affinity.per_camera[serial].requested_cpus: string|null`
- `gui_display_frame_rate: object` (optional, GUI recordings after
  finalization)
  - `schema_version: integer`
  - `source: string` (currently `imgui_io_delta_time`)
  - `stream_downsample: integer` (GUI display preview downsample)
  - `display_preview_max_fps: integer` (main display preview cadence cap)
  - `swap_interval: integer` (GLFW swap interval; `0` means vsync disabled)
  - `frame_max_fps: integer` (GUI loop frame cap; `0` means uncapped)
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
  - `imgui_glfw_size_cache: object` (optional, GUI recordings after the
    ImGui GLFW size-cache shim)
    - `schema_version: integer`
    - `source: string`
    - `cache_context_registered: boolean`
    - `window_size_cache_hits`, `window_size_fallbacks`,
      `framebuffer_size_cache_hits`, `framebuffer_size_fallbacks`,
      `null_window_requests`, and `total_size_requests`
  - `timing_diagnosis` is not persisted in `recording_snapshot.json`. It is a
    derived field produced by `scripts/validate_gui_ptp_recording.py --json-out`
    and `scripts/summarize_gui_validation.py` from the timing buckets. It names
    the largest non-total p95 timing bucket and reports its share of
    `frame_total_ms.p95_ms`.

`recording_outputs` object:
- Key: camera identifier string (serial or camera_id string).
- Value: object keyed by output kind:
  - `full`: ingest-authoritative full-frame output descriptor.
  - `crop`: optional runtime-derived acquisition-media stream descriptor.
- Each descriptor carries:
  - `schema_version: integer`
  - `camera_serial: string`
  - `output_kind: string`
  - `role: string`:
    - full output: `ingest_authoritative`
    - current crop output: `runtime_derived_acquisition_input`
    - historical crop schema v1: `sidecar`
  - `backend: string` (`in_process`, `external_ipc`, or diagnostic sink mode)
  - `status: string` (`pending`, `completed`, `incomplete`, or `disabled`)
  - Optional artifact paths: `video`, `metadata`, `keyframes`, `perf`,
    `sidecar_perf`, `summary`
  - Optional counts: `frame_count`, `first_recording_frame_id`,
    `last_recording_frame_id`, `recording_frame_id_gaps`, `packet_count`,
    `packet_count_source`
  - Optional media details: `width`, `height`, `frame_rate`, `codec`,
    `container`, `tuning`, `pixel_source_format`, `encoded_format`,
    `coordinate_space`, `video_pixel_coordinate_space`,
    `source_geometry_coordinate_space`
  - Optional `details` object for backend-specific metadata. For GUI external
    crop outputs, `details` includes static recorder routing/config fields such
    as `stream_id`, `stream_kind`, `output_kind`, `camera_serial`, `env_key`,
    `analytics_gpu_id`, `recorder_gpu_id`, `encode_queue_depth`, `socket_path`,
    and `summary_json`.

Current crop descriptors use descriptor `schema_version = 2` and distinguish:

- `video_pixel_coordinate_space = "crop_frame_pixels"`: coordinates within
  the encoded crop-video raster; and
- `source_geometry_coordinate_space = "full_frame_pixels"`: the frame in
  which `crop_x/y/w/h`, detection geometry, and crop placement are expressed.

The legacy `coordinate_space = "full_frame_pixels"` field is retained as a
deprecated compatibility alias for source/placement geometry. It must not be
used to interpret crop-video pixel coordinates. Historical schema-v1 crop
descriptors with `role = "sidecar"` remain inspectable without rewriting their
artifact bytes.

For GUI external crop recording, `recording_session.json`
`recording_backend.crop_recording` also carries per-camera maps keyed by serial:

- `stream_config`: static recorder config copied from the supervised recorder
  plan, including `stream_id`, `stream_kind = "crop"`,
  `output_kind = "crop"`, real `camera_serial`, crop-suffixed `env_key`,
  `analytics_gpu_id`, `recorder_gpu_id`, `encode_queue_depth`, socket path, FPS,
  GOP, codec, and tuning. The `analytics_gpu_id` is the source/crop-production
  GPU; `recorder_gpu_id` is the external process encode GPU. External crop
  recorder contracts also carry `same_gpu_as_analytics` for this same-GPU
  placement check.
- `frames_received`, `frames_encoded`, `encode_dropped`,
  `external_frames_dropped`: count telemetry copied from each external crop
  recorder summary.
- `encode_queue_depth`, `encode_queue_high_water`, `enqueue_age_p95_ms`:
  queue-pressure telemetry copied from each external crop recorder summary.

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
  - `coordinate_frame: object` (schema-1 camera raster contract independent of
    spatial calibration)
  - `runtime: object` (full resolved camera config JSON actually used for the run)

`camera_runtime[serial].coordinate_frame` object:
- `schema_version: 1`
- `coordinate_space: "camera_native_pixels"`
- `units: "pixels"`
- `origin.name: "top_left_pixel"`
- `axes.x.positive_direction: "right"`
- `axes.y.positive_direction: "down"`
- `point_order: "xy"`
- `pixel_indexing.index_base: 0`
- `pixel_indexing.valid_x_index_max` and `valid_y_index_max`: largest valid
  integer pixel indices for the resolved camera dimensions.
- `extent`: half-open pixel extent with `width_px`, `height_px`,
  `x_min_px = 0`, `y_min_px = 0`, `x_max_exclusive_px = width_px`, and
  `y_max_exclusive_px = height_px`.
- `image_shape`: `{ "height": height_px, "width": width_px }`.
- `orientation_reference: "orange_live_stream"`.

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

`system_monitoring.nic_thermal` is emitted by GUI and headless recording
lifecycles. Its process record identifies the helper executable, CSV, summary,
stderr log, sampling period, start/stop times, and terminal exit status. The
helper reads Linux hwmon sysfs directly in its own process; Orange never runs
sensor reads on the GUI, acquisition, inference, or encoder threads. The child
also requests a Linux parent-death signal, so an abnormal Orange exit does not
leave an unbounded orphan monitor.

The CSV contains one row per discovered mlx5 temperature sensor per sampling
batch, with raw millidegree Celsius input/maximum/critical/highest values plus
PCI BDF and network-interface identity. A missing mlx5 hwmon device produces a
`no_mlx5_hwmon` sentinel row. An mlx5 `temp*_input` value less than or equal to
zero is invalid and is recorded with `status = unavailable_zero`; it must not
be interpreted as a measured 0 degrees Celsius. The summary uses schema
`orange.nic_thermal_monitor_summary` version 1 and aggregates valid minima,
maxima, warnings, critical samples, invalid reasons, and sensor-set changes.
Monitoring is observational in this first slice: failure is preserved in the
artifacts but does not reject or stop a recording.

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
  - `recording_camera_minus_realtime_ns.{samples,min,max,last,mean}`
    (paired `timestamp - timestamp_sys` values for frames assigned a
    `recording_frame_id`)
  - `recording_timestamp_pair_source.{camera,host,population}`
  - `ptp_mode_readback.{samples,first,last,changes}`
  - `ptp_status_readback.{samples,first,last,changes}`
  - `ptp_readback_observations[]` with compact low-rate mode/status state runs,
    sample counts, and first/last UTC, local-frame, and recording-frame bounds
  - `delta_samples`
  - `avg_frame_delta_ns_running`
  - `avg_latch_delta_ns_running`

Contract notes:
- This sidecar is updated during acquisition and finalized when the active
  recording folder lifetime ends for that camera thread.
- It is primarily a summarized diagnostic artifact, not a per-frame timing
  trace. `ptp_readback_observations[]` is intentionally low-rate, run-compacted
  control-plane evidence; Orange does not add per-frame PTP register reads to
  the hot path.
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
  from any Orange SHM publish timestamp. Orange copies
  `Emergent::CEmergentFrame::timestamp` without conversion. Current
  Orange/Emergent deployments treat this timestamp domain as nanoseconds. When
  the camera is PTP-synchronized, runtime comparison with the camera's latched
  PTP clock shows that the field is in the camera PTP clock domain. Its observed
  37-second lead over `CLOCK_REALTIME` is consistent with IEEE-1588/TAI
  nanoseconds from `1970-01-01T00:00:00 TAI`; that epoch/timescale is currently
  inferred from runtime behavior and the PTP standard rather than declared by
  the installed Emergent SDK header. When PTP is disabled or unlocked, do not
  assume this field is epoch-based TAI time.
- `timestamp_sys`: system wall-clock timestamp from `CLOCK_REALTIME` in
  nanoseconds (`uint64`), captured by Orange immediately after
  `EVT_CameraGetFrame` returns. It is POSIX epoch time beginning
  `1970-01-01T00:00:00 UTC`; it is not a monotonic or recording-relative
  timestamp and, like `CLOCK_REALTIME`, can follow host clock adjustments.

### Crop Metadata CSV (`Cam<serial>_crop_meta.csv`)

Header (exact):

```text
recording_frame_id,local_frame_id,camera_frame_id,timestamp,timestamp_sys,has_detection,blank_frame,detection_confidence,crop_x,crop_y,crop_w,crop_h,detection_x,detection_y,detection_w,detection_h,crop_video_frame_index,crop_state,crop_rect_valid,crop_rect_coordinate_space,crop_rect_layout,crop_rect_semantics,detection_rect_valid,detection_rect_coordinate_space,detection_rect_layout,detection_rect_semantics,detection_source,selection_policy,session_crop_video_frame_index
```

Field semantics:
- `recording_frame_id`: recording-frame counter (`uint64`).
- `local_frame_id`: Orange local acquisition frame counter (`uint64`).
- `camera_frame_id`: camera SDK frame id (`uint64`).
- `timestamp`: camera/acquisition timestamp from the SDK (`uint64`), in the
  `camera_timestamp_ns` domain used by Orange docs and diagnostics. Current
  Orange/Emergent deployments treat this timestamp domain as nanoseconds. The
  PTP epoch/timescale interpretation is conditional and has the provenance
  qualification documented for the main metadata CSV above.
- `timestamp_sys`: POSIX epoch nanoseconds from `CLOCK_REALTIME` (`uint64`),
  with the capture semantics documented for the main metadata CSV above.
- `has_detection`: integer boolean (`0|1`).
- `blank_frame`: integer boolean (`0|1`), true when the encoded crop frame is
  an explicit no-detection blank frame.
- `detection_confidence`: source detection confidence (`float`, `0` when no
  detection is present).
- `crop_x,crop_y,crop_w,crop_h`: actual crop rectangle in source-frame pixels.
- `detection_x,detection_y,detection_w,detection_h`: selected source detection
  rectangle in source-frame pixels.
- `crop_video_frame_index`: zero-based row/video-frame index within this CSV.
  Session-aggregate crop CSVs and per-clip rolling crop CSVs each start at `0`.
- `session_crop_video_frame_index`: zero-based session-global monotonic crop
  row/video-frame index, written at acquisition time. In the session-aggregate
  (root) crop CSV it equals `crop_video_frame_index`. When finalization splits
  the root crop CSV into per-clip rolling crop CSVs, this column is preserved
  verbatim while `crop_video_frame_index` is rewritten per clip from `0`, so
  the two values diverge in per-clip CSVs. This column is intentionally last
  so older consumers that address the original columns are unaffected.
- `crop_state`: `detected_crop` for source ROI crop frames, or
  `blank_no_detection` for explicit blank no-detection frames.
- `crop_rect_valid`: integer boolean (`0|1`), true when `crop_*` names an
  actual source-frame ROI. Blank no-detection rows set this to `0`.
- `crop_rect_coordinate_space`: currently `full_frame_pixels`.
- `crop_rect_layout`: currently `xywh_top_left`.
- `crop_rect_semantics`: currently `actual_clamped_source_roi`.
- `detection_rect_valid`: integer boolean (`0|1`), true when `detection_*`
  names the selected live detection bbox. Blank no-detection rows set this to
  `0`.
- `detection_rect_coordinate_space`: currently `full_frame_pixels`.
- `detection_rect_layout`: currently `xywh_top_left`.
- `detection_rect_semantics`: currently
  `selected_postprocessed_model_detection`.
- `detection_source`: `model` for normal detection rows and `none` for blank
  no-detection rows. For full detection provenance, including synthetic
  diagnostics, use `Cam<serial>_yolo_events.jsonl`.
- `selection_policy`: currently `largest_detection_by_confidence`.

Behavior note:
- Blank crop frames may still be encoded when no detection exists.
- Metadata is now intended to contain one row per encoded crop frame so crop
  video frames can be joined against crop metadata without inferring missing
  no-detection frames.
- The crop CSV is self-describing by header shape. The original columns remain
  first for compatibility; the appended fields define the coordinate and
  semantic contract for crop and detection rectangles.
- Join recipe (per-clip row to session-global row): join on
  `recording_frame_id`, which is unique across the session and preserved by
  splitting; or, in CSVs that carry `session_crop_video_frame_index`, use that
  column directly as the session-global crop video/row ordinal (it indexes the
  merged session-aggregate crop MP4 and root crop CSV).
- Compatibility rule: `session_crop_video_frame_index` is optional. Root crop
  CSVs written before the column existed still split and validate exactly as
  before (rows pass through with only the `crop_video_frame_index` rewrite).
  Validators treat the column as optional-but-checked-when-present: when the
  header contains it, every row must carry a contiguous ascending value
  (starting at `0` in session-aggregate CSVs, continuing the session-global
  count in per-clip CSVs); when the header lacks it, no check applies.

### YOLO Perf CSV (`Cam<serial>_yolo_perf.csv`)

Header (exact order):

```text
frame_id,recording_frame_id,timestamp,timestamp_sys,queue_depth,queue_depth_at_enqueue,queue_depth_after_enqueue,queue_depth_after_dequeue,queue_depth_at_worker_start,fps,ok,yolo_affinity_configured,yolo_affinity_applied,yolo_affinity_env_key,yolo_affinity_requested_cpus,yolo_affinity_effective_cpus,acquisition_to_worker_start_ms,acquisition_to_ptp_done_ms,ptp_done_to_yolo_resource_ready_ms,yolo_resource_ready_to_pointer_attrs_done_ms,pointer_attrs_done_to_ingress_event_record_ms,ingress_event_record_to_yolo_dispatch_ready_ms,yolo_dispatch_ready_to_yolo_enqueue_ms,recording_submit_call_ms,recording_submit_to_yolo_enqueue_ms,acquisition_to_yolo_enqueue_ms,yolo_enqueue_push_ms,yolo_enqueue_to_dequeue_ms,yolo_dequeue_to_worker_start_ms,yolo_queue_wait_ms,oldest_frame_age_at_worker_start_ms,oldest_queued_frame_age_at_worker_start_ms,ingress_event_record_to_worker_start_ms,acquisition_to_yolo_input_ready_ms,worker_start_to_yolo_input_ready_ms,acquisition_to_detect_done_ms,worker_start_to_detect_done_ms,service_sequence,camera_service_sequence,active_camera_count,same_camera_service_gap_ms,service_skew_latest_other_ms,service_skew_oldest_other_ms,service_count_skew_vs_min,service_count_skew_range,ingress_event_ready_before_wait,wait_ms,pre_ms,gap_ms,enqueue_ms,infer_ms,sync_ms,completion_event_ready_before_sync,cpu_wait_event_ms,cpu_ingress_event_query_ms,cpu_stream_wait_event_ms,cpu_npp_set_stream_ms,cpu_preprocess_ms,cpu_input_ready_event_record_ms,cpu_dump_ms,cpu_infer_call_ms,cpu_event_record_ms,cpu_pre_sync_ms,cpu_pre_sync_other_ms,cpu_post_sync_ms,queue_ms,post_ms,track_ms,ipc_ms,enet_ms,total_ms
```

Field semantics:
- ID/timestamp fields: integer counters/timestamps.
- `ok`: integer success flag.
- `queue_depth`: YOLO queue depth sampled at log emission, after processing.
- `queue_depth_at_enqueue`: YOLO input queue depth just before acquisition
  enqueued this frame.
- `queue_depth_after_enqueue`: YOLO input queue depth immediately after
  acquisition enqueued this frame.
- `queue_depth_after_dequeue`: YOLO input queue depth immediately after the
  worker popped this frame.
- `queue_depth_at_worker_start`: YOLO input queue depth immediately after this
  worker popped the frame for service.
- `yolo_affinity_configured`: `1` when a YOLO affinity env var was configured
  for this worker, otherwise `0`.
- `yolo_affinity_applied`: `1` when `pthread_setaffinity_np` succeeded, `0`
  when it failed, and `-1` when no affinity was configured.
- `yolo_affinity_env_key`: env key used for the affinity request.
- `yolo_affinity_requested_cpus`: parsed requested CPU set using `|` between
  CPUs.
- `yolo_affinity_effective_cpus`: `pthread_getaffinity_np` result after the
  affinity attempt, using the same `|` format.
- `yolo_queue_wait_ms`: host time from YOLO enqueue to worker start.
- `oldest_frame_age_at_worker_start_ms`: age of the frame being serviced at
  worker start, measured from acquisition receive.
- `oldest_queued_frame_age_at_worker_start_ms`: age of the next queued frame at
  worker start, or `-1` when no frame is waiting behind the current one.
- `acquisition_to_yolo_input_ready_ms` and
  `worker_start_to_yolo_input_ready_ms`: host timing to the point where YOLO has
  enqueued its input-ready event. These fields are populated when YOLO input
  detach is enabled, which is the default. Set `ORANGE_YOLO_DETACH_INPUT=0`
  to recover the older non-detached diagnostic path where these fields are
  `-1`.
- `service_*`: cross-camera YOLO service-order instrumentation used to spot
  multi-camera fairness skew.
- `cpu_wait_event_ms`: aggregate host time spent checking and optionally
  enqueuing the ingress event dependency.
- `cpu_ingress_event_query_ms`: host time spent in `cudaEventQuery` for the
  ingress event.
- `cpu_stream_wait_event_ms`: host time spent in `cudaStreamWaitEvent` for the
  ingress event. This should be near zero when
  the ready-event fast path is enabled and `ingress_event_ready_before_wait=1`.
- `cpu_input_ready_event_record_ms`: host time spent recording the
  YOLO-input-ready CUDA event when YOLO input detach is enabled.
- `*_ms` and fps fields: floating-point timing/throughput metrics.

Gate:
- Disabled when `ORANGE_YOLO_PERF_LOG=0`.
- Sampling controlled by `ORANGE_YOLO_PERF_SAMPLE`.
- `ORANGE_YOLO_READY_EVENT_FASTPATH` defaults to enabled and skips
  `cudaStreamWaitEvent` when
  `cudaEventQuery` has already proven the ingress event is complete. It
  preserves the wait path when the event is not ready. Set
  `ORANGE_YOLO_READY_EVENT_FASTPATH=0` for old-path comparisons.
- `ORANGE_YOLO_DETACH_INPUT` defaults to enabled and records an event after
  YOLO has copied/preprocessed the source frame into its owned TensorRT input
  buffer. With
  `ORANGE_RECORDING_DETECT_PRIORITY=1`, source/primary recording gates on this
  event instead of full detection completion. Set `ORANGE_YOLO_DETACH_INPUT=0`
  for old-path comparisons.

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
  --require-imgui-glfw-size-cache \
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
`--expect-external-crop-recorder-gpu-id <N>` to assert one recorder GPU for all
external crop streams, or repeat
`--expect-external-crop-recorder-gpu <serial>=<N>` for per-camera placement.
Use
`--max-external-crop-encode-queue-high-water <N>` and
`--max-external-crop-enqueue-age-p95-ms <ms>` when tuning or validating the
external crop queue so backlog stays bounded.
The validator always treats `encode_queue_high_water > encode_queue_depth` as
an invalid recorder summary when both fields are present.
For newer GUI external crop artifacts, the validator also cross-checks the
optional `recording_backend.crop_recording` per-camera telemetry maps against
the external crop recorder summaries when those maps are present.
Use `--require-external-crop-backend-metadata` for current-build external crop
GUI runs; it requires the `recording_backend.crop_recording` mode, per-camera
  `stream_config`, routing fields (`stream_id`, `stream_kind`, `output_kind`,
  real `camera_serial`, `env_key`, analytics GPU, recorder GPU, socket path, and
  queue depth), descriptor `details` consistency, and telemetry maps to be
  present instead of treating them as optional compatibility fields.
For rolling external crop artifacts, the same strict gate also requires
`recording_outputs[serial].crop` to be a finalized external-IPC
session-aggregate descriptor with `details.scope = "session_aggregate"`, so an
early pending/in-process snapshot descriptor cannot pass as current metadata.
Use `--require-source-version` on current GUI validation runs to require the
recording snapshot Git provenance block, and add
`--expect-source-git-command-user-mode sudo_invoking_user` for sudo-launched
runs that should execute git after dropping to `SUDO_UID`/`SUDO_GID`. Before a
commit, pair this with `--expect-source-dirty-tracked 1`; after committing,
use `--expect-source-dirty-tracked 0`.
`scripts/summarize_gui_validation.py` reports the same external crop queue and
drop telemetry, and also displays the external crop `stream_config` GPU/socket
placement from `recording_backend.crop_recording` when available. If a future
artifact has the manifest telemetry but lacks a readable external crop summary
file, the summarizer falls back to the manifest maps so operators can still see
the recorder-side counts and queue pressure that Orange indexed.
`--require-crop-preview-sampling` is for visible bounded-preview runs; it fails
unless the sidecar shows cadence skips and fewer preview updates than offers.
GUI runs also write `recording_snapshot.json`
`session.gui_display_frame_rate`, with overall, crop-preview-visible, and
crop-preview-hidden ImGui delta-time FPS buckets plus
`stream_downsample`, `display_preview_max_fps`, `swap_interval`,
`frame_max_fps`, and
`yolo_speed_graphs_enabled`. The `--min-gui-*fps-p05`,
`--expect-gui-stream-downsample`, `--expect-display-preview-max-fps`,
`--expect-gui-swap-interval`, and `--expect-gui-frame-max-fps` validator
thresholds use those fields to reject UI-refresh collapses and stale display
configuration. Use `--expect-yolo-speed-graphs-enabled 0` to prove the
recording-time ImPlot speed graphs were disabled. Keep
`ORANGE_GUI_SHOW_SPEED_GRAPHS=0` for performance validation unless the run
specifically needs live speed plots. The GUI validation launcher's fast display
profile currently defaults to `ORANGE_GUI_SWAP_INTERVAL=0`,
`ORANGE_GUI_FRAME_MAX_FPS=60`, and `ORANGE_DISPLAY_PREVIEW_MAX_FPS=15`: that
avoids vblank/compositor stalls without letting the GUI loop run unbounded on
the display GPU. Use
`scripts/run_gui_fourcam_external_ipc_validation.sh --citrus-display-safe`
when Citrus is sharing the display GPU; that profile defaults to
`ORANGE_GUI_SWAP_INTERVAL=1`, `ORANGE_GUI_FRAME_MAX_FPS=30`, and
`ORANGE_DISPLAY_PREVIEW_MAX_FPS=10`.
Persistent workstation policy can also be configured in
`~/orange_data/config/app/default.json` under `gui.display`; launcher/env
values intentionally override app config for explicit validation runs.
The same four-camera launcher pins YOLO workers to a Citrus-safe CPU set by
default: `2010093->6`, `2010094->8`, `2010095->10`, and `2010096->12`.
Those cores are intended to be isolated or otherwise kept free of ordinary OS
work for the YOLO queue-wakeup discriminator. Keep Citrus' current CPU `1`,
CPU `2`, IPC-reader range starting at `20`, and arena-worker CPUs `24-27`
reserved for Citrus. The launcher also defaults
`ORANGE_GUI_REQUIRE_ISOLATED_CPUS=6,8,10,12,38,40,42,44`, so the printed
validation command includes
`--require-isolated-cpus 6,8,10,12,38,40,42,44`. That gate checks
`recording_snapshot.json` `session.system_cpu.isolated_cpus`, not the current
machine state at validation time. Orange still pins YOLO only to the primary
cores `6,8,10,12`; the sibling cores `38,40,42,44` are required to be isolated
so they can remain unused during low-jitter validation. The same profile also
defaults `ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS=6,8,10,12,38,40,42,44` and
`ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_OPTIONS=isolcpus,nohz_full,rcu_nocbs`, so
the printed validation command also requires each recorded
`session.system_cpu.kernel_cmdline.options` CPU list to include those CPUs.

Newer GUI runs also include `session.gui_display_frame_rate.timings` so slow
GUI refresh can be attributed to texture upload, camera/crop window drawing,
speed-graph drawing, or render/present. Use `--require-gui-timing-telemetry`
when a run is intended to diagnose GUI refresh performance. The validation and
summary scripts additionally derive a dominant-p95 diagnosis from those timing
buckets:

- `scripts/validate_gui_ptp_recording.py --json-out` writes it under
  `gui_display_frame_rate.timing_diagnosis`.
- `scripts/summarize_gui_validation.py --json` writes it under
  `gui_display_diagnosis`.

For runs intended to prove the ImGui GLFW backend display-size cache, also use
`--require-imgui-glfw-size-cache`. That gate requires
`session.gui_display_frame_rate.imgui_glfw_size_cache` to show cached
window/framebuffer size hits, zero fallback GLFW size calls, and zero
null-window requests during active recording.

Those counters are reset at GUI recording start. A clean run should report
`cache_context_registered=true`, positive `window_size_cache_hits` and
`framebuffer_size_cache_hits`, zero fallback/null-window counters, and
`total_size_requests` equal to the sum of the component counters. Fallback
counts are diagnostic: they usually mean the ImGui GLFW backend asked about a
non-main platform window or the compile-time shim scope changed.

GUI local-control stop provenance is stored in `recording_session.json` under
`recording.control` and summarized by `scripts/summarize_gui_validation.py` with
`--json` plus `scripts/validate_gui_ptp_recording.py --json-out` under
`recording_session.local_control_stop`. That summary carries the stop method,
request/operation identity, command source, optional Citrus terminal fields,
receive/trigger timestamps, grace seconds, and configured drain-timeout
threshold when present. New GUI local-control runs also persist drain lifecycle
evidence there, including `drain_completed`, `drain_completed_at_utc`,
`drain_timed_out`, `forced_finalize_requested`,
`forced_finalize_stream_stop_requested`, terminal `ack_state`, `health`,
`error_code`, and `last_event` / `last_event_at_utc`. A clean finalized stop
must persist `ack_state="executed"`; a drain-timeout path must persist
`ack_state="failed_timeout"`. Conversely, a persisted failed-timeout ACK is
valid only with `drain_timed_out=true`, `forced_finalize_requested=true`, and
`error_code="drain_timeout"`. Forced-finalize fields are timeout-only:
`forced_finalize_requested=true` requires `drain_timed_out=true`, and a
completed timeout must also carry
`forced_finalize_stream_stop_requested=true` with
`last_event="finalized_after_drain_timeout"`. The GUI validator rejects
contradictory persisted stop-control evidence when local-control stop
expectations are enabled.

This derived diagnosis is intentionally script-owned. It should not be treated
as a persisted Orange snapshot schema field.

To compare visible-preview and hidden-preview validation JSON files, use
`scripts/compare_gui_crop_preview_validation.py`. It summarizes GUI FPS, crop
recording rows/drops, fanout counters, external crop queue pressure, external
crop analytics-GPU-to-recorder-GPU placement, and the dominant GUI timing p95
bucket side by side. The `--require-matching-yolo-runtime-config` gate compares
per-camera YOLO requested/effective affinity, the recorded isolated CPU set,
and recorded `isolcpus` / `nohz_full` / `rcu_nocbs` boot CPU-list options.
Those boot options are normalized before comparison, so equivalent CPU range
syntax compares equal while different CPU sets or non-CPU `isolcpus` flags
still fail. It can also fail the comparison
with:

- `--require-pass`
- `--require-zero-crop-drops`
- `--min-gui-overall-p05-fps <fps>`
- `--min-gui-visible-p05-fps <fps>`
- `--min-gui-hidden-p05-fps <fps>`
- `--require-visible-samples`
- `--require-hidden-samples`
- `--require-matching-cameras`
- `--require-matching-display-config`
- `--require-matching-crop-config`
- `--require-matching-yolo-runtime-config`
- `--max-external-crop-queue-high-water <N>`
- `--max-external-crop-enqueue-age-p95-ms <ms>`

All numeric compare gate values must be nonnegative. With `--json`, the compare
helper writes `status`, `threshold_failures`, and `runs` so automation can
consume both the comparison table data and the gate result without scraping
stderr.
The visible/hidden FPS compare thresholds only apply to runs with samples in
that bucket, so a visible-preview run can be compared with a hidden-preview run
using both thresholds. Add the sample-presence flags when the comparison is
expected to include at least one visible-preview run and at least one
hidden-preview run.
Use the matching flags for A/B validation so visible and hidden results cannot
be accidentally compared across different camera sets, display settings, crop
backends, external crop queue depths, external crop GPU placement, preview FPS
caps, preview-disable settings, or crop frame pool sizes.

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
  into ordinary device memory. Staged-source mode is enabled by default; set
  `ORANGE_CROP_STAGE_SOURCE=0` for direct-source comparisons. This field is
  `0` when staged-source mode is disabled.
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
  completed by row write. Copy timing is disabled by default; set
  `ORANGE_CROP_COPY_TIMING=1` before launching Orange when a diagnostic run
  needs GPU copy timings.
- `ORANGE_CROP_STAGE_SOURCE` defaults to enabled and stages the GPUDirect source
  frame into ordinary device memory before crop extraction so the ROI path can
  be compared against the direct GPUDirect-backed source path.
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
camera_serial,gpu_id,worker,queue_size,producer_jobs_offered,producer_jobs_enqueued,producer_queue_full_drops,producer_blank_jobs_offered,producer_blank_jobs_enqueued,producer_dropped_jobs_offered,producer_dropped_jobs_enqueued,consumer_jobs_enqueued,consumer_queue_full_drops,consumer_queue_high_water,crop_frame_pool_size,producer_encode_job_pool_capacity,producer_encode_job_pool_available,producer_encode_job_pool_active,producer_encode_job_pool_high_water,producer_encode_job_pool_misses_total,producer_encode_job_pool_invalid_returns_total,producer_encode_job_pool_double_returns_total,producer_preview_job_pool_capacity,producer_preview_job_pool_available,producer_preview_job_pool_active,producer_preview_job_pool_high_water,producer_preview_job_pool_misses_total,producer_preview_job_pool_invalid_returns_total,producer_preview_job_pool_double_returns_total,producer_recording_crop_frame_offered,producer_recording_crop_frame_accepted,producer_recording_crop_frame_dropped,producer_preview_crop_frame_offered,producer_preview_crop_frame_accepted,producer_preview_crop_frame_dropped,producer_pose_crop_frame_offered,producer_pose_crop_frame_accepted,producer_pose_crop_frame_dropped,producer_frames_produced_total,producer_frames_recycled_total,producer_crop_frame_release_total,producer_crop_frame_pool_misses_total,producer_source_release_event_misses_total,producer_pending_source_releases,producer_pending_crop_frame_recycles,preview_max_fps,preview_disabled,preview_display_enabled_final,preview_frames_offered,preview_frames_updated,preview_frames_skipped_by_cadence,preview_clears_updated,preview_queue_full_drops,preview_queue_high_water,preview_serial_final
```

Preview fields are GUI display telemetry. `preview_frames_skipped_by_cadence`
does not indicate crop recording loss; crop recording drops remain reported by
`Cam<serial>_crop_perf.csv` `dropped` and `drop_reason`.
`preview_disabled` means the preview CUDA/PBO path was unavailable or explicitly
disabled. `preview_display_enabled_final` records whether the GUI crop preview
windows were enabled at summary time.
Newer sidecars include `crop_frame_pool_size` and crop job-pool counters after
`consumer_queue_high_water`; validators should use these columns by name. The
job-pool fields report bounded wrapper-pool capacity, availability, active
count, high-water, pool misses, invalid returns, and double returns for encode
and preview job wrappers.
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
  (`camera_timestamp_ns` terminology in current Orange docs/diagnostics), copied
  unchanged from `Emergent::CEmergentFrame::timestamp`. In the current
  Orange/Emergent path this is treated as nanoseconds. For a PTP-synchronized
  camera, Orange treats it as a camera-hardware PTP timestamp; the more specific
  IEEE-1588/TAI epoch interpretation is currently inferred, not explicitly
  declared by the installed SDK header. Do not apply that interpretation to an
  unsynchronized or non-PTP camera timestamp.
- `timestamp_sys` in metadata rows is Orange host-system POSIX epoch time in
  nanoseconds, sampled from `CLOCK_REALTIME` immediately after frame receive.
- Neither `timestamp` nor `timestamp_sys` is time since process, stream, or
  recording start.
- Richer derived formats such as Zarr must describe each clock separately with
  its unit, origin, timescale, clock domain, source field, producer, and whether
  those semantics are producer-declared or inferred. Do not collapse the two
  fields into a generic `producer_clock_domain` label.
- Finalized `recording_session.json` files written through the shared session
  manifest writer contain `timestamp_clock_contract` schema
  `orange.recording.timestamp_clocks` version `1`. It maps the unchanged CSV
  columns to stable clock IDs, declares the producer-defined host
  `CLOCK_REALTIME` clock, freezes the available per-camera PTP evidence, and
  fingerprints the source `ptp_sync_summary.json`.
- Camera clocks default to `classification = "device_defined"`, `origin =
  null`, and `timescale = "device_defined"`. The version-1 classifier emits
  `classification = "ieee1588_tai"` only when session/camera PTP is enabled,
  PTP offset and camera-latch checks are bounded, low-rate PTP readbacks are
  stable, and recording-frame `timestamp - timestamp_sys` statistics match the
  expected TAI-minus-UTC offset. The resulting epoch/timescale authority is
  `inferred_from_recording_evidence`, never producer-declared.
- `timestamp_clock_contract.clock_state_intervals` binds each camera clock
  classification to recording-frame IDs. The PTP readback observations remain
  available inside the frozen evidence snapshot so downstream conversion can
  reject or subdivide a recording if the readback changed.
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
