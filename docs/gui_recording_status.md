# GUI Recording Status

## Purpose

This note captures the current state of GUI-driven recording on
`exp/gop-split-a16` after the recent split-GOP, schema-3 config, app-storage,
validation, and modularization work.

It is a status snapshot, not a full design document.

## Current Architecture

### Config Sources

The GUI currently uses three relevant config layers:

1. Per-camera config loaded from:
   - `~/orange_data/config/local/<folder>/<serial>.json`
2. Session-wide GUI recording controls:
   - codec
   - preset
   - tuning
   - rate control
   - quality
   - GOP
   - recording output shape
3. App storage config loaded from:
   - `~/orange_data/config/app/default.json`

### Runtime Flow

Current GUI recording behavior is split into:

- `src/gui/recording_panel.*`
  - session-wide recording controls
  - bitrate summary
  - per-camera resize overrides
  - advanced validation summary
- `src/session/recording_session.*`
  - stream-time recording pipeline creation
  - recording pipeline start/stop requests
  - recording pipeline shutdown
- `src/orange.cpp`
  - record start/stop toggle state machine
  - recording folder initialization
  - snapshot / metadata creation
  - remaining app-shell orchestration

This means stream start still owns pipeline lifetime, while the record button
only controls whether those pipelines are actively writing.

## What Is Implemented

### Schema-3 Camera Recording Config

The GUI can now load schema-3 camera configs through normal local config
folders under `~/orange_data/config/local`.

The validated local test folder used so far is:

- `~/orange_data/config/local/100_cam4`

That folder currently contains:

- `2010095.json`
- `2010096.json`

Both are copied from the validated branch-local schema-3 configs.

There is also a deliberate negative-test folder:

- `~/orange_data/config/local/100_cam4_invalid_no_helper`

That folder currently contains:

- `2010096.json`

and is intentionally invalid for the current split-GOP GUI validation because
it sets:

- `source_gpu_id = 5`
- `recording.split_gop.encoder_gpu_ids = [5]`

which resolves to no non-source helper GPU.

### GUI Recording Controls Sync From Camera Defaults

When cameras are opened, the GUI now attempts to populate the session-wide
recording controls from the loaded camera configs.

That sync currently applies only when:

- all open cameras use schema-3 configs
- all open cameras agree on recording encode defaults
- all open cameras agree on recording output defaults

If those conditions are not met, the GUI leaves the current session controls
unchanged and shows a warning/status line in the recording panel.

### Split-GOP Validation And Preflight

The GUI now has:

- a read-only advanced split-GOP validation summary
- shared validation logic with unit tests
- preflight gating before `Start streaming`
- preflight gating before record start

The same validation is also reused in headless mode.

### App Storage / Latest Recording Pointers

The branch now supports:

- local pointer:
  - `<recording base>/.orange/latest_recording.json`
- canonical durable pointer:
  - `~/orange_data/.orange/latest_recording.json`
- live IPC pointer:
  - `/run/orange/latest_recording.json`

`/run/orange` is expected to be provisioned via `tmpfiles.d`, not ad hoc.

## Validated Behavior

### Headless

Validated split-GOP runs already exist for:

- `2010096` on `GPU5 + GPU6`
- `2010095` on `GPU1 + GPU2`

Validated settings:

- `hevc`
- `100 fps`
- `gop=25`
- `split_gop`
- `multi_gpu`
- `hybrid_split`
- `raw`

The final pipeline-sample shutdown fix was also rerun and validated in headless
mode using:

- `/home/jeremy/orange_data/exp/unsorted/2010096_split_gop_hevc_100fps_gop25_a16_gpu5_6_snapshotcheck1`

That rerun confirmed that the snapshot now carries the true final helper
routing state instead of lagging the last periodic sample. In the validated
artifact:

- `helper_requested_frames = 700`
- `helper_dispatched_frames = 700`
- `helper_fallback_frames = 0`
- `source_to_helper_copy_samples_total = 700`
- `latency.source_to_helper_copy.samples = 700`

Those values also match the final row in `Cam2010096_pipeline_perf.csv`.

### GUI

GUI startup and non-split recording path still work after the modularization
and validation changes.

A recent GUI recording run also confirmed that the GUI now honors the
schema-3 camera defaults for `2010096`.

Validated GUI artifact:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_17_11_28_46`

Confirmed from that run:

- config source:
  - `~/orange_data/config/local/100_cam4/2010096.json`
- runtime encode:
  - `codec = hevc`
  - `gop_length = 25`
  - `idr_period = 25`
- runtime strategy:
  - `mode = split_gop`
  - helper GPU ids `[5, 6]`
  - topology `PIX`
- helper path was active:
  - nonzero `source_to_helper_copy_samples_total`
  - nonzero `helper_dispatched_frames`
  - helper lane visible in `Cam2010096_pipeline_perf.csv`

Additional GUI checks completed:

- non-split recording still starts, records, stops, and drains cleanly
- the `100_cam4` schema-3 config folder is selectable in the normal GUI local
  config flow
- the GUI session recording controls now pick up the schema-3 camera encode and
  output defaults on camera open
- the negative-test folder `100_cam4_invalid_no_helper` immediately shows the
  split-GOP validation failure in red when `record=true` is selected

What is still not manually confirmed yet:

- a fresh post-`eaf8619` GUI run proving the final pipeline sample makes the
  routing totals in `recording_snapshot.json` line up with the true final
  shutdown state
- an explicit GUI attempt to press `Start streaming` with the invalid
  `100_cam4_invalid_no_helper` config and confirm the hard preflight gate blocks
  the session, not just the read-only validation summary

## Known Caveats

### Routing Totals Vs Copy Sample Totals

The pipeline routing counters and the helper-copy latency sample count are
different metrics:

- routing totals come from `RecordingIngress`
- helper-copy samples come from measured cross-GPU copy timings

There was also a small reporting-timing mismatch because the pipeline snapshot
used the last 1 Hz sample instead of a final exact sample.

That has now been tightened by forcing one final pipeline sample at acquire
thread shutdown.

Status of that fix:

- implemented
- build-verified
- runtime-validated in headless mode
- not yet rerun-validated in the GUI path

### Remaining Monolith In `orange.cpp`

The stream-time recording pipeline lifecycle has been extracted, but the record
start/stop metadata flow still lives in `src/orange.cpp`.

That remaining work includes:

- recording folder initialization
- snapshot creation
- PTP sync summary initialization
- `record_video` / `stop_record` / `recording_draining` transitions

## Recommended Next Steps

1. Rerun one short GUI split-GOP recording after the final pipeline-sample fix.
   Goal:
   confirm that routing totals in the snapshot now reflect the true final
   state.

2. Attempt `Start streaming` with the invalid `100_cam4_invalid_no_helper`
   config.
   Goal:
   confirm the hard preflight gate blocks streaming, not just that the
   validation summary turns red.

3. Extract record start/stop metadata flow into `src/session/recording_session.*`.
   Goal:
   further thin `src/orange.cpp` and align with the modularization plan.

4. Add editable advanced per-camera split-GOP controls.
   Goal:
   move beyond read-only validation summary while keeping the same safety
   checks.

## Related Docs

- [recording_panel_modularization_plan.md](/home/jeremy/orange-gop-split-a16/docs/recording_panel_modularization_plan.md)
- [gui_recording_config_todo.md](/home/jeremy/orange-gop-split-a16/docs/gui_recording_config_todo.md)
- [advanced_recording_validation_plan.md](/home/jeremy/orange-gop-split-a16/docs/advanced_recording_validation_plan.md)
