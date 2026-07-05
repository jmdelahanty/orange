# Orange Local Control Contract

Status: v1 contract, Orange GUI endpoint, opt-in recording start/stop
control slices, and optional epoch/seq command fencing.
Last updated: 2026-07-04.

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

The current Orange GUI endpoint is created by default. Lifecycle effects are
disabled unless an explicit recording-start or recording-stop gate is enabled:

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
`<socket>.events.jsonl` by default. The log includes both socket-thread
request/response rows and GUI-thread lifecycle rows with
`schema_id=orange.local_control.gui_event`, `event_at_utc`, `request_id`, and
`operation_id` where available. GUI-thread events include command acceptance,
start queued/ignored/triggered/failed, stop scheduled/kept/ignored/triggered,
drain timeout, drain finalized, and optional exit-after-finalize transitions.
Set `ORANGE_GUI_LOCAL_CONTROL_DISABLE=1` or `ORANGE_LOCAL_CONTROL_DISABLE=1` to
disable the endpoint for a diagnostic run. If both GUI-specific and generic
local-control env vars are present, the `ORANGE_GUI_*` value wins; this lets a
GUI launcher explicitly set `ORANGE_GUI_LOCAL_CONTROL_DISABLE=0` even if a
stale generic `ORANGE_LOCAL_CONTROL_DISABLE=1` remains in the shell.
Local-control recording stop is disabled by default. For integrated control
tests, enable it with `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=1` or
`ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP=1`. These env vars are true
overrides of app config: `1`/`true` enables and `0`/`false` disables generic
`stop_recording`. Enabled generic stop also allows `citrus_completion`. The
Citrus-specific aliases `ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1` and
`ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1` enable only
`citrus_completion`, leaving generic `stop_recording` disabled; set them to
`0`/`false` to override an app-config Citrus gate off. For ordinary manual GUI
sessions, Citrus completion-stop can be enabled
persistently in app config with
`gui.local_control.citrus_completion_stop_enabled = true`; keep
`gui.local_control.recording_start_enabled = false` when the operator should
still start recording manually.
The app-config updater can set this without hand-editing JSON:

```bash
scripts/update_app_config_display_profile.py \
  --profile citrus_safe \
  --manual-citrus-completion-control
```

Local-control recording start is disabled by default; enable it only for
orchestrator tests with `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=1`,
`ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START=1`, or
`gui.local_control.recording_start_enabled = true`. As with the stop gates,
env values override app config in both directions.

For a hands-on four-camera Orange GUI session that should accept only Citrus
STOP ALL / completion-stop requests, use the four-camera launcher option
`--manual-citrus-completion-control`. It sets `ORANGE_GUI_AUTORUN=0`, keeps
all autorun sub-actions disabled (`ORANGE_GUI_AUTORUN_ENABLE_STREAM=0`,
`ORANGE_GUI_AUTORUN_ENABLE_RECORD=0`, `ORANGE_GUI_AUTORUN_ENABLE_YOLO=0`,
`ORANGE_GUI_AUTORUN_ENABLE_CROP=0`, and
`ORANGE_GUI_AUTORUN_START_RECORDING=0`), sets
`ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=0` so the operator owns
recording start, sets `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=0`, and
enables `ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1`. It also keeps
`ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=0` so the GUI remains open after
manual validation. Use `--manual-local-control` only when the same manual
session should also accept generic `stop_recording` socket requests.

After a manual Orange GUI + Citrus run, validate the latest complete Orange
artifact with the Citrus-completion shortcut. Use `--natural-completion` when
the Citrus protocol ended on its own, or `--stop-all` when the operator used
Citrus STOP ALL:

```bash
scripts/validate_gui_citrus_completion_recording.py --natural-completion
scripts/validate_gui_citrus_completion_recording.py --stop-all
```

Before launching Orange, the manual profile can be preflighted without changing
Orange state:

```bash
scripts/check_gui_citrus_completion_ready.py --check-socket
```

That command validates app-config local-control gates and, if the Orange socket
is live, confirms the running GUI has socket `start_recording` and generic
`stop_recording` disabled while Citrus completion-stop is enabled. After Orange
is running and before starting Citrus, use the stricter live-socket gate:

```bash
scripts/check_gui_citrus_completion_ready.py --require-manual-citrus-ready
```

That profile requires the live socket, local-control start/generic-stop
disabled, Citrus completion-stop enabled, `recording_active=true`,
`ready_for_citrus_experiment=true`, and a non-empty recording folder.
Add `--wait-seconds 120` when running it during the operator start-recording
step.

