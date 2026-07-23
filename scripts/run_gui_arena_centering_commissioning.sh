#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXECUTE=0
ARM_SAVE=0
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
OPERATOR_MONITOR="DP-0"
STIMULUS_MONITOR="DP-3"
TIMEOUT_SECONDS=300
DISPLAY_VALUE="${DISPLAY:-:1}"
XAUTHORITY_VALUE="${XAUTHORITY:-/run/user/1000/gdm/Xauthority}"
XDG_RUNTIME_DIR_VALUE="${XDG_RUNTIME_DIR:-/run/user/1000}"
RESULT_JSON=""
CITRUS_CONFIG="/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json"
PROJECTOR_INTENSITY_REPORT="/home/jeremy/orange_data/calibrations/commissioning/projector_intensity_20260719T014235Z/commissioning_report.json"
CITRUS_BIN="/home/jeremy/citrus/targets/citrus"
CITRUS_PROTOCOL="/home/jeremy/citrus/protocols/good_cop_bad_cop_demo.json"
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

Options:
  --execute
  --save-verified-centers
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
  --operator-monitor <output-name>
  --stimulus-monitor <output-name>
  --result-json <path>
  --timeout-seconds <integer>
  --display <display>
  --xauthority <path>
  --citrus-config <path>
  --help
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
    --stimulus-monitor) shift; require_value --stimulus-monitor "$#"; STIMULUS_MONITOR="$1"; shift ;;
    --result-json) shift; require_value --result-json "$#"; RESULT_JSON="$1"; shift ;;
    --timeout-seconds) shift; require_value --timeout-seconds "$#"; TIMEOUT_SECONDS="$1"; shift ;;
    --display) shift; require_value --display "$#"; DISPLAY_VALUE="$1"; shift ;;
    --xauthority) shift; require_value --xauthority "$#"; XAUTHORITY_VALUE="$1"; shift ;;
    --citrus-config) shift; require_value --citrus-config "$#"; CITRUS_CONFIG="$1"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unsupported argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "${CAMERAS}" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
  echo "--cameras must contain comma-separated numeric serials" >&2; exit 2;
}
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
for value in OPERATOR_MONITOR STIMULUS_MONITOR; do
  [[ "${!value}" =~ ^[A-Za-z0-9_.-]+$ ]] || {
    echo "${value} must be an XRandR output name" >&2; exit 2;
  }
done
[[ "${OPERATOR_MONITOR}" != "${STIMULUS_MONITOR}" ]] || {
  echo "Operator and stimulus monitors must differ" >&2; exit 2;
}
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
[[ -x "${REPO_ROOT}/targets/release/orange" ]] || { echo "Missing Orange binary" >&2; exit 1; }
[[ -f "${CITRUS_CONFIG}" ]] || { echo "Missing Citrus config: ${CITRUS_CONFIG}" >&2; exit 1; }
[[ -f "${CITRUS_PROTOCOL}" ]] || { echo "Missing Citrus protocol" >&2; exit 1; }

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
if [[ -z "${RESULT_JSON}" ]]; then
  RESULT_JSON="/tmp/orange_gui_arena_centering_${STAMP}.json"
fi
CITRUS_LOG="/tmp/orange_gui_arena_centering_${STAMP}_citrus.log"
ORANGE_LOG="/tmp/orange_gui_arena_centering_${STAMP}_orange.log"
TIMING_CONFIG_DIR="$(dirname "${ORANGE_CONFIG_SOURCE}")/arena_centering_runtime_${STAMP}_$$"

