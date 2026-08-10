#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXECUTE=0
PROFILE=""
RECIPE="black_reference"
RECIPE_EXPLICIT=0
RECIPE_SEQUENCE=""
FIXTURE_APERTURE_SHAPE="circle"
PURPOSE=""
PURPOSE_EXPLICIT=0
CAMERAS="2010093,2010094,2010095,2010096"
FRAME_COUNT=1
APPLY_CALIBRATION_PREFLIGHT=1
CALIBRATION_FRAME_RATE_HZ=5
CALIBRATION_EXPOSURE_US=100000
CAMERA_IRIS_OVERRIDES=""
FOREGROUND_GRAY_U8=255
FOREGROUND_GRAY_EXPLICIT=0
SWEEP_FOREGROUND_GRAYS_U8=""
SWEEP_REPEATS=1
INCLUDE_ARENA_OUTLINE_REFERENCE=0
SAVE=0
TIMEOUT_SECONDS=180
TIMEOUT_EXPLICIT=0
DISPLAY_VALUE="${DISPLAY:-:1}"
XAUTHORITY_VALUE="${XAUTHORITY:-/run/user/1000/gdm/Xauthority}"
XDG_RUNTIME_DIR_VALUE="${XDG_RUNTIME_DIR:-/run/user/1000}"
RESULT_JSON=""
CITRUS_CONFIG="/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json"
CITRUS_BIN="/home/jeremy/citrus/targets/citrus"
CITRUS_PROTOCOL="/home/jeremy/citrus/protocols/good_cop_bad_cop_demo.json"
CITRUS_SOCKET="/tmp/citrus_local_control.sock"
ORANGE_SOCKET="/tmp/orange_local_control.sock"
ORANGE_CONFIG_SOURCE="/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam"
ORANGE_CONFIG_DIR="${ORANGE_CONFIG_SOURCE}"
START_AT_CALIBRATION_TIMING=1
PROJECTED_SURFACE_TARGETS_READY=0
ACCEPT_PROJECTED_SURFACE_SCALES=0
FIT_HOMOGRAPHIES=0
TIMING_CONFIG_DIR=""
TIMING_OVERRIDE_ARGS=()

