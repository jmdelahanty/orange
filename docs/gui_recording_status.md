# GUI Recording Status

## Purpose

This note captures the current state of GUI-driven recording on
`exp/gop-split-a16` after the recent split-GOP, schema-3 config, app-storage,
validation, and modularization work.

It is a status snapshot, not a full design document.

Related note:

- `docs/multi_camera_failure_modes.md`
- `docs/gui_display_recording_buffer_ownership_plan.md`

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

### Stop-Recording Drain While Streaming

The GUI record stop path now explicitly requests a recording drain instead of
waiting for the later `Stop streaming` teardown to wake the recording workers.

Current behavior:

- pressing the record pause/stop button sets recording to off and enters a
  draining state
- the active recording pipelines are woken with drain sentinels while camera
  streaming remains live
- full-frame encoders finalize queued frames once their preprocess/helper
  queues are drained
- crop/pose drain wakeups are chained from the crop producer path when those
  workers are present
- the record button blocks a new recording start while `recording_draining` is
  still true
- `Stop streaming` uses the same drain request before worker teardown, so the
  shutdown path remains compatible with older operator behavior

Headless shutdown now follows the same ordering: request recording drain, wait
for active recorders to reach zero, then stop acquisition. This prevents a
recording drain from depending on acquisition-stop side effects.

### App Storage / Latest Recording Pointers

The branch now supports:

- local pointer:
  - `<recording base>/.orange/latest_recording.json`
- canonical durable pointer:
  - `~/orange_data/.orange/latest_recording.json`
- live IPC pointer:
  - `/run/orange/latest_recording.json`

`/run/orange` is expected to be provisioned via `tmpfiles.d`, not ad hoc.

The latest-recording pointer also includes
`recording_session_manifest_path`, pointing at
`<recording_folder>/recording_session.json`.

### GUI Recording Session Manifest

In-process GUI recordings now write the shared
`orange.recording_session` single-clip manifest after the recording drain
completes. The manifest uses `producer = "orange_gui"` and includes per-camera
video, metadata, keyframe, frame-count, frame-range, and packet-count fields.
`recording_snapshot.json` is updated at the same time with
`session.recording_mode = "single_clip"`,
`session.recording_session_manifest_path`, and
`session.recording_session_status = "completed"`.

GUI external IPC recordings now use the supervised external recorder lifecycle
for the first process-isolated recording slice. The GUI starts recorder
processes on record start, drains IPC handoff queues on stop, stops the
recorders during finalization, and writes a shared single-clip manifest with
`producer = "orange_gui_external_ipc"` and
`recording_backend.mode = "external_ipc"`. The first two-camera hardware GUI
validation passed on 2026-05-21. The standard GUI validator now follows the
external video paths in `recording_session.json`, so GUI external IPC artifacts
do not need root-level `Cam*.mp4` files.

### GUI PTP Register-Read Decimation

`recording.ptp_register_read_decimate` in app storage can now provide the GUI
default for `ORANGE_PTP_REGISTER_READ_DECIMATE`. The environment variable still
wins when set explicitly; otherwise the GUI applies the app-storage value before
starting acquisition threads.

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

- `2 x 100 fps` is validated for the recabled A16 headless `free_run`
  split-GOP HEVC path after commit `951f910`
- `2 x 100 fps` is also validated for the recabled A16 headless no-stagger
  `ptp_gate` split-GOP HEVC path in short runs after commit `951f910`
- `2 x 100 fps` GUI PTP/AQ-off recording is validated for the local
  `100_cam4_ptp` setup as of artifact
  `/home/jeremy/orange_data/exp/unsorted/2026_04_25_18_22_25`
- `2 x 80 fps` remains a useful lower-rate baseline
- positive-detection crop/pose/track latency still needs separate validation
  with a detectable subject

