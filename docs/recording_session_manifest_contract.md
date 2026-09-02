# Recording Session Manifest Contract

## Purpose

This document defines the recording-session artifact contract Orange should use
for the compatibility single-video layout and the experimental headless
rolling-clip layout. The shared C++ helper lives in
`src/session/recording_session.*`; headless recording uses that helper so the
GUI/session path and external-recorder supervision can converge on the same
manifest shapes instead of carrying separate contracts.

Current implementation rule:

- `clip_seconds = 0` keeps the existing flat single-video layout.
- `clip_seconds > 0` is implemented for headless `fixed.recording_control`
  with seamless GOP-boundary writer switching.
- External IPC rolling is implemented for supervised headless diagnostic
  recorder runs. The external recorder owns GOP-boundary writer rotation and
  writes its clip list in `external_recorder_summary.json`; after recorder
  finalization, Orange mirrors that clip list into the shared analytics
  `recording_session.json`.
- GUI in-process recordings now write the same single-clip
  `recording_session.json` contract after the recording drain completes.
- GUI external IPC single-clip supervision is implemented for the diagnostic
  recorder path: the GUI launches supervised recorder processes on record
  start, drains/finalizes them, and writes an
  `orange_gui_external_ipc` single-clip `recording_session.json`.
- GUI external IPC full-frame rolling finalization is now wired for supervised
  recorder contracts that request `clip_seconds > 0`: after recorder shutdown,
  the GUI mirrors each external `rolling_output.clips[]` list into an
  `orange_gui_external_ipc` `rolling_clips` manifest, writes per-clip
  `clip_manifest.json`, writes `recording_clip_index.json/csv`, and updates
  `recording_snapshot.json` session pointers.
- GUI validation tooling now treats full-frame `rolling_clips` manifests as a
  first-class layout for `--latest-complete` discovery, per-clip artifact
  checks, packet-count checks, and cross-clip `recording_frame_id` continuity.
- GUI external crop IPC now follows the GUI external full-frame rolling control
  when full-frame external IPC rolling is active. The crop recorder uses the
  same `external_recorder_gop_boundary_writer_rotation` implementation with
  crop GOP size `1`, plus a terminal-tail coalesce window matched to the
  full-frame GOP so crop clips stay aligned with parent clips. GUI finalization
  mirrors per-camera crop `rolling_output.clips[]` into
  `recording_backend.crop_recording.rolling_clips` plus per-clip crop outputs
  in each `clips[].recording_outputs` list. It also writes a top-level
  `recording_outputs[serial].crop` session-aggregate descriptor for the merged
  external crop MP4 and root crop CSV sidecars, with
  `details.scope = "session_aggregate"`, so `recording_snapshot.json` no longer
  retains the early pending in-process crop descriptor after finalization. When
  `clip_seconds = 0`, crop outputs remain explicit single-clip sidecars.
- In-GUI full-frame external IPC rolling controls now edit the loaded in-memory
  app config before streaming starts. App config `recording.recording_control` and
  `ORANGE_GUI_RECORD_FOR_SECONDS` / `ORANGE_GUI_CLIP_SECONDS` can now supply
  that control for full-frame GUI external IPC runs.

The current headless rolling implementation keeps acquisition and recording
active during clip rollover:

- it preopens the next clip writer while the current writer is active,
- switches writers at a GOP first-frame boundary,
- forces the first submitted NVENC picture in the new clip to IDR with SPS/PPS,
- finalizes the previous clip after the switch,
- keeps `recording_frame_id` continuous across clips.

The manifest records this with `rollover.implementation =
"headless_gop_boundary_writer_switch"`, `seamless_writer_switch = true`,
`records_during_rollover = true`, and `next_writer_preopened = true`.