usage() {
  cat <<'EOF'
Usage: scripts/run_gui_guided_capture_smoke.sh [options]

Runs a real Orange GUI + Citrus guided grouped-capture smoke. Dry-run by
default; pass --execute to control the real display and cameras.

Options:
  --execute
  --profile <unobstructed_canvas_commissioning|holder_installed_projected_surface|wet_tank_projected_surface|installed_tank_registration>
  --recipe <black_reference|uniform_gray|arena_outline|experimental_area_center_and_outline|homography_grid|homography_rings|verification_dots>
  --recipe-sequence <comma-separated recipes>
  --fixture-aperture-shape <circle|rectangle|rounded_rectangle|polygon|unknown>
  --purpose <purpose>
  --cameras <comma-separated serials>
  --frame-count <count>
  --foreground-gray-u8 <0-255>
  --targets-ready
  --accept-projected-surface-scales
  --fit-homographies
  --sweep-foreground-grays-u8 <comma-separated 0-255 values>
  --sweep-repeats <count>
  --include-arena-outline-reference
  --calibration-frame-rate-hz <hz>
  --calibration-exposure-us <microseconds>
  --camera-iris-overrides <serial=value[,serial=value...]>
  --no-start-at-calibration-timing
  --no-calibration-preflight
  --save
  --result-json <path>
  --timeout-seconds <seconds>
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
    --execute)
      EXECUTE=1
      shift
      ;;
    --profile)
      shift
      require_value --profile "$#"
      PROFILE="$1"
      shift
      ;;
    --recipe)
      shift
      require_value --recipe "$#"
      RECIPE="$1"
      RECIPE_EXPLICIT=1
      shift
      ;;
    --recipe-sequence)
      shift
      require_value --recipe-sequence "$#"
      RECIPE_SEQUENCE="$1"
      shift
      ;;
    --fixture-aperture-shape)
      shift
      require_value --fixture-aperture-shape "$#"
      FIXTURE_APERTURE_SHAPE="$1"
      shift
      ;;
    --purpose)
      shift
      require_value --purpose "$#"
      PURPOSE="$1"
      PURPOSE_EXPLICIT=1
      shift
      ;;
    --cameras)
      shift
      require_value --cameras "$#"
      CAMERAS="$1"
      shift
      ;;
    --frame-count)
      shift
      require_value --frame-count "$#"
      FRAME_COUNT="$1"
      shift
      ;;
    --foreground-gray-u8)
      shift
      require_value --foreground-gray-u8 "$#"
      FOREGROUND_GRAY_U8="$1"
      FOREGROUND_GRAY_EXPLICIT=1
      shift
      ;;
    --targets-ready)
      PROJECTED_SURFACE_TARGETS_READY=1
      shift
      ;;
    --accept-projected-surface-scales)
      ACCEPT_PROJECTED_SURFACE_SCALES=1
      shift
      ;;
    --fit-homographies)
      FIT_HOMOGRAPHIES=1
      shift
      ;;
    --sweep-foreground-grays-u8)
      shift
      require_value --sweep-foreground-grays-u8 "$#"
      SWEEP_FOREGROUND_GRAYS_U8="$1"
      shift
      ;;
    --sweep-repeats)
      shift
      require_value --sweep-repeats "$#"
      SWEEP_REPEATS="$1"
      shift
      ;;
    --include-arena-outline-reference)
      INCLUDE_ARENA_OUTLINE_REFERENCE=1
      shift
      ;;
    --calibration-frame-rate-hz)
      shift
      require_value --calibration-frame-rate-hz "$#"
      CALIBRATION_FRAME_RATE_HZ="$1"
      shift
      ;;
    --calibration-exposure-us)
      shift
      require_value --calibration-exposure-us "$#"
      CALIBRATION_EXPOSURE_US="$1"
      shift
      ;;
    --camera-iris-overrides)
      shift
      require_value --camera-iris-overrides "$#"
      CAMERA_IRIS_OVERRIDES="$1"
      shift
      ;;
    --no-calibration-preflight)
      APPLY_CALIBRATION_PREFLIGHT=0
      shift
      ;;
    --no-start-at-calibration-timing)
      START_AT_CALIBRATION_TIMING=0
      shift
      ;;
    --save)
      SAVE=1
      shift
      ;;
    --result-json)
      shift
      require_value --result-json "$#"
      RESULT_JSON="$1"
      shift
      ;;
    --timeout-seconds)
      shift
      require_value --timeout-seconds "$#"
      TIMEOUT_SECONDS="$1"
      TIMEOUT_EXPLICIT=1
      shift
      ;;
    --display)
      shift
      require_value --display "$#"
      DISPLAY_VALUE="$1"
      shift
      ;;
    --xauthority)
      shift
      require_value --xauthority "$#"
      XAUTHORITY_VALUE="$1"
      shift
      ;;
    --citrus-config)
      shift
      require_value --citrus-config "$#"
      CITRUS_CONFIG="$1"
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "${PROFILE}" in
  ""|unobstructed_canvas_commissioning|holder_installed_projected_surface|wet_tank_projected_surface|installed_tank_registration)
    ;;
  *)
    echo "Unsupported workflow profile: ${PROFILE}" >&2
    exit 2
    ;;
esac
if [[ -n "${PROFILE}" && ${RECIPE_EXPLICIT} -eq 0 ]]; then
  case "${PROFILE}" in
    unobstructed_canvas_commissioning) RECIPE="homography_grid" ;;
    holder_installed_projected_surface|wet_tank_projected_surface) RECIPE="homography_rings" ;;
    installed_tank_registration) RECIPE="experimental_area_center_and_outline" ;;
  esac
