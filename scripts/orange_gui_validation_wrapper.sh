#!/usr/bin/env bash
set -euo pipefail

DEFAULT_ORANGE_ROOT="/home/jeremy/orange-jeremy"
EXPERIMENT_ORANGE_ROOT="/home/jeremy/orange-gop-split-a16"
DEFAULT_ORANGE_BIN="$DEFAULT_ORANGE_ROOT/build/orange"
EXPERIMENT_ORANGE_BIN="$EXPERIMENT_ORANGE_ROOT/targets/release/orange"
ORANGE_BIN="$EXPERIMENT_ORANGE_BIN"
EVT_PROFILE="/etc/profile.d/evt.sh"
DRY_RUN=0
PTP_STACK_MODE="off"
ENV_ITEMS=()

usage() {
  cat <<'EOF'
Usage:
  orange_gui_validation_wrapper.sh [--orange-bin <path>] [--env KEY=VALUE]...

Runs the Orange GUI as root for local display/camera validation.

Options:
  --orange-bin <path>       Use an allowed Orange GUI binary.
  --env KEY=VALUE           Export one whitelisted runtime environment value.
  --ptp-stack-mode <mode>   Host PTP preflight: off, require, or auto.
  --dry-run                 Validate arguments and print the command/env only.
  --help

Allowed Orange binaries:
  /home/jeremy/orange-jeremy/build/orange
  /home/jeremy/orange-gop-split-a16/targets/release/orange

Allowed env values are intentionally limited to the GUI validation launcher
contract: display/session variables, GUI autorun controls such as
ORANGE_GUI_AUTORUN, recording sink and recording-control values such as
ORANGE_GUI_CLIP_SECONDS, bounded camera-startup concurrency, local-control
start/stop gates, crop-recorder controls, YOLO/PTP diagnostics, and known path
roots.

PTP stack modes:
  off      Do not check the host linuxptp stack.
  require  Require ptp4l, phc2sys, and /var/run/ptp4l before launch.
  auto     Start the host PTP stack when the preflight is not healthy.
EOF
}

is_bool() {
  [[ "$1" =~ ^[01]$ ]]
}

is_nonnegative_integer() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_nonnegative_number() {
  [[ "$1" =~ ^([0-9]+([.][0-9]*)?|[.][0-9]+)$ ]]
}