The preflight's default app-config lookup matches Orange:
`ORANGE_APP_CONFIG_PATH`, then `ORANGE_GUI_APP_CONFIG_PATH`, then the
`SUDO_USER`/`HOME` Orange data root.

For an exact-folder live validation, capture Orange's active recording folder
and write a handoff JSON after the strict preflight succeeds:

```bash
ORANGE_CITRUS_HANDOFF=/tmp/orange_manual_citrus_completion_handoff.json
ORANGE_RECORDING_FOLDER=$(scripts/check_gui_citrus_completion_ready.py --require-manual-citrus-ready --wait-seconds 120 --write-handoff "${ORANGE_CITRUS_HANDOFF}" --print-recording-folder)
eval "$(scripts/validate_gui_citrus_completion_recording.py --handoff "${ORANGE_CITRUS_HANDOFF}" --print-citrus-env)"
scripts/validate_gui_citrus_completion_recording.py --handoff "${ORANGE_CITRUS_HANDOFF}" --stop-all
```

The handoff JSON includes its own absolute path, the exact Orange folder, the
Orange status response that proved readiness, the Citrus completion env values,
and handoff-based validation command arrays for STOP ALL and natural completion
outcomes. The handoff-aware validator audits the embedded status response
before dispatching the artifact validator, and can print shell exports for Citrus with
`scripts/validate_gui_citrus_completion_recording.py --handoff "${ORANGE_CITRUS_HANDOFF}" --print-citrus-env`.
Those shell exports are an override/check path, not the only supported Citrus
configuration path. Citrus can instead read the matching notify settings from
`/home/jeremy/citrus/system_config.yml`; the export path remains useful for
one-off runs because it checks that Citrus notification is enabled, the Citrus
socket equals the proven Orange socket, and the completion grace seconds value
is numeric and nonnegative.
It can also print the handoff-stored exact validation command with
`scripts/validate_gui_citrus_completion_recording.py --handoff "${ORANGE_CITRUS_HANDOFF}" --print-validation-command --natural-completion`
or `--stop-all`.

The step-by-step operator runbook lives in
`docs/manual_orange_citrus_completion_runbook.md`.

The GUI validation launcher and installed sudo wrapper forward these variables
when paths point under `/tmp`, `/run/user/1000`, or
`/home/jeremy/orange_data`.

The endpoint acknowledges `status`, `citrus_completion`, opt-in
`start_recording`, and opt-in `stop_recording`. Accepted mutating requests are
deduplicated by both `request_id` and `method + operation_id`, queued onto a
thread-safe bridge, and drained by the GUI thread. With the Citrus-only stop
gate enabled, `citrus_completion` can schedule a delayed recording stop while
generic `stop_recording` remains disabled. With the generic recording-stop gate
enabled, both `citrus_completion` and `stop_recording` can schedule a delayed
stop. With both stop gates disabled, Citrus completion requests are logged only
and `stop_recording` is rejected.
If that field is omitted, Orange resolves `citrus_completion` to a 10-second
grace period and `stop_recording` to an immediate `0`-second stop. The client
utility also defaults `citrus-completion --grace-seconds` to `10.0` and uses
`source=citrus` for that subcommand unless `--source` is explicitly supplied.
With the recording-start gate enabled, when Orange is streaming but not already
recording or finalizing, `start_recording` goes through the same GUI-thread
recording preflight and operator start path as the record button.

When a local-control stop actually finalizes a normal GUI recording, Orange
also copies the local-control JSONL event log into the recording folder as
`orange_local_control.events.jsonl` and patches `recording_session.json` under
`recording.control.event_log`. That makes the socket ACK and GUI-thread
lifecycle evidence travel with the recording artifact instead of depending on
the current contents of `/tmp`. When that manifest field is present,
`scripts/validate_gui_ptp_recording.py` audits the copied log even without an
explicit `--orange-local-control-event-log` path. The live JSONL event log is
truncated when the Orange local-control server starts, so copied manual-run
logs are scoped to the current GUI process.

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

For `citrus-completion`, the client defaults both `request_id` and
`operation_id` to the same stable Citrus retry key:
`citrus_completion:<experiment_id>:<terminal_state>:<reason>`. Passing
`--request-id` overrides only the request id; the operation id still defaults
to the terminal-state key unless `--operation-id` is also provided.

`start-recording` and `stop-recording` subcommands render/send the v1 schema.
`start-recording` requires the recording-start gate above; `stop-recording`
requires the recording-stop gate above.

## Request