The external IPC rolling recorder records the same high-level intent with
`rollover.implementation =
"external_recorder_gop_boundary_writer_rotation"`. The external summaries remain
the recorder's per-stream truth, and the analytics `recording_session.json`
mirrors those summaries into one multi-camera `rolling_clips` manifest. Each
clip carries continuous `recording_frame_id` metadata and clip-local MP4
timestamps.

The current session contract's optional `recording_outputs[serial].crop` entry
is the legacy scalar YOLO-driven crop sidecar, including its existing rolling
behavior. It does not represent the detector-independent spatial ROI product.
The latter remains default-off and is not session-integrated until the separate
per-ROI descriptor collection, collision-free frame identity, and strict
finalization gates in `docs/spatial_roi_recording_v1_foundation.md` are complete.
The additive v3 shape below is the metadata integration seam; it does not by
itself arm or launch an ROI recorder.

The shared descriptor helper now defines an additive output-index envelope for
the collection integration seam. When spatial ROI descriptors are supplied,
session and clip manifests may carry `recording_outputs_v3` with
`schema_id = "orange.recording_outputs"` and `schema_version = 3`:

```json
"recording_outputs_v3": {
  "schema_id": "orange.recording_outputs",
  "schema_version": 3,
  "cameras": {
    "2010096": {
      "full": { "output_kind": "full" },
      "crop": { "output_kind": "crop" },
      "spatial_roi": {
        "2010096_spatial_roi_roi_1": {
          "output_kind": "spatial_roi",
          "camera_serial": "2010096",
          "logical_stream_id": "2010096_spatial_roi_roi_1"
        }
      }
    }
  }
}
```

`full` stays ingest-authoritative and scalar, and the compatibility
`recording_outputs[serial].full`/`.crop` entries are not replaced. Writers
reject missing or duplicate spatial-ROI logical stream keys and identity-field
mismatches; consumers must validate the v3 envelope before using it.

## Single-Video Layout

When `clip_seconds = 0`, keep the existing structure:

```text
<recording_folder>/
  recording_session.json
  recording_snapshot.json
  recording_geometry_contract.json
  recording_geometry_assets/
    manifest.json
    cameras/...
    tank_designs/...
  ptp_sync_summary.json
  Cam2010095.mp4
  Cam2010095_meta.csv
  Cam2010095_keyframe.json
  Cam2010095_pipeline_perf.csv
  Cam2010096.mp4
  Cam2010096_meta.csv
  Cam2010096_keyframe.json
  Cam2010096_pipeline_perf.csv
```

This remains the compatibility layout for existing Orange, Citrus, and analysis
consumers. Headless `recording_control.record_for_seconds` with
`clip_seconds = 0` writes `recording_session.json` with
`mode = "single_clip"`.

Single-clip manifests now also emit `recording_outputs[serial].full` as the
canonical output descriptor for the ingest-authoritative full-frame MP4 and its
metadata/keyframe sidecars. The legacy `camera_artifacts` and
`clips[].artifacts` maps remain compatibility aliases and should match the
descriptor paths and counts. Optional crop videos are represented as
`recording_outputs[serial].crop` with `role = "sidecar"` when crop writing is
enabled.

When `recording_geometry_contract.json` exists, both single-clip and rolling
session manifests add `metadata.recording_geometry_contract`. That reference
contains the schema identity/version, contract status, absolute and
recording-folder-relative paths, and the SHA-256 of the exact sidecar bytes.
New contracts also expose `metadata.recording_geometry_contract.materialized_assets`,
which points at the checksummed, recording-local exact-byte evidence bundle.
Geometry remains optional and non-blocking; consumers must inspect its status
before using any transform or scale. See
`docs/recording_geometry_contract.md`.

If a selected daily registration is active, that same contract reference leads
to the exact accepted registration and per-participating-camera schema-v2 dish
mask. Consumers should use
`recording_snapshot.json.calibrations[serial].dish_top_rim_observation` for the
direct numerical view and follow its `recording_local_assets` paths when they
need the immutable source observation or mask exports. The session manifest
does not duplicate the circles as an independent authority.

