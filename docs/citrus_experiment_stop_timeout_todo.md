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
- Citrus stop paths exist (`STOP ALL`, protocol finish, or explicit arena stop),
  but Citrus does not currently emit a stop-control message to `orange-jeremy`.
- `orange-jeremy` ENet receive handling currently updates calibration and
  peer-state signals, but does not map incoming control to recording actions.

Explicit limitation note (current state):

- Citrus currently does not emit any dedicated "experiment ended" request to
  Orange's local control endpoint.

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
- No Citrus-side emission hook, no Orange-side delayed-stop scheduler, and no drain-timeout policy are implemented yet.
- Existing SHM queues are per-camera frame/update queues (`/shm_cam_<serial>`), not a dedicated control channel, so the recommended control IPC transport remains future work.

## Desired Behavior

1. Citrus emits one `citrus_completion` request to Orange.
2. Orange validates, deduplicates, logs, and ACKs the request.
3. Once stop control is explicitly enabled, Orange starts a 10-second grace
   timer.
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
  - same `request_id` must be ignored after first accept.
- [ ] Define repeated request policy while countdown active:
  - either keep earliest deadline or replace with latest, but pick one.
- [ ] Define what "exit" means:
  - stop recording only, not process shutdown.

## Phase 1: Citrus Emission Hook

- [ ] Add local-control request emission at experiment terminal boundary in Citrus:
  - arena stop command path,
  - protocol-finish path that leads to `Arena::Stop()`.
- [ ] Ensure emission happens once per experiment end event.
- [ ] Write command to Orange's Unix-domain JSON control socket with idempotent
      `request_id`.

Candidate hook points:

- `citrus/src/core/arena.cpp:459`
- `citrus/src/ui/arena_view_ui.cpp:1305`

## Phase 2: Orange Control Ingress

- [x] Add opt-in diagnostic Orange GUI local-control socket.
- [x] Accept, validate, log, and ACK `status` and `citrus_completion`.
- [x] Keep `start_recording` / `stop_recording` unsupported in diagnostic mode.
- [ ] Do not mutate `CameraControl` directly from socket-reader context.
- [ ] Introduce a thread-safe pending command bridge consumed by the main/UI
      thread before wiring recording stop.

Candidate hook points:

- `src/orange.cpp` main loop control section around recording toggle.

## Phase 3: Delayed Stop Scheduler

- [ ] Add recording-stop scheduler state in `orange-jeremy`:
  - `stop_scheduled` flag,
  - `stop_deadline_ns` (steady clock),
  - `stop_request_id`.
- [ ] On accepted command:
  - if recording is active, set deadline = now + `grace_seconds`,
  - if not recording, no-op with status log.
- [ ] On main loop tick:
  - when deadline reached, trigger same stop transition as UI button path
    (`record_video=false`, `recording_draining=true`, `stop_record=true`).

## Phase 4: Safe Drain Timeout

- [ ] Add bounded drain timeout after delayed stop trigger (for example 30-60s).
- [ ] Monitor `active_recorders` and drain progress.
- [ ] On timeout, emit critical status and execute forced-safe finalize path.
- [ ] Ensure this path is idempotent and does not deadlock queues.

Refs:

- `src/orange.cpp:1128`
- `src/encoder_hw_worker.cpp:332`
- `src/crop_and_encode_worker.cpp:166`
- `src/gpu_video_encoder.cpp:346`

## Phase 5: Acknowledgment and Observability

- [ ] Emit explicit status logs:
  - command received,
  - countdown started,
  - stop triggered,
  - drain complete or timeout.
- [ ] Optionally send ACK back to Citrus:
  - accepted,
  - executed,
  - failed-timeout.
- [ ] Include `request_id` and timestamps in all related logs.

## Phase 6: Validation

- [ ] Test manual STOP ALL in Citrus and protocol natural finish.
- [ ] Test duplicate stop-control packets.
- [ ] Test stop-control while not recording.
- [ ] Test delayed stop during high-throughput recording.
- [ ] Test forced-timeout path with induced writer stall.

## Definition of Done

- [ ] Citrus experiment end causes `orange-jeremy` delayed stop automatically.
- [ ] Delay is configurable and defaults to 10 seconds.
- [ ] Stop uses existing safe drain/finalize behavior.
- [ ] Drain timeout path is bounded and observable.
- [ ] Duplicate/out-of-order control packets do not cause inconsistent state.
