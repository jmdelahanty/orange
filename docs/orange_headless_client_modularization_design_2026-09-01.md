# Deferred modularization design: `orange_headless_client.cpp`

Date: 2026-09-01
Status: deferred design; implementation is intentionally out of scope for the current spatial-ROI feature

`src/orange_headless_client.cpp` is now a roughly 12,000-line translation unit. It is both a command-line front end and the owner of camera, CUDA, worker-thread, recording, socket, child-process, monitoring, and manifest lifetimes. That concentration makes error-path review and incremental feature work increasingly risky. This document records a future extraction plan; it is not approval to refactor the current spatial-ROI change.

## Current responsibility inventory

The function locations below are representative locations in the file as of this date and should be rechecked before an extraction PR. The important boundary is ownership and ordering, not a particular line number.

| Responsibility | Representative code | State and side effects |
| --- | --- | --- |
| CLI/options and usage | `HeadlessCliOptions`, `parse_headless_cli_options` (around lines 71–289 and 3211–3697) | Parses command-line/config values, records defaults and diagnostics, selects local versus remote mode, and carries spatial-ROI and recording overrides. |
| Experiment schema and run matrix | experiment load/validation/build helpers (around lines 8587–9460) | Reads JSON, validates camera/GPU/clip combinations, expands runs, and emits validation failures before hardware starts. |
| Camera discovery and setup | camera selection/GPU override helpers, `open_cameras`, `create_camera_manager` (around lines 3698–3901, 4442–4484, and 5337 onward) | Touches camera SDK state, camera arrays, inventory, PTP parameters, and the legacy remote-manager path. |
| Acquisition orchestration | `start_camera_thread` and its `acquire_frames` calls (around lines 4496–5275) | Starts per-camera threads, allocates resources, supplies controller pointers, and establishes join/failure ordering. |
| Full-frame recording lifecycle | `prepare_headless_recording_artifacts`, `shutdown_headless_run`, recording/rolling helpers (around lines 3987–4192 and throughout the local run) | Creates directories and snapshots, owns pipelines and drain requests, joins camera threads, and closes cameras. Ordering is part of the safety contract. |
| Optional spatial-ROI lifecycle | `HeadlessSpatialRoiRecordingOwner`, `prepare_headless_spatial_roi_recording`, `start_headless_spatial_roi_before_acquisition`, `update_headless_spatial_roi_recording_snapshot` (around lines 5702–6280) | Verifies plan/root/GPU mapping, creates three artifact references, starts the session before acquisition, records socket-bound versus recorder-ready state, and calls Finish/Abort. It must not own full-frame cleanup or the outer supervisor. |
| Frame IPC | frame IPC naming/configuration, runtime and manager helpers (around lines 708–829 and 4194–4440) | Creates AF_UNIX endpoints/queues, launches or joins IPC workers, and writes IPC summaries. |
| Vision and pose workers | worker setup/stop/event helpers, called from the run and shutdown paths | Owns worker threads, event logs, and stop/join behavior, with CUDA-sensitive failure paths. |
| Synchronization | `ensure_headless_host_ptp_stack`, `teardown_headless_host_ptp_stack`, PTP/camera sync helpers (around lines 299–550 and call sites) | Invokes host services and camera synchronization; failures affect whether a run may start. |
| Device monitoring | GPU dmon and NIC monitor start/stop/snapshot code (around lines 7699–8000) | Starts auxiliary processes/threads, drains output, and stores run diagnostics. |
| External recorder bridge | command/verifier helpers and `write_supervised_external_recorder_single_clip_manifest`, `finalize_supervised_external_recorder_run` (around lines 6725–7595) | Supervises another recorder, validates its summaries, and translates its results into manifests. It is distinct from the full-frame and spatial-ROI owners. |
| Manifest and snapshot output | `read_json_file`, `write_json_file`, recording/ROI snapshot builders, `write_experiment_manifests` (around lines 5650–5700, 5724–6170, and 11684 onward) | Writes recording, session, clip, experiment, and failure artifacts. File identity, schema, and update timing are observable behavior. |
| Local run state machine | `run_local_recording_session`, `run_local_experiment` (around lines 9598–11680 and 11876 onward) | Opens devices, applies overrides, starts subsystems, waits for duration/clip boundaries, handles cancellation, shuts down, and aggregates results. |
| Entry points and compatibility path | `run_local_mode`, `run_remote_mode`, `main` (around lines 12161–12332) | Preserves the command-line ABI and dispatches local and legacy remote operation. |