fi
if [[ "${RECIPE}" == "uniform_gray" && ${FOREGROUND_GRAY_EXPLICIT} -eq 0 ]]; then
  FOREGROUND_GRAY_U8=76
fi
if [[ ${PURPOSE_EXPLICIT} -eq 0 ]]; then
  case "${RECIPE}" in
    arena_outline) PURPOSE="arena_projection" ;;
    experimental_area_center_and_outline) PURPOSE="crosshair_alignment" ;;
    homography_grid|homography_rings) PURPOSE="homography_grid" ;;
    verification_dots) PURPOSE="verification_dots" ;;
    uniform_gray) PURPOSE="projected_surface_scale_calibration" ;;
    black_reference) PURPOSE="diagnostic_black_reference" ;;
  esac
fi

case "${RECIPE}" in
  black_reference|uniform_gray|arena_outline|experimental_area_center_and_outline|homography_grid|homography_rings|verification_dots)
    ;;
  *)
    echo "Unsupported recipe: ${RECIPE}" >&2
    exit 2
    ;;
esac
case "${FIXTURE_APERTURE_SHAPE}" in
  circle|rectangle|rounded_rectangle|polygon|unknown) ;;
  *)
    echo "Unsupported fixture aperture shape: ${FIXTURE_APERTURE_SHAPE}" >&2
    exit 2
    ;;
esac
if [[ -n "${RECIPE_SEQUENCE}" ]]; then
  [[ "${RECIPE_SEQUENCE}" =~ ^[a-z_]+(,[a-z_]+)*$ ]] || {
    echo "--recipe-sequence must be a comma-separated recipe list without spaces" >&2
    exit 2
  }
  IFS=',' read -r -a sequence_recipes <<<"${RECIPE_SEQUENCE}"
  for sequence_recipe in "${sequence_recipes[@]}"; do
    case "${sequence_recipe}" in
      black_reference|uniform_gray|arena_outline|experimental_area_center_and_outline|homography_grid|homography_rings|verification_dots) ;;
      *)
        echo "Unsupported recipe in sequence: ${sequence_recipe}" >&2
        exit 2
        ;;
    esac
  done
  if [[ -n "${SWEEP_FOREGROUND_GRAYS_U8}" || ${SWEEP_REPEATS} -ne 1 || ${INCLUDE_ARENA_OUTLINE_REFERENCE} -ne 0 ]]; then
    echo "--recipe-sequence cannot be combined with sweep options or --include-arena-outline-reference" >&2
    exit 2
  fi
fi
if [[ "${PURPOSE}" == "projected_surface_scale_calibration" ]]; then
  if (( ! TIMEOUT_EXPLICIT )); then
    TIMEOUT_SECONDS=1800
  fi
  if (( ! SAVE )); then
    echo "projected_surface_scale_calibration requires --save" >&2
    exit 2
  fi
  if (( ! PROJECTED_SURFACE_TARGETS_READY )); then
    echo "projected_surface_scale_calibration requires --targets-ready after one 3 mm target is placed in every camera view" >&2
    exit 2
  fi
  if [[ -n "${SWEEP_FOREGROUND_GRAYS_U8}" || ${SWEEP_REPEATS} -ne 1 || ${INCLUDE_ARENA_OUTLINE_REFERENCE} -ne 0 ]]; then
    echo "projected_surface_scale_calibration requires one uniform-gray grouped sample without a sweep or arena-outline reference" >&2
    exit 2
  fi
fi
if (( FIT_HOMOGRAPHIES )); then
  if [[ "${PROFILE}" != "holder_installed_projected_surface" || ${SAVE} -ne 1 ||
        "${RECIPE_SEQUENCE}" != *verification_dots* ]]; then
    echo "--fit-homographies requires the holder-installed profile, --save, and a recipe sequence containing verification_dots" >&2
    exit 2
  fi
