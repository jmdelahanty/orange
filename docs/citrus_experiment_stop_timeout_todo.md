# Citrus Experiment-End Delayed Stop TODO

Date: 2026-02-25
Scope: When a Citrus experiment ends, emit a control signal to `orange-jeremy`
so recording stops automatically after a grace period (default 10 seconds),
using the same safe drain/finalize behavior as manual stop.

## Transport Decision

Updated 2026-05-28: use the Orange local control socket contract for this path,
not SHAMAN/shared-memory queues. See
[orange_local_control_contract.md](./orange_local_control_contract.md).

The reason is boundary control: SHAMAN queues are live stimulus-state transport,
while experiment completion is a lifecycle command. Citrus and a future external
orchestrator should both talk to the same Orange-owned local control endpoint.
Orange remains responsible for translating any accepted request into its safe
recording stop/drain/finalize path.

Keep ENet as optional future fallback only for remote/multi-host deployments.

## Is This Possible?

Yes.

Current code already has:

- explicit recording start/stop state in `CameraControl`,
- encoder drain/finalize paths that run after `record_video` is cleared,
- an ENet receive thread in `orange-jeremy` that already parses wrapped
  FlatBuffer messages.

Refs:

- `src/orange.cpp:1094`
- `src/video_capture.h:71`
- `src/encoder_hw_worker.cpp:452`
- `src/crop_and_encode_worker.cpp:156`
- `src/gpu_video_encoder.cpp:336`
- `src/enet_thread.cpp:54`

## Current Baseline and Gap

- Recording stop in `orange-jeremy` is currently user-driven from the UI toggle.
- Citrus now has its first opt-in real-GUI autorun and local-control socket
  slice with `status`, `start_experiment`, and `stop_experiment`.
- Citrus now has an opt-in Orange completion notifier:
  `CITRUS_ORANGE_COMPLETION_NOTIFY=1`,
  `CITRUS_ORANGE_LOCAL_CONTROL_SOCKET=/tmp/orange_local_control.sock`, and
  `CITRUS_ORANGE_COMPLETION_GRACE_SECONDS=10`.
- `orange-jeremy` ENet receive handling currently updates calibration and
  peer-state signals, but does not map incoming control to recording actions.

Explicit limitation note (current state):

- Citrus completion emission is opt-in, not enabled by default.
- Orange/Citrus orchestrator live validation has passed for the
  orchestrator-owned `stop_recording` path. Direct Citrus completion emission
  into Orange's delayed stop path is still pending live validation.

Refs:

- `src/ui/arena_view_ui.cpp:1305` (Citrus STOP ALL)
- `src/core/arena.cpp:994` (Citrus auto-stop when protocol ends)
- `src/protocols/stimulus_protocol.cpp:345` (Citrus protocol stop)
- `src/enet_thread.cpp:74` (Orange message handling)

## Audit Update (2026-03-16)

- Re-checked the current repo: this control path still does not exist.
- `orange-jeremy` ENet receive handling still only covers:
  - client bringup/state updates,
  - INDIGO peer registration,
  - calibration pose signals.
- Citrus-side GUI automation/local status exists.
- Orange has a default-on local-control endpoint and a GUI-thread pending
  command bridge.
- Orange has an opt-in local-control recording stop scheduler for
  `citrus_completion` and `stop_recording`.
- Citrus has opt-in automatic completion emission to Orange.
- Orange local-control status exposes delayed-stop drain progress, timeout
  reporting, and forced-finalize request telemetry. If a local-control drain
  exceeds its timeout, Orange requests forced-safe finalization through the
  existing stream-shutdown path, which stops acquisition, stops/joins workers,
  and then runs the normal session finalizer.
- Orange local-control status now also derives machine-readable stop
  `state`, `health`, and `error_code` fields, so an active drain timeout is
  reported as `health=critical` without clients inferring that from scheduler
  event strings.
