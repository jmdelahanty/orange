# Orange Calibration Transaction Lease

Date: 2026-07-26

Status: first process-wide implementation complete; live GUI validation pending

## Purpose

Orange calibration workflows can temporarily change camera timing, own an
acquisition stream, ask Citrus to present calibration scenes, and publish or
select calibration products. Those operations must not overlap with another
calibration workflow or with recording start.

The calibration transaction coordinator provides one exclusive, process-wide
lease for that interval. It is an orchestration safety boundary. It does not
change calibration mathematics or make a candidate authoritative.

## Invariants

1. At most one calibration transaction or recording-start reservation is
   active in one Orange process.
2. Every lease has a stable owner ID, workflow kind, camera scope, declared
   owner mutations, and operator-readable reason.
3. Conflicting acquisition reports the current owner and reason instead of
   silently sharing state.
4. A child capture can use a parent lease only when it names the exact parent
   owner, stays inside the parent's camera scope, and requests a declared
   mutation.
5. Recording start is rejected while any calibration transaction is active.
   Recording stop and process shutdown remain safety actions.
6. A workflow does not release its lease merely because capture finished. It
   releases after the relevant camera/Citrus restoration or candidate
   resolution has completed.

The coordinator is deliberately process-wide even when two operations name
disjoint cameras. Citrus scene state, illumination routing, and the shared GUI
acquisition lifecycle make disjoint-camera concurrency unsafe without a more
complete resource model.

## Mutation vocabulary

The first implementation declares these possible owner mutations:

| Mutation | Meaning |
| --- | --- |
| `camera_parameters` | Exposure, frame rate, lens, or related camera state |
| `camera_stream_lifecycle` | Starting, stopping, or temporarily owning acquisition |
| `camera_open_close` | Opening or closing configured cameras |
| `citrus_scene` | Presenting/restoring a Citrus calibration scene or operating a linked Citrus transaction |
| `recording_start` | Activating a prepared recording; granted only to the short-lived recording-start reservation |

Permissions authorize only the owning workflow's explicitly routed action.
They do not make a generic GUI button an owner action.

## Current owners

| Workflow | Scope and declared mutations | Release boundary |
| --- | --- | --- |
| Recording-start reservation | Cameras selected for recording; recording start | Recording activation succeeds/fails or a pending start is canceled |
| Manual Spatial grouped capture | Selected camera set; Citrus scene | Post-capture Citrus restore fence |
| Spatial direct still | Selected camera; stream lifecycle | Direct stream capture/teardown returns |
| Manual calibration preflight | Prepared cameras/light-control camera; camera parameters, stream lifecycle, Citrus scene | Every saved camera/light state is restored |
| Guided commissioning | Selected camera set; camera parameters, stream lifecycle, Citrus scene | Result is written after restoration and stream stop |
| Arena-centering commissioning | Selected camera set; camera parameters, stream lifecycle, Citrus scene | Centering and any homography candidate are explicitly committed/rejected, restoration completes, and result is written |
| Daily registration | Selected camera set; camera parameters and Citrus scene | Abort/reject is acknowledged by Citrus, or accepted runtime selection is confirmed |
| Aperture characterization | Selected camera; camera parameters and stream lifecycle | Worker joins after restoration |
| FOV alignment | Selected camera; camera parameters and stream lifecycle | Preview worker stops and joins after restoration |
| USAF live preview | Selected camera; camera parameters and stream lifecycle | Preview worker stops and joins after restoration |
| USAF artifact publication | Selected camera; no camera mutation | Artifact worker joins |

Grouped captures nested under daily registration, guided commissioning, or
arena-centering commissioning borrow the parent lease explicitly. They never
release it.

## Guarded surfaces

The main GUI now uses the coordinator rather than independent aperture/USAF
busy flags to guard:

- camera property edits;
- camera open/close;
- generic stream start/stop;
- recording start through the shared GUI/local-control operator path;
- starting another calibration window's work;
- manual Spatial captures and calibration preflight;
- manual daily runtime-mode changes; and
- unrelated homography, scale, and commissioning authority mutations.

Guided commissioning and arena-centering may still issue their own stream
requests because those requests are checked against the exact active owner and
the parent's declared permission. Guided projected-surface scale review remains
available while its owning guided transaction is active; unrelated manual
candidate loading and authority changes remain disabled.

The GUI displays the active owner, workflow kind, and reason. Programmatic
recording starts also pass through the same recording-start guard, so disabling
the visible button is not the only protection. External-recorder startup holds
a recording-start reservation across its asynchronous supervisor launch. This
prevents a calibration transaction from starting in the gap between the
operator's request and `record_video` becoming active.

## Failure and restoration policy

The default is fail closed:

- a manual grouped capture retains ownership until Citrus acknowledges the
  restore presentation fence;
- manual camera preflight retains ownership while any saved restore state
  remains;
- daily registration retains ownership while abort/reject restoration is
  unresolved;
- arena-centering retains ownership while its Citrus transaction or
  homography candidate remains active; and
- worker-based optical tools release only after the worker has joined.

The lease is also RAII-backed so normal scope destruction cannot leave the
in-process coordinator permanently claimed. That fallback records
`scope_destroyed`; it is not evidence that external hardware or Citrus state
was restored.

There is intentionally no ordinary force-unlock button. Recording stop and
application shutdown remain available for safety.

## Implementation locations

- Coordinator and lease: [`src/calibration_transaction.h`](../src/calibration_transaction.h)
  and [`src/calibration_transaction.cpp`](../src/calibration_transaction.cpp)
- Spatial owner bridge: [`src/gui/spatial_layout/calibration_transaction_bridge.h`](../src/gui/spatial_layout/calibration_transaction_bridge.h)
- Process guards: [`src/orange.cpp`](../src/orange.cpp)
- Grouped child/parent enforcement: [`src/gui/spatial_layout/group_capture_controller.cpp`](../src/gui/spatial_layout/group_capture_controller.cpp)
- Focused contract tests: [`tools/calibration_transaction_tests.cpp`](../tools/calibration_transaction_tests.cpp)

## Current limitations and next slice

This first implementation is intentionally narrower than a durable calibration
journal:

- ownership exists only inside one Orange process;
- a process crash cannot prove or complete camera/Citrus restoration;
- pre-mutation state and restoration progress are not yet persisted in one
  common transaction artifact;
- independent CLI/headless processes are not coordinated by this in-memory
  lease;
- enforcement is currently at known orchestration/UI entry points rather than
  by passing an unforgeable lease token through every low-level camera and
  Citrus function; and
- live four-camera GUI behavior has not yet been exercised after this change.

The next transaction slice should persist a transaction journal before the
first mutation, checkpoint restoration state, and expose startup recovery for
an abandoned transaction. After that, low-level mutation APIs can progressively
require an owner token, and headless structured acquisitions can join the same
contract through an inter-process coordinator or a single owning service.

## Validation performed

The focused tests cover exclusive ownership, normalized camera scopes,
operator-readable conflict diagnostics, mutation permission checks, child
scope enforcement, recording-start exclusion, explicit terminal status, and
RAII release. The Orange
target and focused test target build successfully. A live GUI validation is
still required before treating the workflow integration as operationally
proven.