Historical pre-recable/pre-fix `2 x 100 fps` headless artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_rerun2`

That historical run proved the structure works, but both cameras underperformed
badly:

- `2010095`: `enc_fps_mean = 66.5072`
- `2010096`: `enc_fps_mean = 68.5529`

Current clean recabled `2 x 100 fps` headless artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch`

That run completed with both cameras at `1001` frames, `0` frame-ID gaps,
`0` GetFrame errors, `0` preprocess drops, and `0` encode failures.

Current clean recabled `2 x 100 fps` headless no-stagger `ptp_gate` artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_ptp_real_recabled_stable_frame_patch_12s`

That run completed with `2010095` at `1001` submitted frames and `2010096` at
`1000` submitted frames. Both cameras reported `0` frame-ID gaps, `0` GetFrame
errors, `0` preprocess drops, and `0` encode failures.

Current clean GUI `2 x 100 fps` PTP/AQ-off artifact:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_25_18_22_25`

That run used local config folder `100_cam4_ptp`, recorded
`sync_mode = ptp_gate`, and produced real-content full-frame HEVC videos at
about `150 Mbps` on both cameras. Both cameras reported `0` camera frame-ID
gaps, `0` GetFrame errors, `0` preprocess drops, and `0` encode failures.
YOLO detect p95 remained about `11-12 ms`, matching the headless PTP profile;
the remaining tail is therefore not a GUI-only regression.

