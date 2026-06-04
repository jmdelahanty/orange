# Session Orchestration Architecture

## Purpose

This note defines the target session/orchestration shape for Orange after the
recent split-GOP, schema-3 config, headless, and GUI modularization work.

The goal is to answer a specific design question:

- should GUI recording, local headless recording, and future distributed
  synchronized recording share one orchestration layer?

The answer is **yes**, but not as one giant UI-driven controller.

Orange should move toward:

- one shared session orchestration core
- multiple frontend/adapters that call into that core
- a future coordinator/executor split for distributed synchronized recording

## Current State

Today the branch already shares the most important runtime pieces:

- schema-3 camera recording config
- `ResolvedRecordingConfig`
- `ModernRecordingPipeline`
- split-GOP validation / preflight
- shared recording artifacts and snapshot format
- external recorder contract materialization and fail-fast artifacts

But GUI and headless still have separate orchestration above that runtime:

- GUI session control lives mostly in:
  - [src/orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp)
  - [src/session/recording_session.cpp](/home/jeremy/orange-gop-split-a16/src/session/recording_session.cpp)
- headless orchestration still lives mostly in:
  - [src/orange_headless_client.cpp](/home/jeremy/orange-gop-split-a16/src/orange_headless_client.cpp)

So the branch is in a transitional state:

- shared recording runtime: mostly yes
- shared recording policy/validation: yes
- shared session lifecycle/orchestration: not yet

Related buffer-ownership follow-up:

- `docs/gui_display_recording_buffer_ownership_plan.md`

## Design Principle

The target is **one shared orchestration core**, not one monolithic class that
mixes:

- GUI rendering
- CLI/spec parsing
- network transport
- recording lifecycle

Those concerns should stay separate.

What should be shared is the actual session state machine and lifecycle logic.

## Target Shape

### Shared Session Core

Introduce a reusable session core that owns:

- session preflight
- session preparation
- artifact preparation
- stream/pipeline creation
- record arm/start
- stop request
- drain/finalize
- final session result/status summary

This is the part that should become the single source of truth for recording
session lifecycle semantics.

Possible naming:

- `SessionOrchestrator`
- `RecordingSessionController`
- `CaptureSessionController`

The name matters less than the boundary:

- no ImGui
- no CLI parsing
- no direct ENet protocol logic

### Frontend / Adapter Layers

These should stay separate:

- GUI adapter
  - buttons
  - panel state
  - human-readable status
- local headless adapter
  - experiment spec parsing
  - CLI flags
  - local benchmark wrapper behavior
- future network coordinator adapter
  - command distribution
  - readiness collection
  - start/stop barriers

These layers should translate their local inputs into the shared orchestration
calls instead of reimplementing the lifecycle.

## Future Distributed Model

For distributed synchronized recording, the best end state is not “the GUI does
everything.”

It is:

- one **SessionCoordinator**
- one or more **NodeExecutors**

### SessionCoordinator

The coordinator is the control-plane owner.

Responsibilities:

- build the resolved session plan
- split it into per-node execution plans
- issue prepare/start/stop/finalize commands
- collect readiness from nodes
- manage session barriers
- gather final run status / artifact summaries

The coordinator may be:

- a GUI host
- a headless orchestration host
- a future experiment manager

For local Orange/Citrus runs, the coordinator should use Orange's local control
contract rather than inventing a second stop/start mechanism. See
[orange_local_control_contract.md](./orange_local_control_contract.md). That
same contract is also the intended target for Citrus experiment-completion
notifications.

The first diagnostic local coordinator is
`scripts/orange_citrus_orchestrator.py`. It coordinates already-running or
explicitly-launched Orange/Citrus GUI processes through their Unix-domain JSON
local-control sockets. Its default mode is dry-run so the run plan/request
shape can be reviewed before any stimulus window or recording lifecycle is
touched; `--execute` is required to send socket requests.
The orchestrator consumes Orange's pollable stop ACK state from
`local_control.recording_stop.ack_state`, preserves it as
`orange.local_control_stop_ack_state` in the combined summary, and fails
inconsistent terminal states by default. Clean finalized stops must report
`executed`; drain-timeout paths must report `failed_timeout` and pass the
forced-finalize telemetry checks.

For the local four-camera Citrus integration profile, use
`scripts/run_orange_citrus_fourcam_orchestrator.sh`. That wrapper keeps the
same dry-run-first boundary while composing the production-like Orange
four-camera external-IPC launcher, the Citrus GUI binary, real-display env
defaults, Citrus perf JSONL collection, the Orange/Citrus socket paths, and a
default post-finalization Orange artifact validator. The wrapper also requires
Orange's local-control JSONL event log by default and uses an operation-scoped
`/tmp/<operation_id>_orange_local_control.events.jsonl` path, so a profile run
must show both socket-level requests and GUI-thread start/stop/drain lifecycle
events for the operation before it can pass without copying stale rows from an
older shared socket log. The compact summary keeps socket/GUI timestamps and
timeout/health lifecycle details so the copied `orchestrator_summary.json`
can be used as a first-pass audit artifact before opening the raw JSONL. The
start and stop requests must show both an accepted socket response with
`queued_for_gui_thread=true` and a GUI-thread `gui_command_accepted` row with
the relevant local-control gate enabled before their trigger/finalize lifecycle
rows; the start request must also show a `recording_start_queued` row before
the start trigger, and stop requests must show `recording_stop_scheduled`
before the stop trigger. The start trigger is required to identify
`method=start_recording`, matching the accepted socket request. The event-log
summary preserves row indexes, and the gate uses them to reject out-of-order
command acceptance, start queueing, stop scheduling, trigger, and finalization
evidence. The same gate checks final drain status consistency: finalized rows
must match final `drain_timed_out`, `health`, and `error_code` status, and
timeout rows must carry the expected forced-finalize timeout telemetry. The
check result includes a compact
`request_chains[]` audit for the expected start and stop request ids, including
row indexes, presence booleans, observed method/source values, socket ACK
booleans, terminal metadata, enabled-gate values, and any timeout /
forced-finalize evidence tied to that stop request, so the copied orchestrator
summary can be inspected without manually filtering the raw event arrays.
For bounded local-control smokes,
`--citrus-run-seconds <seconds>` is owned by the orchestrator: it starts Citrus
through local control, waits for active/armed state, sends `stop_experiment`
after that active runtime, and only then asks Orange to stop/drain/finalize.
The wrapper now has three stop-policy modes:

- `stop_recording`: the orchestrator sends Orange `stop_recording`; default
  grace is `0` seconds.
- `citrus_completion`: the orchestrator sends Orange `citrus_completion` after
  Citrus reaches terminal state; default grace is `10` seconds.
- `citrus_completion_notify`: Citrus sends Orange `citrus_completion` itself,
  and the orchestrator only waits for Orange to finalize and validates that
  Orange persisted `method=citrus_completion`, `command_source=citrus`, and
  terminal ACK state.

### NodeExecutor

The executor is the data-plane owner on each machine.

Responsibilities:

- open local cameras
- run local preflight
- create local recording pipelines
- record locally
- drain/finalize locally
- write local artifacts
- return local status to the coordinator

This is the piece that future remote camera servers should run.

## Lifecycle Model

The shared session core should eventually express a lifecycle close to:

1. `configure`
2. `preflight`
3. `prepare`
4. `ready`
5. `start`
6. `recording`
7. `stop_requested`
8. `draining`
9. `finalized`
10. `failed` / `aborted`

That gives GUI, headless, and distributed control the same semantics even when
they enter the system differently.

## What Should Move Into The Shared Core

Near-term shared responsibilities:

- build per-camera session inputs from current config/selection
- run the current shared preflight
- prepare recording folder + metadata artifacts
- create / start / stop / shutdown recording pipelines
- manage record start/stop/drain state
- expose current session status to callers

Later shared responsibilities:

- PTP/barrier coordination hooks
- session id / generation handling
- per-node status aggregation
- coordinated stop/failure semantics

## What Should Stay Outside

Keep these out of the shared session core:

- ImGui widgets and panel rendering
- CLI/spec file parsing
- ENet packet definitions and wire protocol handling
- host-specific monitoring sidecars unless abstracted as optional hooks

Those are adapters around the orchestration layer, not the orchestration layer
itself.

## Near-Term Refactor Direction

The current branch should move incrementally toward this shape.

### Phase 1

Complete the current GUI/session extraction:

- keep thinning `src/orange.cpp`
- move remaining record start/stop lifecycle work into
  `src/session/recording_session.*`

### Phase 2

Start lifting shared lifecycle helpers out of GUI-specific assumptions:

- artifact preparation
- external recorder contract materialization
- external recorder supervisor plan generation
- session status/result objects
- stop/drain/finalize semantics

At this point `recording_session.*` becomes the first draft of the shared
session core. The GUI external-recorder path now uses the same
`orange.external_recorder.contract` and supervisor-plan contract as headless,
and has a first supervised single-clip lifecycle slice for `external_ipc`.

The first concrete helper extracted for this is
`src/external_recorder_contract_utils.*`. It owns contract extraction,
per-camera materialization, supervisor-plan artifact writing, and shared
external-recorder artifact shapes used by both GUI/session and headless paths.
The helper is linked into both `orange` and `orange_client` so later
headless/session consolidation can reuse the same materialization rules instead
of copying entrypoint-local JSON construction.

The second extracted helper is `src/external_recorder_lifecycle.*`, which moves
supervised recorder process start/stop, socket/session environment handoff,
runtime artifact writing, and verifier-handoff writing behind a shared call
boundary. Headless uses it, the 2026-05-07 two-camera supervised PTP validation
passed through that path, and GUI/session external IPC now uses it for record
start, drain/finalization, and recorder summary collection.

### Phase 3

Refactor headless to call the same orchestration core instead of owning a
parallel lifecycle in `orange_headless_client.cpp`.

This is the first point where GUI and headless stop being “two entry paths
feeding the same runtime” and become “two adapters around the same session
controller.”

### Phase 4

Add an explicit coordinator/executor boundary for distributed synchronized
recording.

This should happen only after local GUI/headless lifecycle parity is strong.

## Non-Goals For The Current Branch

This note does **not** propose:

- building full distributed orchestration immediately
- making the GUI the long-term owner of all coordination logic
- merging all code paths into one mega-class
- changing split-GOP behavior as part of the orchestration refactor

The target is architectural convergence, not a large feature rewrite.

## Recommendation

For Orange’s future synchronized multi-machine recording goals, one shared
orchestration layer is the right foundation.

But the shape should be:

- shared session core
- separate GUI/headless/network adapters
- later, coordinator + executor roles

That is the cleanest path to:

- local GUI recording
- local headless experiments
- future remote synchronized recording

without keeping three different session lifecycles in permanent parallel.

## Related Docs

- [gui_modularization_plan.md](/home/jeremy/orange-gop-split-a16/docs/gui_modularization_plan.md)
- [recording_panel_modularization_plan.md](/home/jeremy/orange-gop-split-a16/docs/recording_panel_modularization_plan.md)
- [gui_recording_status.md](/home/jeremy/orange-gop-split-a16/docs/gui_recording_status.md)
- [experiment_runner_plan.md](/home/jeremy/orange-gop-split-a16/docs/experiment_runner_plan.md)
