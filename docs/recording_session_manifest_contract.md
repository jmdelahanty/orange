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
- GUI/session rolling supervision is still future work. GUI fail-fast manifests
  carry `recording_control` and `rollover` metadata, but the GUI path refuses
  external recorder supervision until lifecycle, drain, and finalization state
  are wired through the session UI.

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

## Single-Video Layout

When `clip_seconds = 0`, keep the existing structure:

```text
<recording_folder>/
  recording_session.json
  recording_snapshot.json
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

## Rolling-Clip Layout

When headless `recording_control.clip_seconds > 0`, the parent session folder is
the discovery root and each clip gets its own subfolder:

```text
<recording_folder>/
  recording_session.json
  recording_snapshot.json
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
  Cam2010096_external_shard0_gpu5.mp4
  Cam2010096_external_shard1_gpu6.mp4

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
shards, so per-shard MP4s remain diagnostic outputs while the merged MP4 and
clip MP4s are the consumer-facing media.

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
- `camera_artifacts.<serial>.metadata` points at the external recorder summary
  JSON, not a per-frame `Cam*_meta.csv`; frame-count validation must therefore
  compare `camera_artifacts.<serial>.frame_count` to the external summary's
  `frames_received`, `acks_sent`, and `frames_encoded`
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

- Add GUI/session controls and validation for rolling clips.
- Carry external-recorder rolling supervision into the GUI/session lifecycle.
- Add direct muxer-reported packet counters if they become available; current
  native indexes use ffprobe after finalization and external IPC indexes use
  recorder summary `packets_written`.