fi
[[ "${CAMERAS}" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
  echo "--cameras must be a comma-separated list of numeric serials" >&2
  exit 2
}
[[ "${FRAME_COUNT}" =~ ^[1-9][0-9]*$ ]] || {
  echo "--frame-count must be a positive integer" >&2
  exit 2
}
[[ "${FOREGROUND_GRAY_U8}" =~ ^[0-9]+$ ]] &&
  (( FOREGROUND_GRAY_U8 >= 0 && FOREGROUND_GRAY_U8 <= 255 )) || {
  echo "--foreground-gray-u8 must be an integer from 0 through 255" >&2
  exit 2
}
if [[ -n "${SWEEP_FOREGROUND_GRAYS_U8}" ]]; then
  [[ "${SWEEP_FOREGROUND_GRAYS_U8}" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
    echo "--sweep-foreground-grays-u8 must be comma-separated integers" >&2
    exit 2
  }
  IFS=',' read -r -a sweep_grays <<<"${SWEEP_FOREGROUND_GRAYS_U8}"
  for gray in "${sweep_grays[@]}"; do
    (( gray >= 0 && gray <= 255 )) || {
      echo "--sweep-foreground-grays-u8 values must be from 0 through 255" >&2
      exit 2
    }
  done
fi
[[ "${SWEEP_REPEATS}" =~ ^[1-9][0-9]*$ ]] || {
  echo "--sweep-repeats must be a positive integer" >&2
  exit 2
}
[[ "${CALIBRATION_FRAME_RATE_HZ}" =~ ^[1-9][0-9]*$ ]] || {
  echo "--calibration-frame-rate-hz must be a positive integer" >&2
  exit 2
}
[[ "${CALIBRATION_EXPOSURE_US}" =~ ^[1-9][0-9]*$ ]] || {
  echo "--calibration-exposure-us must be a positive integer" >&2
  exit 2
}
[[ "${TIMEOUT_SECONDS}" =~ ^[1-9][0-9]*$ ]] || {
  echo "--timeout-seconds must be a positive integer" >&2
  exit 2
}
[[ -x "${CITRUS_BIN}" ]] || { echo "Missing Citrus binary: ${CITRUS_BIN}" >&2; exit 1; }
[[ -x "${REPO_ROOT}/targets/release/orange" ]] || {
  echo "Missing Orange GUI binary: ${REPO_ROOT}/targets/release/orange" >&2
  exit 1
}
[[ -f "${CITRUS_CONFIG}" ]] || { echo "Missing Citrus config: ${CITRUS_CONFIG}" >&2; exit 1; }
[[ -f "${CITRUS_PROTOCOL}" ]] || { echo "Missing Citrus protocol: ${CITRUS_PROTOCOL}" >&2; exit 1; }

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
if [[ -z "${RESULT_JSON}" ]]; then
  RESULT_JSON="/tmp/orange_gui_guided_capture_${STAMP}.json"
fi
CITRUS_LOG="/tmp/orange_gui_guided_capture_${STAMP}_citrus.log"
ORANGE_LOG="/tmp/orange_gui_guided_capture_${STAMP}_orange.log"

if (( START_AT_CALIBRATION_TIMING )); then
  # The GUI intentionally discovers selectable camera configurations only as
  # direct children of orange_data/config/local.  Keep the generated profile
  # ephemeral, but place it inside that discovery boundary so autorun can
  # select it before the cameras are opened and the shared PTP gate is formed.
  TIMING_CONFIG_DIR="$(dirname "${ORANGE_CONFIG_SOURCE}")/guided_calibration_runtime_${STAMP}_$$"
  if (( EXECUTE )); then
    if [[ -n "${CAMERA_IRIS_OVERRIDES}" ]]; then
      TIMING_OVERRIDE_ARGS=(--camera-iris-overrides "${CAMERA_IRIS_OVERRIDES}")
    fi
    python3 "${REPO_ROOT}/scripts/make_guided_calibration_timing_config.py" \
      "${ORANGE_CONFIG_SOURCE}" \
      "${TIMING_CONFIG_DIR}" \
      --frame-rate-hz "${CALIBRATION_FRAME_RATE_HZ}" \
      --exposure-us "${CALIBRATION_EXPOSURE_US}" \
      "${TIMING_OVERRIDE_ARGS[@]}" >/dev/null
    ORANGE_CONFIG_DIR="${TIMING_CONFIG_DIR}"
  fi
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
  "CITRUS_THREADING_SUMMARY_LOG=1"
)

ORANGE_ENV=(
  "DISPLAY=${DISPLAY_VALUE}"
  "XAUTHORITY=${XAUTHORITY_VALUE}"
  "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR_VALUE}"
  "XDG_SESSION_TYPE=x11"
  "ORANGE_GUI_CONFIG_DIR=${ORANGE_CONFIG_DIR}"
  "ORANGE_GUI_EXPECT_CAMERAS=2010093,2010094,2010095,2010096"
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
  "ORANGE_GUI_GUIDED_CAPTURE_AUTORUN=1"
  "ORANGE_GUI_GUIDED_CAPTURE_CITRUS_CONFIG_PATH=${CITRUS_CONFIG}"
  "ORANGE_GUI_GUIDED_CAPTURE_RECIPE=${RECIPE}"
  "ORANGE_GUI_GUIDED_CAPTURE_PURPOSE=${PURPOSE}"
  "ORANGE_GUI_GUIDED_CAPTURE_CAMERAS=${CAMERAS}"
  "ORANGE_GUI_GUIDED_CAPTURE_FRAME_COUNT=${FRAME_COUNT}"
  "ORANGE_GUI_GUIDED_CAPTURE_APPLY_CALIBRATION_PREFLIGHT=${APPLY_CALIBRATION_PREFLIGHT}"
  "ORANGE_GUI_GUIDED_CAPTURE_FRAME_RATE_HZ=${CALIBRATION_FRAME_RATE_HZ}"
  "ORANGE_GUI_GUIDED_CAPTURE_EXPOSURE_US=${CALIBRATION_EXPOSURE_US}"
  "ORANGE_GUI_GUIDED_CAPTURE_FOREGROUND_GRAY_U8=${FOREGROUND_GRAY_U8}"
  "ORANGE_GUI_GUIDED_CAPTURE_RECIPE_SEQUENCE=${RECIPE_SEQUENCE}"
  "ORANGE_GUI_FIXTURE_APERTURE_SHAPE=${FIXTURE_APERTURE_SHAPE}"
  "ORANGE_GUI_GUIDED_CAPTURE_INCLUDE_ARENA_OUTLINE_REFERENCE=${INCLUDE_ARENA_OUTLINE_REFERENCE}"
  "ORANGE_GUI_PROJECTED_SURFACE_SCALE_TARGETS_READY=${PROJECTED_SURFACE_TARGETS_READY}"
  "ORANGE_GUI_ACCEPT_PROJECTED_SURFACE_SCALES_ARMED=${ACCEPT_PROJECTED_SURFACE_SCALES}"
  "ORANGE_GUI_GUIDED_CAPTURE_FIT_HOMOGRAPHIES=${FIT_HOMOGRAPHIES}"
  "ORANGE_GUI_GUIDED_CAPTURE_SAVE=${SAVE}"
  "ORANGE_GUI_GUIDED_CAPTURE_EXIT_AFTER_COMPLETION=1"
  "ORANGE_GUI_GUIDED_CAPTURE_STARTUP_TIMEOUT_SECONDS=120"
  "ORANGE_GUI_GUIDED_CAPTURE_WORKFLOW_TIMEOUT_SECONDS=${TIMEOUT_SECONDS}"
  "ORANGE_GUI_GUIDED_CAPTURE_RESULT_JSON=${RESULT_JSON}"
)
if [[ -n "${SWEEP_FOREGROUND_GRAYS_U8}" ]]; then
  ORANGE_ENV+=(
    "ORANGE_GUI_GUIDED_CAPTURE_SWEEP_FOREGROUND_GRAYS_U8=${SWEEP_FOREGROUND_GRAYS_U8}"
    "ORANGE_GUI_GUIDED_CAPTURE_SWEEP_REPEATS=${SWEEP_REPEATS}"
  )
fi
if [[ -n "${PROFILE}" ]]; then
  ORANGE_ENV+=("ORANGE_GUI_GUIDED_CAPTURE_PROFILE=${PROFILE}")
fi

echo "Guided capture smoke plan:"
echo "  profile=${PROFILE:-none} recipe=${RECIPE} purpose=${PURPOSE} cameras=${CAMERAS} frame_count=${FRAME_COUNT} save=${SAVE}"
if [[ -n "${RECIPE_SEQUENCE}" ]]; then
  echo "  recipe_sequence=${RECIPE_SEQUENCE} fixture_aperture_shape=${FIXTURE_APERTURE_SHAPE} one_process=true one_session=true"
fi
echo "  projected_gray_u8=${FOREGROUND_GRAY_U8} preflight=${APPLY_CALIBRATION_PREFLIGHT} timing=${CALIBRATION_FRAME_RATE_HZ}fps/${CALIBRATION_EXPOSURE_US}us"
if [[ "${PURPOSE}" == "projected_surface_scale_calibration" ]]; then
  echo "  physical_targets_ready=${PROJECTED_SURFACE_TARGETS_READY} target_thickness_mm=3 operator_review=$((1-ACCEPT_PROJECTED_SURFACE_SCALES)) auto_accept=${ACCEPT_PROJECTED_SURFACE_SCALES}"
fi
if (( FIT_HOMOGRAPHIES )); then
  echo "  homography_fit=operational_candidate persist_for_external_review=true auto_promote=false"
fi
if [[ -n "${SWEEP_FOREGROUND_GRAYS_U8}" ]]; then
  echo "  sweep_grays_u8=${SWEEP_FOREGROUND_GRAYS_U8} sweep_repeats=${SWEEP_REPEATS} arena_outline_reference=${INCLUDE_ARENA_OUTLINE_REFERENCE} one_process=true one_session=true"
fi
echo "  ptp_gate_start_timing=${START_AT_CALIBRATION_TIMING} config=${ORANGE_CONFIG_DIR}"
echo "  result=${RESULT_JSON}"
echo "  citrus_log=${CITRUS_LOG}"
echo "  orange_log=${ORANGE_LOG}"
if (( ! EXECUTE )); then
  echo "  Citrus: env ${CITRUS_ENV[*]} ${CITRUS_BIN}"
  echo "  Orange: env ${ORANGE_ENV[*]} scripts/run_gui_fourcam_external_ipc_validation.sh --hidden-crop-preview --citrus-display-safe"
  echo "Dry-run only; add --execute to control the real display and cameras."
  exit 0
fi

if [[ -e "${CITRUS_SOCKET}" || -e "${ORANGE_SOCKET}" ]]; then
  echo "Refusing to start while a local-control socket already exists:" >&2
  ls -l "${CITRUS_SOCKET}" "${ORANGE_SOCKET}" 2>/dev/null || true
  if [[ -n "${TIMING_CONFIG_DIR}" && -d "${TIMING_CONFIG_DIR}" ]]; then
    rm -rf -- "${TIMING_CONFIG_DIR}"
  fi
  exit 1
fi

rm -f "${RESULT_JSON}" "${RESULT_JSON}.tmp"

citrus_pid=""
cleanup_citrus() {
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
  if [[ -n "${TIMING_CONFIG_DIR}" && -d "${TIMING_CONFIG_DIR}" ]]; then
    rm -rf -- "${TIMING_CONFIG_DIR}"
  fi
}
trap cleanup_citrus EXIT INT TERM

env "${CITRUS_ENV[@]}" "${CITRUS_BIN}" >"${CITRUS_LOG}" 2>&1 &
citrus_pid=$!
echo "Started Citrus pid=${citrus_pid}; waiting for ${CITRUS_SOCKET}"
for _ in $(seq 1 300); do
  [[ -S "${CITRUS_SOCKET}" ]] && break
  if ! kill -0 "${citrus_pid}" 2>/dev/null; then
    echo "Citrus exited before opening its control socket" >&2
    tail -n 80 "${CITRUS_LOG}" >&2 || true
    exit 1
  fi
  sleep 0.1
done
[[ -S "${CITRUS_SOCKET}" ]] || {
  echo "Timed out waiting for Citrus control socket" >&2
  tail -n 80 "${CITRUS_LOG}" >&2 || true
  exit 1
}

echo "Citrus control socket is ready; starting Orange guided GUI capture"
set +e
timeout --signal=INT --kill-after=10s "${TIMEOUT_SECONDS}s" \
  env "${ORANGE_ENV[@]}" \
  "${REPO_ROOT}/scripts/run_gui_fourcam_external_ipc_validation.sh" \
    --hidden-crop-preview \
    --citrus-display-safe >"${ORANGE_LOG}" 2>&1
orange_status=$?
set -e

cleanup_citrus
trap - EXIT INT TERM

if [[ ${orange_status} -ne 0 ]]; then
  echo "Orange guided GUI process failed with status ${orange_status}" >&2
  tail -n 120 "${ORANGE_LOG}" >&2 || true
  tail -n 80 "${CITRUS_LOG}" >&2 || true
  exit "${orange_status}"
fi
if [[ ! -f "${RESULT_JSON}" ]]; then
  echo "Orange exited without writing guided capture result: ${RESULT_JSON}" >&2
  tail -n 120 "${ORANGE_LOG}" >&2 || true
  exit 1
fi

VALIDATOR_ARGS=(
  "${RESULT_JSON}"
  "--expected-cameras" "${CAMERAS}"
  "--expected-recipe" "${RECIPE}"
  "--expected-foreground-gray-u8" "${FOREGROUND_GRAY_U8}"
)
if [[ -n "${RECIPE_SEQUENCE}" ]]; then
  VALIDATOR_ARGS+=(
    "--expected-recipe-sequence" "${RECIPE_SEQUENCE}"
    "--expected-fixture-aperture-shape" "${FIXTURE_APERTURE_SHAPE}"
  )
fi
if [[ -n "${SWEEP_FOREGROUND_GRAYS_U8}" ]]; then
  VALIDATOR_ARGS+=(
    "--expected-sweep-foreground-grays-u8" "${SWEEP_FOREGROUND_GRAYS_U8}"
    "--expected-sweep-repeats" "${SWEEP_REPEATS}"
  )
  if (( INCLUDE_ARENA_OUTLINE_REFERENCE )); then
    VALIDATOR_ARGS+=("--expect-arena-outline-reference")
  fi
fi
if (( APPLY_CALIBRATION_PREFLIGHT )); then
  VALIDATOR_ARGS+=(
    "--require-calibration-preflight"
    "--expected-frame-rate-hz" "${CALIBRATION_FRAME_RATE_HZ}"
    "--expected-exposure-us" "${CALIBRATION_EXPOSURE_US}"
  )
fi
if (( START_AT_CALIBRATION_TIMING )); then
  VALIDATOR_ARGS+=(
    "--require-ptp-gate"
    "--max-ptp-capture-span-ns" "100000"
  )
fi
if [[ -n "${PROFILE}" ]]; then
  VALIDATOR_ARGS+=("--expected-profile" "${PROFILE}")
fi
if (( SAVE )); then
  VALIDATOR_ARGS+=("--require-saved")
fi
if (( FIT_HOMOGRAPHIES )); then
  VALIDATOR_ARGS+=("--expect-persisted-homography-candidate")
fi
"${REPO_ROOT}/scripts/validate_gui_guided_capture_result.py" "${VALIDATOR_ARGS[@]}"
echo "Guided capture result: ${RESULT_JSON}"
echo "Orange log: ${ORANGE_LOG}"
echo "Citrus log: ${CITRUS_LOG}"
