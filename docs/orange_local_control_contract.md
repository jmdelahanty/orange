# Orange Local Control Contract

Status: v1 contract, Orange GUI endpoint, and opt-in recording start/stop
control slices.
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
`<socket>.events.jsonl` by default. Set `ORANGE_GUI_LOCAL_CONTROL_DISABLE=1` or
`ORANGE_LOCAL_CONTROL_DISABLE=1` to disable the endpoint for a diagnostic run.
Local-control recording stop is disabled by default; enable it only for
integrated control tests with `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=1`
or `ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP=1`. The older Citrus-specific
aliases `ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1` and
`ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1` also enable the same recording-stop
gate.
Local-control recording start is disabled by default; enable it only for
orchestrator tests with `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=1` or
`ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START=1`.

The GUI validation launcher and installed sudo wrapper forward these variables
when paths point under `/tmp`, `/run/user/1000`, or
`/home/jeremy/orange_data`.

The endpoint acknowledges `status`, `citrus_completion`, opt-in
`start_recording`, and opt-in `stop_recording`. Accepted mutating requests are
deduplicated by both `request_id` and `method + operation_id`, queued onto a
thread-safe bridge, and drained by the GUI thread. With the recording-stop gate disabled, Citrus
completion requests are logged only and `stop_recording` is rejected. With the
gate enabled and Orange actively recording, both `citrus_completion` and
`stop_recording` schedule a delayed stop using `params.grace_seconds`.
With the recording-start gate enabled, when Orange is streaming but not already
recording or finalizing, `start_recording` goes through the same GUI-thread
recording preflight and operator start path as the record button.

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
`method + operation_id` values are idempotent. `start_recording` is accepted
only when local-control recording start is enabled. `stop_recording` is accepted
only when local-control recording stop is enabled.

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
- `local_control.recording_stop`: whether local-control recording stop is
  enabled, whether a stop is scheduled, whether one has triggered, the current
  method/request/operation/experiment ids, terminal state/reason, remaining
  deadline seconds, drain telemetry, and the latest scheduler event. Drain
  telemetry includes `drain_active`, `drain_timed_out`,
  `drain_timeout_seconds`, `drain_elapsed_seconds`,
  `stop_triggered_at_utc`, and `drain_completed_at_utc`.
- `local_control.citrus_completion_stop`: compatibility alias for the same
  recording-stop status

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
operation id, source, Citrus experiment fields when present, receive timestamp,
trigger timestamp, grace seconds, and the configured drain-timeout threshold.
This makes the artifact self-describing even after the socket server and
orchestrator process have exited.

Orange also records local-control drain observability after a triggered stop.
`ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS` sets the telemetry threshold,
falling back to `ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS`; the default is
`60` seconds and `0` disables timeout reporting. When the threshold is exceeded
while the GUI is still finalizing, Orange sets
`local_control.recording_stop.drain_timed_out=true`, logs a
`drain_timeout` event, and keeps using the normal safe drain/finalize path.
This is telemetry only; forced-safe finalize behavior is a separate future
policy.

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

Current Citrus validation: Citrus builds, its unit suite passes with 84 tests,
and a real-display smoke on `DISPLAY=:1` answered `status` on DP-3 at
`1920x1080 @ 120Hz` with render loop active. Orange completion notification was
disabled by default, as intended.

Updated Citrus notifier slice: Citrus can now opt into notifying Orange after a
terminal experiment state with:

- `CITRUS_ORANGE_COMPLETION_NOTIFY=1`
- `CITRUS_ORANGE_LOCAL_CONTROL_SOCKET=/tmp/orange_local_control.sock`
- `CITRUS_ORANGE_COMPLETION_GRACE_SECONDS=10`

Citrus uses stable retry request ids shaped like
`citrus_completion:<experiment_id>:<terminal_state>:<reason>`.
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
Citrus local-control socket env. It also sets
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
`orange.local_control_stop_drain_timed_out`. Use
`--allow-orange-drain-timeout` only for diagnostic runs where the timeout is an
expected observation rather than a pass/fail gate.

When the orchestrator launches Orange or Citrus itself, readiness waits also
poll those child processes. If a launched GUI exits before its relevant
readiness/completion evidence is captured, the orchestrator fails immediately
with the process label, PID, return code, and log path instead of waiting for a
generic socket timeout. Once Citrus terminal/perf-path evidence has been
captured, a later Citrus process exit is recorded in `started_processes[]` but
does not make Orange finalization fail.

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