`2 x 80 fps` headless artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_rerun1`

That run completed cleanly in `free_run` mode and is the current best
validated dual-camera headless baseline:

- `2010095`: `enc_fps_mean = 80.0002`
- `2010096`: `enc_fps_mean = 79.6502`
- no split-GOP backlog overflow
- helper routing remained balanced on both cameras

For a compact taxonomy of the current multi-camera failure modes and what has
been ruled out, see:

- `docs/multi_camera_failure_modes.md`

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

Post-drain-fix GUI drain validation:

- `/home/jeremy/orange_data/exp/unsorted/2026_05_06_20_32_36`

Operator result:

- pressing the record pause/stop button drained/finalized the current recording
  without requiring a later `Stop streaming` click
- no GUI hang was observed

Artifact summary:

- sync summary reported `mode = ptp_local`
- both cameras recorded `463` valid `4512 x 4512` frames, duration `4.630 s`
- full-frame video bitrates were about `154.2 Mbps` for `2010095` and
  `152.8 Mbps` for `2010096`
- both cameras reported `0` frame-ID gaps, `0` GetFrame errors, `0` preprocess
  drops, and `0` encode failures
- both cameras reported `enc_slow = 5`
- YOLO detect p95 was `12.193 ms` for `2010095` and `12.020 ms` for `2010096`
- this was a drain/lifecycle validation run; it used PTP register-read
  decimation `1`, so it should not be counted as validation of the newer
  decimated hot-path setting

GUI validation with launcher defaults:

- `/home/jeremy/orange_data/exp/unsorted/2026_05_06_20_39_47`

This run used `./scripts/run_gui_aq_off_validation.sh`, so the GUI inherited
the current launcher defaults:

- `ORANGE_PTP_REGISTER_READ_DECIMATE=100`
- `ORANGE_YOLO_DETACH_INPUT=1`
- `ORANGE_DEFAULT_DETECT_ENGINE` set to the high-effort A16 TensorRT detect
  engine candidate

Artifact summary:

- both cameras recorded valid `4512 x 4512` videos at about `151.6 Mbps`
- `2010095`: `615` frames, `0` gaps, `0` GetFrame errors, `0` preprocess
  drops, `0` encode failures, `enc_slow = 29`
- `2010096`: `614` frames, `0` gaps, `0` GetFrame errors, `0` preprocess
  drops, `0` encode failures, `enc_slow = 37`
- PTP register reads dropped to `14` per camera with
  `ptp_register_read_decimate = 100`
- `acquisition_to_ptp_done_ms p95` was `0.000 ms` for both cameras, confirming
  that the per-frame PTP register polling was removed from the hot path
- YOLO detect p95 still remained high: `11.149 ms` for `2010095` and
  `10.486 ms` for `2010096`
- the remaining p95 tail moved to YOLO preprocess/submission/completion timing:
  `cpu_pre_sync_ms p95 = 8.621 ms / 8.261 ms`, with YOLO queue wait still low
  at about `0.12 ms p95`

Interpretation:

- the GUI launcher now validates the decimated PTP register-read path
- the old PTP register polling cost is no longer visible in
  `acquisition_to_ptp_done_ms`
- decimation improves the old `12 ms` GUI run only modestly because the GUI is
  still using in-process full-frame split-GOP recording
- this does not match the much faster headless external-recorder profile; the
  remaining GUI tail is still consistent with same-process/same-runtime
  recording contention rather than PTP polling or YOLO queue backlog

Drain-specific headless validation:

- `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_pose_noop_expected_fail_a16_gpu5_drain_smoke2`

That smoke predates the headless noop pose wiring and intentionally failed the
pose-noop expectation because no pose event log was produced. The recording
drain behavior itself was healthy: recording was toggled off before acquisition
stop, full-frame HW encoders finalized while the stream was still alive, and no
active-recorder drain timeout or `SharedRecordingOutput received packets before
open` warning was observed.

GUI dual-camera split-GOP was also exercised with:

- `2010095`
- `2010096`
- their disjoint PIX pairs

That historical in-process GUI result was:

- multi-camera GUI recording is structurally working
- `2 x 100 fps` in-process GUI split-GOP recording was not yet revalidated in
  that artifact after the recabled headless receive/requeue fix

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

So the conclusion for that historical in-process artifact is:

- multi-camera split-GOP works structurally
- this historical GUI artifact failed via split-GOP backlog overflow
- the newer clean `2 x 100 fps` GUI validation uses external IPC and is
  documented below; in-process split-GOP remains a separate baseline if needed

### External Recorder / Detect-Latency Status

Update (2026-05-04): the current best two-camera detect-latency results are
from the headless external-recorder path, not from the GUI path.

Relevant completed headless work:

- `recording_sink_mode = external_ipc` keeps full-frame encode/harvest outside
  the analytics process.
- `external_recorder_ipc_probe --shard-gpu-ids` now routes whole GOPs across
  recorder shards and writes a merged GOP-ordered MP4 per camera.
- `scripts/run_external_recorder_two_camera_ptp_smoke.sh` validates the local
  two-camera `100_cam4_ptp` topology with external split-GOP recording.
- `fixed.ptp_register_read_decimate = 100` keeps PTP gate sync and embedded
  frame timestamps while removing the per-frame camera-clock register read from
  the YOLO hot path.
- The high-effort A16 TensorRT detect engine candidate brought the latest
  long headless external-recorder steady detect p95 to about `3.95 ms` on both
  cameras.

Current GUI implication:

- GUI PTP/AQ-off in-process recording remains a validated production-like
  baseline.
- GUI has now been validated with decimated PTP register reads and the A16
  engine candidate; that removes PTP polling from the hot path but does not
  recover the headless external-recorder latency profile.
- GUI/session now recognizes `recording.sink_mode = "external_ipc"` from app
  config or `ORANGE_GUI_RECORDING_SINK_MODE=external_ipc`. On record start it
  writes the intended `external_recorder_contract.json`, starts supervised
  external recorder processes, and records
  `external_recorder_supervisor_plan.json`.
- GUI finalization now waits for the IPC handoff queues to drain, closes the
  socket connections, stops the supervised recorders, and writes
  `recording_session.json` from external recorder summaries.
- The first GUI external-recorder hardware validation passed:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_21_12_39_24`.
  The run used `ORANGE_GUI_RECORDING_SINK_MODE=external_ipc`,
  `ORANGE_PTP_REGISTER_READ_DECIMATE=100`, `100_cam4_ptp`, and the A16
  `640x640` TensorRT detect engine.