The effective shared state includes `quit_server`, camera/control arrays, PTP and GPU mappings, thread vectors, recording pipelines, frame-IPC objects, worker objects, monitor handles, and the optional spatial-ROI owner. Side effects include camera SDK and CUDA calls, AF_UNIX endpoints, child processes, filesystem writes, signal/cancellation handling, and joins. These effects explain why a purely textual split is unsafe.

## Proposed module boundaries and dependency direction

The eventual design should make the run controller the only place that orders independent lifetimes. Public/internal headers should prefer value objects and narrow ports over exposure of global state. A possible target layout is:

* `headless_cli`: option values, usage, and syntax-level parsing. It may depend on configuration value types, but not on camera SDK, CUDA, threads, or child processes.
* `headless_experiment`: experiment JSON schema, validation, run-matrix expansion, and immutable run plans. It should be usable in host-only tests.
* `headless_camera_runtime`: discovery, selection, GPU mapping, open/close, and camera resource setup. Camera SDK and PTP dependencies terminate here.
* `headless_acquisition_runtime`: camera-thread start/join, per-thread context, failure propagation, and the narrow acquisition callback/controller interface. It consumes recording and optional ROI ports rather than reaching into their concrete owners.
* `headless_recording_lifecycle`: full-frame pipelines, drain/rolling state, camera-control transitions, and their teardown protocol. It owns neither external-recorder processes nor ROI sockets.
* `headless_spatial_roi_lifecycle`: the current `HeadlessSpatialRoiRecordingOwner` cluster—plan/contract verification, artifacts, session startup, snapshots, Finish/Abort, and status interpretation—behind a small session/artifact interface.
* `headless_frame_ipc`: frame-IPC configuration, endpoint/runtime management, worker joins, and summaries.
* `headless_workers`: YOLO/pose setup, event output, stop, and joins.
* `headless_sync` and `headless_monitoring`: PTP/host-stack operations and GPU/NIC diagnostics respectively.
* `headless_external_recorder_bridge`: bounded child-process supervision, summary verification, and external-recorder manifest conversion. It has no authority over full-frame or ROI lifecycle.
* `headless_manifest`: recording/session/clip/experiment DTOs and serialization. It may depend on value DTOs and narrow artifact ports, but not on camera SDK, CUDA, or thread implementation details.
* `headless_run_controller`: local-run state machine and dependency assembly. It orders preparation, bind/readiness, acquisition, cancellation, drain, joins, finalization, and manifest updates. Local and remote mode adapters can call it with different ports.

The remaining `orange_headless_client.cpp` should contain `main`, mode dispatch, compatibility wrappers, and construction of the run-controller dependencies. The intended direction is:

```text
main / mode adapters
        -> run controller
             -> camera runtime, acquisition, recording, ROI, IPC, workers,
                synchronization, monitoring, external recorder, manifests
        -> value/config types
```

The arrows denote orchestration/dependency direction. No extracted module should include the monolith, and modules should not call one another through globals. In particular, cancellation should be passed through an explicit port; the controller remains responsible for the outer-supervisor guarantee when a CUDA call or destructor can remain stuck.

## Staged extraction checklist

Each stage is a separately reviewable change with characterization tests and no intended semantic change.

1. Capture baseline behavior first: host build/link, GPU build where available, representative local/remote runs, artifact schemas, teardown order, and timing/error diagnostics.
2. Introduce internal value DTOs and pure helpers while retaining the existing functions as wrappers. Preserve default values, diagnostics, JSON key order where consumers depend on it, and error codes.
3. Extract CLI parsing and experiment planning. Keep the existing entry-point functions and signatures as compatibility wrappers; compare plans and validation failures against golden fixtures.
4. Extract camera discovery/open/selection and resource setup behind the existing `CameraParams`/SDK boundary. Preserve call order, GPU affinity, PTP behavior, and close semantics.
5. Extract frame IPC and worker lifecycle one subsystem at a time. Make ownership and join points explicit before moving code; do not change queue/socket protocols.
6. Extract full-frame recording and rolling state. Preserve drain-request, camera-thread join, worker stop, final drain, monitor stop, and camera-close ordering exactly.
7. After the current spatial-ROI feature has stabilized, extract the ROI owner and status/snapshot helpers. Preserve contract authentication, bind-versus-ready distinction, controller lifetime, Finish/Abort behavior, socket/artifact identity, and the documented outer-supervisor requirement.
8. Extract the external-recorder bridge and manifest builders. Keep process/PID authority, verifier behavior, and schema-compatible output unchanged.
9. Introduce a thin run controller and move local/remote mode bodies behind it. The old free functions remain forwarding wrappers until all callers migrate.
10. Only in a dedicated build-integration change, register the new translation units in CMake and remove wrappers after downstream callers and ABI/link checks are green. This document does not authorize that CMake change.

