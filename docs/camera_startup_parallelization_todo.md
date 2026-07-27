# Camera Startup Parallelization TODO

Date: 2026-02-24
Scope: reduce perceived camera startup latency by parallelizing per-camera bring-up work in `orange-jeremy` (GUI + headless).

## Is This Possible?

Yes. Most startup work is per-camera and currently serialized, so parallelization is feasible and likely beneficial:

- Camera open/configure is done in per-camera loops.
  - `src/orange.cpp:714`
  - `src/orange_headless_client.cpp:39`
- Stream open + frame buffer allocation is also serialized.
  - `src/orange.cpp:936`
  - `src/project.cpp:658`
- PTP mode setup is serialized.
  - `src/orange.cpp:941`
  - `src/orange_headless_client.cpp:74`

The main caveat is verifying Emergent SDK thread-safety for concurrent camera API calls in one process.

## Goal

Cut user-visible startup time (open cameras + start streaming) while preserving deterministic behavior, clear error reporting, and safe rollback.

## Current Startup Hotspots (Observed)

1. GUI camera open/configure path is strictly sequential (`open_camera_with_params` / `update_camera_params`).
   - Ref: `src/orange.cpp:714`
2. GUI streaming prep is sequential (`camera_open_stream` + `allocate_frame_buffer`).
   - Ref: `src/orange.cpp:936`
3. Headless open path is sequential.
   - Ref: `src/orange_headless_client.cpp:34`
4. Headless stream prep is sequential via shared helper.
   - Refs: `src/orange_headless_client.cpp:53`, `src/project.cpp:658`
5. PTP sync setup loop is sequential in both paths.
   - Refs: `src/orange.cpp:941`, `src/orange_headless_client.cpp:74`

## Audit Update (2026-03-16)

- Re-checked against current code: startup is still fully serialized in both GUI and headless paths.
- Verified serialized stages still include:
  - camera open/configure,
  - stream open + frame-buffer allocation,
  - PTP mode setup.
- No startup executor, feature flag, or per-stage timing instrumentation exists yet.
- The newer focus-bootstrap logic in `src/camera.cpp` adds more per-camera work during open/configure, which increases the value of baseline timing before parallelization.

## Instrumentation And GUI Responsiveness Update (2026-07-27)

Phase 0 startup timing, the asynchronous GUI lifecycle, and the first bounded
camera-open/configuration experiment are implemented. Each camera-open attempt
and stream-start attempt writes an atomic, user-owned JSON report under:

```text
<orange_root>/diagnostics/camera_startup/
```

The reports include per-camera stages, global stages, GUI-handler duration,
acquisition-thread entry/setup, PTP arm/gate wait, first valid frame, time until
all selected cameras have produced a frame, host first-frame spread, and the
slowest global/per-camera stage. Failed preflight and construction attempts are
also retained. Acquisition threads only update an in-memory record; JSON I/O
is performed by the GUI thread.

Implementation and interpretation details are in
[`gui_camera_startup_timing.md`](gui_camera_startup_timing.md).

Camera open/configuration, CUDA/IPC preparation, worker construction, stream
open, buffer allocation, and PTP configuration run off the GUI thread.
Open/configuration can use a bounded per-camera task group; concurrency one is
the default and preserves serial order, while two and four are opt-in values.
OpenGL/CUDA display texture creation and the final ownership handoff remain on
the GUI thread. The GUI exposes phase progress and cancellation, and
`CameraControl::subscribe` is not asserted until runtime arrays are complete.

Stream open, buffer allocation, PTP configuration, and the headless lifecycle
remain serial. The bounded implementation does not by itself prove every
Emergent SDK call class safe under concurrency, but live camera open/configure
has now succeeded at measured width four across repeated close/reopen trials.

The first two-camera startup-only A/B on 2026-07-27 passed. Width two reduced
the camera-open task-group wall time from 1920.238 ms to 1241.067 ms (35.37%)
with measured peak concurrency two. Both PTP streams then sustained about
100 FPS with zero frame-ID gaps and GetFrame errors and shut down normally.
The matched four-camera baseline and four width-four trials then reduced mean
camera-open task-group wall time from 4025.371 ms to 1318.048 ms: 67.26%, or a
3.05x speedup. All four trials sustained approximately 100 FPS with zero
frame-ID gaps, GetFrame errors, preprocessing drops, or encode failures and
closed normally. These were still startup-only runs with recording, YOLO, crop,
and pose disabled, so they are not yet a rollout result. Exact artifacts and
interpretation are in
[`gui_camera_startup_timing.md`](gui_camera_startup_timing.md).

The measured stream-start trace identifies the next safe slice. Before the
acquisition threads launch, per-camera CUDA resource initialization consumes
about 0.50 seconds and serial stream-open/buffer preparation about 0.32 seconds.
Those independent, per-camera products are candidates for the same bounded task
model with deterministic rollback. GUI OpenGL texture work stays on the GUI
thread; the roughly 7 ms PTP-mode configuration remains serial initially. The
four acquisition threads already enter `start_ptp_sync` concurrently, and the
remaining roughly 3.24 seconds is the intentional shared future-gate countdown,
not serial work. Its barrier and gate semantics must not be weakened merely to
improve startup timing.