- Both cameras produced `1645` submitted/ACKed/encoded frames with
  `0` external IPC failures, `0` ACK timeouts, `0` frame-ID gaps,
  `0` GetFrame errors, and `0` encode failures.
- External MP4s were `4512x4512`, `100 fps`, `1645` frames, about
  `151.3 Mbps`, and decoded video sanity passed.
- YOLO steady detect p95 was `4.314 ms` for `2010095` and `4.227 ms` for
  `2010096`; YOLO queue p95 stayed `0.019/0.017 ms`.
- Four-camera GUI autorun external IPC validation passed on 2026-05-28:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_00_34_27`.
  This used `100_cam4_ptp_fourcam`, full-frame external IPC, external crop IPC,
  crop preview hidden, per-camera external crop recorder GPUs
  `2010093 -> 4`, `2010094 -> 2`, `2010095 -> 8`, `2010096 -> 6`, and the
  launcher auto-sized `ORANGE_CROP_FRAME_POOL_SIZE=128`.
- In that run all four full-frame MP4s were valid `4512x4512` videos with
  `1016` frames, all crop streams wrote `1016` metadata/perf/keyframe/video
  frames, crop fanout matched detection rows, and crop pool misses were `0`.
  The full validator passed with `0` warnings.
- A later four-camera GUI timing validation on 2026-05-28 used the same
  external full-frame/crop IPC shape at
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_01_05_07`. The artifact
  stayed healthy (`0` PTP gaps, `0` GetFrame errors, `0` encode/crop drops,
  valid full-frame and crop MP4s) and the GUI hidden-preview FPS gate passed
  with p05 `66.3 fps` and p50 `165.1 fps` when
  `ORANGE_GUI_SWAP_INTERVAL=0` was uncapped. The code now records
  `frame_max_fps` and the launcher defaults to `ORANGE_GUI_FRAME_MAX_FPS=60`
  so no-vsync validation is paced rather than unbounded.
- The main GUI refresh fix was not crop preview rendering: the recording panel
  had been running advanced split-GOP topology validation every frame. That
  work is now behind the `Advanced Recording Validation` tree expansion.
- The GUI now polls supervised recorder child processes while recording or
  draining and shows full-frame/crop recorder process and socket readiness in
  the status area. Unexpected nonzero or signal exits are carried into final
  `recording_session.json`/finalization failure reporting.
- The GUI also consumes the recorder-owned
  `orange.external_recorder.status` sidecar when present. It shows heartbeat
  coverage and recorder-side received/encoded frame totals, and invalid or
  failed status sidecars are surfaced as status-line errors. Remaining
  external-recorder health gaps are richer in-band protocol messages beyond
  this sidecar. The PTP stack guard exists in the validation launcher/wrapper
  path, but manual GUI operation still depends on the Host PTP Stack panel or
  an operator shell command before streaming PTP-gated cameras.

Earlier GUI external-recorder fail-fast artifact:

- `/home/jeremy/orange_data/exp/unsorted/2026_05_07_17_54_23`
- `recording_snapshot.json` reports `recording_sink_mode = "external_ipc"` and
  `full_frame_video_enabled = false`.
- `recording_session.json` reports `status = "failed"`,
  `recording_backend.status = "not_implemented"`, and the expected clear
  reason.
- The folder contains `external_recorder_contract.json` and
  `external_recorder_supervisor_plan.json`.
- No real full-frame `Cam*.mp4` files were written.

Current GUI validation tooling:

- `scripts/run_gui_aq_off_validation.sh` now prints the expected post-run
  validator commands, including expected `stream_downsample`,
  `display_preview_max_fps`, `swap_interval`, and `frame_max_fps`.