## Rolling-Clip Layout

When headless `recording_control.clip_seconds > 0`, the parent session folder is
the discovery root and each clip gets its own subfolder:

```text
<recording_folder>/
  recording_session.json
  recording_snapshot.json
  recording_geometry_contract.json
  recording_geometry_assets/
    manifest.json
    cameras/...
    tank_designs/...
  ptp_sync_summary.json
  Cam2010095_pipeline_perf.csv
  Cam2010096_pipeline_perf.csv

  clips/
    clip_000000/
      clip_manifest.json
      Cam2010095.mp4
      Cam2010095_meta.csv
      Cam2010095_keyframe.json
      Cam2010096.mp4
      Cam2010096_meta.csv
      Cam2010096_keyframe.json

    clip_000001/
      clip_manifest.json
      Cam2010095.mp4
      Cam2010095_meta.csv
      Cam2010095_keyframe.json
      Cam2010096.mp4
      Cam2010096_meta.csv
      Cam2010096_keyframe.json
```

## External IPC Rolling Layout

Supervised headless `recording_sink_mode = "external_ipc"` writes the external
recorder artifacts under the contract `artifact_root`:

```text
<external_artifact_root>/
  external_recorder_session.json
  external_recorder_summary.json
  external_recorder_finalization.json
  external_video_sanity.json
  external_gop_routing.csv
  external_detach.csv
  external_encode_shard0_gpu5.csv
  external_encode_shard1_gpu6.csv
  Cam2010096_external.mp4
  Cam2010096_external_keyframes.json
  Cam2010096_external_shard0_gpu5.mp4      # temporary unless preserved
  Cam2010096_external_shard1_gpu6.mp4      # temporary unless preserved

  clips/
    clip_000000/
      Cam2010096_external.mp4
      Cam2010096_external_meta.csv
      Cam2010096_external_keyframe.json

    clip_000001/
      Cam2010096_external.mp4
      Cam2010096_external_meta.csv
      Cam2010096_external_keyframe.json
```

The external recorder keeps the merged full-session MP4 for compatibility and
also writes one MP4 per rolling clip. Full-rate A16 validation uses split-GOP
shards; by default, per-shard MP4s are deleted after clean merged finalization
and remain only as opt-in diagnostic outputs. The merged MP4 and clip MP4s are
the consumer-facing media.

Rolling sessions also write session-level clip indexes:

```text
recording_clip_index.json
recording_clip_index.csv
```

Each index row is one `(clip, camera)` range. It carries the clip status,
rollover/start/stop reason, first/last `recording_frame_id`, frame count,
per-camera artifact paths, the `clip_manifest.json` pointer, `packet_count`,
and `packet_count_source`. Native in-process clips use
`packet_count_source = "ffprobe_nb_read_packets"`. Supervised external IPC
clips use `external_recorder_summary.packets_written` as the packet count
source.

Each clip manifest and top-level `clips[]` entry also carries
`recording_outputs[serial].full` for the clip-local full-frame output. For
rolling sessions, clip descriptors are the ingest-authoritative range contract.
GUI external IPC may additionally include top-level session-aggregate
descriptors for merged compatibility outputs, currently used for external crop
recording metadata and snapshot finalization.

## Rolling Session Manifest

The top-level rolling manifest uses this shape:

```json
{
  "schema_id": "orange.recording_session",
  "schema_version": 1,
  "producer": "orange_headless",
  "session_id": "run_0001__codec_hevc__...",
  "mode": "rolling_clips",
  "status": "completed",
  "recording_folder": "/abs/path/to/session",
  "created_at_utc": "2026-05-09T02:32:10Z",
  "updated_at_utc": "2026-05-09T02:32:31Z",
  "cameras": ["2010096"],
  "recording_control": {
    "record_for_seconds": 18,
    "clip_seconds": 6
  },
  "rollover": {
    "implementation": "headless_gop_boundary_writer_switch",
    "seamless_writer_switch": true,
    "records_during_rollover": true,
    "boundary": "gop_first_frame_id",
    "next_writer_preopened": true
  },
  "recording": {
    "started": true,
    "stop_requested": true,
    "stop_reason": "record_for_seconds_elapsed",
    "control": {
      "source": "orange_gui_local_control",
      "method": "stop_recording",
      "request_id": "run-42:orange:stop_recording",
      "operation_id": "run-42",
      "received_at_utc": "2026-05-29T00:00:00Z",
      "stop_triggered_at_utc": "2026-05-29T00:00:00Z",
      "drain_completed": true,
      "drain_timed_out": false,
      "ack_state": "executed",
      "event_log": {
        "source_path": "/tmp/orange_local_control.sock.events.jsonl",
        "copied_path": "/path/to/recording/orange_local_control.events.jsonl",
        "relative_path": "orange_local_control.events.jsonl",
        "copied": true,
        "copied_at_utc": "2026-05-29T00:00:01Z"
      }
    },
    "drain_completed": true,
    "actual_recording_duration_s": 18.0,
    "sum_clip_actual_duration_s": 18.0
  },
  "stream": {
    "requested_duration_seconds": 24,
    "actual_elapsed_s": 24.0,
    "interrupted": false
  },
  "indexes": {
    "schema_id": "orange.recording_session.indexes",
    "schema_version": 1,
    "clip_index_json": "recording_clip_index.json",
    "clip_index_csv": "recording_clip_index.csv",
    "row_granularity": "clip_camera",
    "path_style": "relative_to_recording_folder",
    "clip_count": 3,
    "row_count": 3
  },
  "clips": [
    {
      "clip_index": 0,
      "clip_id": "clip_000000",
      "directory": "clips/clip_000000",
      "status": "completed",
      "start_reason": "recording_start",
      "stop_reason": "clip_seconds_elapsed",
      "drain_completed": true,
      "rollover": {
        "request_id": 1,
        "rollover_at_recording_frame_id": 601,
        "first_recording_frame_id": 1,
        "last_recording_frame_id": 600,
        "pending_next_clip": false
      },
      "final_clip": false
    },
    {
      "clip_index": 1,
      "clip_id": "clip_000001",
      "directory": "clips/clip_000001",
      "status": "completed",
      "start_reason": "rollover",
      "stop_reason": "clip_seconds_elapsed",
      "drain_completed": true,
      "rollover": {
        "request_id": 2,
        "rollover_at_recording_frame_id": 1226,
        "first_recording_frame_id": 601,
        "last_recording_frame_id": 1225,
        "pending_next_clip": false
      },
      "final_clip": false
    },
    {
      "clip_index": 2,
      "clip_id": "clip_000002",
      "directory": "clips/clip_000002",
      "status": "completed",
      "start_reason": "rollover",
      "stop_reason": "record_for_seconds_elapsed",
      "drain_completed": true,
      "rollover": {
        "request_id": 2,
        "rollover_at_recording_frame_id": 1226,
        "first_recording_frame_id": 1226,
        "last_recording_frame_id": 1800,
        "pending_next_clip": false
      },
      "final_clip": true
    }
  ]
}
```

For GUI local-control stops, `recording.control` is the durable stop
provenance. Clean finalized drains use `ack_state="executed"`. If
`drain_timed_out=true`, the manifest must carry
`forced_finalize_requested=true`, `error_code="drain_timeout"`, and
`ack_state="failed_timeout"`. Once that timed-out drain is completed, it must
also carry `forced_finalize_stream_stop_requested=true` and
`last_event="finalized_after_drain_timeout"`. A forced-finalize flag without
`drain_timed_out=true` is invalid. A failed-timeout ACK without
`drain_timed_out=true`, forced-finalize evidence, and
`error_code="drain_timeout"` is also invalid.