## Parallelization Plan

## Phase 0: Baseline and Safety Checks

- [x] Add GUI startup timing instrumentation per stage and per camera:
  - discover/select config
  - camera open/configure
  - stream open
  - frame buffer allocation
  - ptp mode setup
  - first frame received.
- [ ] Add equivalent operation-level timing to the headless startup path.
- [ ] Record baseline P50/P95 startup times for 1, 2, 4, 8+ cameras.
- [ ] Verify SDK behavior under concurrent per-camera API calls:
  - if safe: proceed with bounded parallelism
  - if not safe: define lock domains (global lock, per-NIC lock, or per-call class lock).

## Phase 1: Introduce Startup Executor and Feature Flag

- [x] Move blocking GUI startup behind a single joinable, cancelable executor.
- [x] Keep serial camera call ordering as the safe default.
- [x] Add a bounded indexed per-camera startup executor.
- [x] Add runtime control `ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY` with accepted
  widths `1`, `2`, and `4`.
- [x] Keep concurrency one as the hard sequential default/fallback.

## Phase 2: Parallelize Camera Open/Configure

- [x] Refactor GUI open-camera flow to submit one task per selected camera:
  - task runs `open_camera_with_params` or `update_camera_params`
  - gather per-camera result + error details.
- [ ] Refactor headless `open_cameras` similarly.
- [x] Enforce deterministic error aggregation:
  - if any task fails, abort startup
  - close already-opened cameras
  - surface per-camera failure summary.

## Phase 3: Parallelize Stream Prep

- [ ] Initialize each camera's `CameraResources` in a bounded per-camera task:
  - one distinct result slot and CUDA device per task
  - no shared `cudaSetDevice` state across threads
  - join all tasks before ownership handoff or cleanup.
- [ ] Refactor stream prep helper (`allocate_camera_frame_buffers`) to per-camera tasks:
  - `camera_open_stream`
  - frame allocation + queueing
  - optional reorder buffer allocation.
- [ ] Apply same model to GUI startup path where equivalent logic is inlined.
- [ ] Keep OpenGL/display texture creation on the GUI thread that owns the
  context; do not include it in the task group.
- [ ] Keep worker construction serial for the first stream-prep experiment;
  reconsider it only after the resource and stream-open slices are measured.
- [ ] Ensure rollback on partial failure:
  - release allocated buffers
  - close opened streams
  - clean every initialized CUDA resource product
  - return to clean pre-start state.

## Phase 4: PTP Setup Strategy

- [x] Keep `ptp_camera_sync` sequential for the first stream-prep experiment:
  the measured four-camera loop is only about 7 ms, so concurrent SDK control
  calls have negligible upside here.
- [x] Keep one explicit barrier after PTP mode setup before starting acquisition threads.
- [x] Maintain existing PTP start gate semantics (`start_ptp_sync` in acquisition threads).
- [x] Confirm that acquisition threads already arm and wait concurrently; the
  roughly three-second interval is a shared future-gate safety window, not a
  serial loop.
- [ ] Treat any reduction of the three-second future-gate delay as a separate
  PTP safety experiment with explicit minimum-arm-margin telemetry.

## Phase 5: UX and Observability

- [x] Add phase-level startup progress and cancellation in the GUI:
  - `opening_cameras`, `preparing_stream_resources`, GUI texture handoff,
    `constructing_stream_runtime`, and `waiting_for_first_frames`.
- [ ] Add per-camera progress and expose it to the local-control/status API.
- [x] Emit per-camera startup timing artifacts, including task begin/end.
- [x] Emit task-group timing with requested/effective/peak concurrency and
  total wall time; the existing report also identifies the slowest stage.

## Phase 6: Testing and Rollout

- [x] Unit-test the generic nonblocking worker, exception conversion,
  cooperative cancellation, and shutdown join.
- [x] Unit-test bounded overlap, deterministic result slots, peer failure,
  external cancellation, thread joining, exception conversion, and serial
  fallback sanitization.
- [ ] Unit-test hardware-independent controller result aggregation and rollback.
- [ ] Integration tests:
  - one camera fails open
  - one camera fails stream open
  - one camera fails buffer allocation
  - mixed success/failure should leave clean state.
- [ ] Soak test parallel startup/stop loops (100+ cycles) for leaks/hangs.
- [x] A/B compare startup latency against sequential baseline.
- [ ] Rollout:
  - enable feature flag in test environments first
  - switch default on only after stability criteria are met.

## Definition of Done

- [x] Startup latency improves materially (target: >=30% faster for multi-camera setups, measured).
- [ ] No increase in startup failure rate compared to sequential baseline.
- [ ] Partial failures always rollback to clean state.
- [x] Startup logs clearly identify per-camera stage timing and failures.
- [x] Sequential fallback remains available and tested.
