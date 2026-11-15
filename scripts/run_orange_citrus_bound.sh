#!/usr/bin/env bash
# One unattended, bound Orange+Citrus recording (a stimulus experiment):
# launches the Orange GUI (declared start) and Citrus (autorun, waiting for
# control), checks both, starts Orange then Citrus, waits for Citrus's
# completion and receipts, and writes one summary. The steps and their rules
# are scripts/orange_citrus_orchestrator.py; this script only fills in the
# rig's launch settings (first run 2026_11_14_23_09_55).
#
#   scripts/run_orange_citrus_bound.sh --protocol <citrus protocol .json> \
#       --declared-by <identity> [--camera-config-dir <preset> \
#       --camera-config-sha256 <sha256:...>] [--execute]
#
# Without --execute it prints the plan (dry run). --execute runs it: send HOLD
# to the other agents first, and only on Jeremy's go (running it is the START
# go for Citrus).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CITRUS_ROOT="${CITRUS_ROOT:-/home/jeremy/citrus}"
RIG="omnifin0"
CANVAS="shadow"
PROTOCOL=""
DECLARED_BY=""
EXPECTED_ARENAS=4
CAMERA_CONFIG_DIR="${ORANGE_GUI_CONFIG_DIR:-$HOME/orange_data/config/local/100_cam4_ptp_fourcam}"
CAMERA_CONFIG_EXPECTED_SHA256=""
CLIP_SECONDS=""
OUT_DIR=""
EXECUTE=0

usage() {
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
  cat <<'EOF'

Options:
  --protocol <path>        Citrus protocol JSON (required), e.g.
                           ~/citrus/protocols/validation_chaser_last_white0118_black_dot_1chase_112s.json
  --declared-by <identity> The person declaring the recording (required), e.g. delahantyj
  --rig <id>               Citrus rig (default omnifin0)
  --canvas <name>          Citrus canvas (default shadow); Orange uses
                           <citrus>/targets/rigs/<rig>/<canvas>/<canvas>.json
  --expected-arenas <n>    Arenas Citrus must have selected (default 4; Citrus selects all)
  --camera-config-dir <d>  Orange camera preset folder (default 100_cam4_ptp_fourcam);
                           e.g. ~/orange_data/config/local/30_cam4_ptp_fourcam
  --camera-config-sha256 <sha256:...>
                           Pin the preset (rule orange_camera_config_dir_sha256_v1,
                           scripts/camera_config_dir_digest.py): refuse to launch when
                           the folder's digest differs. The digest is always recorded
                           in <out-dir>/camera_config.json and the summary.
  --clip-seconds <n>       GUI rolling clips of n seconds (default: the app config's
                           recording.recording_control.clip_seconds)
  --out-dir <dir>          Logs and summary (default ~/orange_data/orchestrator_runs/<UTC stamp>)
  --execute                Run it (default: print the plan only)

Subjects: Citrus autorun declares every arena blank, so Orange declares every
camera blank too (they must match, or Orange's pre-arm refuses the arm).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --protocol) PROTOCOL="${2:?--protocol needs a path}"; shift 2 ;;
    --declared-by) DECLARED_BY="${2:?--declared-by needs an identity}"; shift 2 ;;
    --rig) RIG="${2:?}"; shift 2 ;;
    --canvas) CANVAS="${2:?}"; shift 2 ;;
    --expected-arenas) EXPECTED_ARENAS="${2:?}"; shift 2 ;;
    --camera-config-dir) CAMERA_CONFIG_DIR="${2:?--camera-config-dir needs a folder}"; shift 2 ;;
    --camera-config-sha256) CAMERA_CONFIG_EXPECTED_SHA256="${2:?--camera-config-sha256 needs a digest}"; shift 2 ;;
    --clip-seconds) CLIP_SECONDS="${2:?--clip-seconds needs a value}"; shift 2 ;;
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --execute) EXECUTE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$PROTOCOL" && -n "$DECLARED_BY" ]] || { echo "--protocol and --declared-by are required" >&2; exit 2; }
PROTOCOL="$(realpath -e "$PROTOCOL")" || { echo "protocol not found" >&2; exit 2; }
CANVAS_CONFIG="$CITRUS_ROOT/targets/rigs/$RIG/$CANVAS/$CANVAS.json"
[[ -f "$CANVAS_CONFIG" ]] || { echo "canvas config not found: $CANVAS_CONFIG" >&2; exit 2; }
[[ -x "$CITRUS_ROOT/targets/citrus" ]] || { echo "Citrus binary not found: $CITRUS_ROOT/targets/citrus" >&2; exit 2; }
[[ -z "$CLIP_SECONDS" || "$CLIP_SECONDS" =~ ^[1-9][0-9]*$ ]] || { echo "--clip-seconds must be a positive integer: $CLIP_SECONDS" >&2; exit 2; }
CAMERA_CONFIG_DIR="$(realpath -e "$CAMERA_CONFIG_DIR")" || { echo "camera config folder not found" >&2; exit 2; }
compgen -G "$CAMERA_CONFIG_DIR/*.json" >/dev/null || { echo "no camera JSONs in $CAMERA_CONFIG_DIR" >&2; exit 2; }
OUT_DIR="${OUT_DIR:-$HOME/orange_data/orchestrator_runs/$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT_DIR"
# The preset record (rule orange_camera_config_dir_sha256_v1); with a pin, a
# folder whose digest differs refuses the launch.
DIGEST_ARGS=("$CAMERA_CONFIG_DIR" --write "$OUT_DIR/camera_config.json")
[[ -n "$CAMERA_CONFIG_EXPECTED_SHA256" ]] && DIGEST_ARGS+=(--expect "$CAMERA_CONFIG_EXPECTED_SHA256")
CAMERA_CONFIG_SHA256="$(python3 "$REPO_ROOT/scripts/camera_config_dir_digest.py" "${DIGEST_ARGS[@]}")" || {
  echo "camera preset check failed for $CAMERA_CONFIG_DIR (see above); not launching" >&2
  exit 2
}
ORANGE_COMMAND="$REPO_ROOT/scripts/run_gui_fourcam_external_ipc_validation.sh --declared-start --citrus-display-safe"
[[ -n "$CLIP_SECONDS" ]] && ORANGE_COMMAND+=" --clip-seconds $CLIP_SECONDS"