The top-level manifest is the stable discovery entrypoint. Consumers should use
the `clips[*].directory` entries to enumerate clip folders and then read each
clip's manifest for per-camera frame ranges.

The per-clip `rollover` object carries the session-continuous frame range for
that clip. For non-final clips, `rollover_at_recording_frame_id` is the first
recording frame of the next clip. For a final clip that was opened by a prior
rollover, it records that opening boundary and can match the final clip's
`first_recording_frame_id`.

## Clip Manifest

Each rolling clip writes:

```text
clips/clip_000000/clip_manifest.json
```

Shape:

```json
{
  "schema_id": "orange.recording_clip",
  "schema_version": 1,
  "producer": "orange_headless",
  "session_id": "run_0001__codec_hevc__...",
  "clip_id": "clip_000000",
  "clip_index": 0,
  "status": "completed",
  "directory": "clips/clip_000000",
  "recording_folder": "/abs/path/to/session/clips/clip_000000",
  "start_reason": "recording_start",
  "stop_reason": "clip_seconds_elapsed",
  "requested_duration_s": 6.0,
  "actual_duration_s": 6.0,
  "drain_completed": true,
  "drain_duration_s": 0.03,
  "rollover": {
    "request_id": 1,
    "rollover_at_recording_frame_id": 601,
    "first_recording_frame_id": 1,
    "last_recording_frame_id": 600,
    "pending_next_clip": false
  },
  "cameras": ["2010096"],
  "camera_artifacts": {
    "2010096": {
      "video": "/abs/path/to/session/clips/clip_000000/Cam2010096.mp4",
      "metadata": "/abs/path/to/session/clips/clip_000000/Cam2010096_meta.csv",
      "keyframes": "/abs/path/to/session/clips/clip_000000/Cam2010096_keyframe.json",
      "frame_count": 600,
      "first_recording_frame_id": 1,
      "last_recording_frame_id": 600,
      "recording_frame_id_gaps": 0,
      "packet_count": 600,
      "packet_count_source": "ffprobe_nb_read_packets"
    }
  }
}
```

Per-clip `Cam<serial>_meta.csv` currently uses the existing header:

```text
frame_id,timestamp,timestamp_sys
```

Here `frame_id` is the session-continuous `recording_frame_id`. The MP4
container timeline is clip-local and starts at zero for each clip, even though
metadata frame IDs continue across clips.

## Frame Identity

- `recording_frame_id` is session-continuous across clips.
- MP4 presentation timestamps are clip-local and start at zero for each MP4.
- `camera_frame_id` is not in the current per-clip CSV; camera/vendor frame
  continuity is still summarized by pipeline and run health fields.
- Consumers should join across clips with `recording_frame_id`.
- Seamless rollover switches at a GOP boundary. Individual clip media durations
  can vary by up to one GOP while the total requested recording duration and
  `recording_frame_id` coverage remain continuous.

## Validation

Use:

```bash
scripts/verify_timed_recording.py <experiment_root>
```

The verifier now handles both `mode = "single_clip"` and
`mode = "rolling_clips"`. For rolling clips it checks:

- parent `recording_session.json`,
- per-clip `clip_manifest.json`,
- per-camera clip MP4/metadata/keyframe paths,
- per-clip and cross-clip `recording_frame_id` continuity,
- parent rollover contract for seamless writer switching,
- session-level `recording_clip_index.json` / `recording_clip_index.csv`,
- real per-clip packet counts in camera artifacts and clip indexes,
- `recording_snapshot.json` pointers back to `recording_session.json` and the
  clip index artifacts,
- each clip starts with keyframe frame `0`,
- `runs.json` health/pass fields when present,
- total ffprobe video duration within tolerance.

Latest validated headless rolling smoke:

- artifact:
  `/tmp/orange_seamless_rolling_bt1/2010096_headless_seamless_rolling_clip_smoke_bt1`