validate_existing_path_under_allowed_roots() {
  local value="$1"
  shift
  local resolved
  resolved="$(realpath -e "$value")" || return 1
  local root
  for root in "$@"; do
    case "$resolved" in
      "$root"|"$root"/*)
        printf '%s\n' "$resolved"
        return 0
        ;;
    esac
  done
  return 1
}

validate_path_under_allowed_roots() {
  local value="$1"
  shift
  local resolved
  resolved="$(realpath -m "$value")" || return 1
  local root
  for root in "$@"; do
    case "$resolved" in
      "$root"|"$root"/*)
        printf '%s\n' "$resolved"
        return 0
        ;;
    esac
  done
  return 1
}

resolve_ptp_stack_script_path() {
  local candidate
  case "$ORANGE_BIN" in
    "$DEFAULT_ORANGE_BIN")
      candidate="$DEFAULT_ORANGE_ROOT/scripts/ptp_stack.sh"
      ;;
    "$EXPERIMENT_ORANGE_BIN")
      candidate="$EXPERIMENT_ORANGE_ROOT/scripts/ptp_stack.sh"
      ;;
    *)
      candidate="$EXPERIMENT_ORANGE_ROOT/scripts/ptp_stack.sh"
      ;;
  esac

  if [[ -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  if [[ -x "$EXPERIMENT_ORANGE_ROOT/scripts/ptp_stack.sh" ]]; then
    printf '%s\n' "$EXPERIMENT_ORANGE_ROOT/scripts/ptp_stack.sh"
    return 0
  fi
  if [[ -x "$DEFAULT_ORANGE_ROOT/scripts/ptp_stack.sh" ]]; then
    printf '%s\n' "$DEFAULT_ORANGE_ROOT/scripts/ptp_stack.sh"
    return 0
  fi
  return 1
}

ptp_status_healthy_from_output() {
  local output="$1"
  [[ "$output" != *"(no ptp4l/phc2sys process)"* ]] || return 1
  [[ "$output" == *"ptp4l"* ]] || return 1
  [[ "$output" == *"phc2sys"* ]] || return 1
  [[ "$output" != *"(socket "*" not found)"* ]] || return 1
  [[ "$output" == *"sending: GET TIME_STATUS_NP"* ]] || return 1
}

ensure_ptp_stack_for_gui() {
  case "$PTP_STACK_MODE" in
    off)
      return 0
      ;;
    require|auto)
      ;;
    *)
      echo "Invalid PTP stack mode: $PTP_STACK_MODE" >&2
      return 2
      ;;
  esac

  local ptp_script
  ptp_script="$(resolve_ptp_stack_script_path)" || {
    echo "Could not find executable scripts/ptp_stack.sh for PTP preflight." >&2
    return 1
  }

  local status_before
  status_before="$("$ptp_script" status 2>&1)"
  if ptp_status_healthy_from_output "$status_before"; then
    echo "[sudo-wrapper] host PTP stack healthy"
    return 0
  fi

  if [[ "$PTP_STACK_MODE" == "require" ]]; then
    echo "[sudo-wrapper] host PTP stack is not healthy and --ptp-stack-mode=require was used" >&2
    printf '%s\n' "$status_before" >&2
    return 1
  fi

  echo "[sudo-wrapper] host PTP stack is not healthy; starting it"
  printf '%s\n' "$status_before"
  "$ptp_script" start

  local status_after
  status_after="$("$ptp_script" status 2>&1)"
  if ptp_status_healthy_from_output "$status_after"; then
    echo "[sudo-wrapper] host PTP stack healthy after start"
    printf '%s\n' "$status_after"
    return 0
  fi

  echo "[sudo-wrapper] host PTP stack is still not healthy after start" >&2
  printf '%s\n' "$status_after" >&2
  return 1
}

validate_env_item() {
  local item="$1"
  local key="${item%%=*}"
  local value="${item#*=}"
  if [[ "$item" != *=* || -z "$key" ]]; then
    echo "--env must be KEY=VALUE: $item" >&2
    return 2
  fi

  case "$key" in
    DISPLAY)
      [[ -z "$value" || "$value" =~ ^(:[0-9]+(\.[0-9]+)?|[A-Za-z0-9_.-]+:[0-9]+(\.[0-9]+)?)$ ]] || {
        echo "Invalid DISPLAY: $value" >&2
        return 2
      }
      ;;
    XAUTHORITY)
      if [[ -n "$value" ]]; then
        value="$(validate_existing_path_under_allowed_roots "$value" \
          "/home/jeremy" \
          "/run/user/1000" \
          "/tmp")" || {
          echo "XAUTHORITY path is outside allowed roots or missing: $value" >&2
          return 2
        }
      fi
      ;;
    WAYLAND_DISPLAY)
      [[ -z "$value" || "$value" =~ ^[A-Za-z0-9_.-]+$ ]] || {
        echo "Invalid WAYLAND_DISPLAY: $value" >&2
        return 2
      }
      ;;
    XDG_RUNTIME_DIR)
      [[ -z "$value" || "$value" == "/run/user/1000" ]] || {
        echo "Invalid XDG_RUNTIME_DIR: $value" >&2
        return 2
      }
      ;;
    XDG_SESSION_TYPE)
      [[ -z "$value" || "$value" =~ ^(x11|wayland|tty)$ ]] || {
        echo "Invalid XDG_SESSION_TYPE: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_RECORDING_SINK_MODE|ORANGE_CROP_RECORDING_SINK_MODE)
      [[ "$value" =~ ^(real|external_ipc|in_process|inprocess|none|disabled|preprocess_only)$ ]] || {
        echo "Invalid $key: $value" >&2
        return 2
      }
      ;;
    ORANGE_DEFAULT_DETECT_ENGINE)
      value="$(validate_existing_path_under_allowed_roots "$value" \
        "/home/jeremy/orange_data/detect" \
        "/home/jeremy/orange-gop-split-a16" \
        "/tmp")" || {
        echo "Detect engine path is outside allowed roots or missing: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_CONFIG_DIR)
      value="$(validate_existing_path_under_allowed_roots "$value" \
        "/home/jeremy/orange_data/config/local" \
        "/tmp")" || {
        echo "GUI config dir is outside allowed roots or missing: $value" >&2
        return 2
      }
      ;;
    ORANGE_APP_CONFIG_PATH|ORANGE_GUI_APP_CONFIG_PATH)
      value="$(validate_existing_path_under_allowed_roots "$value" \
        "/home/jeremy/orange_data/config/app" \
        "/tmp")" || {
        echo "App config path is outside allowed roots or missing: $value" >&2
        return 2
      }
      ;;
    ORANGE_LOCAL_CONTROL_SOCKET|ORANGE_GUI_LOCAL_CONTROL_SOCKET|ORANGE_LOCAL_CONTROL_LOG|ORANGE_GUI_LOCAL_CONTROL_LOG)
      value="$(validate_path_under_allowed_roots "$value" \
        "/tmp" \
        "/run/user/1000" \
        "/home/jeremy/orange_data")" || {
        echo "Local control path is outside allowed roots: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT_PATH)
      value="$(validate_existing_path_under_allowed_roots "$value" \
        "/home/jeremy/orange_data" \
        "/home/jeremy/orange-gop-split-a16" \
        "/tmp")" || {
        echo "External recorder contract path is outside allowed roots or missing: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_CITRUS_CONFIG_PATH)
      value="$(validate_existing_path_under_allowed_roots "$value" \
        "/home/jeremy/citrus" \
        "/tmp")" || {
        echo "Guided-capture Citrus config is outside allowed roots or missing: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_RESULT_JSON)
      value="$(validate_path_under_allowed_roots "$value" \
        "/tmp" \
        "/home/jeremy/orange_data")" || {
        echo "Guided-capture result path is outside allowed roots: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_ARENA_CENTERING_CITRUS_CONFIG_PATH)
      value="$(validate_existing_path_under_allowed_roots "$value" \
        "/home/jeremy/citrus" \
        "/tmp")" || {
        echo "Arena-centering Citrus config is outside allowed roots or missing: $value" >&2
        return 2
      }
      ;;
    ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH)
      value="$(validate_existing_path_under_allowed_roots "$value" \
        "/home/jeremy/citrus" \
        "/tmp")" || {
        echo "Recording Citrus config is outside allowed roots or missing: $value" >&2
        return 2
      }
      ;;
    ORANGE_CITRUS_OBSERVATION_BINDING_SOCKET)
      value="$(validate_path_under_allowed_roots "$value" \
        "/tmp" \
        "/run/user/1000")" || {
        echo "Observation-binding socket is outside allowed roots: $value" >&2
        return 2
      }
      ;;
    ORANGE_CITRUS_OBSERVATION_BINDING_MODE)
      [[ "$value" =~ ^(required|optional|not_applicable)$ ]] || {
        echo "Invalid observation-binding mode: $value" >&2
        return 2
      }
      ;;
    ORANGE_CITRUS_OBSERVATION_BINDING_TIMEOUT_MS)
      is_positive_integer "$value" && (( value >= 50 && value <= 5000 )) || {
        echo "$key must be an integer from 50 through 5000" >&2
        return 2
      }
      ;;
    ORANGE_GUI_ARENA_CENTERING_RESULT_JSON)
      value="$(validate_path_under_allowed_roots "$value" \
        "/tmp" \
        "/home/jeremy/orange_data")" || {
        echo "Arena-centering result path is outside allowed roots: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_RECIPE)
      [[ "$value" =~ ^(black_reference|uniform_gray|arena_outline|experimental_area_center_and_outline|homography_grid|homography_rings|verification_dots)$ ]] || {
        echo "Invalid guided-capture recipe: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_RECIPE_SEQUENCE)
      [[ -z "$value" || "$value" =~ ^(black_reference|uniform_gray|arena_outline|experimental_area_center_and_outline|homography_grid|homography_rings|verification_dots)(,(black_reference|uniform_gray|arena_outline|experimental_area_center_and_outline|homography_grid|homography_rings|verification_dots))*$ ]] || {
        echo "Invalid guided-capture recipe sequence: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_FIXTURE_APERTURE_SHAPE)
      [[ "$value" =~ ^(circle|rectangle|rounded_rectangle|polygon|unknown)$ ]] || {
        echo "Invalid fixture aperture shape: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_PROFILE)
      [[ "$value" =~ ^(unobstructed_canvas_commissioning|holder_installed_projected_surface|wet_tank_projected_surface|installed_tank_registration)$ ]] || {
        echo "Invalid guided-capture workflow profile: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_PURPOSE)
      [[ "$value" =~ ^[A-Za-z0-9_.-]+$ ]] || {
        echo "Invalid guided-capture purpose: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_CAMERAS)
      [[ "$value" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
        echo "Invalid guided-capture camera list: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_ARENA_CENTERING_CAMERAS)
      [[ "$value" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
        echo "Invalid arena-centering camera list: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_OPERATOR_MONITOR|ORANGE_GUI_RESERVED_MONITOR|ORANGE_CITRUS_EXPECTED_STIMULUS_MONITOR)
      [[ "$value" =~ ^[A-Za-z0-9_.-]+$ ]] || {
        echo "Invalid display output name for $key: $value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_FOREGROUND_GRAY_U8)
      is_nonnegative_integer "$value" && (( value <= 255 )) || {
        echo "$key must be an integer from 0 through 255" >&2
        return 2
      }
      ;;
    ORANGE_GUI_ARENA_CENTERING_FOREGROUND_GRAY_U8|ORANGE_GUI_HOMOGRAPHY_SATURATION_PIXEL_THRESHOLD_U8)
      is_nonnegative_integer "$value" && (( value <= 255 )) || {
        echo "$key must be an integer from 0 through 255" >&2
        return 2
      }
      ;;
    ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_PATH)
      [[ "$value" =~ ^/home/jeremy/orange_data/calibrations/commissioning/projector_intensity_[A-Za-z0-9_.-]+/commissioning_report\.json$ ]] &&
        [[ -f "$value" ]] || {
        echo "$key must name an existing projector-intensity commissioning report" >&2
        return 2
      }
      ;;
    ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_SHA256)
      [[ "$value" =~ ^[0-9a-f]{64}$ ]] || {
        echo "$key must be a lowercase SHA-256 value" >&2
        return 2
      }
      ;;
    ORANGE_GUI_GUIDED_CAPTURE_SWEEP_FOREGROUND_GRAYS_U8)
      [[ "$value" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
        echo "$key must be comma-separated integers from 0 through 255" >&2
        return 2
      }
      local gray
      local -a grays
      IFS=',' read -r -a grays <<<"$value"
      for gray in "${grays[@]}"; do
        (( gray <= 255 )) || {
          echo "$key values must be from 0 through 255" >&2
          return 2
        }
      done
      ;;
    ORANGE_YOLO_PERF_LOG|ORANGE_CROP_COPY_TIMING|ORANGE_CROP_STAGE_SOURCE|ORANGE_ANALYTICS_EARLY_OWNED_FRAME|ORANGE_YOLO_DETACH_INPUT|ORANGE_YOLO_READY_EVENT_FASTPATH|ORANGE_RECORDING_DETECT_PRIORITY|ORANGE_GUI_SHOW_SPEED_GRAPHS|ORANGE_GUI_AUTORUN|ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE|ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW|ORANGE_GUI_AUTORUN_ENABLE_STREAM|ORANGE_GUI_AUTORUN_ENABLE_RECORD|ORANGE_GUI_AUTORUN_ENABLE_YOLO|ORANGE_GUI_AUTORUN_ENABLE_CROP|ORANGE_GUI_AUTORUN_START_RECORDING|ORANGE_GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP|ORANGE_GUI_GUIDED_CAPTURE_AUTORUN|ORANGE_GUI_GUIDED_CAPTURE_SAVE|ORANGE_GUI_GUIDED_CAPTURE_EXIT_AFTER_COMPLETION|ORANGE_GUI_GUIDED_CAPTURE_APPLY_CALIBRATION_PREFLIGHT|ORANGE_GUI_GUIDED_CAPTURE_INCLUDE_ARENA_OUTLINE_REFERENCE|ORANGE_GUI_GUIDED_CAPTURE_FIT_HOMOGRAPHIES|ORANGE_GUI_PROJECTED_SURFACE_SCALE_TARGETS_READY|ORANGE_GUI_ACCEPT_PROJECTED_SURFACE_SCALES_ARMED|ORANGE_GUI_ARENA_CENTERING_AUTORUN|ORANGE_GUI_ARENA_CENTERING_APPLY_CALIBRATION_PREFLIGHT|ORANGE_GUI_ARENA_CENTERING_SAVE_CAPTURES|ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_CENTERS_ARMED|ORANGE_GUI_ARENA_CENTERING_RESIZE_ARENAS|ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_LAYOUT_ARMED|ORANGE_GUI_ARENA_CENTERING_FIT_HOMOGRAPHIES|ORANGE_GUI_ARENA_CENTERING_ACCEPT_HOMOGRAPHIES_ARMED|ORANGE_GUI_ARENA_CENTERING_EXIT_AFTER_COMPLETION|ORANGE_GUI_ARENA_CENTERING_REQUIRE_STABILITY_CAPTURE|ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU|ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT|ORANGE_CROP_PREVIEW_DISABLE|ORANGE_SHAMAN_V2_LIVE_STATE|ORANGE_LOCAL_CONTROL_DISABLE|ORANGE_GUI_LOCAL_CONTROL_DISABLE|ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START|ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START|ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP|ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP|ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP|ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP|ORANGE_LOCAL_CONTROL_EXIT_AFTER_FINALIZE|ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE)
      is_bool "$value" || {
        echo "$key must be 0 or 1" >&2
        return 2
      }
      ;;
    ORANGE_YOLO_PERF_SAMPLE)
      is_positive_integer "$value" || {
        echo "$key must be a positive integer" >&2
        return 2
      }
      ;;
    ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY)
      [[ "$value" =~ ^(1|2|4)$ ]] || {
        echo "$key must be 1, 2, or 4" >&2
        return 2
      }
      ;;
    ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS)
      [[ "$value" =~ ^-1$|^[0-9]+$ ]] || {
        echo "$key must be -1 or a non-negative integer" >&2
        return 2
      }
      ;;
    ORANGE_PTP_REGISTER_READ_DECIMATE|ORANGE_GUI_STREAM_DOWNSAMPLE|ORANGE_DISPLAY_PREVIEW_MAX_FPS|ORANGE_GUI_SWAP_INTERVAL|ORANGE_GUI_FRAME_MAX_FPS|ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS|ORANGE_GUI_AUTORUN_RECORD_SECONDS|ORANGE_GUI_GUIDED_CAPTURE_FRAME_COUNT|ORANGE_GUI_GUIDED_CAPTURE_FRAME_RATE_HZ|ORANGE_GUI_GUIDED_CAPTURE_EXPOSURE_US|ORANGE_GUI_GUIDED_CAPTURE_SWEEP_REPEATS|ORANGE_GUI_GUIDED_CAPTURE_STARTUP_TIMEOUT_SECONDS|ORANGE_GUI_GUIDED_CAPTURE_WORKFLOW_TIMEOUT_SECONDS|ORANGE_GUI_ARENA_CENTERING_FRAME_COUNT|ORANGE_GUI_ARENA_CENTERING_FRAME_RATE_HZ|ORANGE_GUI_ARENA_CENTERING_EXPOSURE_US|ORANGE_GUI_ARENA_CENTERING_MAX_PTP_SPAN_NS|ORANGE_GUI_ARENA_CENTERING_PROJECTION_SETTLE_MS|ORANGE_GUI_ARENA_CENTERING_STABILITY_INTERVAL_MS|ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CAPTURE_ATTEMPTS|ORANGE_GUI_GROUP_CAPTURE_POST_PRESENTATION_SETTLE_MS|ORANGE_GUI_RECORD_FOR_SECONDS|ORANGE_GUI_CLIP_SECONDS|ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH|ORANGE_CROP_PREVIEW_MAX_FPS|ORANGE_CROP_FRAME_POOL_SIZE|ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID|ORANGE_YOLO_RT_PRIORITY|ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS|ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS|ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS|ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS)
      is_nonnegative_integer "$value" || {
        echo "$key must be a non-negative integer" >&2
        return 2
      }
      ;;
    ORANGE_GUI_ARENA_CENTERING_PROBE_CANVAS_PX|ORANGE_GUI_ARENA_CENTERING_VERIFICATION_TOLERANCE_CAMERA_PX|ORANGE_GUI_ARENA_CENTERING_MAX_REFINEMENT_CANVAS_PX|ORANGE_GUI_ARENA_CENTERING_RECTANGLE_SAFETY_MARGIN_CAMERA_PX|ORANGE_GUI_ARENA_CENTERING_RECTANGLE_CENTER_TOLERANCE_CAMERA_PX|ORANGE_GUI_ARENA_CENTERING_RECTANGLE_PREDICTION_TOLERANCE_CAMERA_PX|ORANGE_GUI_ARENA_CENTERING_RECTANGLE_MAXIMALITY_SLACK_CAMERA_PX|ORANGE_GUI_ARENA_CENTERING_MAX_ARENA_SCALE_CHANGE_FRACTION|ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CENTER_DELTA_CAMERA_PX|ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CORNER_DELTA_CAMERA_PX|ORANGE_GUI_HOMOGRAPHY_MAXIMUM_RMS_ERROR_CANVAS_PX|ORANGE_GUI_HOMOGRAPHY_MAXIMUM_POINT_ERROR_CANVAS_PX|ORANGE_GUI_HOMOGRAPHY_MINIMUM_INLIER_RATIO|ORANGE_GUI_HOMOGRAPHY_MAXIMUM_HOLDOUT_RMS_ERROR_CANVAS_PX|ORANGE_GUI_HOMOGRAPHY_MAXIMUM_HOLDOUT_ERROR_CANVAS_PX|ORANGE_GUI_HOMOGRAPHY_MAXIMUM_DOT_CORE_SATURATION_FRACTION|ORANGE_GUI_HOMOGRAPHY_MINIMUM_DOT_BACKGROUND_CONTRAST_U8)
      is_nonnegative_number "$value" || {
        echo "$key must be a non-negative number" >&2
        return 2
      }
      ;;
    ORANGE_YOLO_AFFINITY_CAM_*|ORANGE_YOLO_RT_PRIORITY_CAM_*)
      is_nonnegative_integer "$value" || {
        echo "$key must be a non-negative integer" >&2
        return 2
      }
      ;;
    ORANGE_YOLO_SYNC_EVENT|ORANGE_PTP_LATCH_AFTER_FANOUT|ORANGE_HEADLESS_GPU_DMON|ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT|ORANGE_EXTERNAL_RECORDER_DETECT_PRIORITY|ORANGE_EXTERNAL_RECORDER_REGISTERED_SOURCE|ORANGE_POOL_NV12_LAYOUT|ORANGE_YOLO_GPU_TIMING|ORANGE_CROP_EXTERNAL_INTERLEAVE|ORANGE_EXTERNAL_RECORDER_MAX_DEFERRED|ORANGE_EXTERNAL_RECORDER_HARD_MAX_DEFERRED)
      # Detect-latency levers (docs/detect_latency_review_2026_09_03.md).
      [[ "$value" =~ ^(0|1)$ ]] || {
        echo "$key must be 0 or 1" >&2
        return 2
      }
      ;;
    ORANGE_YOLO_RT_POLICY)
      [[ "$value" =~ ^(fifo|rr|round_robin|sched_fifo|sched_rr)$ ]] || {
        echo "$key must be fifo, rr, round_robin, sched_fifo, or sched_rr" >&2
        return 2
      }
      ;;
    ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*)
      is_nonnegative_integer "$value" || {
        echo "$key must be a non-negative integer" >&2
        return 2
      }
      ;;
    *)
      echo "Refusing unsupported env key: $key" >&2
      return 2
      ;;
  esac

  ENV_ITEMS+=("$key=$value")
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --orange-bin)
      shift
      [[ $# -gt 0 ]] || { echo "--orange-bin requires a value." >&2; exit 2; }
      ORANGE_BIN="$(realpath -e "$1")"
      case "$ORANGE_BIN" in
        "$DEFAULT_ORANGE_BIN"|"$EXPERIMENT_ORANGE_BIN")
          ;;
        *)
          echo "Refusing to use Orange GUI binary outside allowed paths: $ORANGE_BIN" >&2
          exit 2
          ;;
      esac
      shift
      ;;
    --env)
      shift
      [[ $# -gt 0 ]] || { echo "--env requires KEY=VALUE." >&2; exit 2; }
      validate_env_item "$1"
      shift
      ;;
    --ptp-stack-mode)
      shift
      [[ $# -gt 0 ]] || { echo "--ptp-stack-mode requires a value." >&2; exit 2; }
      PTP_STACK_MODE="$1"
      case "$PTP_STACK_MODE" in
        off|require|auto)
          ;;
        *)
          echo "--ptp-stack-mode must be off, require, or auto" >&2
          exit 2
          ;;
      esac
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ ! -x "$ORANGE_BIN" ]]; then
  echo "Orange GUI binary not found or not executable at $ORANGE_BIN" >&2
  exit 1
fi

if [[ "$DRY_RUN" != "1" && "${EUID}" -ne 0 ]]; then
  echo "This wrapper must be run as root (typically via sudo -n)." >&2
  exit 1
fi

if [[ -f "$EVT_PROFILE" ]]; then
  # shellcheck disable=SC1090
  source "$EVT_PROFILE"
fi
export EMERGENT_DIR="${EMERGENT_DIR:-/opt/EVT}"
export RIVERMAX_LOG_LEVEL="${RIVERMAX_LOG_LEVEL:-6}"
export VMA_TRACELEVEL="${VMA_TRACELEVEL:-0}"

for item in "${ENV_ITEMS[@]}"; do
  export "$item"
done

echo "[sudo-wrapper] running Orange GUI validation"
echo "[sudo-wrapper] orange_bin=$ORANGE_BIN"
echo "[sudo-wrapper] ptp_stack_mode=$PTP_STACK_MODE"
for item in "${ENV_ITEMS[@]}"; do
  echo "[sudo-wrapper] env $item"
done

if [[ "$DRY_RUN" == "1" ]]; then
  exit 0
fi

ensure_ptp_stack_for_gui

orange_pid=""
parent_guard_pid=""
relay_gui_signal() {
  local signal_name="$1"
  if [[ -n "${orange_pid}" ]]; then
    kill "-${signal_name}" "${orange_pid}" 2>/dev/null || true
  fi
}
stop_parent_guard() {
  if [[ -n "${parent_guard_pid}" ]]; then
    kill "${parent_guard_pid}" 2>/dev/null || true
    wait "${parent_guard_pid}" 2>/dev/null || true
    parent_guard_pid=""
  fi
}
trap 'relay_gui_signal TERM' TERM
trap 'relay_gui_signal INT' INT
trap 'relay_gui_signal HUP' HUP
trap 'stop_parent_guard' EXIT

set +e
# The orchestrator launches this privileged wrapper as a background process.
# Bash/background ancestry may leave SIGINT/SIGTERM ignored in the child; an
# exec preserves ignored dispositions, so merely relaying those signals from
# the wrapper is insufficient.  Reset the GUI child's shutdown signals to
# their defaults explicitly.  The wrapper retains its own traps below and
# relays cancellation to the child PID.
env \
  --default-signal=INT \
  --default-signal=TERM \
  --default-signal=HUP \
  "$ORANGE_BIN" &
orange_pid=$!
wrapper_pid=$$
launcher_parent_pid="$(awk '{print $4}' "/proc/${wrapper_pid}/stat" 2>/dev/null || true)"
if [[ -n "${launcher_parent_pid}" ]]; then
  (
    while kill -0 "${orange_pid}" 2>/dev/null; do
      current_parent_pid="$(awk '{print $4}' "/proc/${wrapper_pid}/stat" 2>/dev/null || true)"
      if [[ -z "${current_parent_pid}" || "${current_parent_pid}" != "${launcher_parent_pid}" ]]; then
        echo "[sudo-wrapper] launcher parent exited; terminating Orange child" >&2
        kill -TERM "${orange_pid}" 2>/dev/null || true
        for _ in $(seq 1 20); do
          kill -0 "${orange_pid}" 2>/dev/null || exit 0
          sleep 0.25
        done
        echo "[sudo-wrapper] Orange child did not exit after TERM; sending KILL" >&2
        kill -KILL "${orange_pid}" 2>/dev/null || true
        exit 0
      fi
      sleep 0.25
    done
  ) &
  parent_guard_pid=$!
fi
while true; do
  wait "${orange_pid}"
  orange_status=$?
  if ! kill -0 "${orange_pid}" 2>/dev/null; then
    break
  fi
done
stop_parent_guard
orange_pid=""
set -e
trap - TERM INT HUP
trap - EXIT

# Orange creates this socket as root under the privileged GUI launcher.  Once
# the child has exited, the unprivileged orchestrator cannot remove a stale
# inode itself.  Remove only the validated local-control path and only when it
# is still a Unix socket; never unlink a regular file supplied through the
# environment.
orange_control_socket="${ORANGE_GUI_LOCAL_CONTROL_SOCKET:-${ORANGE_LOCAL_CONTROL_SOCKET:-}}"
if [[ -n "${orange_control_socket}" && -S "${orange_control_socket}" ]]; then
  rm -f -- "${orange_control_socket}"
fi

# Calibration outputs are the only non-recording products written by this
# wrapper. Repair ownership narrowly, using only a validated result path and
# calibration-session paths emitted by Orange itself. This mirrors the
# recording wrapper's SUDO_UID/SUDO_GID handoff without accepting an arbitrary
# tree.
calibration_result_json="${ORANGE_GUI_ARENA_CENTERING_RESULT_JSON:-${ORANGE_GUI_GUIDED_CAPTURE_RESULT_JSON:-}}"
if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" &&
      -n "${calibration_result_json}" ]]; then
  result_path="$(realpath -e "${calibration_result_json}" 2>/dev/null || true)"
  case "$result_path" in
    /tmp/*|/home/jeremy/orange_data/*)
      chown -- "${SUDO_UID}:${SUDO_GID}" "$result_path"
      ;;
  esac
  if [[ -n "$result_path" ]]; then
    mapfile -t calibration_session_dirs < <(python3 - "$result_path" <<'PY'
import json
import sys
from pathlib import Path

try:
    payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError):
    raise SystemExit(0)

paths = set()
pending = [payload]
while pending:
    node = pending.pop()
    if isinstance(node, dict):
        session_dir = node.get("session_dir")
        if session_dir:
            paths.add(str(session_dir))
        pending.extend(node.values())
    elif isinstance(node, list):
        pending.extend(node)
for path in sorted(paths):
    print(path)
PY
)
    for session_dir in "${calibration_session_dirs[@]}"; do
      resolved_session="$(realpath -e "$session_dir" 2>/dev/null || true)"
      case "$resolved_session" in
        /home/jeremy/orange_data/calibrations/sessions/*)
          chown -R -- "${SUDO_UID}:${SUDO_GID}" "$resolved_session"
          ;;
      esac
    done
  fi
fi

exit "$orange_status"