Each request is one JSON object, written as one line or as one EOF-terminated
payload:

```json
{
  "schema_id": "orange.local_control.request",
  "schema_version": 1,
  "method": "citrus_completion",
  "request_id": "citrus_completion:citrus-exp-42:completed:protocol_finished",
  "operation_id": "citrus_completion:citrus-exp-42:completed:protocol_finished",
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
`method + operation_id` values are idempotent. `start_recording` is accepted
only when local-control recording start is enabled. `stop_recording` is accepted
only when local-control recording stop is enabled.

## Versioning, Unknown Fields, and Epoch/Seq Fencing

The v1 request schema is extended additively. Unknown request fields are
ignored by Orange (existing behavior, now contractual), so clients may send
newer optional fields to an older Orange without breaking; `schema_version`
stays `1` for additive extensions.

Two optional additive fields fence mutating commands to the recording
generation they were issued for: `epoch` and `seq`. Both are positive
integers (`uint64`, values `>= 1`; `0` or non-integers are rejected as
`invalid_request`). When both are absent the request runs in legacy mode:
exactly the pre-fence behavior, gated only by the `request_id` /
`method + operation_id` dedupe described above. Id-based dedupe still applies
to fenced requests as a fallback.

Server state behind the fence:

- `current_epoch` starts at `1` and advances every time a new Orange recording
  generation starts (any recording start: operator button, autorun, or
  local-control `start_recording`).
- `last_done_seq` starts at `0`, is reset to `0` on every epoch change, and
  advances only when a fenced command's effect actually completes on the GUI
  thread: a `start_recording` that triggers a recording start, or a stop whose
  drain finalizes. Acceptance and queueing alone do not advance it.

Every response, and the embedded `status` object, reports `current_epoch` and
`last_done_seq`; responses to fenced requests also echo the request's `epoch`
and `seq`.

Gating for mutating commands that carry `epoch`/`seq`, applied before the
id-based dedupe:

- `epoch < current_epoch`: the command was stamped for an older recording
  generation. Orange rejects it with `ok=false`, `accepted=false`, and error
  code `stale_epoch`, and does not queue it. This is what prevents a delayed
  or replayed stop from a previous run — even one retried with a fresh
  `operation_id` — from truncating the current recording.
- `epoch == current_epoch` and `seq <= last_done_seq`: the command (or a later
  one in the same epoch) already completed. Orange re-ACKs it with `ok=true`,
  `accepted=true`, `duplicate=true`, and does not queue it again.
- `epoch > current_epoch`: the client is ahead of Orange's last epoch bump.
  Orange adopts the client epoch, resets `last_done_seq` to `0`, and processes
  the command normally.

Recommended client pattern: learn `current_epoch` from a `status` request,
stamp every mutating command with that epoch and a monotonically increasing
`seq`, and retry with the identical `epoch`/`seq`/`operation_id` triple until
an ACK arrives. A `stale_epoch` rejection means the command's recording
generation is over and the command must be dropped, not retried with fresh
ids. Requests without `epoch`/`seq` remain fully supported.

The id-based dedupe sets are bounded (4096 entries each, oldest evicted
first), so extremely old `request_id`/`operation_id` values can eventually be
accepted again in very long GUI sessions; fenced clients are protected by the
epoch/seq gates regardless.

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
  active, expected camera selections match, Orange recording is already active,
  and any required external IPC full-frame/crop recorder supervisors are ready
- active recording folder and sink mode
- selected record/YOLO/crop camera serials
- full-frame external recorder lifecycle readiness
- crop external recorder lifecycle readiness
- `local_control.recording_start`: whether local-control recording start is
  enabled, whether a start request is pending, the current request/operation
  ids, source/reason, and the latest GUI-thread event
- `local_control.recording_stop`: whether generic `stop_recording` is enabled,
  whether a stop is scheduled, whether one has triggered, the current
  method/request/operation/experiment ids, terminal state/reason, remaining
  deadline seconds, drain telemetry, derived stop `state`, derived `ack_state`,
  derived `health`,
  `error_code`, and the latest scheduler event. Drain telemetry includes
  `drain_active`, `drain_timed_out`,
  `forced_finalize_requested`, `forced_finalize_stream_stop_requested`,
  `drain_timeout_seconds`, `drain_elapsed_seconds`,
  `stop_triggered_at_utc`, and `drain_completed_at_utc`.
- `local_control.citrus_completion_stop`: same scheduler/drain status, but
  with the `enabled` gate for Citrus `citrus_completion`

Status gate fields are JSON booleans, not string flags. Orchestrators should
not treat `"true"`/`"false"` strings as readiness, timeout, active/armed, or
forced-finalize booleans.

`ack_state` is the pollable local-control acknowledgment state for Citrus and
orchestrators: `accepted` after the GUI thread schedules the stop, `executing`
after the stop has triggered and drain/finalization is in progress, `executed`
after clean finalization, `failed_timeout` after the drain timeout threshold is
crossed, `ignored` for accepted commands that cannot affect recording, and
`idle`/`disabled` when no stop request is active.

The Orange/Citrus orchestrator records this as
`orange.local_control_stop_ack_state` and validates it after stop. A clean
finalized recording must end as `executed`; a drain-timeout path must end as
`failed_timeout` and still satisfy the separate forced-finalize consistency
checks. This gives Citrus and external run supervisors a pollable ACK surface
without adding a second callback transport.

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
  "request_id": "citrus_completion:citrus-exp-42:completed:protocol_finished",
  "operation_id": "citrus_completion:citrus-exp-42:completed:protocol_finished",
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

The response gate fields are JSON booleans, not string flags. Orchestrator
clients should treat `ok`, `accepted`, `duplicate`,
`diagnostic_only`, and `queued_for_gui_thread` as invalid if they arrive as
truthy strings such as `"true"`.

For `citrus_completion`, Orange validates, logs, queues the request for
GUI-thread handling, and acknowledges it. For `start_recording` and
`stop_recording`, Orange does the same when the corresponding opt-in gate is
enabled; otherwise it rejects the request. Duplicate `request_id` values and
duplicate `method + operation_id` values are acknowledged but are not queued a
second time for accepted methods.

The immediate socket response reports only immediate socket-thread effects:
`recording_lifecycle_mutated` is false because the socket thread never mutates
recording state. If local-control recording start or stop is enabled, the GUI
thread may later start, schedule, or trigger the lifecycle action.

The current repeated-request policy while a countdown is active is
earliest-deadline-wins. A later distinct completion request cannot extend an
already scheduled stop. A distinct completion request with an earlier deadline
replaces the pending deadline.

When the delayed stop triggers, it routes through the same GUI/operator stop
path:

- mark GUI recording stop requested
- call `request_drain_recording_run(...)`
- keep camera streaming and YOLO/crop processing alive while recording drains
- wait for `gui_finalize_recording_session_if_ready(...)`
- validate `recording_session.json`, `recording_snapshot.json`, and external
  recorder finalization artifacts

For local-control stops, finalized GUI `recording_session.json` manifests also
persist stop provenance under `recording.control`: method, request id,
operation id, `source="orange_gui_local_control"`, `command_source` for the
caller that sent the local-control command, Citrus experiment fields when
present, receive timestamp, trigger timestamp, grace seconds, and the
configured drain-timeout threshold.
This makes the artifact self-describing even after the socket server and
orchestrator process have exited.
The GUI validator can require this provenance with
`--expect-local-control-stop-method`,
`--expect-local-control-stop-operation-id`, and
`--expect-local-control-stop-command-source`; the four-camera Orange/Citrus
profile passes those checks plus an `ack_state="executed"` check by default.
For `--stop-policy citrus_completion_notify --citrus-run-seconds ...`, the
profile also validates the STOP ALL-like Citrus terminal metadata:
`terminal_state="stopped"` and `reason="stopped_by_local_control"`.
`scripts/summarize_gui_validation.py` and
`scripts/validate_gui_ptp_recording.py --json-out` also surface the same fields
under `recording_session.local_control_stop` so saved validation JSON can be
audited without reopening the manifest. The persisted stop-control object also
records drain lifecycle evidence: `drain_completed`,
`drain_completed_at_utc`, `drain_timed_out`, `forced_finalize_requested`,
`forced_finalize_stream_stop_requested`, terminal `ack_state`, `health`,
`error_code`, and the final `last_event` / `last_event_at_utc` values. Clean
finalization persists `ack_state="executed"`; drain timeout persists
`ack_state="failed_timeout"`. A failed-timeout ACK is valid only with
`drain_timed_out=true`, forced-finalize evidence, and
`error_code="drain_timeout"`. The GUI validator checks these fields for
internal consistency when local-control stop expectations are enabled.

Orange also records local-control drain observability after a triggered stop.
`ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS` sets the telemetry threshold,
falling back to `ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS`; the default is
`60` seconds and `0` disables timeout reporting. When the threshold is exceeded
while the GUI is still finalizing, Orange sets
`local_control.recording_stop.drain_timed_out=true`, logs a
`drain_timeout` event, reports `state=drain_timeout`, `health=critical`, and
`error_code=drain_timeout`, and requests forced-safe finalization through the
same stream-shutdown path used by the operator. If finalization later succeeds
after the timeout, the stop status becomes
`state=finalized_after_drain_timeout` and `health=warning` while retaining the
same error code.

When start triggers, it routes through the same GUI/operator start path:

- run the GUI recording preflight
- call `begin_recording_run(...)`
- rotate crop/pose/recording worker output folders
- update detect/crop/pose/spatial snapshots
- reset GUI recording timing and display frame-rate telemetry

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
after experiment start, `terminal_state`, and `last_operation_id`. It also
reports `output.perf_jsonl_enabled`, `output.perf_jsonl_path`, and
`output.perf_jsonl_path_known`. The orchestrator should treat
`perf_jsonl_enabled=true` as a readiness/config check, and
`perf_jsonl_path_known=true` as an artifact-collection check only after Citrus
has actually started/configured the experiment. Use the exact
`perf_jsonl_path` from status instead of globbing Citrus output directories.

Earlier Citrus validation: Citrus built, its unit suite passed with 84 tests,
and a real-display smoke on `DISPLAY=:1` answered `status` on DP-3 at
`1920x1080 @ 120Hz` with render loop active. At that time Orange completion
notification was disabled by default. Newer Citrus builds can enable the same
notify path through app config or env overrides.

Updated Citrus notifier slice: Citrus can now opt into notifying Orange after a
terminal experiment state through app config:

```yaml
citrus_runtime:
  orange_completion:
    enabled: true
    socket_path: /tmp/orange_local_control.sock
    grace_seconds: 10
    retry_interval_seconds: 2
    shutdown_flush_timeout_seconds: 5
