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
  - recording folder initialization
  - snapshot / metadata initialization
  - record stop / drain transitions
- `src/orange.cpp`
  - top-level record start/stop toggle wiring
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

Headless multi-camera experiment-spec runs are now also validated
structurally.

During that work, a real bug was found and fixed in the headless experiment
path:

- multi-camera camera selections were serialized with `,`
- the headless encoder setup parser also tokenized on `,`
- this corrupted runs like `camera=2010095,2010096`
- fix committed as:
  - `8bb335a` `headless: fix multi-camera encoder setup parsing`

After that fix:

- one experiment spec can select both `2010095` and `2010096`
- each camera can keep its own split-GOP PIX pair
- both helper lanes come up correctly in one shared session

Current dual-camera headless findings:

- `2 x 100 fps` is not stable
- `2 x 80 fps` is currently the validated dual-camera baseline

`2 x 100 fps` headless artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_rerun2`

That run proved the structure works, but both cameras underperformed badly:

- `2010095`: `enc_fps_mean = 66.5072`
- `2010096`: `enc_fps_mean = 68.5529`

`2 x 80 fps` headless artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_rerun1`

That run completed cleanly in `free_run` mode and is the current best
validated dual-camera headless baseline:

- `2010095`: `enc_fps_mean = 80.0002`
- `2010096`: `enc_fps_mean = 79.6502`
- no split-GOP backlog overflow
- helper routing remained balanced on both cameras

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
- pressing `Start streaming` with `100_cam4_invalid_no_helper` is blocked by the
  hard preflight gate
- a fresh post-`eaf8619` GUI run validated the final pipeline-sample fix using:
  - `/home/jeremy/orange_data/exp/unsorted/2026_04_17_12_04_52`
- that GUI artifact confirms the final routed/helper copy counts line up:
  - `helper_requested_frames = 175`
  - `helper_dispatched_frames = 175`
  - `helper_fallback_frames = 0`
  - `source_to_helper_copy_samples_total = 175`
  - `latency.source_to_helper_copy.samples = 175`

GUI dual-camera split-GOP was also exercised with:

- `2010095`
- `2010096`
- their disjoint PIX pairs

The current result is:

- multi-camera GUI recording is structurally working
- `2 x 100 fps` is not currently stable

Relevant GUI artifact:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_17_12_50_42`

That run showed:

- both cameras opened and recorded in one session
- both cameras used `split_gop`
- both helper paths were active
- `2010096` collapsed much harder than `2010095`

The important failure signal in that run is not just low FPS. It is explicit
split-GOP backlog overflow:

- `2010095`: `peak_backlog_gops = 4`, `overflow_events = 53`
- `2010096`: `peak_backlog_gops = 5`, `overflow_events = 83`

So the current GUI conclusion matches headless:

- multi-camera split-GOP works structurally
- `2 x 100 fps` is not yet a validated operating point

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
- runtime-validated in the GUI path

### Dual-Camera Split-GOP Limit

The branch now has a clear current boundary:

- one camera at `100 fps` on its own PIX split-GOP pair is validated
- two cameras at `100 fps` on disjoint PIX split-GOP pairs are not yet
  validated
- two cameras at `80 fps` on disjoint PIX split-GOP pairs do work in headless

For the failed `2 x 100 fps` cases, the dominant recorded failure mode is:

- split-GOP pending GOP backlog overflow

That is captured directly in `recording_snapshot.json` under:

- `recording_strategy.split_gop.pending_gop_buffer`

### Headless PTP Status

The checked-in validated camera configs are currently:

- `sync_mode = free_run`
- `ptp.enabled = false`

The local headless experiment runner now supports:

- `fixed.sync_mode = free_run`
- `fixed.sync_mode = ptp_gate`

And headless `ptp_gate` runs now:

- preflight the host linuxptp stack through `scripts/ptp_stack.sh status`
- auto-start the host stack when needed before camera open
- auto-stop it on exit only when the run started it from an empty state

Current `ptp_gate` artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_rerun7`

That run confirms:

- host linuxptp setup is no longer the blocker
- both cameras open and cross the local PTP gate
- GPU assignment is not the issue:
  - `2010095` uses source GPU `1` with split-GOP pair `[1, 2]`
  - `2010096` uses source GPU `5` with split-GOP pair `[5, 6]`

But performance is still poor under local PTP gating:

- `2010095`: `enc_fps_mean = 55.2923`, `dropped_frames_camera = 266`
- `2010096`: `enc_fps_mean = 55.313`, `dropped_frames_camera = 266`

This means:

- the current `2 x 80 fps` dual-camera baseline is useful for throughput
  characterization
- headless `ptp_gate` no longer has a setup gap
- but dual-camera `2 x 80 fps` under local PTP currently underperforms badly and
  is not yet a validated synchronized baseline

### Remaining Monolith In `orange.cpp`

The recording/session path is much thinner now, but `src/orange.cpp` still
owns top-level GUI orchestration around:

- app-shell state transitions
- stream button wiring
- record button wiring
- non-recording panel coordination

## Recommended Next Steps

1. Validate the new editable advanced split-GOP controls in the GUI.
   Goal:
   confirm the new per-camera editing surface behaves correctly with the
   existing preflight guardrails.

2. Tighten experiment pass/fail policy for multi-camera overload cases.
   Goal:
   make runs like the `2 x 100 fps` dual-camera failure show up as failures in
   `runs.csv`, not misleading passes.

3. Harden the local PTP-gated startup path.
   Goal:
   make headless synchronized benchmarks directly comparable to GUI PTP runs by
   fixing the remaining performance/stability gap after gate start.

4. Add an app-level GUI surface for storage defaults and latest-recording
   pointer settings.
   Goal:
   make the new app config discoverable without requiring direct JSON edits.

## Related Docs

- [recording_panel_modularization_plan.md](/home/jeremy/orange-gop-split-a16/docs/recording_panel_modularization_plan.md)
- [gui_recording_config_todo.md](/home/jeremy/orange-gop-split-a16/docs/gui_recording_config_todo.md)
- [advanced_recording_validation_plan.md](/home/jeremy/orange-gop-split-a16/docs/advanced_recording_validation_plan.md)
- [session_orchestration_architecture.md](/home/jeremy/orange-gop-split-a16/docs/session_orchestration_architecture.md)