if (( EXECUTE )); then
  MONITOR_INVENTORY="$(
    DISPLAY="${DISPLAY_VALUE}" XAUTHORITY="${XAUTHORITY_VALUE}" xrandr --query
  )" || {
    echo "Could not query XRandR monitor inventory on ${DISPLAY_VALUE}" >&2
    exit 1
  }
  for monitor in "${OPERATOR_MONITOR}" "${STIMULUS_MONITOR}"; do
    awk -v wanted="${monitor}" '
      $1 == wanted && $2 == "connected" { found = 1 }
      END { exit(found ? 0 : 1) }
    ' <<<"${MONITOR_INVENTORY}" || {
      echo "Required connected monitor is missing: ${monitor}" >&2
      exit 1
    }
  done
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
  "CITRUS_GUI_AUTORUN_RIG=omnifin0"
  "CITRUS_GUI_AUTORUN_CANVAS=shadow"
  "CITRUS_GUI_AUTORUN_PROTOCOL_PATH=${CITRUS_PROTOCOL}"
  "CITRUS_GUI_AUTORUN_START_DELAY_SECONDS=86400"
  "CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=0"
  "CITRUS_ORANGE_COMPLETION_NOTIFY=0"
  "CITRUS_STIMULUS_DISPLAY_OUTPUT_NAME=${STIMULUS_MONITOR}"
)

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
  "ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_PATH=${PROJECTOR_INTENSITY_REPORT}"
  "ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_SHA256=${PROJECTOR_INTENSITY_REPORT_SHA256}"
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
  "ORANGE_GUI_RESERVED_MONITOR=${STIMULUS_MONITOR}"
  "ORANGE_CITRUS_EXPECTED_STIMULUS_MONITOR=${STIMULUS_MONITOR}"
  "ORANGE_GUI_ARENA_CENTERING_SAVE_CAPTURES=1"
  "ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_CENTERS_ARMED=${ARM_SAVE}"
  "ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_LAYOUT_ARMED=${ARM_LAYOUT_SAVE}"
  "ORANGE_GUI_ARENA_CENTERING_FIT_HOMOGRAPHIES=${FIT_HOMOGRAPHIES}"
  "ORANGE_GUI_ARENA_CENTERING_ACCEPT_HOMOGRAPHIES_ARMED=${ACCEPT_HOMOGRAPHIES}"
  "ORANGE_GUI_ARENA_CENTERING_EXIT_AFTER_COMPLETION=1"
  "ORANGE_GUI_ARENA_CENTERING_RESULT_JSON=${RESULT_JSON}"
)

echo "Arena-centering commissioning plan:"
echo "  cameras=${CAMERAS} gray=${FOREGROUND_GRAY_U8} probe_canvas_px=${PROBE_CANVAS_PX}"
echo "  intensity_report=${PROJECTOR_INTENSITY_REPORT} sha256=${PROJECTOR_INTENSITY_REPORT_SHA256}"
echo "  photometry=max_core_saturation:${MAXIMUM_DOT_CORE_SATURATION_FRACTION}@>=${SATURATION_PIXEL_THRESHOLD_U8} min_contrast_u8:${MINIMUM_DOT_BACKGROUND_CONTRAST_U8}"
echo "  tolerance_camera_px=${VERIFICATION_TOLERANCE_CAMERA_PX} max_ptp_span_ns=${MAX_PTP_SPAN_NS}"
echo "  camera_timing=${CALIBRATION_FRAME_RATE_HZ}fps/${CALIBRATION_EXPOSURE_US}us PTP=enabled"
echo "  displays=operator:${OPERATOR_MONITOR} stimulus_reserved:${STIMULUS_MONITOR} overlap_rejection=enabled"
echo "  projection_settle=${PROJECTION_SETTLE_MS}ms post_fence_settle=${POST_PRESENTATION_SETTLE_MS}ms"
echo "  stability_pair=required interval=${STABILITY_INTERVAL_MS}ms ghosted_marker_rejection=enabled"
echo "  rectangle_edges=enabled resize_arenas=${RESIZE_ARENAS} safety_margin_camera_px=${RECTANGLE_SAFETY_MARGIN_CAMERA_PX}"
echo "  save_verified_centers_armed=${ARM_SAVE} save_verified_layout_armed=${ARM_LAYOUT_SAVE}"
echo "  fit_homographies=${FIT_HOMOGRAPHIES} accept_homographies_armed=${ACCEPT_HOMOGRAPHIES}"
echo "  one_process_pair=true one_session=true"
echo "  result=${RESULT_JSON}"
if (( ! EXECUTE )); then
  echo "Dry-run only; add --execute to control the real projector and cameras."
  exit 0
fi

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
trap cleanup EXIT INT TERM

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
trap - EXIT INT TERM

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
if (( ARM_SAVE )); then VALIDATOR+=("--require-committed"); fi
if (( FIT_HOMOGRAPHIES )); then VALIDATOR+=("--require-homography-fit"); fi
if (( ACCEPT_HOMOGRAPHIES )); then VALIDATOR+=("--require-homography-committed"); fi
"${VALIDATOR[@]}"
echo "Arena-centering result: ${RESULT_JSON}"
echo "Orange log: ${ORANGE_LOG}"
echo "Citrus log: ${CITRUS_LOG}"