```

The same values can be overridden per process with:

- `CITRUS_ORANGE_COMPLETION_NOTIFY=1`
- `CITRUS_ORANGE_COMPLETION_ENABLED=1`
- `CITRUS_ORANGE_LOCAL_CONTROL_SOCKET=/tmp/orange_local_control.sock`
- `CITRUS_ORANGE_COMPLETION_GRACE_SECONDS=10`
- `CITRUS_ORANGE_COMPLETION_RETRY_INTERVAL_SECONDS=2`
- `CITRUS_ORANGE_COMPLETION_SHUTDOWN_FLUSH_TIMEOUT_SECONDS=5`

Citrus uses stable retry request ids shaped like
`citrus_completion:<experiment_id>:<terminal_state>:<reason>`.
Orange returns `ok=true` and `accepted=true` for the first valid request.
Duplicate `request_id` values or duplicate `method + operation_id` values are
also acknowledged with `ok=true`, `accepted=true`, and `duplicate=true`, but
they are not queued to the GUI thread a second time. When Citrus autorun exits
after completion, Citrus should wait up to `shutdown_flush_timeout_seconds` for
one of those ACKs before closing.
Terminal states are stable and machine-parseable:

- `completed`
- `stopped`
- `failed`
- `start_rejected`

The specific cause is reported in `terminal_reason`, for example
`protocol_finished`, `stopped_by_local_control`, or
`stopped_by_autorun_duration`.

Full four-arena GoodCop/BadCop should first be tested with
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

First diagnostic tool:

```bash
scripts/orange_citrus_orchestrator.py \
  --operation-id fourcam-goodcop-smoke-001 \
  --require-citrus-perf-jsonl
