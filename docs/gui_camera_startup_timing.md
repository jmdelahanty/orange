# GUI Camera Startup Timing

Date: 2026-07-27
Status: asynchronous GUI lifecycle implemented; awaiting live four-camera validation

## Purpose

Orange measures the GUI camera-open and stream-start lifecycle and now runs its
blocking preparation stages outside the GUI thread. Emergent SDK calls remain
serial and ordered; this slice improves responsiveness without assuming that
concurrent SDK calls are safe. PTP setup and the first-frame readiness barrier
retain their existing semantics.

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

The operation remains deliberately serial, but executes on the controller's
joinable startup worker. Per-camera interval offsets therefore make the order
and cumulative cost visible while the GUI continues to render. Completion is
handed to the GUI as one owned product; the main UI never observes partially
opened camera arrays.

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

- `gui_handler_duration_ms`: request-to-runtime-activation time, ending when
  the GUI thread installs the complete worker/resource product; and
- `time_to_all_first_frames_ms`: time until every selected acquisition camera
  has delivered its first valid frame.

`first_frame_spread_ms` is computed from host receive times. Camera timestamps
and camera frame IDs are preserved with each first-frame event, but the host
spread is not a replacement for the existing PTP cadence/skew validation.

## Execution and ownership model

The implementation lives in dedicated modules rather than adding another
startup state machine to `orange.cpp`:

- `gui/camera_startup_controller`: phase transitions, serial camera work,
  cancellation, rollback, and GUI-thread handoff;
- `gui/async_startup_worker`: generic joinable worker and exception boundary;
- `gui/texture_resources`: OpenGL/CUDA texture ownership, which must remain on
  the GUI thread that owns the OpenGL context.

Stream startup has two background phases separated by a short GUI phase:

1. recording preflight plus per-camera CUDA/IPC resource initialization;
2. GUI-thread display/crop texture creation;
3. worker construction, serial stream open, frame-buffer allocation, and PTP
   configuration in the startup worker;
4. atomic GUI-thread ownership handoff, recording-pipeline construction,
   worker/acquisition thread launch, then first-frame observation.

`CameraControl::subscribe` is asserted only in step 4, after every pointer and
texture consumed by the render loop has been installed. Before that point,
the controller's `busy` state disables camera mutations, selection changes,
recording controls, and PTP-stack controls. This preserves the pre-existing
meaning of `subscribe`: the stream runtime is safe to consume, not merely
requested.

Cancellation is cooperative between SDK calls. No startup thread is detached;
window shutdown requests cancellation, joins the worker, and rolls back any
pre-activation resources. A late cancel is checked again at both background
completion boundaries so it cannot accidentally activate a completed product.

## Autorun readiness barrier

GUI autorun does not treat `CameraControl::subscribe == true` as sufficient
stream readiness. `subscribe` now proves that runtime ownership is installed,
but a camera may still be waiting at the PTP gate or for its first frame.

Autorun now remains in `start_streaming` until the active `stream_start` timing
report has `status = "complete"`. That state is reached only after every
expected acquisition camera has delivered its first valid frame. Only then
does the configured stream-warmup interval begin; recording can be requested
after that interval.

An artifact status of `failed` or `stopped` fails autorun immediately. A report
that remains incomplete for 45 seconds also fails autorun. Manual recording is
disabled during startup, and the stream button becomes an explicit cancel
action until activation completes.

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

The asynchronous controller does not make startup intrinsically faster: the
same serial SDK work still occurs. Its immediate benefit is that window input,
progress text, redraw, and cancellation remain responsive while that work is
in flight. Bounded per-camera concurrency remains a separate experiment.

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

For validation, preserve reports from repeated one-, two-, and four-camera
starts. Confirm that the window continues redrawing during camera open and
stream preparation, cancellation returns to a clean state, `subscribe` is not
asserted before `stream runtime activated`, and autorun records only after
`status = "complete"`.

## Scope still open

- Equivalent operation reports for the headless startup lifecycle.
- P50/P95 aggregation across repeated starts.
- Per-camera progress rather than the current phase-level GUI message.
- SDK thread-safety validation and bounded parallel startup.
- Hardware-injected partial open/stream/buffer failure tests.
