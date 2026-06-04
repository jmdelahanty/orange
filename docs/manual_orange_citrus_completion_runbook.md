# Manual Orange + Citrus Completion Runbook

Use this for the live manual GUI validation where Orange owns recording and
Citrus sends Orange a `citrus_completion` notification at experiment end.

## 1. Preflight App Config

Run before launching Orange:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/check_gui_citrus_completion_ready.py --check-socket
```

This validates the app-config gates. If Orange is already running, it also
checks the live socket gates.

## 2. Launch Orange

Use the manual Citrus-completion profile:

```bash
cd /home/jeremy/orange-gop-split-a16
DISPLAY=:1 \
XAUTHORITY=/run/user/1000/gdm/Xauthority \
XDG_RUNTIME_DIR=/run/user/1000 \
XDG_SESSION_TYPE=x11 \
./scripts/run_gui_fourcam_external_ipc_validation.sh \
  --hidden-crop-preview \
  --citrus-display-safe \
  --manual-citrus-completion-control
```

In Orange, manually open cameras, start streaming, enable the intended YOLO/crop
state, and start recording. Recording start remains operator-owned.

## 3. Verify Orange Is Ready For Citrus

After Orange is launched and before starting Citrus, run:

```bash
scripts/check_gui_citrus_completion_ready.py \
  --require-manual-citrus-ready \
  --wait-seconds 120
```

This requires a live Orange socket, socket start/generic-stop disabled, Citrus
completion-stop enabled, `recording_active=true`, Orange readiness for the
Citrus experiment, and a non-empty recording folder. With `--wait-seconds`,
the command can be started before or while the operator starts recording in
Orange; it returns only after Orange is ready for the Citrus experiment or the
timeout expires.

Then capture the exact Orange artifact path:

```bash
ORANGE_CITRUS_HANDOFF=/tmp/orange_manual_citrus_completion_handoff.json
ORANGE_RECORDING_FOLDER=$(scripts/check_gui_citrus_completion_ready.py \
  --require-manual-citrus-ready \
  --wait-seconds 120 \
  --write-handoff "${ORANGE_CITRUS_HANDOFF}" \
  --print-recording-folder)
echo "${ORANGE_RECORDING_FOLDER}"
```

The handoff JSON records its own absolute path, the exact Orange recording
folder, the Orange status response that proved readiness, Citrus notification
environment, and handoff-based validation commands for both STOP ALL and
natural completion outcomes. The handoff-aware validator rejects the file if
that status snapshot does not still show the expected manual Citrus gates and
recording folder.

## 4. Launch Citrus With Completion Notify

Citrus can be configured persistently in `/home/jeremy/citrus/system_config.yml`:

```yaml
citrus_runtime:
  orange_completion:
    enabled: true
    socket_path: /tmp/orange_local_control.sock
    grace_seconds: 10
    retry_interval_seconds: 2
    shutdown_flush_timeout_seconds: 5
```

With that config in place, the handoff env export below is optional. Use it when
you want the shell to override Citrus config for a specific run, or when you
want a quick check that the Citrus notify settings match the Orange socket and
grace value captured in the handoff:

```bash
eval "$(scripts/validate_gui_citrus_completion_recording.py \
  --handoff "${ORANGE_CITRUS_HANDOFF}" \
  --print-citrus-env)"
```

That verifies the handoff values and prints/evaluates the equivalent env
overrides:

```bash
export CITRUS_ORANGE_COMPLETION_NOTIFY=1
export CITRUS_ORANGE_LOCAL_CONTROL_SOCKET=/tmp/orange_local_control.sock
export CITRUS_ORANGE_COMPLETION_GRACE_SECONDS=10
```

The equivalent Citrus env override names are
`CITRUS_ORANGE_COMPLETION_NOTIFY` / `CITRUS_ORANGE_COMPLETION_ENABLED`,
`CITRUS_ORANGE_LOCAL_CONTROL_SOCKET`,
`CITRUS_ORANGE_COMPLETION_GRACE_SECONDS`,
`CITRUS_ORANGE_COMPLETION_RETRY_INTERVAL_SECONDS`, and
`CITRUS_ORANGE_COMPLETION_SHUTDOWN_FLUSH_TIMEOUT_SECONDS`.

Run the Citrus experiment. Do not send Orange `start_recording` or generic
`stop_recording`; Citrus should only notify Orange with `citrus_completion`.
If Citrus autorun uses `CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=1`, Citrus waits
up to `shutdown_flush_timeout_seconds` for Orange to ACK the completion request
before closing. Orange should ACK the first request with `ok=true` and
`accepted=true`; retry duplicates are idempotent and return `duplicate=true`
without queuing a second stop.

## 5. Validate Orange Finalization

If the operator used Citrus STOP ALL:

```bash
scripts/validate_gui_citrus_completion_recording.py \
  --handoff "${ORANGE_CITRUS_HANDOFF}" \
  --stop-all
```

If the Citrus protocol ended naturally:

```bash
scripts/validate_gui_citrus_completion_recording.py \
  --handoff "${ORANGE_CITRUS_HANDOFF}" \
  --natural-completion
```

To print the exact validation command stored in the handoff:

```bash
scripts/validate_gui_citrus_completion_recording.py \
  --handoff "${ORANGE_CITRUS_HANDOFF}" \
  --print-validation-command \
  --natural-completion
```

For a compact summary:

```bash
scripts/summarize_gui_validation.py "${ORANGE_RECORDING_FOLDER}"
```

Expected local-control evidence includes `method=citrus_completion`,
`command_source=citrus`, `ack_state=executed`,
`generic_stop_enabled=[False]`, `citrus_completion_enabled=[True]`, and a copied
`orange_local_control.events.jsonl` in the Orange recording artifact.