```

The default mode is dry-run: it prints the exact Orange/Citrus socket paths,
environment overlays, and mutating request shapes but does not open sockets or
launch GUI processes. Add `--execute` only when Orange and Citrus are ready to
be controlled or when explicit launch commands were provided.

Attach to already-running GUI processes:

```bash
scripts/orange_citrus_orchestrator.py \
  --execute \
  --operation-id fourcam-goodcop-smoke-001 \
  --orange-socket /tmp/orange_local_control.sock \
  --citrus-socket /tmp/citrus_local_control.sock \
  --require-citrus-perf-jsonl \
  --summary-json /tmp/orange_citrus_run_summary.json
```

Optional process launch is explicit:

```bash
scripts/orange_citrus_orchestrator.py \
  --execute \
  --orange-command "scripts/run_gui_fourcam_external_ipc_validation.sh --citrus-display-safe" \
  --citrus-command "/home/jeremy/citrus/targets/citrus" \
  --citrus-env DISPLAY=:1 \
  --citrus-env XAUTHORITY=/run/user/1000/gdm/Xauthority \
  --require-citrus-perf-jsonl
```

The process-launch path injects Orange local-control start/stop gates and the
Citrus local-control socket env. It also sets/preserves the Orange
local-control JSONL event log path (`--orange-local-control-log`, or
`<orange-socket>.events.jsonl` by default), summarizes that log under
`orange.local_control_event_log`, and copies it into the Orange recording
folder's orchestrator artifact directory when artifact copying is enabled. It
can also require that log with `--require-orange-local-control-event-log`; in
that mode a run fails unless the log has matching socket request/response rows,
GUI-thread `gui_command_accepted`, `recording_start_queued`, and
`recording_start_triggered` evidence, and, for stop policies other than `none`,
`gui_command_accepted`, `recording_stop_scheduled`,
`recording_stop_triggered`, and `recording_drain_finalized` evidence for the
operation. The four-camera profile enables this requirement by default, with
`--allow-missing-orange-event-log` only for diagnostics. The
compact event-log summary preserves socket and GUI event timestamps plus
lifecycle detail fields such as grace seconds, drain timeout, forced-finalize,
health, and error code; the raw JSONL is copied alongside it for full event
detail. Mutating socket rows must include `queued_for_gui_thread=true`, proving
the socket thread handed the command to the GUI control loop. The summary also
preserves physical `row_index` order and the gate uses it to require queued
socket acceptance before GUI acceptance, GUI acceptance before start queueing,
start queueing before start trigger, stop acceptance before stop scheduling,
stop scheduling before stop trigger, and stop trigger before drain
finalization. The
`orange.local_control_event_log_check.request_chains[]` summary records the
per-request row counts, row indexes, presence booleans, observed
method/source values, socket ACK booleans, terminal metadata, and GUI
enabled-gate values used for that audit. Timeout and forced-finalize rows are
also folded into the stop request chain. The gate also requires timeout
diagnostics to contain both
`recording_drain_timeout` and `recording_drain_forced_finalize_requested` when
Orange status reports `local_control.recording_stop.drain_timed_out=true`, and
those rows must match the final stop request id rather than only existing
somewhere in the log. Their row order must show stop trigger before drain
timeout, drain timeout before forced-finalize request, and forced-finalize
request before drain finalization.
For manual Citrus-only validation, `scripts/validate_gui_ptp_recording.py`
can additionally assert the final stop request's GUI-thread gate values with
`--expect-local-control-generic-stop-enabled 0` and
`--expect-local-control-citrus-stop-enabled 1`; those checks require the copied
or explicit Orange local-control event log.
The orchestrated GUI autorun path also sets
`ORANGE_GUI_AUTORUN_START_RECORDING=0` and
`ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=0`, so an Orange GUI autorun launcher
can open cameras and start streaming while leaving recording start/stop to the
orchestrator. Launched orchestrator runs also set
`ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=1`, which closes Orange only
after a local-control `stop_recording`/`citrus_completion` request has drained
and finalized the recording session. It does not talk to Orange recorder
sockets and does not delete data. Stop/finalization still goes through Orange
`stop_recording` or `citrus_completion`, according to `--stop-policy`.

Post-run validators are opt-in for the base orchestrator:

```bash
scripts/orange_citrus_orchestrator.py \
  --execute \
  --orange-validation-command \
    "scripts/validate_gui_ptp_recording.py {orange_recording_folder} --json-out /tmp/orange_validation.json"
