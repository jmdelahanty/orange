# Orange Local Control Contract

Status: v1 contract and Orange diagnostic endpoint slice.
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

The current Orange GUI endpoint is opt-in and diagnostic-only:

```bash
ORANGE_GUI_LOCAL_CONTROL_SOCKET=/tmp/orange_local_control.sock ./scripts/run_gui_fourcam_external_ipc_validation.sh ...
```

It also accepts `ORANGE_LOCAL_CONTROL_SOCKET`. Events are logged to
`ORANGE_GUI_LOCAL_CONTROL_LOG` / `ORANGE_LOCAL_CONTROL_LOG`, or to
`<socket>.events.jsonl` by default.

The diagnostic endpoint acknowledges `status` and `citrus_completion`. It does
not start or stop recording yet.

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
`operation_id`; duplicate `request_id` values must be idempotent. In the current
diagnostic slice, `start_recording` and `stop_recording` are schema-defined but
return `unsupported_in_diagnostic_mode`.

## Status Semantics

Orange readiness means more than process started. Status reports:

- cameras open
- streaming active
- selected/open camera serials and expected serial match
- recording active or finalizing
- active recording folder and sink mode
- selected record/YOLO/crop camera serials
- full-frame external recorder lifecycle readiness
- crop external recorder lifecycle readiness

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
  "request_id": "uuid-or-run-unique-id",
  "operation_id": "citrus-experiment-id-or-orchestrator-phase-id",
  "method": "citrus_completion",
  "responded_at_utc": "2026-05-28T20:10:00Z",
  "effect": {
    "recording_stop_requested": false,
    "recording_lifecycle_mutated": false
  },
  "status": {}
}
```

For diagnostic `citrus_completion`, Orange validates, logs, and acknowledges the
request but does not stop recording. Future stop wiring must happen by routing a
validated request onto the same GUI/operator stop path:

- mark GUI recording stop requested
- call `request_drain_recording_run(...)`
- wait for `gui_finalize_recording_session_if_ready(...)`
- validate `recording_session.json`, `recording_snapshot.json`, and external
  recorder finalization artifacts

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