ABI/source compatibility strategy: make no public ABI change in the first extraction series; use internal headers/namespaces, move definitions one boundary at a time, retain existing exported/free-function wrappers and default arguments, and avoid changing externally visible struct layouts or ownership. Keep call order and failure classification stable even when implementation moves.

## Test seams and regression gates

Pure host-only tests should cover CLI defaults/overrides, experiment validation and matrix expansion, GPU/camera mapping, JSON serialization, and exact error classifications. Golden fixtures should cover normal, incomplete, failed, rolling, and spatial-ROI-enriched snapshots.

Lifecycle tests should inject a fake camera SDK, clock, thread starter/joiner, recording pipeline, frame-IPC runtime, worker, monitor, and external-recorder child. They should deterministically exercise cancellation at every startup boundary, duplicate Stop/Finish/Abort calls, thread-start failure, join failure, drain timeout, and destructor failure. ROI tests must assert socket-bound is not recorder-ready, producer/controller arm/disarm ordering, snapshot transitions, no reconnect/restart, and preservation of the outer supervisor for stuck CUDA/destructor calls.

Before each extraction lands, require:

* host compile and link plus the focused unit/host tests;
* available GPU build/tests and a representative local headless smoke run;
* legacy remote-manager and one-/two-camera PTP paths;
* stream-only, full-frame, external-recorder, and spatial-ROI paths;
* artifact path/identity, schema, manifest, and teardown-order comparisons;
* ASan/UBSan and, where practical, TSan/deadlock-focused runs;
* no unexpected CMake target omissions, duplicate symbols, ABI changes, or new module dependency cycles.

## Risks and mitigations

* Thread contexts currently cross subsystem boundaries via raw pointers. Define context ownership, require joins before dependent destruction, and add lifetime tests before moving code.
* CUDA calls and destructors can block beyond a cooperative cancellation deadline. Keep the outer supervisor contract and process boundary; do not promise that an in-process join can always complete.
* `quit_server`, signals, and remote mode can create hidden shared-state coupling. Centralize cancellation as an explicit port and retain a compatibility adapter until all paths are covered.
* Socket paths, permissions, fsuid, atomicity, and directory creation are observable. Centralize filesystem/socket helpers and test failure after bind, partial setup, and cleanup authority.
* Static initialization and global handles may change when definitions move. Extract one owner at a time and use link/runtime smoke tests.
* Manifest and snapshot timing can drift even if capture succeeds. Use golden JSON plus event-order assertions and preserve update points.
* Duplicate ownership between full-frame, external-recorder, and ROI components can cause double cleanup or leaks. Give each resource one owner and make controller ordering explicit.
* Extra locks, copies, or serialization in a hot acquisition path can change frame cadence. Keep data-plane interfaces narrow and measure baseline throughput/latency.
* Error paths multiply when the state machine is split. Use an idempotent failure record and centralized teardown policy, with tests for every partially-started state.

## Explicit non-goals for the current spatial-ROI work

This is a deferred design only. Do not refactor `orange_headless_client.cpp` while the current spatial-ROI feature is being implemented or reviewed. In particular, this document does not authorize:

* production source, CMake, acquisition, camera-thread, or public API edits for modularization;
* changes to ROI, frame-IPC, full-frame, external-recorder, socket, or manifest protocols and schemas;
* changes to ownership, reconnect/restart, cancellation deadlines, or outer-supervisor semantics;
* public ABI breaks, remote-manager behavior changes, or performance redesign;
* splitting by arbitrary line count without first establishing ownership and dependency contracts.

The current feature should land with its additive lifecycle behavior intact. Modularization can begin only after its focused tests, integration gates, and artifact compatibility are recorded as the baseline for the staged extractions above.

## Ready-to-start criteria for a future extraction

The spatial-ROI feature is complete and separately tested; baseline artifacts and teardown traces are available; each extraction has one ownership objective, a host-only seam, and a rollback-sized diff; the host/GPU and compatibility gates pass; and a reviewer can verify that no protocol, ABI, timing, or outer-supervisor guarantee changed. At that point, begin with pure value/parsing code and leave the run controller and lifetime-sensitive code until the seams are proven.
