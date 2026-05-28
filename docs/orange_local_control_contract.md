# Orange Local Control Contract

Status: v1 contract, Orange GUI endpoint, and first opt-in Citrus completion
stop slice.
Last updated: 2026-05-28.

## Purpose

Orange needs one local control plane that can serve two related workflows:

- Citrus completion control: Citrus reports that an experiment reached a
  terminal state.
- External orchestration: a third process starts Orange/Citrus, waits for
  readiness, starts recording/experiment phases, stops recording, and validates
  artifacts.

These must not become separate control mechanisms. Orange remains the authority
for camera acquisition, recording start/stop, drain, recorder shutdown, and
artifact finalization.

## Transport

The durable local transport is a Unix-domain JSON socket owned by Orange.

Do not use SHAMAN/shared-memory frame queues for this control path. Those queues
are live stimulus state contracts, not lifecycle command channels. Do not talk
directly to external recorder sockets from Citrus or the orchestrator; those are
Orange-owned recorder implementation details.

The current Orange GUI endpoint is created by default and is diagnostic-only:

```bash
./scripts/run_gui_fourcam_external_ipc_validation.sh ...
```

Default socket:

```text
/tmp/orange_local_control.sock
```

Override it with `ORANGE_GUI_LOCAL_CONTROL_SOCKET` or
`ORANGE_LOCAL_CONTROL_SOCKET`. Events are logged to
`ORANGE_GUI_LOCAL_CONTROL_LOG` / `ORANGE_LOCAL_CONTROL_LOG`, or to
`<socket>.events.jsonl` by default. Set `ORANGE_GUI_LOCAL_CONTROL_DISABLE=1` or
`ORANGE_LOCAL_CONTROL_DISABLE=1` to disable the endpoint for a diagnostic run.
Citrus completion-driven recording stop is disabled by default; enable it only
for integrated control tests with `ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1`
or `ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1`.

The GUI validation launcher and installed sudo wrapper forward these variables
when paths point under `/tmp`, `/run/user/1000`, or
`/home/jeremy/orange_data`.

The endpoint acknowledges `status` and `citrus_completion`. Accepted
`citrus_completion` requests are deduplicated by both `request_id` and
`method + operation_id`, queued onto a thread-safe bridge, and drained by the
GUI thread. With the Citrus stop env disabled, the GUI thread only logs them.
With the env enabled and Orange actively recording, the GUI thread schedules a
delayed stop using `params.grace_seconds`.

Use the client utility to inspect or send requests:

```bash
scripts/orange_local_control_client.py \
  --socket /tmp/orange_local_control.sock \
  status

scripts/orange_local_control_client.py \
  --socket /tmp/orange_local_control.sock \
  citrus-completion \
  --experiment-id citrus-exp-42 \
  --terminal-state completed \
  --reason protocol_finished \
  --grace-seconds 10
```

`start-recording` and `stop-recording` subcommands already render/send the v1
schema, but the current diagnostic Orange endpoint rejects them with
`unsupported_in_diagnostic_mode`.

## Request

Each request is one JSON object, written as one line or as one EOF-terminated
payload:

```json
{
  "schema_id": "orange.local_control.request",
  "schema_version": 1,
  "method": "citrus_completion",
  "request_id": "uuid-or-run-unique-id",
  "operation_id": "citrus-experiment-id-or-orchestrator-phase-id",
  "source": "citrus",
  "sent_at_utc": "2026-05-28T20:10:00Z",
  "params": {
    "experiment_id": "citrus-exp-42",
    "terminal_state": "completed",
    "reason": "protocol_finished",
    "grace_seconds": 10
  }
}
```

Supported methods:

- `status`
- `start_recording`
- `stop_recording`
- `citrus_completion`

`request_id` is required for all requests. Mutating methods also require
`operation_id`; duplicate `request_id` values and duplicate
`method + operation_id` values are idempotent. In the current slice,
`start_recording` and `stop_recording` are schema-defined but return
`unsupported_in_diagnostic_mode`.

## Status Semantics

Orange readiness means more than process started. Status reports:

- derived `phase` (`idle`, `cameras_open`, `streaming`, `recording`, or
  `recording_finalizing`)
- cameras open
- streaming active
- selected/open camera serials and expected serial match
- recording active or finalizing
- `ready_for_recording_request`, true only when cameras are open, streaming is
  active, expected camera selections match, and no recording is active/finalizing
- `ready_for_citrus_experiment`, true only when cameras are open, streaming is
  active, expected camera selections match, and Orange recording is already
  active
- active recording folder and sink mode
- selected record/YOLO/crop camera serials
- full-frame external recorder lifecycle readiness
- crop external recorder lifecycle readiness
- `local_control.citrus_completion_stop`: whether Citrus completion stop is
  enabled, whether a stop is scheduled, whether one has triggered, the current
  request/operation/experiment ids, terminal state/reason, remaining deadline
  seconds, and the latest scheduler event