- `scripts/summarize_gui_validation.py --latest-complete` now includes an
  `External Recorder Status` section when recorder contracts are present. It
  reads full-frame and crop `status_json` heartbeat sidecars, the corresponding
  supervisor runtime JSON, and summary counts so compact run summaries expose
  recorder completion, heartbeat, ACK/received/encoded totals, storage
  preflight health, and count mismatches.
- External IPC validation commands now include
  `--require-external-recorder-storage-preflight`, so new runs fail if recorder
  summary/status sidecars or parsed runtime state omit storage preflight health.
- `scripts/validate_gui_ptp_recording.py <recording_folder>` validates one
  explicit GUI recording artifact.
- `scripts/validate_gui_ptp_recording.py --latest` validates the newest GUI
  artifact attempt under `/home/jeremy/orange_data/exp/unsorted`, including
  metadata-only/fail-fast folders. This is useful when the newest attempt might
  be incomplete and the failure reason matters.
- `scripts/validate_gui_ptp_recording.py --latest-complete` skips
  metadata-only folders and selects the newest direct child with
  `recording_snapshot.json` plus at least one camera that has video either as a
  root-level `Cam*.mp4` or through `recording_session.json` camera artifacts,
  plus `Cam*_pipeline_perf.csv` and `Cam*_yolo_perf.csv`.
- The validator defaults match the current GUI PTP target:
  `sync_mode = ptp_gate`, `ptp.enabled = true`, `ptp.mode = TwoStep`,
  `ptp_register_read_decimate = 100`, zero camera gaps/GetFrame errors/encode
  failures, valid decoded full-frame video content, and low YOLO queue wait.
- For GUI `external_ipc`, the validator accepts
  `producer = "orange_gui_external_ipc"`, validates frame counts against the
  external recorder summaries, and decodes the external MP4s referenced by
  `recording_session.json`.
- For GUI refresh checks, use the printed validator commands with
  `--require-gui-timing-telemetry`, `--expect-gui-swap-interval`, and
  `--expect-gui-frame-max-fps`. The four-camera validation launcher defaults
  to its fast display profile, `ORANGE_GUI_SWAP_INTERVAL=0`,
  `ORANGE_GUI_FRAME_MAX_FPS=60`, and `ORANGE_DISPLAY_PREVIEW_MAX_FPS=15`. If
  Citrus is actively using the same display GPU for `120 Hz` stimulus
  generation, use
  `scripts/run_gui_fourcam_external_ipc_validation.sh --citrus-display-safe`,
  which defaults Orange to `ORANGE_GUI_SWAP_INTERVAL=1`,
  `ORANGE_GUI_FRAME_MAX_FPS=30`, and `ORANGE_DISPLAY_PREVIEW_MAX_FPS=10`.
- The next GUI run should also visually confirm the new status timers:
  stream elapsed while streaming, active recording elapsed while recording, and
  finalizing elapsed during drain after the recording button is paused/stopped.

Current decision:

- the GUI is acceptable at its current capability level for now
- validated GUI external IPC recording can start, stop recording, stop
  streaming, drain workers, stop supervised recorders, and write the final
  `recording_session.json` without manual clicks when autorun is enabled
- GUI external IPC is now the validated low-latency process-isolated recording
  path for the two-camera `100_cam4_ptp` setup and the four-camera
  `100_cam4_ptp_fourcam` setup with crop recording enabled
- the remaining GUI detect-latency gap is specific to in-process recording
  contention, not PTP register polling or the GUI display lifecycle

### PTP Stack Operational Caveat

The 2026-05-21 GUI external IPC retry initially showed `Streaming FPS = 0` and
`YOLO FPS = 0` because the host PTP stack was stopped. Manual GUI runs should
use the `Host PTP Stack` panel to refresh status and click `Start PTP stack`
before opening or streaming PTP-gated cameras. Automated GUI validation uses
the privileged wrapper's `--ptp-stack-mode` path; for
`ORANGE_GUI_AUTORUN=1` with `ptp_gate` configs,
`scripts/run_gui_aq_off_validation.sh` defaults
`ORANGE_GUI_PTP_STACK_MODE=auto`, starts the stack if needed, and rechecks it
before launching Orange.