- `record_for_seconds = 18`, `clip_seconds = 6`, stream duration `24 s`
- `summary.json`: `pass_runs = 1`, `fail_runs = 0`
- camera health: `0` frame-ID gaps, `0` GetFrame errors, `0` encode failures,
  `0` preprocess drops
- clips: three clip directories, continuous frames `1-1800`, total ffprobe
  duration `18.000 s`
- all clips start with keyframe frame `0` after forcing IDR/SPS/PPS on each
  newly opened clip
- verifier total: `18.000 s` for a requested `18.000 s`

Latest longer validation:

- artifact:
  `/tmp/orange_seamless_rolling_long_bt2/2010096_headless_seamless_rolling_clip_long_bt2`
- `record_for_seconds = 36`, `clip_seconds = 6`, stream duration `42 s`
- six clip directories, continuous frames `1-3600`, total ffprobe duration
  `36.000 s`
- `summary.json`: `pass_runs = 1`, `fail_runs = 0`
- camera health: `0` frame-ID gaps, `0` GetFrame errors, `0` encode failures,
  `0` preprocess drops
- `scripts/verify_timed_recording.py` passed

Latest two-camera PTP validation:

- artifact:
  `/tmp/orange_two_camera_ptp_rolling_bt2/2010095_2010096_headless_ptp_seamless_rolling_bt2`
- checked-in spec:
  `experiment_specs/2010095_2010096_headless_ptp_rolling_clip_smoke_a16.json`
- `record_for_seconds = 18`, `clip_seconds = 6`, stream duration `24 s`,
  warmup `2 s`, `ptp_register_read_decimate = 100`
- recording duration is anchored at the first observed `recording_frame_id`;
  this avoids under-recording during the PTP gate startup countdown
- both cameras wrote three clip directories, continuous frames `1-1801`, total
  ffprobe duration `18.010 s`, and video-content status `pass`
- camera health: `0` frame-ID gaps, `0` GetFrame errors, `0` encode failures,
  `0` preprocess drops
- `scripts/verify_timed_recording.py` passed for both cameras

Latest supervised external IPC rolling validation:

- one-camera checked-in spec:
  `experiment_specs/2010096_headless_real_yolo_external_ipc_rolling_smoke_a16_gpu5_6.json`
- one-camera artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_external_ipc_rolling_smoke_a16_gpu5_6`
- one-camera recorder artifact:
  `/tmp/orange_external_recorder_rolling_2010096`
- `record_for_seconds = 6`, `clip_seconds = 2`, `encode_fps = 100`,
  `routing_policy = "gop_modulo"`, shard GPUs `5,6`
- recorder received/ACKed/encoded `602` frames with `0` encode drops
- clips covered continuous frame ranges `1-200`, `201-400`, `401-600`, and
  `601-602`
- merged MP4 video sanity and `scripts/verify_external_recorder_session.py`
  passed
- two-camera PTP checked-in spec:
  `experiment_specs/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_rolling_smoke_a16.json`
- latest two-camera PTP recorder artifact:
  `/tmp/orange_external_recorder_ptp_rolling_20260509_ptp_rolling_bridge_28478`
- latest two-camera PTP analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_rolling_bridge_20260509_ptp_rolling_bridge_28478`
- both cameras received/ACKed/encoded `601` frames with `0` encode drops, four
  rolling clips, and merged MP4 video sanity pass
- analytics `recording_session.json` reported `mode = "rolling_clips"`,
  `producer = "orange_headless_external_ipc"`, `recording_backend.mode =
  "external_ipc"`, and external clip paths for both cameras; the external
  verifier passed against this shared manifest
- latest index-validation native artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_rolling_clip_index_20260509_index_native`

Latest GUI external IPC single-clip validation:

- artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_21_12_39_24`
- launch shape:
  `ORANGE_GUI_RECORDING_SINK_MODE=external_ipc ORANGE_PTP_REGISTER_READ_DECIMATE=100 ./scripts/run_gui_aq_off_validation.sh`
