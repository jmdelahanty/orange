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

Phase 0 startup timing and the first asynchronous GUI lifecycle are implemented
while retaining serial camera-SDK order and PTP gate behavior. Each camera-open attempt
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
open, buffer allocation, and PTP configuration now run on a joinable startup
worker. OpenGL/CUDA display texture creation and the final ownership handoff
remain on the GUI thread. The GUI exposes phase progress and cancellation, and
`CameraControl::subscribe` is not asserted until runtime arrays are complete.

This does **not** yet parallelize calls across cameras or instrument the
headless camera-open/stream-start lifecycle. The next concurrency decision
should be based on live one-, two-, and four-camera reports and an explicit
Emergent SDK safety test.

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
- [ ] Add a bounded per-camera startup executor (`max_parallel_startup_workers`).
- [ ] Add runtime flag/config:
  - `parallel_camera_startup` (default off for first rollout)
  - `max_parallel_startup_workers` (default conservative, e.g. 2-4).
- [ ] Keep a hard fallback to existing sequential flow.

## Phase 2: Parallelize Camera Open/Configure

- [ ] Refactor GUI open-camera flow to submit one task per selected camera:
  - task runs `open_camera_with_params` or `update_camera_params`
  - gather per-camera result + error details.
- [ ] Refactor headless `open_cameras` similarly.
- [ ] Enforce deterministic error aggregation:
  - if any task fails, abort startup
  - close already-opened cameras
  - surface per-camera failure summary.

## Phase 3: Parallelize Stream Prep

- [ ] Refactor stream prep helper (`allocate_camera_frame_buffers`) to per-camera tasks:
  - `camera_open_stream`
  - frame allocation + queueing
  - optional reorder buffer allocation.
- [ ] Apply same model to GUI startup path where equivalent logic is inlined.
- [ ] Ensure rollback on partial failure:
  - release allocated buffers
  - close opened streams
  - return to clean pre-start state.

## Phase 4: PTP Setup Strategy

- [ ] Decide policy after SDK check:
  - Option A: keep `ptp_camera_sync` sequential for safety
  - Option B: parallelize with bounded workers if proven safe.
- [ ] Keep one explicit barrier after PTP mode setup before starting acquisition threads.
- [ ] Maintain existing PTP start gate semantics (`start_ptp_sync` in acquisition threads).

## Phase 5: UX and Observability

- [x] Add phase-level startup progress and cancellation in the GUI:
  - `opening_cameras`, `preparing_stream_resources`, GUI texture handoff,
    `constructing_stream_runtime`, and `waiting_for_first_frames`.
- [ ] Add per-camera progress and expose it to the local-control/status API.
- [ ] Emit per-camera startup timing logs.
- [ ] Emit one startup summary line with total time and slowest camera/stage.

## Phase 6: Testing and Rollout

- [x] Unit-test the generic nonblocking worker, exception conversion,
  cooperative cancellation, and shutdown join.
- [ ] Unit-test hardware-independent controller result aggregation and rollback.
- [ ] Integration tests:
  - one camera fails open
  - one camera fails stream open
  - one camera fails buffer allocation
  - mixed success/failure should leave clean state.
- [ ] Soak test parallel startup/stop loops (100+ cycles) for leaks/hangs.
- [ ] A/B compare startup latency against sequential baseline.
- [ ] Rollout:
  - enable feature flag in test environments first
  - switch default on only after stability criteria are met.

## Definition of Done

- [ ] Startup latency improves materially (target: >=30% faster for multi-camera setups, measured).
- [ ] No increase in startup failure rate compared to sequential baseline.
- [ ] Partial failures always rollback to clean state.
- [ ] Startup logs clearly identify per-camera stage timing and failures.
- [ ] Sequential fallback remains available and tested.
