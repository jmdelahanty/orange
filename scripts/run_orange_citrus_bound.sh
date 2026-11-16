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
#       --camera-config-sha256 <sha256:...>] [--declaration-dir <declare.py out>] \
#       [--empty-background capture_at_setup [--fish-free-test]] [--execute]
#
# Without --execute it prints the plan (dry run). --execute runs it: send HOLD
# to the other agents first, and only on Jeremy's go (running it is the START
# go for Citrus).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CITRUS_ROOT="${CITRUS_ROOT:-/home/jeremy/citrus}"
CITRUS_BIN=""
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
DECLARATION_DIR=""
EMPTY_BACKGROUND=""
FISH_FREE_TEST=0
SCENE_CONFIRMATION_TIMEOUT=""

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
  cat <<'EOF'

Options:
  --protocol <path>        Citrus protocol JSON (required), e.g.
                           ~/citrus/protocols/validation_chaser_last_white0118_black_dot_1chase_112s.json
  --declared-by <identity> The person declaring the recording (required), e.g. delahantyj
  --rig <id>               Citrus rig (default omnifin0)
  --canvas <name>          Citrus canvas (default shadow); Orange uses
                           <citrus>/targets/rigs/<rig>/<canvas>/<canvas>.json
  --expected-arenas <n>    Arenas Citrus must have selected (default 4; Citrus selects all)
  --citrus-bin <path>      Citrus executable to launch (default <citrus root>/targets/citrus).
                           The canvas and working directory stay on the citrus root
                           (CITRUS_ROOT, default ~/citrus); e.g. a branch build for a test.
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
  --declaration-dir <dir>  An orchard plan session's declarations (orchard tools/declare.py
                           <plan> <record> <session> --out <dir>): its *.orange_declaration.json,
                           *.citrus_declaration_request.json and run_plan/. Orange declares the
                           plan's subjects (declaration v2, run_plan/ sealed in the recording)
                           and Citrus gets the same plan through declare_subjects. Needs Citrus
                           acceptance v4 (<citrus root>/system_config.yml): refused otherwise.
                           The plan's preset pin must match --camera-config-dir, and its
                           orange.empty_background rule applies.
  --empty-background <m>   capture_at_setup: once Orange streams and Citrus is checked, capture
                           an empty-dish background (dishes filled, no fish), write
                           <out-dir>/empty_background_capture.json and wait on
                           <out-dir>/scene_confirmation.sock for the operator's
                           scripts/confirm_bound_run_scene.py --run-dir <out-dir> --confirmed-by <id>
                           (fish loaded, dishes not moved; or --abort). The runner records the
                           caller by SO_PEERCRED in scene_confirmation.json before the start
                           names the capture. The capture's display is declared only from
                           Citrus's advertised status (idle clear, projector black rgba8 0,0,0,255,
                           read before and after); a Citrus that does not advertise it refuses.
                           none: no background. Default: the plan's rule, else none.
  --fish-free-test         With capture_at_setup and no subjects declared: the runner confirms
                           the scene itself (recorded as fish_free_test_scene_unchanged).
  --scene-confirmation-timeout-seconds <n>
                           How long to wait for the operator after the capture (default 3600)
  --execute                Run it (default: print the plan only)

Subjects: without --declaration-dir, Citrus autorun declares every arena blank,
so Orange declares every camera blank too (they must match, or Orange's
pre-arm refuses the arm). --declared-by is then the declaration's identity;
with --declaration-dir the declaration names its own, and --declared-by is the
operator who declares the dishes empty at the background capture.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --protocol) PROTOCOL="${2:?--protocol needs a path}"; shift 2 ;;
    --declared-by) DECLARED_BY="${2:?--declared-by needs an identity}"; shift 2 ;;
    --rig) RIG="${2:?}"; shift 2 ;;
    --canvas) CANVAS="${2:?}"; shift 2 ;;
    --expected-arenas) EXPECTED_ARENAS="${2:?}"; shift 2 ;;
    --citrus-bin) CITRUS_BIN="${2:?--citrus-bin needs a path}"; shift 2 ;;
    --camera-config-dir) CAMERA_CONFIG_DIR="${2:?--camera-config-dir needs a folder}"; shift 2 ;;
    --camera-config-sha256) CAMERA_CONFIG_EXPECTED_SHA256="${2:?--camera-config-sha256 needs a digest}"; shift 2 ;;
    --clip-seconds) CLIP_SECONDS="${2:?--clip-seconds needs a value}"; shift 2 ;;
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --declaration-dir) DECLARATION_DIR="${2:?--declaration-dir needs a folder}"; shift 2 ;;
    --empty-background) EMPTY_BACKGROUND="${2:?--empty-background needs capture_at_setup or none}"; shift 2 ;;
    --fish-free-test) FISH_FREE_TEST=1; shift ;;
    --scene-confirmation-timeout-seconds) SCENE_CONFIRMATION_TIMEOUT="${2:?}"; shift 2 ;;
    --execute) EXECUTE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$PROTOCOL" && -n "$DECLARED_BY" ]] || { echo "--protocol and --declared-by are required" >&2; exit 2; }
