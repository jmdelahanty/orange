#!/usr/bin/env bash
# One unattended Orange+Citrus run: Citrus loads and selects, orange_client
# binds and records, the orchestrator sends the one start_experiment, Citrus's
# completion ends the recording and its receipts seal it.
#
#   scripts/run_headless_citrus_session.sh --protocol-path <citrus protocol.json> \
#       [--orange-client <binary>] [--spec <experiment spec>] [--execute]
#
# Without --execute it prints the plan and touches nothing. With --execute it
# launches Citrus (autorun, CITRUS_GUI_AUTORUN_START=wait_for_control, all
# arenas, exit after complete) on the X display, waits until Citrus reports
# ready and waiting, runs scripts/run_detect_latency_spec.sh, and sends
# start_experiment only once orange_client is recording with the binding
# accepted (pre_arm accepted_pending_finalization). The spec needs
# fixed.local_control {enabled, stop_on_citrus_completion} and
# recording_control.record_for_seconds as the ceiling.
#
# Citrus must be a build with wait_for_control (Citrus PR #109); an older one is
# refused at the first status. Every launch and start needs Jeremy's word.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ORANGE_CLIENT="$REPO_ROOT/targets/release/orange_client"
SPEC="fourcam_headless_citrus_validation"
CITRUS_ROOT="/home/jeremy/citrus"
CITRUS_BINARY=""
CITRUS_RIG="omnifin0"
CITRUS_CANVAS="shadow"
PROTOCOL_PATH=""
CITRUS_SOCKET="/tmp/citrus_local_control.sock"
ORANGE_SOCKET="/tmp/orange_local_control.sock"
DISPLAY_VALUE="${DISPLAY:-:1}"
XAUTHORITY_VALUE="${XAUTHORITY:-}"
[[ -z "$XAUTHORITY_VALUE" && -f /run/user/1000/gdm/Xauthority ]] && XAUTHORITY_VALUE=/run/user/1000/gdm/Xauthority
OUT_DIR="$HOME/orange_data/headless_citrus_sessions"
EXECUTE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --orange-client) ORANGE_CLIENT="$2"; shift 2 ;;
    --spec) SPEC="$2"; shift 2 ;;
    --citrus-root) CITRUS_ROOT="$2"; shift 2 ;;
    --citrus-binary) CITRUS_BINARY="$2"; shift 2 ;;
    --rig) CITRUS_RIG="$2"; shift 2 ;;
    --canvas) CITRUS_CANVAS="$2"; shift 2 ;;
    --protocol-path) PROTOCOL_PATH="$2"; shift 2 ;;
    --display) DISPLAY_VALUE="$2"; shift 2 ;;
    --xauthority) XAUTHORITY_VALUE="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --execute) EXECUTE=1; shift ;;
    -h|--help) sed -n 2,21p "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$PROTOCOL_PATH" ]] || { echo "--protocol-path is required" >&2; exit 2; }
[[ -f "$PROTOCOL_PATH" ]] || { echo "protocol not found: $PROTOCOL_PATH" >&2; exit 2; }
[[ -x "$ORANGE_CLIENT" ]] || { echo "orange_client not executable: $ORANGE_CLIENT" >&2; exit 2; }
CITRUS_BINARY="${CITRUS_BINARY:-$CITRUS_ROOT/targets/citrus}"
[[ -x "$CITRUS_BINARY" ]] || { echo "citrus not executable: $CITRUS_BINARY" >&2; exit 2; }

STAMP="$(date +%Y%m%d_%H%M%S)"
SESSION_DIR="$OUT_DIR/$STAMP"
OPERATION_ID="headless_citrus_${STAMP}"
# Citrus runs from its own root, as stim-orange does.
CITRUS_COMMAND="bash -c $(printf '%q' "cd $(printf '%q' "$CITRUS_ROOT") && exec $(printf '%q' "$CITRUS_BINARY")")"
ORANGE_COMMAND="$(printf '%q ' "$REPO_ROOT/scripts/run_detect_latency_spec.sh" --orange-client "$ORANGE_CLIENT" "$SPEC")"

ARGS=(
  --orange-mode headless
  --citrus-start-gate wait_for_control
  --stop-policy citrus_completion_notify
  --operation-id "$OPERATION_ID"
  --source orange_headless_citrus_session
  --orange-socket "$ORANGE_SOCKET"
  --citrus-socket "$CITRUS_SOCKET"
  --orange-command "$ORANGE_COMMAND"
  --citrus-command "$CITRUS_COMMAND"
  --orange-log "$SESSION_DIR/orange_run.log"
  --citrus-log "$SESSION_DIR/citrus.log"
  --summary-json "$SESSION_DIR/orchestrator_summary.json"
  --timeout-seconds 180
  --citrus-terminal-timeout-seconds 3600
  --poll-interval-seconds 0.5
)
for item in \
  "DISPLAY=$DISPLAY_VALUE" \
  "XDG_RUNTIME_DIR=/run/user/$(id -u)" \
  "XDG_SESSION_TYPE=x11" \
  "CITRUS_GUI_AUTORUN=1" \
  "CITRUS_GUI_AUTORUN_START=wait_for_control" \
  "CITRUS_GUI_AUTORUN_RIG=$CITRUS_RIG" \
  "CITRUS_GUI_AUTORUN_CANVAS=$CITRUS_CANVAS" \
  "CITRUS_GUI_AUTORUN_PROTOCOL_PATH=$(realpath "$PROTOCOL_PATH")" \
  "CITRUS_GUI_AUTORUN_SELECT_ALL=1" \
  "CITRUS_GUI_AUTORUN_START_DELAY_SECONDS=0" \
  "CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=1"; do
  ARGS+=(--citrus-env "$item")
done
[[ -n "$XAUTHORITY_VALUE" ]] && ARGS+=(--citrus-env "XAUTHORITY=$XAUTHORITY_VALUE")

if [[ "$EXECUTE" != 1 ]]; then
  echo "[headless_citrus] dry run (add --execute to launch Citrus and orange_client)"
  exec python3 "$REPO_ROOT/scripts/orange_citrus_orchestrator.py" "${ARGS[@]}"
fi
mkdir -p "$SESSION_DIR"
echo "[headless_citrus] session=$SESSION_DIR operation_id=$OPERATION_ID"
set +e
python3 "$REPO_ROOT/scripts/orange_citrus_orchestrator.py" --execute "${ARGS[@]}" > "$SESSION_DIR/orchestrator.out" 2>&1
RC=$?
set -e
python3 - "$SESSION_DIR/orchestrator_summary.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
h = s.get("headless") or {}
print(f"[headless_citrus] result={s.get('result')} error={s.get('error') or '-'}")
print(f"[headless_citrus] recording={h.get('recording_folder', '-')} orange_rc={h.get('orange_returncode')}"
      f" run={h.get('run_pass_fail', '-')} receipts={(h.get('citrus_receipts') or {}).get('status', '-')}"
      f" citrus={s.get('citrus', {}).get('terminal_state', '-')}/{s.get('citrus', {}).get('terminal_reason', '-')}"
      f" start_source={h.get('citrus_start_source')}")
PY
exit "$RC"
