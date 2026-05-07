# Recording Session Manifest Contract

## Purpose

This document defines the recording-session artifact contract Orange should use
for both the current single-video layout and the future rolling-clip layout.

The immediate implementation rule is conservative:

- `clip_seconds = 0` keeps the current flat recording layout.
- `clip_seconds > 0` is rejected with a clear error until rollover is actually
  implemented.

This prevents consumers from treating a partially implemented rolling layout as
production-ready.

## Modes

### Single-Video Layout

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
consumers. The current headless `recording_control.record_for_seconds` slice
uses this layout and writes a `recording_session.json` manifest with
`mode = "single_clip"`.

### Rolling-Clip Layout

When rollover is implemented and `clip_seconds > 0`, use a parent session folder
with per-clip subfolders:

```text
<recording_folder>/
  recording_session.json
  recording_snapshot.json
  ptp_sync_summary.json
  session_events.jsonl
  Cam2010095_session_frames.csv
  Cam2010095_session_status.csv
  Cam2010096_session_frames.csv
  Cam2010096_session_status.csv

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

Rationale:

- Each clip folder is self-contained enough to inspect or copy independently.
- The parent folder remains the session discovery root.
- Existing single-video recordings keep their current flat file contract.
- Session-level CSVs provide cross-clip continuity without forcing consumers to
  scan every per-clip metadata file first.

## Session Manifest

Future rolling mode should use this top-level shape:

```json
{
  "schema_id": "orange.recording_session",
  "schema_version": 1,
  "session_id": "2026_05_07_21_30_00",
  "mode": "rolling_clips",
  "status": "completed",
  "recording_folder": "/abs/path/to/session",
  "created_at_utc": "2026-05-07T21:30:00Z",
  "updated_at_utc": "2026-05-09T21:30:03Z",
  "recording_control": {
    "record_for_seconds": 172800,
    "clip_seconds": 1800,
    "rollover_policy": "gop_boundary",
    "allow_frame_gap_between_clips": false,
    "align_to_wall_clock": false
  },
  "timing": {
    "started_at_utc": "2026-05-07T21:30:00Z",
    "stop_requested_at_utc": "2026-05-09T21:30:00Z",
    "finalized_at_utc": "2026-05-09T21:30:03Z",
    "actual_recording_duration_s": 172800.2,
    "drain_duration_s": 3.1
  },
  "cameras": {
    "2010095": {
      "camera_serial": "2010095",
      "snapshot_camera_key": "2010095",
      "session_frames_csv": "Cam2010095_session_frames.csv",
      "session_status_csv": "Cam2010095_session_status.csv"
    },
    "2010096": {
      "camera_serial": "2010096",
      "snapshot_camera_key": "2010096",
      "session_frames_csv": "Cam2010096_session_frames.csv",
      "session_status_csv": "Cam2010096_session_status.csv"
    }
  },
  "clips": [
    {
      "clip_index": 0,
      "clip_id": "clip_000000",
      "path": "clips/clip_000000/clip_manifest.json",
      "directory": "clips/clip_000000",
      "start_reason": "recording_start",
      "stop_reason": "clip_seconds_elapsed",
      "status": "finalized"
    },
    {
      "clip_index": 1,
      "clip_id": "clip_000001",
      "path": "clips/clip_000001/clip_manifest.json",
      "directory": "clips/clip_000001",
      "start_reason": "rollover",
      "stop_reason": "clip_seconds_elapsed",
      "status": "finalized"
    }
  ]
}
```

The top-level session manifest is the stable discovery entrypoint. It should not
duplicate all per-camera frame ranges for every clip; that belongs in each
`clip_manifest.json` and the session frame index CSVs.

## Clip Manifest

Each rolling clip should write:

```text
clips/clip_000000/clip_manifest.json
```

Recommended shape:

```json
{
  "schema_id": "orange.recording_clip",
  "schema_version": 1,
  "session_id": "2026_05_07_21_30_00",
  "clip_id": "clip_000000",
  "clip_index": 0,
  "status": "finalized",
  "directory": "clips/clip_000000",
  "started_at_utc": "2026-05-07T21:30:00Z",
  "stop_requested_at_utc": "2026-05-07T22:00:00Z",
  "finalized_at_utc": "2026-05-07T22:00:01Z",
  "start_reason": "recording_start",
  "stop_reason": "clip_seconds_elapsed",
  "rollover_boundary": {
    "policy": "gop_boundary",
    "requested_clip_seconds": 1800,
    "boundary_recording_frame_id": 180000,
    "idempotency_key": "clip_000000_to_clip_000001"
  },
  "cameras": {
    "2010095": {
      "status": "finalized",
      "video": "Cam2010095.mp4",
      "metadata": "Cam2010095_meta.csv",
      "keyframes": "Cam2010095_keyframe.json",
      "first_recording_frame_id": 1,
      "last_recording_frame_id": 180000,
      "first_clip_frame_id": 1,
      "last_clip_frame_id": 180000,
      "first_camera_frame_id": 123456,
      "last_camera_frame_id": 303455,
      "frame_count": 180000,
      "frame_gaps": 0,
      "container_duration_s": 1800.0
    }
  }
}
```

## CSV Roles

Per-clip, per-camera metadata:

```text
clips/clip_000000/Cam2010095_meta.csv
```

This describes the encoded frames in that one MP4:

- `recording_frame_id`
- `clip_frame_id`
- `camera_frame_id`
- camera and host timestamps
- keyframe/GOP fields when available
- per-frame recording diagnostics needed to interpret that MP4

Session-level, per-camera frame index:

```text
Cam2010095_session_frames.csv
```

This is the cross-clip join table:

- `recording_frame_id`
- `clip_id`
- `clip_frame_id`
- `camera_frame_id`
- timestamp fields
- relative video path

Session-level, per-camera status:

```text
Cam2010095_session_status.csv
```

This is low-rate health/status telemetry across the whole recording session:

- acquisition FPS
- encode FPS
- queue depths
- dropped-frame and error counters
- PTP/cadence summaries
- encoder slow/fail counters

## Frame-Identity Rules

- `recording_frame_id` is session-continuous across clips.
- `clip_frame_id` resets to `1` for each clip.
- `camera_frame_id` remains the camera/vendor frame id.
- Consumers should join across clips with `recording_frame_id` first.
- A clip boundary must not intentionally create a `recording_frame_id` gap.

## Current Implementation Status

- Single-video headless `recording_control.record_for_seconds` is implemented.
- Current single-video layout remains flat for compatibility.
- Current headless timed recording writes `recording_session.json` with
  `mode = "single_clip"`.
- `clip_seconds > 0` is rejected during experiment-spec validation with a clear
  "rolling clips are not implemented yet" error.
- Rolling writer rollover, per-clip directories, session frame CSVs, and
  session status CSVs are future work.
