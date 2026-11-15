#!/usr/bin/env bash
# Unattended daily projection registration (docs/daily_registration_autorun_plan.md).
#
#   scripts/run_daily_registration_autorun.sh --declared-by <identity> [--execute]
#
# Starts Citrus (autorun omnifin0/shadow, waiting for control, no experiment) and the
# Orange GUI streaming on the normal IR path (recording, YOLO, crops and pose off).
# Orange then imports the Citrus canvas, begins the daily-registration transaction,
# captures the rims and fits each with the known dish radius. It STOPS at the one
# recorded operator judgement and prints the command that gives it:
#
#   python3 scripts/orange_local_control_client.py confirm-daily-registration \
#       --transaction-id <id> --declared-by <identity>
#
# meaning "these rims follow the dishes' inner rim; accept and select the resulting
# translation if Orange's automatic geometry QC passes". After it, Orange writes the
# operator-confirmed rim observations, Citrus computes the per-arena translation,
# Orange checks it, and accepts and selects it for runtime. Rollback: Citrus
# select_daily_registration_runtime_mode base_only.
#
# Without --execute it prints the plan (dry run). --execute needs a quiet rig (HOLD
# the other agents first) and Jeremy's go: it drives Citrus's socket and the cameras.
# --declared-by declares that the dishes and holder are installed, the water has
# settled, and the normal IR filters and illumination are in place.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CITRUS_BIN="${CITRUS_BIN:-/home/jeremy/citrus/targets/citrus}"
CITRUS_CONFIG="/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json"
CITRUS_PROTOCOL="/home/jeremy/citrus/protocols/good_cop_bad_cop_demo.json"
CITRUS_SOCKET="/tmp/citrus_local_control.sock"
ORANGE_SOCKET="/tmp/orange_local_control.sock"
FISH_PLANE_SCALE="/home/jeremy/orange_data/calibrations/commissioning/omnifin0_20261113T230405Z_wet_commissioning_holder_check_fish_plane_scale_/steps/08_fish_plane_scale/fish_plane_scale.json"
CAMERA_CONFIG_DIR="/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam"
DECLARED_BY=""
CONFIRM_TIMEOUT_SECONDS=1800
TIMEOUT_SECONDS=3600
OUT_DIR=""
EXECUTE=0
DISPLAY_VALUE="${DISPLAY:-:1}"
XDG_RUNTIME_DIR_VALUE="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
XAUTHORITY_VALUE="${XAUTHORITY:-${XDG_RUNTIME_DIR_VALUE}/gdm/Xauthority}"

usage() {
  sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
  cat <<'EOF'

Options:
  --declared-by <identity>        Required; ^[a-z][a-z0-9_-]{0,31}$ (e.g. delahantyj)
  --camera-config-dir <dir>       Orange camera preset (default 100_cam4_ptp_fourcam; the
                                  normal IR path: GPO 0 on 2010096 follows exposure)
  --fish-plane-scale <file>       orange.calibration.fish_plane_scale v1 (default: the 2026-11-13 commissioning)
  --citrus-config <file>          Citrus canvas (default omnifin0/shadow)
  --confirm-timeout-seconds <s>   Abort if no confirmation arrives (default 1800)
  --out-dir <dir>                 Logs and result (default ~/orange_data/daily_registration_runs/<UTC stamp>)
  --execute                       Run it (default: print the plan only)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --declared-by) DECLARED_BY="${2:?--declared-by needs an identity}"; shift 2 ;;
    --camera-config-dir) CAMERA_CONFIG_DIR="${2:?}"; shift 2 ;;
    --fish-plane-scale) FISH_PLANE_SCALE="${2:?}"; shift 2 ;;
    --citrus-config) CITRUS_CONFIG="${2:?}"; shift 2 ;;
    --confirm-timeout-seconds) CONFIRM_TIMEOUT_SECONDS="${2:?}"; shift 2 ;;
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --execute) EXECUTE=1; shift ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "${DECLARED_BY}" =~ ^[a-z][a-z0-9_-]{0,31}$ ]] || { echo "--declared-by is required and must match ^[a-z][a-z0-9_-]{0,31}\$" >&2; exit 2; }
[[ "${CONFIRM_TIMEOUT_SECONDS}" =~ ^[1-9][0-9]*$ ]] || { echo "--confirm-timeout-seconds must be a positive integer" >&2; exit 2; }
CAMERA_CONFIG_DIR="$(realpath -e "${CAMERA_CONFIG_DIR}")" || { echo "camera config folder not found" >&2; exit 2; }
FISH_PLANE_SCALE="$(realpath -e "${FISH_PLANE_SCALE}")" || { echo "fish-plane scale file not found" >&2; exit 2; }
CITRUS_CONFIG="$(realpath -e "${CITRUS_CONFIG}")" || { echo "Citrus canvas not found" >&2; exit 2; }
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${OUT_DIR:-$HOME/orange_data/daily_registration_runs/${STAMP}}"
RESULT_JSON="${OUT_DIR}/daily_registration_result.json"
ORANGE_LOG="${OUT_DIR}/orange.log"
CITRUS_LOG="${OUT_DIR}/citrus.log"

