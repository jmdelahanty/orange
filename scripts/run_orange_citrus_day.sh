#!/usr/bin/env bash
# Keep one Orange GUI and one Citrus open for a day of bound sessions
# (docs/orange_persistent_session_design.md on the docs branch; Jeremy,
# 2026-11-21: fewer camera open/close cycles, the same CUDA contexts and
# engines all day). Opt-in: relaunch per session (run_orange_citrus_bound.sh
# without --attach) stays the default and the fallback.
#
#   scripts/run_orange_citrus_day.sh --protocol <citrus protocol.json> \
#       --camera-config-dir <preset> --camera-config-sha256 <sha256:...> [--execute]
#
# Starts Orange (run_gui_fourcam_external_ipc_validation.sh --declared-start
# --keep-open) and Citrus (autorun wait_for_control, no exit after complete),
# waits until both control sockets answer, then stays in the foreground for
# the whole day. This script is the processes' parent: the GUI wrapper stops
# Orange when its launcher's parent goes away, so keep it running (e.g. in
# tmux). Each session is then:
#
#   scripts/run_orange_citrus_bound.sh --attach --declared-by <id> \
#       --protocol <...> --declaration-dir <...> --empty-background capture_at_setup \
#       --camera-config-dir <same preset> --camera-config-sha256 <same digest> --execute
#
# Ctrl-C (or SIGTERM) ends the day: Citrus gets SIGINT, Orange SIGTERM
# (it stops any recording, the stream, then closes). The preset, canvas,
# clip length and Citrus binary are fixed for the day; changing one means
# ending the day and starting again. Without --execute it prints the plan.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CITRUS_ROOT="${CITRUS_ROOT:-/home/jeremy/citrus}"
CITRUS_BIN=""
RIG="omnifin0"
CANVAS="shadow"
PROTOCOL=""
CAMERA_CONFIG_DIR="${ORANGE_GUI_CONFIG_DIR:-$HOME/orange_data/config/local/100_cam4_ptp_fourcam}"
CAMERA_CONFIG_EXPECTED_SHA256=""
CLIP_SECONDS=""
OUT_DIR=""
EXECUTE=0
ORANGE_SOCKET=/tmp/orange_local_control.sock
CITRUS_SOCKET=/tmp/citrus_local_control.sock

