#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORIGINAL_ARGS=("$@")
EXECUTE=0
RECORD_DIR="${ORANGE_COMMISSIONING_RECORD:-}"
RECORD_STEP_NAME=""
RECORD_DONE=0
ARM_SAVE=0
BOOTSTRAP_CENTERS_ONLY=0
RESIZE_ARENAS=0
ARM_LAYOUT_SAVE=0
FIT_HOMOGRAPHIES=0
ACCEPT_HOMOGRAPHIES=0
CAMERAS="2010093,2010094,2010095,2010096"
FRAME_COUNT=1
FOREGROUND_GRAY_U8=""
FOREGROUND_GRAY_EXPLICIT=0
PROBE_CANVAS_PX=3
VERIFICATION_TOLERANCE_CAMERA_PX=2.0
MAX_REFINEMENT_CANVAS_PX=4.0
RECTANGLE_SAFETY_MARGIN_CAMERA_PX=32.0
MAX_PTP_SPAN_NS=1000
CALIBRATION_FRAME_RATE_HZ=5
CALIBRATION_EXPOSURE_US=100000
PROJECTION_SETTLE_MS=1000
STABILITY_INTERVAL_MS=300
POST_PRESENTATION_SETTLE_MS=1000
# Empty = resolved: the stimulus output from the pancakebatter host config
# (rigs[].display_outputs[], the entry Citrus binds), the operator monitor as
# the primary output on Orange's X screen. The options name them explicitly.
OPERATOR_MONITOR=""
STIMULUS_MONITOR=""
STIMULUS_MONITOR_SOURCE="host_config"
RIG_ID="omnifin0"
PROJECTION_PATH="main"
HOST_CONFIG="/etc/pancakebatter/host.yml"
TIMEOUT_SECONDS=300
DISPLAY_VALUE="${DISPLAY:-:1}"
XAUTHORITY_VALUE="${XAUTHORITY:-/run/user/1000/gdm/Xauthority}"
XDG_RUNTIME_DIR_VALUE="${XDG_RUNTIME_DIR:-/run/user/1000}"
RESULT_JSON=""
CANVAS="shadow"
CITRUS_CONFIG=""   # default: $CITRUS_ROOT/targets/rigs/<rig id>/<canvas>/<canvas>.json
PROJECTOR_INTENSITY_REPORT="/home/jeremy/orange_data/calibrations/commissioning/projector_intensity_20260719T014235Z/commissioning_report.json"
PROJECTOR_INTENSITY_REPORT_EXPLICIT=0
CITRUS_ROOT="${CITRUS_ROOT:-/home/jeremy/citrus}"
CITRUS_BIN="${CITRUS_BIN:-${CITRUS_ROOT}/targets/citrus}"
CITRUS_PROTOCOL="${CITRUS_ROOT}/protocols/good_cop_bad_cop_demo.json"
ORANGE_GUI_BINARY="${ORANGE_GUI_BINARY:-${REPO_ROOT}/targets/release/orange}"
CITRUS_SOCKET="/tmp/citrus_local_control.sock"
ORANGE_SOCKET="/tmp/orange_local_control.sock"
ORANGE_CONFIG_SOURCE="/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam"
ORANGE_CONFIG_DIR="${ORANGE_CONFIG_SOURCE}"
TIMING_CONFIG_DIR=""

