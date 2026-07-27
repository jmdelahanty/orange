# GUI Camera Startup Timing

Date: 2026-07-27
Status: bounded camera-open and stream-preparation startup-only trials passed; fault and full-load validation pending

## Purpose

Orange measures the GUI camera-open and stream-start lifecycle and runs its
blocking preparation stages outside the GUI thread. Camera open/configuration,
per-camera CUDA/IPC initialization, and stream-open/buffer preparation now have
an opt-in bounded-concurrency experiment. The safe default remains one worker,
preserving serial Emergent SDK call order. Worker construction, PTP setup, and
the first-frame readiness barrier retain their existing ordered semantics.

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

Configuration files are loaded serially. Camera open/configuration then runs
through a bounded indexed task group. Each task owns one distinct camera
handle and its corresponding parameter/result slots; calls for one camera
remain sequential. Completion is handed to the GUI as one owned product, so
the main UI never observes partially opened camera arrays.

The runtime control is:

```text
ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY=1  # default and serial fallback
ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY=2  # experimental
ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY=4  # experimental
```

No other values are accepted by the validated launcher. A direct unvalidated
launch also fails safe to one worker for unsupported values. The effective
width is clamped to the number of selected cameras.

The camera-open timing context records the requested and effective widths,
their source, whether the requested value was supported, the camera count, and
the ownership policy. The task-group event additionally records actual peak
concurrency, started/completed task counts, launch failure, and cancellation.

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

- `gui/camera_startup_controller`: phase transitions, camera-open task wiring,
  cancellation, rollback, and GUI-thread handoff;
- `gui/async_startup_worker`: generic joinable worker and exception boundary;
- `gui/bounded_startup_tasks`: bounded indexed execution, peer-stop behavior,
  deterministic results, and join-before-return;
- `gui/texture_resources`: OpenGL/CUDA texture ownership, which must remain on
  the GUI thread that owns the OpenGL context.

Stream startup has two background phases separated by a short GUI phase:

1. recording preflight plus bounded per-camera CUDA/IPC resource initialization;
2. GUI-thread display/crop texture creation;
3. serial worker construction, bounded per-camera stream open/frame-buffer
   allocation, and serial PTP configuration in the startup worker;
4. atomic GUI-thread ownership handoff, recording-pipeline construction,
   worker/acquisition thread launch, then first-frame observation.

`CameraControl::subscribe` is asserted only in step 4, after every pointer and
texture consumed by the render loop has been installed. Before that point,
the controller's `busy` state disables camera mutations, selection changes,
recording controls, and PTP-stack controls. This preserves the pre-existing
meaning of `subscribe`: the stream runtime is safe to consume, not merely
requested.

Cancellation is cooperative between SDK calls. A failure prevents new camera
tasks from being scheduled; calls already inside the SDK are allowed to return.
All started tasks are joined before every camera handle is closed, including a
handle whose open succeeded but whose later configuration call failed. No
startup thread is detached. Window shutdown requests cancellation, joins the
worker, and rolls back pre-activation resources. A late cancel is checked again
at both background completion boundaries so it cannot accidentally activate a
completed product.

Each bounded task writes only to its camera's pre-sized result slots. Tasks
select their camera's CUDA device before resource work, and every task is joined
before cleanup or ownership transfer. Partial frame-buffer allocation is
tracked exactly so rollback releases only successfully allocated buffers,
closes every opened stream, and cleans each initialized per-camera resource.
The final runtime owns these products; acquisition threads borrow pointers only
after the atomic GUI handoff. Shutdown preserves the inverse order: stop and
join acquisition first, then destroy the owning runtime resources.

The experiment does not parallelize PTP configuration, worker construction, or
acquisition launch. It also does not change the headless startup path.

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

At the default width of one, the outer joinable startup worker directly runs
the same ordered camera calls without creating child threads. Widths two and
four can reduce wall time only if the Emergent SDK and host networking safely
make progress on independent camera handles. They are not production defaults
until live repeated tests establish that behavior.

## Preliminary two-camera evidence (2026-07-27)

One controlled startup-only A/B used Cam2010095 and Cam2010096 from
`100_cam4_ptp`. Both trials enabled PTP-gated streaming and a two-second warmup,
but deliberately disabled recording, YOLO, and crop work. Both completed normal
GUI shutdown and camera close.

Serial artifact:

```text
/home/jeremy/orange_data/diagnostics/camera_startup/camera_open_20260727T222258Z_4144689_1.json
```

Concurrency-two artifact:

```text
/home/jeremy/orange_data/diagnostics/camera_startup/camera_open_20260727T222836Z_4145354_1.json
```

| Measurement | Width 1 | Width 2 |
| --- | ---: | ---: |
| Requested/effective/peak concurrency | 1 / 1 / 1 | 2 / 2 / 2 |
| Camera-open task-group wall time | 1920.238 ms | 1241.067 ms |
| Cam2010095 open/configure | 1285.467 ms | 1219.481 ms |
| Cam2010096 open/configure | 634.661 ms | 1240.605 ms |
| PTP stream first-frame spread | 0.007543 ms | 0.001117 ms |
| Frame-ID gaps / GetFrame errors | 0 / 0 | 0 / 0 |

The two width-two tasks started within 0.2 ms on distinct worker threads and
overlapped for almost their entire duration. Wall time improved by 679.172 ms,
or 35.37% (1.55x speedup). Per-camera duration increased under overlap,
especially for Cam2010096, so the result shows useful but contended concurrency
rather than perfect scaling.

Each camera sustained approximately 100 FPS after the PTP gate with zero frame
gaps, GetFrame errors, preprocessing drops, or encode failures. The second run
also proves one successful close/reopen cycle after the serial baseline.

The display output-queue warnings observed during this work were not camera
frame drops. The fast monochrome preview path had already copied the selected
frame into its PBO, advanced `PreviewSerial()`, and released its frame-pool
reference, but then returned success to `CThreadWorker`. That return value
published an unowned pointer to a base-class output queue that no caller reads;
the bounded queue later evicted the stale pointer and logged a misleading
`dropped oldest entry` warning. Display completion is communicated entirely
through the PBO and `PreviewSerial()`, so the display worker now returns false
after completing that handoff and never populates the dead queue.

A follow-up concurrency-two startup-only smoke produced camera-open artifact
`camera_open_20260727T223854Z_4147286_1.json` and stream-start artifact
`stream_start_20260727T223856Z_4147286_2.json`. Both previews continued through
the fast monochrome path, both cameras acquired 214 frames at approximately
100 FPS, and shutdown reported zero frame-ID gaps, GetFrame errors,
preprocessing drops, or encode failures. No display output-queue warning was
emitted. The configured 15 FPS preview selection still intentionally skips
most frames for display only; that cadence control is independent of scientific
acquisition and recording.

## Four-camera controlled evidence (2026-07-27)

A matched startup-only comparison used all cameras in
`100_cam4_ptp_fourcam`. It includes one width-one baseline and four width-four
trials: the initial smoke followed by three consecutive close/reopen repeats.

Camera-open artifacts:

```text
width 1: camera_open_20260727T224602Z_4148786_1.json
width 4: camera_open_20260727T224156Z_4148000_1.json
         camera_open_20260727T224655Z_4149082_1.json
         camera_open_20260727T224706Z_4149291_1.json
         camera_open_20260727T224717Z_4149496_1.json
```

| Camera-open task group | Trials | Mean | Median | Range |
| --- | ---: | ---: | ---: | ---: |
| Width 1 | 1 | 4025.371 ms | 4025.371 ms | 4025.371 ms |
| Width 4 | 4 | 1318.048 ms | 1327.710 ms | 1121.838–1494.935 ms |

Width four reduced mean camera-open wall time by 67.26%, a 3.05x speedup over
the matched serial baseline. Every width-four artifact records requested,
effective, and actual peak concurrency four; four completed tasks; no launch
failure or cancellation; and four distinct worker-thread hashes. The first
trial's four tasks began within 0.35 ms.

The corresponding stream-start trials show that camera-open concurrency did
not weaken the later synchronization barrier. The serial baseline delivered all
four first frames in 4361.855 ms with 0.003911 ms host spread. Width-four
time-to-all-first-frames ranged from 4372.399 to 4423.525 ms, and host spread
ranged from 0.003632 to 0.005308 ms. Thus the roughly 4.4-second stream-start
interval is independent of the camera-open speedup and remains dominated by the
intentional future PTP gate.

Each width-four trial acquired 208–215 frames per camera at approximately
100 FPS. Every final report recorded zero frame-ID gaps, GetFrame errors,
preprocessing drops, and encode failures. All four fast monochrome previews ran,
no display output-queue warning appeared, and every worker and camera handle
closed normally before the next trial.