while [[ $# -gt 0 ]]; do
  case "$1" in
    --protocol) PROTOCOL="${2:?--protocol needs a path}"; shift 2 ;;
    --rig) RIG="${2:?}"; shift 2 ;;
    --canvas) CANVAS="${2:?}"; shift 2 ;;
    --citrus-bin) CITRUS_BIN="${2:?--citrus-bin needs a path}"; shift 2 ;;
    --camera-config-dir) CAMERA_CONFIG_DIR="${2:?--camera-config-dir needs a folder}"; shift 2 ;;
    --camera-config-sha256) CAMERA_CONFIG_EXPECTED_SHA256="${2:?--camera-config-sha256 needs a digest}"; shift 2 ;;
    --clip-seconds) CLIP_SECONDS="${2:?--clip-seconds needs a value}"; shift 2 ;;
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --execute) EXECUTE=1; shift ;;
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$PROTOCOL" ]] || { echo "--protocol is required (Citrus's autorun protocol; plan sessions replace it per arena)" >&2; exit 2; }
PROTOCOL="$(realpath -e "$PROTOCOL")" || { echo "protocol not found" >&2; exit 2; }
CANVAS_CONFIG="$CITRUS_ROOT/targets/rigs/$RIG/$CANVAS/$CANVAS.json"
[[ -f "$CANVAS_CONFIG" ]] || { echo "canvas config not found: $CANVAS_CONFIG" >&2; exit 2; }
CITRUS_BIN="$(realpath -e "${CITRUS_BIN:-$CITRUS_ROOT/targets/citrus}")" || { echo "Citrus binary not found" >&2; exit 2; }
[[ -z "$CLIP_SECONDS" || "$CLIP_SECONDS" =~ ^[1-9][0-9]*$ ]] || { echo "--clip-seconds must be a positive integer" >&2; exit 2; }
CAMERA_CONFIG_DIR="$(realpath -e "$CAMERA_CONFIG_DIR")" || { echo "camera config folder not found" >&2; exit 2; }
OUT_DIR="${OUT_DIR:-$HOME/orange_data/orchestrator_runs/day_$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT_DIR"
DIGEST_ARGS=("$CAMERA_CONFIG_DIR" --write "$OUT_DIR/camera_config.json")
[[ -n "$CAMERA_CONFIG_EXPECTED_SHA256" ]] && DIGEST_ARGS+=(--expect "$CAMERA_CONFIG_EXPECTED_SHA256")
CAMERA_CONFIG_SHA256="$(python3 "$REPO_ROOT/scripts/camera_config_dir_digest.py" "${DIGEST_ARGS[@]}")" || {
  echo "camera preset check failed for $CAMERA_CONFIG_DIR; not launching" >&2
  exit 2
}

ORANGE_CMD=("$REPO_ROOT/scripts/run_gui_fourcam_external_ipc_validation.sh" --declared-start --keep-open --citrus-display-safe)
[[ -n "$CLIP_SECONDS" ]] && ORANGE_CMD+=(--clip-seconds "$CLIP_SECONDS")
X_ENV=(DISPLAY=:1 XAUTHORITY=/run/user/1000/gdm/Xauthority XDG_SESSION_TYPE=x11)
# The same Orange environment the bound runner gives a launched GUI
# (orange_citrus_orchestrator.orange_process_env_overlay), except that the GUI
# stays up after each finalize.
ORANGE_ENV=(
  "${X_ENV[@]}"
  "ORANGE_GUI_CONFIG_DIR=$CAMERA_CONFIG_DIR"
  "ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH=$CANVAS_CONFIG"
  "ORANGE_GUI_LOCAL_CONTROL_SOCKET=$ORANGE_SOCKET"
  "ORANGE_GUI_LOCAL_CONTROL_LOG=$ORANGE_SOCKET.events.jsonl"
  ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=1
  ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=1
  ORANGE_GUI_AUTORUN_START_RECORDING=0
  ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=0
  ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=0
)
CITRUS_ENV=(
  "${X_ENV[@]}"
  XDG_RUNTIME_DIR=/run/user/1000
  ORANGE_UPDATE_TIMING_LOG=1 ORANGE_UPDATE_MANAGER_LOG=1
  "CITRUS_GUI_LOCAL_CONTROL_SOCKET=$CITRUS_SOCKET"
  CITRUS_GUI_AUTORUN=1
  "CITRUS_GUI_AUTORUN_RIG=$RIG"
  "CITRUS_GUI_AUTORUN_CANVAS=$CANVAS"
  "CITRUS_GUI_AUTORUN_PROTOCOL_PATH=$PROTOCOL"
  CITRUS_GUI_AUTORUN_SELECT_ALL=1
  CITRUS_GUI_AUTORUN_START=wait_for_control
  CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=0
)

echo "[day] out_dir=$OUT_DIR preset=$CAMERA_CONFIG_DIR ($CAMERA_CONFIG_SHA256) canvas=$CANVAS_CONFIG citrus=$CITRUS_BIN execute=$EXECUTE"
echo "[day] Orange: env ${ORANGE_ENV[*]} ${ORANGE_CMD[*]}"
echo "[day] Citrus: env ${CITRUS_ENV[*]} $CITRUS_BIN (cwd $CITRUS_ROOT)"
if (( ! EXECUTE )); then
  echo "Dry run only; add --execute (HOLD first; Jeremy's go)."
  exit 0
fi

for sock in "$ORANGE_SOCKET" "$CITRUS_SOCKET"; do
  if [[ -e "$sock" ]]; then
    echo "Refusing to start: $sock already exists (another Orange or Citrus, or a stale socket)" >&2
    exit 1
  fi
done

orange_pid=""
citrus_pid=""
end_day() {
  echo "[day] ending: stopping Citrus, then Orange"
  [[ -n "$citrus_pid" ]] && kill -INT "$citrus_pid" 2>/dev/null || true
  [[ -n "$orange_pid" ]] && kill -TERM "$orange_pid" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap 'end_day; exit 130' INT TERM

env "${ORANGE_ENV[@]}" "${ORANGE_CMD[@]}" > "$OUT_DIR/orange.log" 2>&1 &
orange_pid=$!
socket_up() {
  python3 "$REPO_ROOT/scripts/orange_local_control_client.py" --socket "$1" status >/dev/null 2>&1
}
for _ in $(seq 1 180); do socket_up "$ORANGE_SOCKET" && break; sleep 1; done
socket_up "$ORANGE_SOCKET" || { echo "Orange's control socket did not come up; see $OUT_DIR/orange.log" >&2; end_day; exit 1; }
(cd "$CITRUS_ROOT" && exec env "${CITRUS_ENV[@]}" "$CITRUS_BIN") > "$OUT_DIR/citrus.log" 2>&1 &
citrus_pid=$!
for _ in $(seq 1 120); do [[ -S "$CITRUS_SOCKET" ]] && break; sleep 1; done
[[ -S "$CITRUS_SOCKET" ]] || { echo "Citrus's control socket did not come up; see $OUT_DIR/citrus.log" >&2; end_day; exit 1; }
echo "[day] ready: Orange pid $orange_pid, Citrus pid $citrus_pid. Run sessions with run_orange_citrus_bound.sh --attach. Ctrl-C ends the day."
# Stay the parent until either process exits (then end the day).
wait -n "$orange_pid" "$citrus_pid" || true
echo "[day] a process exited; ending the day" >&2
end_day