usage() {
  cat <<'EOF'
Usage: scripts/run_gui_arena_centering_commissioning.sh [options]

Runs the four-camera dry-shelf canonical arena-centering workflow in one
Citrus and one Orange invocation. It is a dry-run unless --execute is given.
Verified centers are rolled back unless --save-verified-centers is also given.
Arena resizing is opt-in and is independently persistence-armed.
Homography fitting is opt-in; promotion has a separate explicit arm.
Use --bootstrap-centers-only after camera motion, before a full-grid intensity
report exists. This mode verifies center fiducials and symmetric probes only;
it cannot resize arenas, fit homographies, or qualify projector intensity.

Options:
  --execute
  --save-verified-centers
  --bootstrap-centers-only
  --resize-arenas
  --save-verified-layout
  --fit-homographies
  --accept-homographies
  --cameras <comma-separated serials>
  --frame-count <count>
  --foreground-gray-u8 <0-255>
  --projector-intensity-report <commissioning_report.json>
  --probe-canvas-px <integer>
  --verification-tolerance-camera-px <number>
  --max-refinement-canvas-px <number>
  --rectangle-safety-margin-camera-px <number>
  --max-ptp-span-ns <integer>
  --calibration-frame-rate-hz <integer>
  --calibration-exposure-us <integer>
  --projection-settle-ms <integer>
  --stability-interval-ms <integer>
  --post-presentation-settle-ms <integer>
  --operator-monitor <output-name>   (default: primary output on --display's X screen)
  --stimulus-monitor <output-name>   (default: the host config's output for --rig-id; naming
                                      one also tells Citrus by name, a test-only override)
  --rig-id <rig id>                  (default: omnifin0)
  --canvas <canvas name>             (default: shadow)
  --host-config <host.yml>           (default: /etc/pancakebatter/host.yml)
  --result-json <path>
  --timeout-seconds <integer>
  --display <display>
  --xauthority <path>
  --citrus-config <path>
  --record <dir>                     commissioning record to append this run to
                                     (default: $ORANGE_COMMISSIONING_RECORD)
  --record-step-name <name>          step name in the record (default from the mode)
  --help

Environment:
  ORANGE_COMMISSIONING_RECORD  default for --record.
  CITRUS_ROOT Citrus checkout (default: /home/jeremy/citrus).
  CITRUS_BIN  Citrus executable (default: $CITRUS_ROOT/targets/citrus).
  XRANDR_BIN  xrandr executable (default: xrandr; tests substitute a fake).
  ORANGE_GUI_BINARY  Orange binary that must exist (default: targets/release/orange).

Displays: nothing about the outputs is fixed here. The stimulus output and its
X screen come from the pancakebatter host config entry Citrus binds
(scripts/host_stimulus_output.py), the operator monitor is the primary output
on the X screen of --display (Orange draws there), and the launcher checks with
xrandr that both are connected where they should be. When the stimulus output
shares Orange's X screen, Orange reserves it and refuses an overlapping
window; on another X screen it cannot overlap and is not reserved. Orange
checks the monitor name Citrus reports either way.
EOF
}

require_value() {
  local option="$1"
  local count="$2"
  if (( count == 0 )); then
    echo "${option} requires a value" >&2
    exit 2
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --execute) EXECUTE=1; shift ;;
    --save-verified-centers) ARM_SAVE=1; shift ;;
    --bootstrap-centers-only) BOOTSTRAP_CENTERS_ONLY=1; shift ;;
    --resize-arenas) RESIZE_ARENAS=1; shift ;;
    --save-verified-layout)
      RESIZE_ARENAS=1
      ARM_SAVE=1
      ARM_LAYOUT_SAVE=1
      shift
      ;;
    --fit-homographies) FIT_HOMOGRAPHIES=1; shift ;;
    --accept-homographies)
      FIT_HOMOGRAPHIES=1
      ACCEPT_HOMOGRAPHIES=1
      shift
      ;;
    --cameras) shift; require_value --cameras "$#"; CAMERAS="$1"; shift ;;
    --frame-count) shift; require_value --frame-count "$#"; FRAME_COUNT="$1"; shift ;;
    --foreground-gray-u8)
      shift
      require_value --foreground-gray-u8 "$#"
      FOREGROUND_GRAY_U8="$1"
      FOREGROUND_GRAY_EXPLICIT=1
      shift
      ;;
    --projector-intensity-report)
      shift
      require_value --projector-intensity-report "$#"
      PROJECTOR_INTENSITY_REPORT="$1"
      PROJECTOR_INTENSITY_REPORT_EXPLICIT=1
      shift
      ;;
    --probe-canvas-px) shift; require_value --probe-canvas-px "$#"; PROBE_CANVAS_PX="$1"; shift ;;
    --verification-tolerance-camera-px) shift; require_value --verification-tolerance-camera-px "$#"; VERIFICATION_TOLERANCE_CAMERA_PX="$1"; shift ;;
    --max-refinement-canvas-px) shift; require_value --max-refinement-canvas-px "$#"; MAX_REFINEMENT_CANVAS_PX="$1"; shift ;;
    --rectangle-safety-margin-camera-px) shift; require_value --rectangle-safety-margin-camera-px "$#"; RECTANGLE_SAFETY_MARGIN_CAMERA_PX="$1"; shift ;;
    --max-ptp-span-ns) shift; require_value --max-ptp-span-ns "$#"; MAX_PTP_SPAN_NS="$1"; shift ;;
    --calibration-frame-rate-hz) shift; require_value --calibration-frame-rate-hz "$#"; CALIBRATION_FRAME_RATE_HZ="$1"; shift ;;
    --calibration-exposure-us) shift; require_value --calibration-exposure-us "$#"; CALIBRATION_EXPOSURE_US="$1"; shift ;;
    --projection-settle-ms) shift; require_value --projection-settle-ms "$#"; PROJECTION_SETTLE_MS="$1"; shift ;;
    --stability-interval-ms) shift; require_value --stability-interval-ms "$#"; STABILITY_INTERVAL_MS="$1"; shift ;;
    --post-presentation-settle-ms) shift; require_value --post-presentation-settle-ms "$#"; POST_PRESENTATION_SETTLE_MS="$1"; shift ;;
    --operator-monitor) shift; require_value --operator-monitor "$#"; OPERATOR_MONITOR="$1"; shift ;;
    --stimulus-monitor) shift; require_value --stimulus-monitor "$#"; STIMULUS_MONITOR="$1"; STIMULUS_MONITOR_SOURCE="option"; shift ;;
    --rig-id) shift; require_value --rig-id "$#"; RIG_ID="$1"; shift ;;
    --canvas) shift; require_value --canvas "$#"; CANVAS="$1"; shift ;;
    --host-config) shift; require_value --host-config "$#"; HOST_CONFIG="$1"; shift ;;
    --result-json) shift; require_value --result-json "$#"; RESULT_JSON="$1"; shift ;;
    --timeout-seconds) shift; require_value --timeout-seconds "$#"; TIMEOUT_SECONDS="$1"; shift ;;
    --display) shift; require_value --display "$#"; DISPLAY_VALUE="$1"; shift ;;
    --xauthority) shift; require_value --xauthority "$#"; XAUTHORITY_VALUE="$1"; shift ;;
    --citrus-config) shift; require_value --citrus-config "$#"; CITRUS_CONFIG="$1"; shift ;;
    --record) shift; require_value --record "$#"; RECORD_DIR="$1"; shift ;;
    --record-step-name) shift; require_value --record-step-name "$#"; RECORD_STEP_NAME="$1"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unsupported argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "${CAMERAS}" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
  echo "--cameras must contain comma-separated numeric serials" >&2; exit 2;
}
if (( BOOTSTRAP_CENTERS_ONLY )); then
  if (( RESIZE_ARENAS || ARM_LAYOUT_SAVE || FIT_HOMOGRAPHIES || ACCEPT_HOMOGRAPHIES ||
        PROJECTOR_INTENSITY_REPORT_EXPLICIT || ! FOREGROUND_GRAY_EXPLICIT )); then
    echo "--bootstrap-centers-only requires an explicit provisional gray and forbids a commissioned report, resize, or homography options" >&2
    exit 2
  fi
  PROJECTOR_INTENSITY_REPORT=""
  PROJECTOR_INTENSITY_REPORT_SHA256=""
  SATURATION_PIXEL_THRESHOLD_U8=250
  MAXIMUM_DOT_CORE_SATURATION_FRACTION=0.005
  MINIMUM_DOT_BACKGROUND_CONTRAST_U8=20