This remains pre-production evidence. Four successful width-four starts are not
a 100-cycle soak, and the test did not inject cancellation/failure or enable
recording, YOLO, crop, or pose work. Concurrency one therefore remains the
default; width four is a validated startup-only candidate rather than a
production default.

## Four-camera stream-preparation evidence (2026-07-27)

A same-binary width-one/width-four comparison then exercised bounded CUDA/IPC
resource initialization and stream-open/frame-buffer preparation. It used all
four `100_cam4_ptp_fourcam` cameras, retained serial worker construction and PTP
configuration, and disabled recording, YOLO, crop, and pose.

Stream-start artifacts:

```text
width 1: /home/jeremy/orange_data/diagnostics/camera_startup/stream_start_20260727T232036Z_4157758_2.json
width 4: /home/jeremy/orange_data/diagnostics/camera_startup/stream_start_20260727T231910Z_4157314_2.json
```

| Measurement | Width 1 | Width 4 | Change |
| --- | ---: | ---: | ---: |
| Resource task-group wall time | 497.392 ms | 475.201 ms | -4.5% |
| Serial worker-construction span | 189.592 ms | 190.916 ms | +0.7% |
| Stream-open/buffer task-group wall time | 339.083 ms | 231.991 ms | -31.6% |
| Serial PTP-configuration span | 6.562 ms | 11.482 ms | +4.920 ms |
| GUI-handler duration | 1163.798 ms | 1035.473 ms | -11.0% |
| Time to all first frames | 4374.463 ms | 4289.877 ms | -1.9% |
| First-frame host spread | 0.005028 ms | 0.005029 ms | unchanged |

Both bounded task groups reported requested/effective/peak concurrency four,
four completed tasks, no launch failure, and no cancellation. Every camera
acquired 208 frames at approximately 100 FPS with zero frame-ID gaps, GetFrame
errors, preprocessing drops, or encode failures. Shutdown was clean. Resource
initialization overlapped but showed little wall-time gain, while stream-open
and buffer preparation saved 107.092 ms. The end-to-end reduction is smaller
because the unchanged shared PTP future gate still contributes about 3.24
seconds.

One additional diagnostic tried bounded worker construction:

```text
/home/jeremy/orange_data/diagnostics/camera_startup/stream_start_20260727T232334Z_4158703_2.json
```

Its four-camera worker task group took 219.351 ms versus 189.592 ms for the
serial baseline, a 15.7% regression. These workers allocate large pinned host
buffers and shared-GPU resources, so parallel construction introduced
contention without useful overlap. The experiment remained healthy at runtime,
but the code was reverted and the timing context now records
`worker_construction_policy = "serial"`.

## Live validation

First preserve a serial baseline, then run the same startup with width two.
Use the established four-camera GUI PTP procedure, open the cameras, start
streaming, wait until all previews have produced frames, and inspect the newest
reports:

```bash
ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY=1 \
  ./scripts/run_gui_aq_off_validation.sh

ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY=2 \
  ./scripts/run_gui_aq_off_validation.sh
```

Width four has passed four startup-only trials. Continue to use it as an
explicit validation setting until cancellation/fault recovery, soak, and
recording/analytics runs also pass:

```bash
ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY=4 \
  ./scripts/run_gui_aq_off_validation.sh
```

Inspect the reports with:

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
context.camera_open_concurrency
context.stream_preparation_concurrency
global_instants.camera_open_task_group_complete
global_instants.camera_resource_task_group_complete
global_instants.stream_open_buffer_task_group_complete
time_to_all_first_frames_ms
first_frame_spread_ms
slowest_global_stage
slowest_camera_stage
cameras.<serial>.stages
cameras.<serial>.first_frame
```

For validation, preserve reports from repeated one-, two-, and four-camera
starts. Compare the `open_and_configure_camera` overlap and task-group wall
time. Require the expected peak width, no camera ownership/configuration
cross-talk, no hangs, clean cancellation/rollback, successful reopen after a
cancel or fault, unchanged PTP/first-frame behavior, and no new stream or frame
drops. Confirm that the window continues redrawing, `subscribe` is not asserted
before `stream runtime activated`, and autorun records only after
`status = "complete"`.

## Scope still open

- Equivalent operation reports for the headless startup lifecycle.
- P50/P95 aggregation across repeated starts.
- Per-camera progress rather than the current phase-level GUI message.
- Full recording/YOLO/crop/pose validation at widths two and four.
- A production-default decision; concurrency one remains the default.
- Any per-NIC or SDK-call-class lock domain found necessary by live testing.
- Hardware-injected partial open/stream/buffer failure tests.