X_ENV=(DISPLAY=:1 XAUTHORITY=/run/user/1000/gdm/Xauthority XDG_SESSION_TYPE=x11)
ARGS=(
  --declared-by "$DECLARED_BY" --intent stimulus_experiment --blank-subjects
  --expected-citrus-arenas "$EXPECTED_ARENAS"
  --stop-policy citrus_completion_notify
  --orange-command "$ORANGE_COMMAND"
  --camera-config-json "$OUT_DIR/camera_config.json"
  --orange-env "ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH=$CANVAS_CONFIG"
  --orange-env "ORANGE_GUI_CONFIG_DIR=$CAMERA_CONFIG_DIR"
  --citrus-command "env -C $CITRUS_ROOT ./targets/citrus"
  --citrus-env XDG_RUNTIME_DIR=/run/user/1000
  --citrus-env ORANGE_UPDATE_TIMING_LOG=1 --citrus-env ORANGE_UPDATE_MANAGER_LOG=1
  --citrus-env CITRUS_GUI_AUTORUN=1
  --citrus-env "CITRUS_GUI_AUTORUN_RIG=$RIG"
  --citrus-env "CITRUS_GUI_AUTORUN_CANVAS=$CANVAS"
  --citrus-env "CITRUS_GUI_AUTORUN_PROTOCOL_PATH=$PROTOCOL"
  --citrus-env CITRUS_GUI_AUTORUN_SELECT_ALL=1
  --citrus-env CITRUS_GUI_AUTORUN_START=wait_for_control
  --citrus-env CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=0
  --orange-log "$OUT_DIR/orange.log" --citrus-log "$OUT_DIR/citrus.log"
  --summary-json "$OUT_DIR/summary.json"
)
for kv in "${X_ENV[@]}"; do ARGS+=(--orange-env "$kv" --citrus-env "$kv"); done
(( EXECUTE )) && ARGS=(--execute "${ARGS[@]}")

echo "[bound-run] out_dir=$OUT_DIR protocol=$PROTOCOL canvas=$CANVAS_CONFIG camera_config=$CAMERA_CONFIG_DIR camera_config_sha256=$CAMERA_CONFIG_SHA256 clip_seconds=${CLIP_SECONDS:-app_config} execute=$EXECUTE" >&2
exec python3 "$REPO_ROOT/scripts/orange_citrus_orchestrator.py" "${ARGS[@]}"