else
  [[ -f "${PROJECTOR_INTENSITY_REPORT}" ]] || {
    echo "Missing projector-intensity commissioning report: ${PROJECTOR_INTENSITY_REPORT}" >&2
    exit 1
  }
  jq -e '
    .schema_id == "orange.projector_intensity_commissioning.report" and
    .schema_version == 1 and
    .status == "pass"
  ' "${PROJECTOR_INTENSITY_REPORT}" >/dev/null || {
    echo "Projector-intensity report is not a passing schema-v1 report" >&2
    exit 1
  }
  COMMISSIONED_FOREGROUND_GRAY_U8="$(
    jq -er '.recommended_foreground_gray_u8 | select(type == "number")' \
      "${PROJECTOR_INTENSITY_REPORT}"
  )"
  if (( ! FOREGROUND_GRAY_EXPLICIT )); then
    FOREGROUND_GRAY_U8="${COMMISSIONED_FOREGROUND_GRAY_U8}"
  fi
  SATURATION_PIXEL_THRESHOLD_U8="$(
    jq -er '.method.saturation_pixel_threshold_u8 | select(type == "number")' \
      "${PROJECTOR_INTENSITY_REPORT}"
  )"
  MAXIMUM_DOT_CORE_SATURATION_FRACTION="$(
    jq -er '.method.quality_gates.max_core_saturation_fraction | select(type == "number")' \
      "${PROJECTOR_INTENSITY_REPORT}"
  )"
  MINIMUM_DOT_BACKGROUND_CONTRAST_U8="$(
    jq -er '.method.quality_gates.min_dot_background_contrast_u8 | select(type == "number")' \
      "${PROJECTOR_INTENSITY_REPORT}"
  )"
  read -r PROJECTOR_INTENSITY_REPORT_SHA256 _ < <(
    sha256sum "${PROJECTOR_INTENSITY_REPORT}"
  )
fi
for value in FRAME_COUNT PROBE_CANVAS_PX MAX_PTP_SPAN_NS CALIBRATION_FRAME_RATE_HZ CALIBRATION_EXPOSURE_US TIMEOUT_SECONDS; do
  [[ "${!value}" =~ ^[1-9][0-9]*$ ]] || {
    echo "${value} must be a positive integer" >&2; exit 2;
  }
done
for value in PROJECTION_SETTLE_MS STABILITY_INTERVAL_MS POST_PRESENTATION_SETTLE_MS; do
  [[ "${!value}" =~ ^[0-9]+$ ]] || {
    echo "${value} must be a non-negative integer" >&2; exit 2;
  }
