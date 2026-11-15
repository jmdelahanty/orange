# Orange local-control event log, v1

What Orange writes to its local-control event log, for programs that follow
it instead of polling Orange's socket (the MHS adapters in `~/marmalade`,
the orchestrator, monitors). Written down 2026-11-15 on Jeremy's word; the
socket itself is `docs/orange_local_control_contract.md`.
`tools/local_control_events_contract_tests.py` fails if Orange writes an
event this file does not list, or this file lists one Orange no longer
writes.

## Where it is

- Live log: `ORANGE_GUI_LOCAL_CONTROL_LOG` if set, else
  `<socket path>.events.jsonl`; by default
  `/tmp/orange_local_control.sock.events.jsonl`. orange_client uses the same
  naming.
- It is **truncated when Orange's control server starts** (each GUI or
  orange_client launch, and a server restart after a listener failure). Follow
  it by file identity and size, and treat a shrink as a new log, never as an
  offset to resume from.
- Each recording also keeps a copy, `orange_local_control.events.jsonl` at the
  recording root, sealed with the folder. That copy ends at the recording's
  `recording_drain_finalized`: the exit and receipt events after it are only in
  the live log.

## Rows

One JSON object per line, appended. Two kinds:

1. **Socket rows**, written by the control server for every request except
   `status`: `received_at_utc`, `request` (the request as received),
   `response` (Orange's reply), and for `start_recording` also `peer`
   (`{uid, gid, pid, user}` from `SO_PEERCRED`). No `schema_id`.
2. **Event rows**: `schema_id` `orange.local_control.gui_event` (GUI) or
   `orange.local_control.orange_client_event` (orange_client),
   `schema_version` 1, `event`, `event_at_utc`, then fields per event. Most
   rows about a command also carry `method`, `request_id`, `operation_id`,
   `command_source`, `received_at_utc`.

## Event rows, by what they mean

Recording start (local-control `start_recording`):

| event | meaning |
| --- | --- |
| `gui_command_accepted` | the owner took a queued command off the socket |
| `recording_start_queued` | a start is queued; `declared_by` (null without a declaration) and `peer` |
| `recording_start_ignored` | a queued start was dropped; `reason` (e.g. `cameras_not_open`, `already_recording`, `invalid_declaration` with `detail`) |
| `recording_start_pending` | the start passed its checks; recorders are starting |
| `recording_start_triggered` | recording is on |
| `recording_start_failed` | the start failed after it was pending; `reason` |

Recording stop (Citrus completion or `stop_recording`):

| event | meaning |
| --- | --- |
| `recording_stop_scheduled` | a stop is scheduled; `terminal_state`, `grace_seconds`, `experiment_id` |
| `recording_stop_schedule_kept` | a second stop request kept the first schedule |
| `recording_stop_ignored` | a stop request was dropped; `reason` |
| `recording_stop_triggered` | recording stopped; drain started |
| `recording_drain_timeout` | the drain passed its timeout |
| `recording_drain_forced_finalize_requested` | finalize forced after the drain timeout |
| `recording_drain_finalized` | the recording is finalized (its manifest is written) |

After the recording (live log only):

| event | meaning |
| --- | --- |
| `finalized_exit_pending`, `finalized_exit_pending_stream_stop` | exit-after-finalize is pending (streaming or not) |
| `exit_waiting_for_citrus_receipts` | a bound recording: the exit waits for Citrus's receipts; `recording_folder`, `limit_s` |
| `citrus_receipts_finalized` | Citrus's receipts are stored for `recording_folder`; `waited_s` (GUI) |
| `citrus_receipts_not_received` | the receipt wait ended without them; `reason` (`timeout`, `citrus_exited`) |
| `finalized_stream_stop_requested` | streaming stop requested before the exit |
| `finalized_exit_requested` | the GUI is closing |

Control server:

| event | meaning |
| --- | --- |
| `server_down` | the listener failed; `reason` |
| `server_recovered` | the listener is back; `reason` |

## Compatibility

- Consumers must ignore event names and fields they do not know.
- Within `schema_version` 1 Orange may **add** events and fields (and lists them
  here in the same commit), but does not rename or remove an event, or change
  what one means; that needs `schema_version` 2 and a heads-up to the
  consumers named above.
- Timestamps are UTC ISO-8601 with `Z`.