For manual shell checks, run:

```bash
sudo -n ./scripts/ptp_stack.sh status
```

If `ptp4l`/`phc2sys` or `/var/run/ptp4l` are missing, click `Start PTP stack`
in the GUI panel or start them from the shell:

```bash
sudo -n ./scripts/ptp_stack.sh start
sudo -n ./scripts/ptp_stack.sh status
```

Headless PTP runs may also repair this automatically. Keep the PTP stack
running while doing repeated GUI validation, then stop it explicitly when no
more PTP-gated runs are planned.

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
- two cameras at `100 fps` on recabled disjoint PIX split-GOP pairs are
  validated for headless `free_run` and no-stagger headless `ptp_gate`
- two cameras at `80 fps` on disjoint PIX split-GOP pairs do work in headless
- GUI `2 x 100 fps` still needs separate validation after the recabled
  stable-requeue fix

For the historical failed `2 x 100 fps` cases, the dominant recorded failure
mode is:

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

Current clean no-stagger `ptp_gate` recording artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_ptp_real_recabled_stable_frame_patch_12s`

That run confirms:

- host linuxptp setup is no longer the blocker
- both cameras open, cross the local PTP gate, and record real split-GOP HEVC
- the recabled GPU assignment and stable GPUDirect receive/requeue path are
  sufficient for a short `2 x 100 fps` synchronized run
- `2010095`: `1001` submitted frames, `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, `0` encode failures
- `2010096`: `1000` submitted frames, `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, `0` encode failures
- split-GOP output remained stable with `overflow_events = 0` and
  `peak_backlog_gops = 2`
- `ptp_sync_summary.json` reports sub-microsecond mean PTP offset and
  latch-minus-frame around `9.2 ms`

Remaining caveat:

- early startup `PTP_STALE_DUMP` logs still appear while encoders are coming up,
  but the steady-state summaries and artifacts show no frame loss, receive
  errors, preprocess drops, encode failures, or GOP overflow

Historical pre-recable/pre-fix PTP characterization artifacts:

- single-camera `80 fps` PTP:
  - `/home/jeremy/orange_data/exp/unsorted/2010096_split_gop_hevc_80fps_gop25_ptp_rerun1`
- dual-camera `80 fps` PTP, no stagger:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_rerun7`
- dual-camera `60 fps` PTP:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_60fps_gop25_dual_pix_ptp_rerun2`
- dual-camera `100 fps` PTP stream-only:
  - no recording artifacts are written for this mode, but the direct local
    benchmark completed cleanly with:
    - `2010095`: `902/902` frames, `0` camera drops, `99.908340 fps`
    - `2010096`: `902/902` frames, `0` camera drops, `99.910065 fps`
- dual-camera `80 fps` PTP with `2 ms` stagger:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_stagger2ms_rerun1`
- dual-camera `100 fps` PTP stagger sweep:
  - `25 us`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger25us_rerun1`
  - `50 us`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger50us_rerun1`
  - `100 us`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger100us_rerun1`
  - `250 us`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger0p25ms_rerun1`
  - `2 ms`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_rerun2`
  - swapped `2 ms` order:
    `/home/jeremy/orange_data/exp/unsorted/2010096_2010095_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_swaporder_rerun1`
  - experimental `Continuous` gate acquisition mode with `2 ms` stagger:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_continuous_rerun1`

Those historical runs showed:

- single-camera `80 fps` under `ptp_gate` is healthy
  - `2010096`: `enc_fps_mean ≈ 80`
  - `camera_dropped_frames = 0`
- dual-camera `60 fps` under `ptp_gate` is healthy
  - `2010095`: `enc_fps_mean = 61.2507`, `dropped_frames_camera = 0`
  - `2010096`: `enc_fps_mean = 61.2`, `dropped_frames_camera = 0`