done
[[ "${RIG_ID}" =~ ^[A-Za-z0-9_.-]+$ ]] || { echo "--rig-id must be a rig id" >&2; exit 2; }
[[ "${CANVAS}" =~ ^[A-Za-z0-9_.-]+$ ]] || { echo "--canvas must be a canvas name" >&2; exit 2; }
if [[ -z "${CITRUS_CONFIG}" ]]; then
  CITRUS_CONFIG="${CITRUS_ROOT}/targets/rigs/${RIG_ID}/${CANVAS}/${CANVAS}.json"
fi
[[ "${FOREGROUND_GRAY_U8}" =~ ^[0-9]+$ ]] &&
  (( FOREGROUND_GRAY_U8 >= 0 && FOREGROUND_GRAY_U8 <= 255 )) || {
  echo "--foreground-gray-u8 must be in [0,255]" >&2; exit 2;
}
if (( FIT_HOMOGRAPHIES )); then
  jq -e --arg gray "${FOREGROUND_GRAY_U8}" \
    '.level_passes_all_cameras[$gray] == true' \
    "${PROJECTOR_INTENSITY_REPORT}" >/dev/null || {
    echo "Foreground gray ${FOREGROUND_GRAY_U8} is not commissioned for all cameras in ${PROJECTOR_INTENSITY_REPORT}" >&2
    exit 1
  }
  IFS=',' read -r -a CAMERA_ARRAY <<<"${CAMERAS}"
  for camera in "${CAMERA_ARRAY[@]}"; do
    jq -e --arg camera "${camera}" --argjson gray "${FOREGROUND_GRAY_U8}" '
      any(.camera_level_summaries[];
          .camera_serial == $camera and
          .foreground_gray_u8 == $gray and
          .passes_quality_gate == true)
    ' "${PROJECTOR_INTENSITY_REPORT}" >/dev/null || {
      echo "Foreground gray ${FOREGROUND_GRAY_U8} lacks passing commissioning evidence for camera ${camera}" >&2
      exit 1
    }
  done
fi
if (( FIT_HOMOGRAPHIES && ! ARM_SAVE )); then
  echo "--fit-homographies requires --save-verified-centers so the fitted geometry is committed" >&2
  exit 2
fi
if (( FIT_HOMOGRAPHIES && RESIZE_ARENAS && ! ARM_LAYOUT_SAVE )); then
  echo "--fit-homographies with --resize-arenas requires --save-verified-layout" >&2
  exit 2
fi
[[ -x "${CITRUS_BIN}" ]] || { echo "Missing Citrus binary: ${CITRUS_BIN}" >&2; exit 1; }
[[ -x "${ORANGE_GUI_BINARY}" ]] || { echo "Missing Orange binary: ${ORANGE_GUI_BINARY}" >&2; exit 1; }
[[ -f "${CITRUS_CONFIG}" ]] || { echo "Missing Citrus config: ${CITRUS_CONFIG}" >&2; exit 1; }
[[ -f "${CITRUS_PROTOCOL}" ]] || { echo "Missing Citrus protocol" >&2; exit 1; }

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
if [[ -z "${RESULT_JSON}" ]]; then
  RESULT_JSON="/tmp/orange_gui_arena_centering_${STAMP}.json"
fi
CITRUS_LOG="/tmp/orange_gui_arena_centering_${STAMP}_citrus.log"
ORANGE_LOG="/tmp/orange_gui_arena_centering_${STAMP}_orange.log"
TIMING_CONFIG_DIR="$(dirname "${ORANGE_CONFIG_SOURCE}")/arena_centering_runtime_${STAMP}_$$"
if [[ -z "${RECORD_STEP_NAME}" ]]; then
  if (( BOOTSTRAP_CENTERS_ONLY )); then
    RECORD_STEP_NAME="arena_centering_bootstrap_centers"
  elif (( FIT_HOMOGRAPHIES )); then
    RECORD_STEP_NAME="arena_centering_reference_homography"
  else
    RECORD_STEP_NAME="arena_centering"
  fi
fi

# Append this run to the commissioning record (launcher result, both logs,
# the sessions it saved), passed or failed.
record_step() {
  local exit_code="$1"
  if [[ -z "${RECORD_DIR}" || ${EXECUTE} -ne 1 || ${RECORD_DONE} -ne 0 ]]; then
    return 0
  fi
  RECORD_DONE=1
  local step_status="passed"
  (( exit_code == 0 )) || step_status="failed"
  python3 "${REPO_ROOT}/scripts/commissioning_record.py" step --record "${RECORD_DIR}" \
    --name "${RECORD_STEP_NAME}" --status "${step_status}" --exit-code "${exit_code}" \
    --result-json "${RESULT_JSON}" --log "${ORANGE_LOG}" --log "${CITRUS_LOG}" \
    -- "${BASH_SOURCE[0]}" "${ORIGINAL_ARGS[@]}" >/dev/null ||
    echo "WARNING: could not append this run to the commissioning record ${RECORD_DIR}" >&2
}

