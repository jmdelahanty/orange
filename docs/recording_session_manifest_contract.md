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
- GUI/session rolling supervision and external-recorder rolling supervision are
  still future work. External IPC contracts and GUI fail-fast manifests carry
  `recording_control` and `rollover` metadata, but external IPC rejects
  `clip_seconds > 0` until the recorder owns GOP-boundary rollover.

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

Future multi-day production rollover should add session-level frame/status CSVs.
The current headless slice keeps cross-clip continuity in the per-clip metadata
and records per-clip frame ranges in the parent `recording_session.json`.

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
      "recording_frame_id_gaps": 0
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

## Remaining Work

- Add GUI/session controls and validation for rolling clips.
- Add external-recorder rolling supervision using the same manifest contract.
- Add session-level frame/status CSVs for easier downstream indexing across
  many clips.