- dual-camera `100 fps` under `ptp_gate` is also healthy when recording is
  disabled entirely
  - both cameras sustain about `100 fps`
  - camera drops remain `0`
- dual-camera `80 fps` under `ptp_gate` becomes healthy again when a small
  `2 ms` stagger is introduced between the cameras
  - `2010095`: `enc_fps_mean = 80.025`, `dropped_frames_camera = 0`
  - `2010096`: `enc_fps_mean = 80.0509`, `dropped_frames_camera = 0`
  - `overflow_events = 0` on both cameras
- historical dual-camera `100 fps` under `ptp_gate` did not become healthy with
  the nonzero staggers tried before recabling and the stable receive/requeue fix
  - `25 us`: both cameras collapse to about `2-4 fps`
  - `50 us`: both cameras collapse to about `2-4 fps`
  - `100 us`: the `0 ns` camera stays near `100 fps`, the offset camera
    collapses to about `2 fps`
  - `250 us`: the `0 ns` camera stays near `100 fps`, the offset camera
    collapses to about `7 fps`
  - `2 ms`: the `0 ns` camera stays near `100 fps`, the offset camera
    collapses to about `6 fps`
  - swapped `2 ms`: the failure follows the camera with the offset
  - experimental `Continuous` mode with `2 ms`: the `0 ns` camera still stays
    near `100 fps`, while the offset camera still collapses to about `7 fps`

Those older runs narrowed the then-current problem to a rate-sensitive
dual-camera synchronized interaction, not a general `ptp_gate` setup bug:

- single-camera `80 fps` PTP works
- dual-camera `60 fps` PTP works
- historical dual-camera `80 fps` PTP without stagger failed
- dual-camera `80 fps` `free_run` works
- dual-camera `100 fps` `ptp_gate` stream-only works
- dual-camera `80 fps` `ptp_gate` with a small stagger works
- historical dual-camera `100 fps` `ptp_gate` with nonzero stagger remained
  unstable
- switching the camera-side PTP gate acquisition mode from `MultiFrame` to
  experimental `Continuous` does not resolve the `100 fps` offset-camera
  instability by itself

This means:

- headless `ptp_gate` no longer has a setup gap
- current no-stagger recabled `2 x 100 fps` headless `ptp_gate` is validated in
  short recording runs
- the older nonzero-stagger artifacts remain historical evidence for a separate
  offset-camera stale-frame failure mode
- GUI `2 x 100 fps`, longer soaks, and more than two cameras still need
  validation before treating this as a broad production envelope

Follow-on diagnostic plan:

- `docs/ptp_recording_sink_experiment_plan.md`