# Displays: the host config names the stimulus output and its X screen;
# xrandr says what is connected on each X screen of this server.
XRANDR_BIN="${XRANDR_BIN:-xrandr}"
DISPLAY_SERVER="${DISPLAY_VALUE%.*}"
if [[ "${DISPLAY_VALUE}" == *.* ]]; then
  ORANGE_X_SCREEN="${DISPLAY_VALUE##*.}"
else
  ORANGE_X_SCREEN=0
fi
screen_inventory() {
  DISPLAY="${DISPLAY_SERVER}" XAUTHORITY="${XAUTHORITY_VALUE}" \
    "${XRANDR_BIN}" --screen "$1" --query 2>/dev/null
}
output_connected_on_screen() {
  local inventory
  inventory="$(screen_inventory "$2")" || return 1
  awk -v wanted="$1" '
    $1 == wanted && $2 == "connected" { found = 1 }
    END { exit(found ? 0 : 1) }
  ' <<<"${inventory}"
}
DISPLAY_REFUSAL=""
STIMULUS_X_SCREEN=""
if [[ "${STIMULUS_MONITOR_SOURCE}" == "host_config" ]]; then
  if HOST_STIMULUS="$(
    python3 "${REPO_ROOT}/scripts/host_stimulus_output.py" \
      --host-config "${HOST_CONFIG}" --rig-id "${RIG_ID}" \
      --projection-path "${PROJECTION_PATH}" 2>&1
  )"; then
    read -r STIMULUS_MONITOR STIMULUS_X_SCREEN <<<"${HOST_STIMULUS}"
    [[ "${STIMULUS_X_SCREEN}" != "-" ]] || STIMULUS_X_SCREEN="${ORANGE_X_SCREEN}"
  else
    DISPLAY_REFUSAL="${HOST_STIMULUS}"
  fi
else
  # Named on the command line: find it on this server's X screens.
  for screen in $(seq 0 15); do
    screen_inventory "${screen}" >/dev/null || break
    if output_connected_on_screen "${STIMULUS_MONITOR}" "${screen}"; then
      STIMULUS_X_SCREEN="${screen}"
      break
    fi
  done
fi
if [[ -z "${OPERATOR_MONITOR}" ]]; then
  OPERATOR_MONITOR="$(
    screen_inventory "${ORANGE_X_SCREEN}" |
      awk '$2 == "connected" && $3 == "primary" { print $1; exit }'
  )" || true