```

Validation commands run after Citrus reaches a terminal state and after Orange
finalization, when `--stop-policy` waits for finalization. Non-zero exit status
or timeout fails the orchestrator, and stdout/stderr tails plus return codes are
written under `validations[]` in the combined summary. Commands may use
`{operation_id}`, `{orange_recording_folder}`, and `{citrus_perf_jsonl_path}`
placeholders. Prefer `{orange_recording_folder}` for Orange validators so the
orchestrator validates the exact run reported by Orange status instead of the
newest artifact on disk. Placeholder expansion is strict: if a command uses an
artifact placeholder and the corresponding status field is missing or empty, the
orchestrator fails before running that validator.

After Orange finalization, the orchestrator also inspects
`local_control.recording_stop.drain_timed_out`. A timed-out drain fails the
orchestrator by default even if Orange eventually finalized, because it means
the run crossed the configured drain observability threshold. The combined
summary records `orange.local_control_recording_stop` and
`orange.local_control_stop_drain_timed_out`; it also records
`orange.local_control_stop_timeout_status_check`, which captures timeout policy
and forced-finalize consistency evidence. For
`--stop-policy citrus_completion_notify`, the orchestrator additionally records
`orange.local_control_citrus_notify_stop_status_check` and fails before
post-run validators if Orange's final local-control status does not show
`method=citrus_completion`, `source=citrus`, the expected operation id, and
terminal state/reason matching Citrus. The event-log summary records whether
GUI-thread stop trigger, drain timeout, forced-finalize request, and drain
finalization events were observed. When
`--require-orange-local-control-event-log` is enabled,
the combined summary also records
`orange.local_control_event_log_check`, and missing/invalid lifecycle evidence
fails the orchestrator before post-run validators execute. Required event-log
checks are request-specific. The start request must have an accepted `ok=true`
socket request/response row with a nonempty source and
`queued_for_gui_thread=true`, a matching `gui_command_accepted` row with
`start_enabled=true`, a matching `recording_start_queued` row, and a matching
`recording_start_triggered` GUI lifecycle row with `method=start_recording`.
The stop request must have an accepted
`ok=true` socket request/response row with `queued_for_gui_thread=true`, a
matching `gui_command_accepted` row with the method-specific gate enabled
(`stop_enabled=true` for `stop_recording`, or `citrus_completion_enabled=true`
for Citrus-owned completion stop), a stop-scheduled GUI lifecycle row, a
stop-trigger GUI lifecycle row, and a drain-finalized GUI lifecycle row
matching the final Orange stop request id and final stop
metadata: method, command source, operation id, terminal state, and reason
when present. The final stop status
must include method, command source/source, and operation id, so those
comparisons cannot silently degrade. When summarized row indexes are available,
those rows must appear in lifecycle order. Drain-finalized rows must also agree
with the final stop status for `drain_timed_out`, `health`, and `error_code`;
timeout rows must carry `forced_finalize_requested=true`,
`health=critical`, and `error_code=drain_timeout`, and forced-finalize rows
must identify the `stream_shutdown` action.
`citrus_completion_notify` runs additionally require the final stop metadata
and those socket/lifecycle events to show `method=citrus_completion` and
`command_source=citrus` / socket `source=citrus`. Use
`--allow-orange-drain-timeout` only for diagnostic runs where the timeout is an
expected observation rather than a pass/fail gate. That flag does not allow an
inconsistent timeout status: if `drain_timed_out=true`, the orchestrator still
requires request-specific `recording_drain_timeout` and
`recording_drain_forced_finalize_requested` event-log rows,
`forced_finalize_requested=true`, `ack_state=failed_timeout`,
`error_code=drain_timeout`, and lifecycle row ordering from stop trigger
through timeout, forced-finalize request, and drain finalization. Conversely,
request-specific timeout or forced-finalize rows are treated as contradictory
if the final Orange stop status does not report the matching timeout and
forced-finalize flags. The status-only check also rejects
`ack_state=failed_timeout`, forced-finalize flags, or
`state=finalized_after_drain_timeout` unless `drain_timed_out=true`. Once Orange reports
`finalized_after_drain_timeout` it also requires
`forced_finalize_stream_stop_requested=true`. The artifact validator applies
the same invariant to persisted `recording.control`: forced-finalize fields
are timeout-only, and completed timeouts must persist both the forced
stream-stop marker and `last_event="finalized_after_drain_timeout"`.

When Orange itself sees a local-control drain exceed
`ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS` /
`ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS`, it marks
`recording_stop.drain_timed_out=true`,
`recording_stop.forced_finalize_requested=true`, emits a
`recording_drain_timeout` GUI event, and requests forced-safe finalization by
taking the existing stream-shutdown path. Once that stream-shutdown request has
been issued, status also reports
`recording_stop.forced_finalize_stream_stop_requested=true`. That path stops
acquisition, stops and joins workers, shuts down recording pipelines, and then
calls the normal recording-session finalizer. The forced request is
idempotent; repeated GUI ticks do not re-request stream shutdown.

When the orchestrator launches Orange or Citrus itself, readiness waits also
poll those child processes. If a launched GUI exits before its relevant
readiness/completion evidence is captured, the orchestrator fails immediately
with the process label, PID, return code, and log path instead of waiting for a
generic socket timeout. Once Citrus terminal/perf-path evidence has been
captured, a later Citrus process exit is recorded in `started_processes[]` but
does not make Orange finalization fail.

There is one narrow Orange process-exit acceptance path. If launched Orange
exits with return code `0` while the orchestrator is waiting for
`recording_finalized`, the orchestrator may infer finalization only from the
last known recording folder's `recording_session.json`, and only when that
manifest's stop-control `operation_id` matches the current operation and its
control metadata proves drain completion. This covers the expected
`ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=1` race where Orange finalizes,
writes the manifest, removes the socket, and exits before one last status poll
can observe `readiness.recording_finalized=true`. Early Orange exits without
matching manifest evidence still fail.

After the orchestrator has captured Citrus terminal/perf-path evidence, waited
for Orange finalization, and run validators, it owns cleanup of the launched GUI
processes. Any launched child process still alive at that point is terminated by
process group and the action/return code are recorded in `started_processes[]`.
The orchestrator also removes the local-control socket files for GUI processes
it launched. Attach mode remains different: if Orange or Citrus was not
launched by this orchestrator, the orchestrator does not own that process or its
socket cleanup.

Launch mode also preflights the target local-control socket before starting a
new GUI process. If `/tmp/orange_local_control.sock` or
`/tmp/citrus_local_control.sock` is already answering, the orchestrator refuses
to launch over it so a stale/running GUI cannot be mistaken for the new run.
Use attach mode for already-running GUIs, or
`--allow-preexisting-orange-socket` / `--allow-preexisting-citrus-socket` only
for explicit diagnostics. The four-camera wrapper exposes this as
`--allow-preexisting-sockets`.

Four-camera Citrus profile wrapper:

```bash
scripts/run_orange_citrus_fourcam_orchestrator.sh
```

This wrapper is also dry-run by default. It composes the production-like
four-camera Orange launcher, the local Citrus GUI binary, real-display defaults
for tmux/ssh sessions, the Citrus-safe Orange display profile, Citrus perf
JSONL requirement, and the same local-control sockets. Add `--execute` only
for a live run.

The wrapper uses Citrus autorun only as a setup loader: it loads
`omnifin0` / `shadow` / `good_cop_bad_cop_demo.json`, then sets
`CITRUS_GUI_AUTORUN_START_DELAY_SECONDS=86400` so Citrus does not race ahead of
Orange recording start. The orchestrator still sends the real
`start_experiment` local-control request after Orange reports
`ready_for_citrus_experiment=true`. Citrus Orange-completion notification is
disabled by default in this profile so the orchestrator remains the single
recording-stop owner; use `--enable-citrus-orange-completion-notify` only for
an explicit notifier integration test.

The profile also adds a default Orange GUI PTP validation command. It checks the
four expected cameras, crop recording artifacts, hidden crop-preview counters,
external recorder status/hello/storage preflight, separate crop-recorder GPU
placement, source provenance, YOLO CPU affinity, active CPU isolation, and the
Citrus-safe display profile (`swap_interval=1`, GUI frame cap `30`, display
preview cap `10`). It also requires GUI timing telemetry and clean ImGui GLFW
size-cache telemetry, so a fresh orchestrated run must show cached
window/framebuffer size hits with no fallback/null-window calls. The default
command targets `{orange_recording_folder}`, not `--latest-complete`, so it
validates the specific artifact from this orchestrated run. It intentionally
does not enforce the old `45 fps` GUI p05 threshold because Citrus-safe mode
caps Orange's GUI loop at `30 fps`. Use `--skip-orange-validation` for
lifecycle-only diagnostics or `--orange-validation-command` to replace the
default.

The wrapper also forwards rolling-control options to the launched Orange
profile:

```bash
scripts/run_orange_citrus_fourcam_orchestrator.sh \
  --execute \
  --record-seconds 6 \
  --warmup-seconds 2 \
  --clip-seconds 2 \
  --citrus-run-seconds 6
```

When `--clip-seconds` is present, the default validator expects
`recording_session.json` mode `rolling_clips` and checks both the requested
recording-control `record_for_seconds` and clip duration. In orchestrated runs,
`--record-seconds` is a manifest/recorder contract intent, not the owner of the
stop clock: the orchestrator still stops Orange through the selected stop policy
after Citrus reaches a terminal state. The profile now forwards
`ORANGE_GUI_RECORD_FOR_SECONDS=<record-seconds>` explicitly so the live Orange
runtime and the validation command use the same recording-control value.
`--citrus-run-seconds` is optional; when present, the orchestrator waits until
Citrus reports the experiment active/armed, sends Citrus `stop_experiment` after
that active runtime, then waits for Citrus terminal state. That gives short
orchestrator smoke tests a deterministic Citrus terminal state while preserving
Orange-owned recording drain/finalization.