- Integrated Orange/Citrus validation passed on 2026-05-29 for the
  orchestrator-owned `start_recording` / Citrus `start_experiment` /
  Orange `stop_recording` path:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_29_02_26_12`. Citrus reached
  `completed/protocol_finished`, Orange finalized with
  `recording.control.ack_state="executed"`, and the Orange GUI validator passed
  with `0` failures and `0` warnings. The direct Citrus
  `citrus_completion` notifier path remains an explicit remaining validation
  item.
- Existing SHM queues are per-camera frame/update queues (`/shm_cam_<serial>`), not a dedicated control channel, so the recommended control IPC transport remains future work.

## Desired Behavior

1. Citrus emits one `citrus_completion` request to Orange.
2. Orange validates, deduplicates, logs, and ACKs the request.
3. Once stop control is explicitly enabled, Orange starts a configurable grace
   timer. `citrus_completion` defaults to 10 seconds when the request omits
   `params.grace_seconds`; explicit `stop_recording` defaults to 0 seconds.
4. After timer expiry, Orange triggers recording stop through the same path as
   the GUI/operator stop.
5. Encoders and external recorders drain and finalize.
6. If drain exceeds timeout, Orange escalates with explicit error
   handling and telemetry.

## Implementation Plan

## Phase 0: Contract and Semantics

- [x] Define the first local control request/response contract:
  - `request_id` for idempotency,
  - `operation_id` for the Citrus experiment or orchestrator phase,
  - `method = citrus_completion`,
  - `params.reason`, `params.terminal_state`, and `params.grace_seconds`.
- [x] Define replay and dedup policy:
  - same `request_id` or same `method + operation_id` must be ignored after
    first accept.
- [x] Define repeated request policy while countdown active:
  - earliest-deadline-wins; later requests cannot extend recording.
- [x] Define what "exit" means:
  - stop recording only, not process shutdown.

## Phase 1: Citrus Emission Hook

- [x] Add local-control request emission at experiment terminal boundary in Citrus:
  - arena stop command path,
  - protocol-finish path that leads to `Arena::Stop()`.
- [x] Add first Citrus local-control/status socket and GUI autorun surface:
  - `CITRUS_GUI_LOCAL_CONTROL_SOCKET`,
  - `status`,
  - `start_experiment`,
  - `stop_experiment`.
- [x] Ensure emission happens once per experiment end event.
- [x] Write command to Orange's Unix-domain JSON control socket with idempotent
      `request_id`.
  - Citrus uses stable retry ids shaped like
    `citrus_completion:<experiment_id>:<terminal_state>:<reason>`.

Candidate hook points:

- `citrus/src/core/arena.cpp:459`
- `citrus/src/ui/arena_view_ui.cpp:1305`

## Phase 2: Orange Control Ingress

- [x] Add default-on diagnostic Orange GUI local-control socket with env disable.
- [x] Accept, validate, log, and ACK `status` and `citrus_completion`.
- [x] Keep `start_recording` / `stop_recording` unsupported in default
      diagnostic mode.
- [x] Accept opt-in `stop_recording` when local-control recording stop is
      explicitly enabled.
- [x] Accept opt-in `start_recording` when local-control recording start is
      explicitly enabled.
- [x] Do not mutate `CameraControl` directly from socket-reader context.
- [x] Introduce a thread-safe pending command bridge consumed by the main/UI
      thread before wiring recording stop.

Candidate hook points:

- `src/orange.cpp` main loop control section around recording toggle.

## Phase 3: Delayed Stop Scheduler

- [x] Add recording-stop scheduler state in `orange-jeremy`:
  - `stop_scheduled` flag,
  - `stop_deadline_ns` (steady clock),
  - `stop_request_id`.
- [x] On accepted command:
  - if recording is active, set deadline = now + `grace_seconds`,
  - if not recording, no-op with status log.
- [x] On main loop tick:
  - when deadline reached, trigger same stop transition as UI button path
    (`record_video=false`, `recording_draining=true`, `stop_record=true`).

## Phase 4: Safe Drain Timeout

- [x] Expose drain timeout telemetry in Orange local-control status:
  - `drain_active`,
  - `drain_timed_out`,
  - `drain_timeout_seconds`,
  - `drain_elapsed_seconds`,
  - `stop_triggered_at_utc`,
  - `drain_completed_at_utc`.
- [x] Add bounded drain timeout after delayed stop trigger (for example 30-60s).
- [x] Monitor `active_recorders` and drain progress for status/log telemetry.
- [x] Use drain progress monitoring for bounded forced-finalize policy.
- [x] On timeout, emit critical status.
- [x] On timeout, request forced-safe finalization through the existing stream
      shutdown path, which stops acquisition, stops/join workers, and then
      runs the normal session finalizer.
- [x] Ensure this path is idempotent and does not deadlock queues.

Refs:

- `src/orange.cpp:1128`
- `src/encoder_hw_worker.cpp:332`
- `src/crop_and_encode_worker.cpp:166`
- `src/gpu_video_encoder.cpp:346`

## Phase 5: Acknowledgment and Observability

- [x] Emit explicit status logs:
  - command received,
  - countdown started,
  - stop triggered,
  - drain complete or timeout.
- [x] Make the Orange/Citrus orchestrator summarize
      `local_control.recording_stop` and fail by default when
      `drain_timed_out=true`.
- [x] Expose pollable stop ACK state in `local_control.recording_stop.ack_state`
      and its `citrus_completion_stop` compatibility alias:
  - accepted,
  - executed,
  - failed_timeout.
- [x] Make the Orange/Citrus orchestrator preserve and validate that ACK state
      in `orange.local_control_stop_ack_state` and
      `orange.local_control_stop_ack_status_check`.
- [x] Persist terminal stop ACK state in `recording_session.json`
      `recording.control.ack_state` and validate it in GUI artifacts.
- [ ] Optionally add a future push/callback ACK back to Citrus if polling the
      shared Orange status is not enough.
- [x] Include `request_id` and timestamps in structured local-control JSONL
      logs:
  - socket-thread request/response rows,
  - GUI-thread start/stop scheduling rows,
  - stop trigger, drain timeout, and drain finalization rows.
- [x] Make the orchestrator preserve and summarize the Orange local-control
      JSONL event log as a run artifact.
- [x] Add an orchestrator gate for that event log so production-profile runs
      require matching socket rows and GUI-thread start/stop/drain lifecycle
      evidence before validators can pass.
- [x] Require forced-finalize event-log evidence when Orange status reports a
      local-control drain timeout.
- [x] Expose whether the forced-finalize path has requested Orange's existing
      stream-shutdown path in `local_control.recording_stop`.
- [x] Keep `--allow-orange-drain-timeout` diagnostic-only: the orchestrator
      still fails inconsistent timeout status that lacks forced-finalize or
      stream-stop-request evidence.
- [x] Persist timeout policy/consistency checks in the orchestrator summary.
- [x] Persist local-control drain completion, timeout, and forced-finalize
      evidence in `recording_session.json` stop-control metadata.
- [x] Validate persisted stop-control timeout/forced-finalize consistency in
      GUI artifact validation.

## Phase 6: Validation

- [x] Test protocol natural finish under the external orchestrator:
  - 2026-05-29 four-camera rolling run completed
    `good_cop_bad_cop_demo.json`, then the orchestrator stopped Orange through
    local control and observed/persisted `ack_state="executed"`.
- [ ] Test manual STOP ALL in Citrus and direct Citrus completion notifier after
      protocol natural finish.
- [x] Test duplicate stop-control packets:
  - socket-layer duplicate `request_id`,
  - socket-layer duplicate `method + operation_id`,
  - GUI-thread earliest-deadline policy for later stop requests.
- [x] Test stop-control while not recording:
  - GUI-thread command drain records `ignored_not_recording`,
  - no stop schedule is created.
- [ ] Test delayed Citrus-completion stop during high-throughput recording.
  Immediate orchestrator `stop_recording` during four-camera rolling external IPC
  recording passed on 2026-05-29; the default 10-second Citrus completion grace
  path still needs its own live gate.
- [ ] Test forced-timeout path with induced writer stall.

## Definition of Done

- [ ] Citrus experiment end causes `orange-jeremy` delayed stop automatically.
- [ ] Delay is configurable and defaults to 10 seconds.
- [x] Stop uses existing safe drain/finalize behavior for the orchestrator-owned
      `stop_recording` path.
- [ ] Drain timeout path is bounded and observable.
- [x] Duplicate/out-of-order control packets do not cause inconsistent state in
      the local-control scheduler tests.