Camera-set comparisons are order-insensitive. If no expected serials are
configured, match fields are reported as `null` and readiness falls back to the
observed runtime state.

External recorder readiness means the Orange supervisor lifecycle has started,
all expected recorder processes are active, and all recorder sockets are ready.

## Response

Every response is one JSON object:

```json
{
  "schema_id": "orange.local_control.response",
  "schema_version": 1,
  "ok": true,
  "accepted": true,
  "duplicate": false,
  "diagnostic_only": true,
  "queued_for_gui_thread": true,
  "request_id": "uuid-or-run-unique-id",
  "operation_id": "citrus-experiment-id-or-orchestrator-phase-id",
  "method": "citrus_completion",
  "responded_at_utc": "2026-05-28T20:10:00Z",
  "effect": {
    "gui_lifecycle_command_deferred": true,
    "recording_stop_requested": false,
    "recording_lifecycle_mutated": false
  },
  "status": {}
}
```

For `citrus_completion`, Orange validates, logs, queues the request for
GUI-thread handling, and acknowledges it. Duplicate `request_id` values and
duplicate `method + operation_id` values are acknowledged but are not queued a
second time.

The immediate socket response reports only immediate socket-thread effects:
`recording_lifecycle_mutated` is false because the socket thread never mutates
recording state. If `ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1` is set, the
GUI thread may later schedule and trigger the stop.

The current repeated-request policy while a countdown is active is
earliest-deadline-wins. A later distinct completion request cannot extend an
already scheduled stop. A distinct completion request with an earlier deadline
replaces the pending deadline.

When the delayed stop triggers, it routes through the same GUI/operator stop
path:

- mark GUI recording stop requested
- call `request_drain_recording_run(...)`
- wait for `gui_finalize_recording_session_if_ready(...)`
- validate `recording_session.json`, `recording_snapshot.json`, and external
  recorder finalization artifacts

## Citrus GUI Control Slice

Citrus now has its first opt-in real-GUI automation and local-control status
slice. Orange should treat it as a peer endpoint, not a child process.

Citrus autorun envs:

- `CITRUS_GUI_AUTORUN=1`
- `CITRUS_GUI_AUTORUN_RIG=omnifin0`
- `CITRUS_GUI_AUTORUN_CANVAS=shadow`
- `CITRUS_GUI_AUTORUN_PROTOCOL=good_cop_bad_cop_demo.json`
- `CITRUS_GUI_AUTORUN_START_DELAY_SECONDS=2`
- `CITRUS_GUI_AUTORUN_RUN_SECONDS=20`
- `CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=1`

Citrus local-control socket:

- `CITRUS_GUI_LOCAL_CONTROL_SOCKET=/tmp/citrus_local_control.sock`

Citrus socket methods:

- `status`
- `start_experiment`
- `stop_experiment`

Citrus request semantics match Orange's control shape:

- `schema_id = citrus.local_control.request`
- `schema_version = 1`
- `request_id` required for all requests
- `operation_id` required for mutating commands
- duplicate `request_id` is idempotent
- duplicate `method + operation_id` is idempotent

Citrus status schema is `citrus.gui_runtime.status` v1.
`readiness.ready_to_start` means:

- main GUI window exists
- stimulus display window exists
- stimulus render loop active
- at least one arena selected
- all selected arenas have a loaded protocol with steps
- no arena already running or armed
- no automation configuration error

Citrus status includes process/display identity, stimulus monitor geometry and
refresh, local-control socket/log info, selected rig/canvas/protocols,
per-arena runtime fields, latest stimulus/camera frame ids, output directory
after experiment start, `terminal_state`, and `last_operation_id`.

Current Citrus validation: Citrus builds, its unit suite passes with 83 tests,
and a real-display smoke on `DISPLAY=:1` answered `status` on DP-3 at
`1920x1080 @ 120Hz` with render loop active. Readiness was false when no arena
or protocol was loaded, as expected.

Known Citrus caveat: this slice does not yet send Citrus completion to Orange
automatically. Full four-arena GoodCop/BadCop should first be tested with
`good_cop_bad_cop_demo.json`.

## Orchestrator Role

A future orchestrator should use this same Orange endpoint. It should:

1. Start Orange.
2. Wait for Orange status readiness.
3. Start Citrus.
4. Wait for Citrus render/experiment readiness through the Citrus endpoint or
   CLI status.
5. Send Orange `start_recording`.
6. Start Citrus experiment.
7. Observe Citrus terminal state.
8. Send Orange `stop_recording` or `citrus_completion` depending on the chosen
   policy.
9. Wait for Orange finalized status.
10. Run Orange and Citrus artifact validators.

The orchestrator must not kill recorder/camera processes to end a run. Orange
owns that lifecycle.
