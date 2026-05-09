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
  only, using conservative drain/rearm rollover.
- GUI/session rolling supervision and external-recorder rolling supervision are
  still future work.

The current headless rolling implementation is intentionally not seamless:

- it stops recording at the clip boundary,
- waits for the active encoders/writers to drain,
- writes the finalized clip manifest,
- arms the next clip folder,
- resumes recording while acquisition continues.

The manifest records this with `rollover.implementation =
"headless_drain_rearm"`, `seamless_writer_switch = false`, and
`records_during_drain_gap = false`.

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

Future seamless rollover should add session-level frame/status CSVs. The current
headless drain/rearm slice keeps cross-clip continuity in the per-clip metadata
and the parent `recording_session.json`.

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
    "record_for_seconds": 12,
    "clip_seconds": 6
  },
  "rollover": {
    "implementation": "headless_drain_rearm",
    "seamless_writer_switch": false,
    "records_during_drain_gap": false,
    "note": "headless rolling clips currently rotate by draining the current clip and arming the next clip; seamless GOP-boundary writer switching is not implemented yet."
  },
  "recording": {
    "started": true,
    "stop_requested": true,
    "stop_reason": "record_for_seconds_elapsed",
    "drain_completed": true,
    "actual_recording_duration_s": 12.0,
    "sum_clip_actual_duration_s": 11.9
  },
  "stream": {
    "requested_duration_seconds": 20,
    "actual_elapsed_s": 20.0,
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
      "final_clip": false
    },
    {
      "clip_index": 1,
      "clip_id": "clip_000001",
      "directory": "clips/clip_000001",
      "status": "completed",
      "start_reason": "rollover",
      "stop_reason": "record_for_seconds_elapsed",
      "drain_completed": true,
      "final_clip": true
    }
  ]
}
```

The top-level manifest is the stable discovery entrypoint. Consumers should use
the `clips[*].directory` entries to enumerate clip folders and then read each
clip's manifest for per-camera frame ranges.

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
- Current drain/rearm rollover may produce a small intentional recording gap
  while the previous clip drains and the next writer opens. The manifest marks
  this with `records_during_drain_gap = false`.

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
- `runs.json` health/pass fields when present,
- total ffprobe video duration within tolerance.

Latest validated headless rolling smoke:

- artifact:
  `/tmp/orange_rolling_spec_validation_bt11/2010096_headless_rolling_clip_smoke_a16_gpu5_bt11`
- `record_for_seconds = 12`, `clip_seconds = 6`, stream duration `20 s`
- `summary.json`: `pass_runs = 1`, `fail_runs = 0`
- camera health: `0` frame-ID gaps, `0` GetFrame errors, `0` encode failures,
  `0` preprocess drops
- clips: `clip_000000` ffprobe duration `6.000 s`, `clip_000001` `6.000 s`
- both clips start with keyframe frame `0` after forcing IDR/SPS/PPS on each
  newly opened clip
- verifier total: `12.000 s` for a requested `12.000 s`

## Remaining Work

- Implement seamless GOP-boundary writer switching so recording continues during
  rollover without the conservative drain/rearm gap.
- Add GUI/session controls and validation for rolling clips.
- Add external-recorder rolling supervision using the same manifest contract.
- Add session-level frame/status CSVs once seamless rollover needs richer
  cross-clip indexing.