CITRUS_ENV=(
  "DISPLAY=${DISPLAY_VALUE}"
  "XAUTHORITY=${XAUTHORITY_VALUE}"
  "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR_VALUE}"
  "XDG_SESSION_TYPE=x11"
  "CITRUS_GUI_LOCAL_CONTROL_SOCKET=${CITRUS_SOCKET}"
  "CITRUS_GUI_AUTORUN=1"
  "CITRUS_GUI_AUTORUN_RIG=omnifin0"
  "CITRUS_GUI_AUTORUN_CANVAS=shadow"
  "CITRUS_GUI_AUTORUN_PROTOCOL_PATH=${CITRUS_PROTOCOL}"
  "CITRUS_GUI_AUTORUN_START_DELAY_SECONDS=86400"
  "CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=0"
  "CITRUS_ORANGE_COMPLETION_NOTIFY=0"
)

ORANGE_ENV=(
  "DISPLAY=${DISPLAY_VALUE}"
  "XAUTHORITY=${XAUTHORITY_VALUE}"
  "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR_VALUE}"
  "XDG_SESSION_TYPE=x11"
  "ORANGE_GUI_CONFIG_DIR=${CAMERA_CONFIG_DIR}"
  "ORANGE_GUI_EXPECT_CAMERAS=2010093,2010094,2010095,2010096"
  "ORANGE_GUI_AUTORUN=1"
  "ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=2"
  "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=0"
  "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=1"
  "ORANGE_GUI_AUTORUN_ENABLE_STREAM=1"
  "ORANGE_GUI_AUTORUN_ENABLE_RECORD=0"
  "ORANGE_GUI_AUTORUN_ENABLE_YOLO=0"
  "ORANGE_GUI_AUTORUN_ENABLE_CROP=0"
  "ORANGE_GUI_AUTORUN_ENABLE_POSE=0"
  "ORANGE_GUI_AUTORUN_START_RECORDING=0"
  "ORANGE_GUI_RECORDING_SINK_MODE=none"
  # Local control on: the confirmation arrives over the socket, under the start permission.
  "ORANGE_GUI_LOCAL_CONTROL_DISABLE=0"
  "ORANGE_GUI_LOCAL_CONTROL_SOCKET=${ORANGE_SOCKET}"
  "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=1"
  "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=0"
  "ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=0"
  "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN=1"
  "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_CITRUS_CONFIG_PATH=${CITRUS_CONFIG}"
  "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_FISH_PLANE_SCALE_PATH=${FISH_PLANE_SCALE}"
  "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_DECLARED_BY=${DECLARED_BY}"
  "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_RESULT_JSON=${RESULT_JSON}"
  "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_CONFIRM_TIMEOUT_SECONDS=${CONFIRM_TIMEOUT_SECONDS}"
  "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_EXIT_AFTER_COMPLETION=1"
)

echo "Daily registration autorun plan:"
echo "  declared_by=${DECLARED_BY} camera_config=${CAMERA_CONFIG_DIR}"
echo "  citrus_config=${CITRUS_CONFIG}"
echo "  fish_plane_scale=${FISH_PLANE_SCALE} ($(sha256sum "${FISH_PLANE_SCALE}" | cut -c1-16)...)"
echo "  result=${RESULT_JSON} confirm_timeout=${CONFIRM_TIMEOUT_SECONDS}s"
echo "  steps: Citrus (wait_for_control, rig claim omnifin0 if that build) -> Orange stream (IR path, no recording) -> canvas import ->"
echo "         begin -> rim capture -> known-radius rim fit -> WAIT for confirm_daily_registration ->"
echo "         rim observations -> Citrus candidate -> automatic geometry QC -> accept -> select"
echo "  Citrus: env ${CITRUS_ENV[*]} ${CITRUS_BIN}"
echo "  Orange: env ${ORANGE_ENV[*]} scripts/run_gui_fourcam_external_ipc_validation.sh --hidden-crop-preview --citrus-display-safe"
if (( ! EXECUTE )); then
  echo "Dry-run only; add --execute to drive Citrus and the cameras (HOLD first; Jeremy's go)."
  exit 0
fi

[[ -x "${CITRUS_BIN}" ]] || { echo "Missing Citrus binary: ${CITRUS_BIN}" >&2; exit 1; }
if [[ -e "${CITRUS_SOCKET}" || -e "${ORANGE_SOCKET}" ]]; then
  echo "Refusing to start while a local-control socket already exists:" >&2
  ls -l "${CITRUS_SOCKET}" "${ORANGE_SOCKET}" 2>/dev/null || true
  exit 1