- `recording_session.json` reported `mode = "single_clip"`,
  `producer = "orange_gui_external_ipc"`, and
  `recording_backend.mode = "external_ipc"`
- `camera_artifacts.<serial>.video` points at
  `external_recorder/Cam<serial>_external.mp4`; root-level `Cam*.mp4` files
  are not required for this GUI external IPC layout
- Historical artifact note: this 2026-05-21 run predates the external
  single-clip frame-clock sidecar, so its metadata pointer targets the recorder
  summary JSON. New recordings instead point at
  `external_recorder/Cam<serial>_external_meta.csv`; completion requires one
  continuous row per encoded frame with both `timestamp` and `timestamp_sys`.
- both cameras recorded `1645` submitted/ACKed/encoded frames with no frame
  gaps, GetFrame errors, external IPC failures, or ACK timeouts
- the standard GUI validator now accepts this manifest shape and
  `scripts/validate_gui_ptp_recording.py --latest-complete` passes against the
  artifact
- latest index-validation external recorder artifact:
  `/tmp/orange_external_recorder_ptp_rolling_20260509_index_external`
- latest index-validation external analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_external_ipc_rolling_index_20260509_index_external`
- those runs wrote `recording_clip_index.json`, `recording_clip_index.csv`, and
  `recording_snapshot.json` index pointers; `scripts/verify_timed_recording.py`
  and `scripts/verify_external_recorder_session.py` both passed with index
  checks enabled

Latest packet-count index validation:

- native in-process rolling artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_rolling_packet_counts_20260509/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25__aq_off__tempaq_off__lookahead_off`
- native rows reported clip packet counts `200`, `225`, and `177` with
  `packet_count_source = "ffprobe_nb_read_packets"`
- supervised two-camera external IPC rolling analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_external_ipc_rolling_packet_counts_20260509_021533`
- external recorder artifact:
  `/tmp/orange_external_recorder_ptp_rolling_packet_counts_20260509`
- both external cameras received/ACKed/encoded `601` frames, wrote four rolling
  clips with packet counts `200`, `200`, `200`, and `1`, and passed
  `scripts/verify_external_recorder_session.py`

Latest terminal-tail coalescing validation:

- supervised two-camera external IPC rolling analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_external_ipc_rolling_tail_coalesce_20260509_120903`
- external recorder artifact:
  `/tmp/orange_external_recorder_ptp_rolling`
- both external cameras received/ACKed/encoded `601` frames with `0` encode
  drops
- the recorder coalesced the one-frame post-duration tail into the final clip:
  `1-200`, `201-400`, and `401-601`
- recorder summaries reported `target_frame_count = 600`,
  `terminal_tail_coalesced_frames = 1`, and
  `terminal_tail_coalesce_frames = 25`
- analytics `recording_clip_index.json` reported packet counts `200`, `200`,
  and `201` per camera, all sourced from
  `external_recorder_summary.packets_written`
- `scripts/verify_external_recorder_session.py` passed and now checks that a
  tiny terminal tail does not become a standalone clip

## Remaining Work

- Add live GUI validation/soak coverage for external-recorder rolling
  finalization.
- Add live GUI validation/soak coverage for external-recorder rolling status.
  The live heartbeat/status slice surfaces current clip index, next rollover
  frame, and last completed clip outcome from the external recorder sidecar.
  Offline verifiers now check those sidecar fields against recorder summaries
  and the parsed supervisor runtime snapshot when rolling output is present.
- Add live GUI validation/soak coverage for crop rolling finalization. Offline
  GUI validation now checks per-clip crop video, metadata, perf, keyframe rows,
  and total row agreement, but this still needs a real four-camera GUI rolling
  artifact before it should be treated as production-proven.
- Add direct muxer-reported packet counters if they become available; current
  native indexes use ffprobe after finalization and external IPC indexes use
  recorder summary `packets_written`.