Newest handoff-side probe:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_handoffprobe1`

That probe adds one more important constraint:

- the bad camera still goes stale only when recording is enabled
- but at first stale onset, the recent recording-submit history is still
  primary-only with:
  - no helper routing pressure yet
  - no preprocess waits/drops
  - near-full preprocess resource pools
  - only shallow queue depth at the acquisition-to-recording handoff

So the current narrowest read is:

- recording enable is part of the trigger
- but the first visible failure still happens upstream of the usual recording
  hot spots like helper routing, preprocess starvation, or output backlog

Newest helper-path baseline:

- `free_run` helper probe:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe5`
- `ptp_gate` helper probe:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe5`

Those runs used the lighter host-side helper sampler and showed:

- both modes still degrade to about `69-70 fps`
- the first helper-routed frames incur a large helper queue-wait spike
  in both modes
- helper worker service itself stays tiny

Representative first helper-frame timings:

- `free_run`:
  - queue wait about `28.5-28.8 ms`
  - worker service about `0.05-0.07 ms`
- `ptp_gate`:
  - queue wait about `33.3-33.6 ms`
  - worker service about `0.04-0.05 ms`

So the latest baseline suggests:

- helper preprocessing itself is not slow
- the helper path begins with a startup backlog when routing switches to the
  helper GPU at recording frame `101`
- `ptp_gate` is somewhat worse at onset, but the startup backlog is not unique
  to PTP in this probe

Newest sink results:

- `immediate_recycle`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_immediaterecycle_rerun2`
- `threaded_handoff_only`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_threadedhandoff_rerun2`

Those runs show:

- both sink modes sustain about `100 fps` on both cameras
- both have `0` camera drops
- neither reproduces the stale-frame failure

So the current read tightened again:

- bookkeeping alone is not enough
- a simple cross-thread recording handoff is not enough
- the bad `100 fps` PTP-stagger failure requires real downstream recording
  work

Current working hypothesis:

- this does not look like an average-bandwidth limit
- the stronger interpretation is an instantaneous burst-capacity or queueing
  problem
- `free_run` naturally smears the two cameras apart in time, while `ptp_gate`
  aligns them very tightly
- with recording enabled, those aligned arrivals likely create short bursts of
  shared work in transport, acquisition handoff, or recording-side queueing

That is why a small deliberate stagger between the two `ptp_gate` cameras is a
useful next diagnostic:

- both cameras would stay PTP-synchronized and keep the same nominal FPS
- but their frame arrivals would no longer land at exactly the same instant
- if a tiny offset restores throughput at `2 x 80 fps`, that strongly supports
  the synchronized-burst-contention explanation

That diagnostic has now been exercised successfully:

- a `2 ms` stagger restored dual-camera `80 fps` `ptp_gate` throughput to the
  expected range
- this is strong evidence that the unfixed case is dominated by synchronized
  burst contention rather than a wrong GPU assignment or generic PTP setup bug

The follow-on `100 fps` sweep sharpens that result:

- stagger is a valid mitigation at `80 fps`
- but at `100 fps`, every nonzero stagger tried so far remains unstable
- for larger offsets, the failure largely follows the camera receiving the
  offset
- the `100 fps` failures are also not the old split-GOP backlog-overflow mode;
  they instead show stale-frame behavior after gate open

### Remaining Monolith In `orange.cpp`

The recording/session path is much thinner now, but `src/orange.cpp` still
owns top-level GUI orchestration around:

- app-shell state transitions
- stream button wiring
- record button wiring
- non-recording panel coordination

## Recommended Next Steps

1. Tighten experiment pass/fail policy for multi-camera overload cases.
   Goal:
   make runs like the `2 x 100 fps` dual-camera failure show up as failures in
   `runs.csv`, not misleading passes.

2. Harden the local PTP-gated startup path.
   Goal:
   keep startup-only stale diagnostics and barrier/reset behavior from
   obscuring otherwise healthy PTP runs.

3. Add GUI/session supervision for the external recorder when GUI latency work
   becomes the priority again.
   Goal:
   start, monitor, drain, finalize, and fail visibly for external recorder
   processes instead of relying on the headless diagnostic harness.

4. Validate the new editable advanced split-GOP controls in the GUI.
   Goal:
   confirm the per-camera editing surface behaves correctly with the existing
   preflight guardrails.

5. Add an app-level GUI surface for storage defaults and latest-recording
   pointer settings.
   Goal:
   make the new app config discoverable without requiring direct JSON edits.

## Related Docs

- [recording_panel_modularization_plan.md](/home/jeremy/orange-gop-split-a16/docs/recording_panel_modularization_plan.md)
- [gui_recording_config_todo.md](/home/jeremy/orange-gop-split-a16/docs/gui_recording_config_todo.md)
- [advanced_recording_validation_plan.md](/home/jeremy/orange-gop-split-a16/docs/advanced_recording_validation_plan.md)
- [session_orchestration_architecture.md](/home/jeremy/orange-gop-split-a16/docs/session_orchestration_architecture.md)