fi
for value in OPERATOR_MONITOR STIMULUS_MONITOR; do
  if [[ -z "${DISPLAY_REFUSAL}" && ! "${!value}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
    DISPLAY_REFUSAL="${value} is not resolved to an XRandR output name (got '${!value}')"
  fi
done
if [[ -z "${DISPLAY_REFUSAL}" && "${OPERATOR_MONITOR}" == "${STIMULUS_MONITOR}" ]]; then
  DISPLAY_REFUSAL="Operator and stimulus monitors must differ (${OPERATOR_MONITOR})"
fi
if [[ -z "${DISPLAY_REFUSAL}" ]] &&
   ! output_connected_on_screen "${OPERATOR_MONITOR}" "${ORANGE_X_SCREEN}"; then
  DISPLAY_REFUSAL="Operator monitor ${OPERATOR_MONITOR} is not connected on ${DISPLAY_SERVER}.${ORANGE_X_SCREEN}, where Orange draws"
fi
if [[ -z "${DISPLAY_REFUSAL}" ]]; then
  if [[ -z "${STIMULUS_X_SCREEN}" ]] ||
     ! output_connected_on_screen "${STIMULUS_MONITOR}" "${STIMULUS_X_SCREEN}"; then
    DISPLAY_REFUSAL="Stimulus monitor ${STIMULUS_MONITOR} is not connected on ${DISPLAY_SERVER}.${STIMULUS_X_SCREEN:-?} (${STIMULUS_MONITOR_SOURCE})"
  fi
fi
DISPLAY_LAYOUT="unresolved"
if [[ -z "${DISPLAY_REFUSAL}" ]]; then
  if [[ "${STIMULUS_X_SCREEN}" == "${ORANGE_X_SCREEN}" ]]; then
    DISPLAY_LAYOUT="shared_x_screen"
  else
    DISPLAY_LAYOUT="separate_x_screens"
  fi
fi

if (( EXECUTE )); then
  [[ -z "${DISPLAY_REFUSAL}" ]] || { echo "${DISPLAY_REFUSAL}" >&2; exit 1; }
  python3 "${REPO_ROOT}/scripts/make_guided_calibration_timing_config.py" \
    "${ORANGE_CONFIG_SOURCE}" \
    "${TIMING_CONFIG_DIR}" \
    --frame-rate-hz "${CALIBRATION_FRAME_RATE_HZ}" \
    --exposure-us "${CALIBRATION_EXPOSURE_US}" >/dev/null
  ORANGE_CONFIG_DIR="${TIMING_CONFIG_DIR}"
fi

CITRUS_ENV=(
  "DISPLAY=${DISPLAY_VALUE}"
  "XAUTHORITY=${XAUTHORITY_VALUE}"
  "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR_VALUE}"
  "XDG_SESSION_TYPE=x11"
  "CITRUS_GUI_LOCAL_CONTROL_SOCKET=${CITRUS_SOCKET}"
  "CITRUS_GUI_AUTORUN=1"
  "CITRUS_GUI_AUTORUN_RIG=${RIG_ID}"
  "CITRUS_GUI_AUTORUN_CANVAS=${CANVAS}"
  "CITRUS_GUI_AUTORUN_PROTOCOL_PATH=${CITRUS_PROTOCOL}"
  "CITRUS_GUI_AUTORUN_START_DELAY_SECONDS=86400"
  "CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=0"
  "CITRUS_ORANGE_COMPLETION_NOTIFY=0"
)
CITRUS_OUTPUT_SOURCE="host_config"
if [[ "${STIMULUS_MONITOR_SOURCE}" == "option" ]]; then
  # Named on the command line: Citrus's test-only overrides, which skip its
  # host-config check.
  CITRUS_ENV+=("CITRUS_STIMULUS_DISPLAY_OUTPUT_NAME=${STIMULUS_MONITOR}")
  CITRUS_OUTPUT_SOURCE="output_name_override"
  if [[ -n "${STIMULUS_X_SCREEN}" && "${STIMULUS_X_SCREEN}" != "${ORANGE_X_SCREEN}" ]]; then
    CITRUS_ENV+=("CITRUS_STIMULUS_X_SCREEN=${STIMULUS_X_SCREEN}")
  fi
fi

ORANGE_ENV=(
  "DISPLAY=${DISPLAY_VALUE}"
  "XAUTHORITY=${XAUTHORITY_VALUE}"
  "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR_VALUE}"
  "XDG_SESSION_TYPE=x11"
  "ORANGE_GUI_CONFIG_DIR=${ORANGE_CONFIG_DIR}"
  "ORANGE_GUI_EXPECT_CAMERAS=${CAMERAS}"
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
  "ORANGE_CROP_RECORDING_SINK_MODE=none"
  "ORANGE_GUI_LOCAL_CONTROL_DISABLE=1"
  "ORANGE_GUI_GUIDED_CAPTURE_AUTORUN=0"
  "ORANGE_GUI_ARENA_CENTERING_AUTORUN=1"
  "ORANGE_GUI_ARENA_CENTERING_CITRUS_CONFIG_PATH=${CITRUS_CONFIG}"
  "ORANGE_GUI_ARENA_CENTERING_CAMERAS=${CAMERAS}"
  "ORANGE_GUI_ARENA_CENTERING_FRAME_COUNT=${FRAME_COUNT}"
  "ORANGE_GUI_ARENA_CENTERING_FOREGROUND_GRAY_U8=${FOREGROUND_GRAY_U8}"
  "ORANGE_GUI_HOMOGRAPHY_SATURATION_PIXEL_THRESHOLD_U8=${SATURATION_PIXEL_THRESHOLD_U8}"
  "ORANGE_GUI_HOMOGRAPHY_MAXIMUM_DOT_CORE_SATURATION_FRACTION=${MAXIMUM_DOT_CORE_SATURATION_FRACTION}"
  "ORANGE_GUI_HOMOGRAPHY_MINIMUM_DOT_BACKGROUND_CONTRAST_U8=${MINIMUM_DOT_BACKGROUND_CONTRAST_U8}"
  "ORANGE_GUI_ARENA_CENTERING_PROBE_CANVAS_PX=${PROBE_CANVAS_PX}"
  "ORANGE_GUI_ARENA_CENTERING_VERIFICATION_TOLERANCE_CAMERA_PX=${VERIFICATION_TOLERANCE_CAMERA_PX}"
  "ORANGE_GUI_ARENA_CENTERING_MAX_REFINEMENT_CANVAS_PX=${MAX_REFINEMENT_CANVAS_PX}"
  "ORANGE_GUI_ARENA_CENTERING_RESIZE_ARENAS=${RESIZE_ARENAS}"
  "ORANGE_GUI_ARENA_CENTERING_RECTANGLE_SAFETY_MARGIN_CAMERA_PX=${RECTANGLE_SAFETY_MARGIN_CAMERA_PX}"
  "ORANGE_GUI_ARENA_CENTERING_MAX_PTP_SPAN_NS=${MAX_PTP_SPAN_NS}"
  "ORANGE_GUI_ARENA_CENTERING_APPLY_CALIBRATION_PREFLIGHT=1"
  "ORANGE_GUI_ARENA_CENTERING_FRAME_RATE_HZ=${CALIBRATION_FRAME_RATE_HZ}"
  "ORANGE_GUI_ARENA_CENTERING_EXPOSURE_US=${CALIBRATION_EXPOSURE_US}"
  "ORANGE_GUI_ARENA_CENTERING_PROJECTION_SETTLE_MS=${PROJECTION_SETTLE_MS}"
  "ORANGE_GUI_ARENA_CENTERING_REQUIRE_STABILITY_CAPTURE=1"
  "ORANGE_GUI_ARENA_CENTERING_STABILITY_INTERVAL_MS=${STABILITY_INTERVAL_MS}"
  "ORANGE_GUI_GROUP_CAPTURE_POST_PRESENTATION_SETTLE_MS=${POST_PRESENTATION_SETTLE_MS}"
  "ORANGE_GUI_OPERATOR_MONITOR=${OPERATOR_MONITOR}"
  "ORANGE_CITRUS_EXPECTED_STIMULUS_MONITOR=${STIMULUS_MONITOR}"
  "ORANGE_GUI_ARENA_CENTERING_SAVE_CAPTURES=1"
  "ORANGE_GUI_ARENA_CENTERING_BOOTSTRAP_CENTERS_ONLY=${BOOTSTRAP_CENTERS_ONLY}"
  "ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_CENTERS_ARMED=${ARM_SAVE}"
  "ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_LAYOUT_ARMED=${ARM_LAYOUT_SAVE}"
  "ORANGE_GUI_ARENA_CENTERING_FIT_HOMOGRAPHIES=${FIT_HOMOGRAPHIES}"
  "ORANGE_GUI_ARENA_CENTERING_ACCEPT_HOMOGRAPHIES_ARMED=${ACCEPT_HOMOGRAPHIES}"
  "ORANGE_GUI_ARENA_CENTERING_EXIT_AFTER_COMPLETION=1"
  "ORANGE_GUI_ARENA_CENTERING_RESULT_JSON=${RESULT_JSON}"
)
ORANGE_RESERVED_MONITOR="none_separate_x_screen"
if [[ "${DISPLAY_LAYOUT}" != "separate_x_screens" ]]; then
  # Same X screen: Orange must not cover the projector output. On another X
  # screen GLFW cannot see it (and Orange cannot cover it).
  ORANGE_ENV+=("ORANGE_GUI_RESERVED_MONITOR=${STIMULUS_MONITOR}")
  ORANGE_RESERVED_MONITOR="${STIMULUS_MONITOR}"
fi
if (( ! BOOTSTRAP_CENTERS_ONLY )); then
  ORANGE_ENV+=(
    "ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_PATH=${PROJECTOR_INTENSITY_REPORT}"
    "ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_SHA256=${PROJECTOR_INTENSITY_REPORT_SHA256}"
  )
fi

echo "Arena-centering commissioning plan:"
echo "  bootstrap_centers_only=${BOOTSTRAP_CENTERS_ONLY}"
echo "  cameras=${CAMERAS} gray=${FOREGROUND_GRAY_U8} probe_canvas_px=${PROBE_CANVAS_PX}"
if (( BOOTSTRAP_CENTERS_ONLY )); then
  echo "  intensity_report=none gray_qualification=provisional_for_center_fiducial_only"
else
  echo "  intensity_report=${PROJECTOR_INTENSITY_REPORT} sha256=${PROJECTOR_INTENSITY_REPORT_SHA256}"
  echo "  photometry=max_core_saturation:${MAXIMUM_DOT_CORE_SATURATION_FRACTION}@>=${SATURATION_PIXEL_THRESHOLD_U8} min_contrast_u8:${MINIMUM_DOT_BACKGROUND_CONTRAST_U8}"
fi
echo "  tolerance_camera_px=${VERIFICATION_TOLERANCE_CAMERA_PX} max_ptp_span_ns=${MAX_PTP_SPAN_NS}"
echo "  camera_timing=${CALIBRATION_FRAME_RATE_HZ}fps/${CALIBRATION_EXPOSURE_US}us PTP=enabled"
echo "  displays=operator:${OPERATOR_MONITOR:-?}@screen${ORANGE_X_SCREEN} stimulus:${STIMULUS_MONITOR:-?}@screen${STIMULUS_X_SCREEN:-?} layout=${DISPLAY_LAYOUT} rig=${RIG_ID} stimulus_source=${STIMULUS_MONITOR_SOURCE}"
echo "  citrus_output=${CITRUS_OUTPUT_SOURCE} orange_reserved_monitor=${ORANGE_RESERVED_MONITOR} citrus_monitor_check=${STIMULUS_MONITOR}"
echo "  display_check=${DISPLAY_REFUSAL:-ok}"
echo "  projection_settle=${PROJECTION_SETTLE_MS}ms post_fence_settle=${POST_PRESENTATION_SETTLE_MS}ms"
echo "  stability_pair=required interval=${STABILITY_INTERVAL_MS}ms ghosted_marker_rejection=enabled"
echo "  rectangle_edges=$((1 - BOOTSTRAP_CENTERS_ONLY)) resize_arenas=${RESIZE_ARENAS} safety_margin_camera_px=${RECTANGLE_SAFETY_MARGIN_CAMERA_PX}"
echo "  save_verified_centers_armed=${ARM_SAVE} save_verified_layout_armed=${ARM_LAYOUT_SAVE}"
echo "  fit_homographies=${FIT_HOMOGRAPHIES} accept_homographies_armed=${ACCEPT_HOMOGRAPHIES}"
echo "  one_process_pair=true one_session=true"
echo "  result=${RESULT_JSON}"
echo "  record=${RECORD_DIR:-none} step=${RECORD_STEP_NAME}"
if (( ! EXECUTE )); then
  echo "Dry-run only; add --execute to control the real projector and cameras."
  exit 0
fi
if [[ -n "${RECORD_DIR}" ]]; then
  python3 "${REPO_ROOT}/scripts/commissioning_record.py" show --record "${RECORD_DIR}" |
    grep -q '"status": "open"' || {
      echo "Commissioning record is not an open record: ${RECORD_DIR}" >&2
      rm -rf -- "${TIMING_CONFIG_DIR}"
      exit 2
    }
fi
trap 'record_step $?' EXIT

if [[ -e "${CITRUS_SOCKET}" || -e "${ORANGE_SOCKET}" ]]; then
  echo "Refusing to start while a local-control socket exists" >&2
  ls -l "${CITRUS_SOCKET}" "${ORANGE_SOCKET}" 2>/dev/null || true
  rm -rf -- "${TIMING_CONFIG_DIR}"
  exit 1
fi
rm -f "${RESULT_JSON}" "${RESULT_JSON}.tmp"

citrus_pid=""
cleanup() {
  if [[ -n "${citrus_pid}" ]] && kill -0 "${citrus_pid}" 2>/dev/null; then
    kill -INT "${citrus_pid}" 2>/dev/null || true
    for _ in $(seq 1 50); do
      kill -0 "${citrus_pid}" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "${citrus_pid}" 2>/dev/null; then
      kill -TERM "${citrus_pid}" 2>/dev/null || true
    fi
    wait "${citrus_pid}" 2>/dev/null || true
  fi
  rm -f "${CITRUS_SOCKET}"
  if [[ -d "${TIMING_CONFIG_DIR}" ]]; then rm -rf -- "${TIMING_CONFIG_DIR}"; fi
}
trap 'exit_code=$?; cleanup; record_step "${exit_code}"' EXIT
trap 'cleanup; record_step 130; exit 130' INT TERM

env "${CITRUS_ENV[@]}" "${CITRUS_BIN}" >"${CITRUS_LOG}" 2>&1 &
citrus_pid=$!
for _ in $(seq 1 300); do
  [[ -S "${CITRUS_SOCKET}" ]] && break
  if ! kill -0 "${citrus_pid}" 2>/dev/null; then
    echo "Citrus exited before opening its control socket" >&2
    tail -n 100 "${CITRUS_LOG}" >&2 || true
    exit 1
  fi
  sleep 0.1
done
[[ -S "${CITRUS_SOCKET}" ]] || {
  echo "Timed out waiting for Citrus control socket" >&2
  tail -n 100 "${CITRUS_LOG}" >&2 || true
  exit 1
}

set +e
timeout --signal=INT --kill-after=10s "${TIMEOUT_SECONDS}s" \
  env "${ORANGE_ENV[@]}" \
  "${REPO_ROOT}/scripts/run_gui_fourcam_external_ipc_validation.sh" \
    --hidden-crop-preview \
    --citrus-display-safe >"${ORANGE_LOG}" 2>&1
orange_status=$?
set -e

cleanup
trap 'record_step $?' EXIT
trap - INT TERM

if [[ ${orange_status} -ne 0 ]]; then
  echo "Orange arena-centering GUI failed with status ${orange_status}" >&2
  tail -n 160 "${ORANGE_LOG}" >&2 || true
  tail -n 120 "${CITRUS_LOG}" >&2 || true
  exit "${orange_status}"
fi
[[ -f "${RESULT_JSON}" ]] || {
  echo "Orange exited without a result JSON" >&2
  tail -n 160 "${ORANGE_LOG}" >&2 || true
  exit 1
}

VALIDATOR=(
  "${REPO_ROOT}/scripts/validate_gui_arena_centering_result.py"
  "${RESULT_JSON}"
  "--expected-cameras" "${CAMERAS}"
)
if (( BOOTSTRAP_CENTERS_ONLY )); then VALIDATOR+=("--require-bootstrap-centers"); fi
if (( ARM_SAVE )); then VALIDATOR+=("--require-committed"); fi
if (( FIT_HOMOGRAPHIES )); then VALIDATOR+=("--require-homography-fit"); fi
if (( ACCEPT_HOMOGRAPHIES )); then VALIDATOR+=("--require-homography-committed"); fi
"${VALIDATOR[@]}"
echo "Arena-centering result: ${RESULT_JSON}"
echo "Orange log: ${ORANGE_LOG}"
echo "Citrus log: ${CITRUS_LOG}"