PROTOCOL="$(realpath -e "$PROTOCOL")" || { echo "protocol not found" >&2; exit 2; }
CANVAS_CONFIG="$CITRUS_ROOT/targets/rigs/$RIG/$CANVAS/$CANVAS.json"
[[ -f "$CANVAS_CONFIG" ]] || { echo "canvas config not found: $CANVAS_CONFIG" >&2; exit 2; }
CITRUS_BIN="${CITRUS_BIN:-$CITRUS_ROOT/targets/citrus}"
CITRUS_BIN="$(realpath -e "$CITRUS_BIN")" || { echo "Citrus binary not found" >&2; exit 2; }
[[ -x "$CITRUS_BIN" ]] || { echo "Citrus binary not executable: $CITRUS_BIN" >&2; exit 2; }
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
# The recording's declaration: every camera blank (Citrus autorun declares
# every arena blank), or an orchard plan session's (declare.py --out folder).
if [[ -n "$DECLARATION_DIR" ]]; then
  DECLARATION_DIR="$(realpath -e "$DECLARATION_DIR")" || { echo "declaration folder not found" >&2; exit 2; }
  mapfile -t ORANGE_DECLARATIONS < <(compgen -G "$DECLARATION_DIR/*.orange_declaration.json" || true)
  mapfile -t CITRUS_REQUESTS < <(compgen -G "$DECLARATION_DIR/*.citrus_declaration_request.json" || true)
  (( ${#ORANGE_DECLARATIONS[@]} == 1 && ${#CITRUS_REQUESTS[@]} == 1 )) || {
    echo "$DECLARATION_DIR must hold exactly one *.orange_declaration.json and one *.citrus_declaration_request.json" >&2
    exit 2
  }
  # Orange's pre-arm cross-check v2 refuses an orchard-declared arm unless
  # Citrus declared the same plan (declare_subjects, acceptance v4). Refuse
  # here, before anything launches or any fish is loaded.
  CITRUS_SYSTEM_CONFIG="$CITRUS_ROOT/system_config.yml"
  CITRUS_ACCEPTANCE="$(sed -n 's/^[[:space:]]*recording_observation_acceptance_schema_version:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$CITRUS_SYSTEM_CONFIG" 2>/dev/null | tail -1 || true)"
  if [[ "$CITRUS_ACCEPTANCE" != "4" ]]; then
    echo "Citrus writes acceptance v${CITRUS_ACCEPTANCE:-unknown} ($CITRUS_SYSTEM_CONFIG); a plan-declared run needs" \
         "acceptance v4 (Citrus declare_subjects). Orange's pre-arm would refuse the arm. Not launching;" \
         "run without --declaration-dir (every camera blank) or switch Citrus to v4 first (Jeremy's word)." >&2
    exit 2
  fi
  DECLARATION_ARGS=(--declaration-file "${ORANGE_DECLARATIONS[0]}" --declaration-files-root "$DECLARATION_DIR"
                    --citrus-subject-declaration-request "${CITRUS_REQUESTS[0]}"
                    --background-declared-by "$DECLARED_BY")
else
  DECLARATION_ARGS=(--declared-by "$DECLARED_BY" --intent stimulus_experiment --blank-subjects)
fi
BACKGROUND_ARGS=(--run-dir "$OUT_DIR")
[[ -n "$EMPTY_BACKGROUND" ]] && BACKGROUND_ARGS+=(--empty-background "$EMPTY_BACKGROUND")
(( FISH_FREE_TEST )) && BACKGROUND_ARGS+=(--fish-free-test)
[[ -n "$SCENE_CONFIRMATION_TIMEOUT" ]] && BACKGROUND_ARGS+=(--scene-confirmation-timeout-seconds "$SCENE_CONFIRMATION_TIMEOUT")

ORANGE_COMMAND="$REPO_ROOT/scripts/run_gui_fourcam_external_ipc_validation.sh --declared-start --citrus-display-safe"
[[ -n "$CLIP_SECONDS" ]] && ORANGE_COMMAND+=" --clip-seconds $CLIP_SECONDS"

X_ENV=(DISPLAY=:1 XAUTHORITY=/run/user/1000/gdm/Xauthority XDG_SESSION_TYPE=x11)
ARGS=(
  "${DECLARATION_ARGS[@]}" "${BACKGROUND_ARGS[@]}"
  --expected-citrus-arenas "$EXPECTED_ARENAS"
  --stop-policy citrus_completion_notify
  --orange-command "$ORANGE_COMMAND"
  --camera-config-json "$OUT_DIR/camera_config.json"
  --orange-env "ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH=$CANVAS_CONFIG"
  --orange-env "ORANGE_GUI_CONFIG_DIR=$CAMERA_CONFIG_DIR"
  --citrus-command "env -C $CITRUS_ROOT $CITRUS_BIN"
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

echo "[bound-run] out_dir=$OUT_DIR protocol=$PROTOCOL canvas=$CANVAS_CONFIG camera_config=$CAMERA_CONFIG_DIR camera_config_sha256=$CAMERA_CONFIG_SHA256 clip_seconds=${CLIP_SECONDS:-app_config} declaration=${DECLARATION_DIR:-blank_subjects} empty_background=${EMPTY_BACKGROUND:-auto} fish_free_test=$FISH_FREE_TEST execute=$EXECUTE" >&2
exec python3 "$REPO_ROOT/scripts/orange_citrus_orchestrator.py" "${ARGS[@]}"
