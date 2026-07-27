# GUI Camera Startup Timing

Date: 2026-07-27
Status: implemented; awaiting live four-camera validation

## Purpose

Orange now measures the existing GUI camera-open and stream-start lifecycle
before changing its threading model. The instrumentation is observational: it
does not parallelize Emergent SDK calls, alter PTP setup or gating, or redefine
when streaming is ready.

Each attempted operation produces one schema-versioned JSON artifact in:

```text
<orange_root>/diagnostics/camera_startup/
```

For the normal local installation this is:

```text
/home/jeremy/orange_data/diagnostics/camera_startup/
```

Artifacts use schema ID `orange.gui.camera_startup_timing` and schema version
`1`. Files are written through a temporary file and atomic rename. When Orange
is launched through a privileged validation wrapper, the filesystem-identity
guard creates the report as the invoking user rather than root.

## Camera-open report

The `camera_open` operation records:

- configuration discovery and camera selection;
- per-camera configuration loading;
- per-camera Emergent open/configuration;
- post-open GUI configuration; and
- total GUI-handler time.

The current operation remains deliberately serial. Per-camera interval offsets
therefore also make the order and cumulative cost visible.

## Stream-start report

The `stream_start` operation records:

- recording preflight;
- session/pipeline storage allocation;
- per-camera CUDA resource and IPC initialization;
- display/crop texture setup;
- worker construction and worker-thread startup;
- recording-pipeline construction;
- per-camera stream open;
- per-camera frame-buffer allocation and queueing;
- per-camera PTP mode configuration;
- acquisition-thread launch and thread-local setup;
- PTP arm/countdown/gate wait (or non-PTP acquisition start); and
- the first successfully received camera frame.

The operation has two useful totals:

- `gui_handler_duration_ms`: time until the synchronous GUI start handler
  returns; and
- `time_to_all_first_frames_ms`: time until every selected acquisition camera
  has delivered its first valid frame.

`first_frame_spread_ms` is computed from host receive times. Camera timestamps
and camera frame IDs are preserved with each first-frame event, but the host
spread is not a replacement for the existing PTP cadence/skew validation.

## Autorun readiness barrier

GUI autorun no longer treats `CameraControl::subscribe == true` as sufficient
stream readiness. `subscribe` is asserted before camera preparation and thread
startup complete, so it is a requested/running lifecycle flag rather than a
proof that images are available.

Autorun now remains in `start_streaming` until the active `stream_start` timing
report has `status = "complete"`. That state is reached only after every
expected acquisition camera has delivered its first valid frame. Only then
does the configured stream-warmup interval begin; recording can be requested
after that interval.

An artifact status of `failed` or `stopped` fails autorun immediately. A report
that remains incomplete for 45 seconds also fails autorun. Manual stream and
record controls are unchanged by this slice; the lightweight status interface
is reusable when the same readiness policy is later applied to them.

## Runtime cost and failure behavior

The first successful frame causes one small mutex-protected in-memory update in
each acquisition thread. After that, the steady-state acquisition loop performs
only the existing local boolean check and no recorder work. Acquisition threads
never write JSON. The GUI thread flushes changed snapshots atomically.

Instrumentation methods are fail-open: recorder or filesystem failures are
logged but must not decide whether cameras or streams start. A stopped attempt
is retained as `stopped`; preflight or pipeline-construction failures are
retained as `failed`; a normal stream reaches `complete` only after all expected
first frames arrive.

## Live validation

Run the established four-camera GUI PTP procedure. Open the cameras, start
streaming, wait until all previews have produced frames, and then inspect the
newest reports:

```bash
ls -1t /home/jeremy/orange_data/diagnostics/camera_startup/*.json | head
```

The existing four-camera autorun can exercise only the startup lifecycle,
without creating a recording:

```bash
ORANGE_GUI_AUTORUN_START_RECORDING=0 \
  ./scripts/run_gui_fourcam_external_ipc_validation.sh \
  --hidden-crop-preview --warmup-seconds 3
```

Useful fields are:

```text
operation
status
gui_handler_duration_ms
time_to_all_first_frames_ms
first_frame_spread_ms
slowest_global_stage
slowest_camera_stage
cameras.<serial>.stages
cameras.<serial>.first_frame
```

For the baseline, preserve reports from repeated one-, two-, and four-camera
starts. Compare medians and tails by stage before deciding whether to move
camera open/configuration, stream preparation, or both off the GUI thread.

## Scope still open

- Equivalent operation reports for the headless startup lifecycle.
- P50/P95 aggregation across repeated starts.
- Explicit startup progress in the GUI.
- SDK thread-safety validation and bounded parallel startup.
- Partial-start rollback tests before any threading change.