fi
mkdir -p "${OUT_DIR}"

citrus_pid=""
orange_pid=""
cleanup() {
  if [[ -n "${citrus_pid}" ]] && kill -0 "${citrus_pid}" 2>/dev/null; then
    kill -INT "${citrus_pid}" 2>/dev/null || true
    for _ in $(seq 1 50); do kill -0 "${citrus_pid}" 2>/dev/null || break; sleep 0.1; done
    kill -0 "${citrus_pid}" 2>/dev/null && kill -TERM "${citrus_pid}" 2>/dev/null || true
    wait "${citrus_pid}" 2>/dev/null || true
  fi
  rm -f "${CITRUS_SOCKET}"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

env "${CITRUS_ENV[@]}" "${CITRUS_BIN}" >"${CITRUS_LOG}" 2>&1 &
citrus_pid=$!
echo "Started Citrus pid=${citrus_pid}; waiting for ${CITRUS_SOCKET}"
for _ in $(seq 1 300); do
  [[ -S "${CITRUS_SOCKET}" ]] && break
  kill -0 "${citrus_pid}" 2>/dev/null || { echo "Citrus exited before opening its socket" >&2; tail -n 60 "${CITRUS_LOG}" >&2; exit 1; }
  sleep 0.1
done
[[ -S "${CITRUS_SOCKET}" ]] || { echo "Timed out waiting for Citrus's socket" >&2; exit 1; }
# A Citrus that claims one rig per process (draft #124) refuses socket commands until
# its rig is claimed; one without status.rig_claim is ready now.
python3 "${REPO_ROOT}/scripts/citrus_rig_claim_ready.py" --socket "${CITRUS_SOCKET}" --rig-id omnifin0 \
  --timeout-seconds 120 || { echo "Citrus did not become ready for omnifin0" >&2; tail -n 40 "${CITRUS_LOG}" >&2; exit 1; }

timeout --signal=INT --kill-after=30s "${TIMEOUT_SECONDS}s" \
  env "${ORANGE_ENV[@]}" "${REPO_ROOT}/scripts/run_gui_fourcam_external_ipc_validation.sh" \
    --hidden-crop-preview --citrus-display-safe >"${ORANGE_LOG}" 2>&1 &
orange_pid=$!
echo "Started Orange pid=${orange_pid}; log ${ORANGE_LOG}"

# Show the confirmation request once, then wait for Orange to finish.
announced=0
while kill -0 "${orange_pid}" 2>/dev/null; do
  if (( ! announced )) && [[ -S "${ORANGE_SOCKET}" ]]; then
    status_json="$(python3 "${REPO_ROOT}/scripts/orange_local_control_client.py" --socket "${ORANGE_SOCKET}" status 2>/dev/null || true)"
    awaiting="$(python3 - "${status_json}" <<'PY' 2>/dev/null || true
import json, sys
try:
    s = json.loads(sys.argv[1])
except Exception:
    raise SystemExit(0)
d = (s.get("status") or {}).get("local_control", {}).get("daily_registration_autorun") or {}
a = d.get("awaiting")
if a:
    print(a["transaction_id"], a.get("pending_confirmation_path", ""))
PY
)"
    if [[ -n "${awaiting}" ]]; then
      read -r transaction_id pending_path <<<"${awaiting}"
      echo
      echo "AWAITING OPERATOR CONFIRMATION (transaction ${transaction_id})."
      echo "Review ${pending_path} and the rim fits it lists, then run:"
      echo "  python3 ${REPO_ROOT}/scripts/orange_local_control_client.py --socket ${ORANGE_SOCKET} \\"
      echo "      confirm-daily-registration --transaction-id ${transaction_id} --declared-by <your identity>"
      echo "No confirmation within ${CONFIRM_TIMEOUT_SECONDS}s aborts the transaction (runtime unchanged)."
      announced=1
    fi
  fi
  sleep 2
done
set +e
wait "${orange_pid}"
orange_status=$?
set -e
echo "Orange exited with status ${orange_status}"
if [[ -f "${RESULT_JSON}" ]]; then
  python3 - "${RESULT_JSON}" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
print("result:", r.get("status"), "error:", r.get("error") or "-", "transaction:", r.get("transaction_id"))
for t in r.get("targets", []):
    print("  ", t["camera_serial"], t["arena_id"], "translation canvas px", t.get("applied_translation_canvas_px"),
          "rim centre", [round(v, 1) for v in t.get("accepted_rim_center_camera_px", [])])
print("accepted registration:", r.get("accepted_registration"))
PY
else
  echo "No result JSON at ${RESULT_JSON}" >&2
  tail -n 80 "${ORANGE_LOG}" >&2 || true
  exit 1
fi
