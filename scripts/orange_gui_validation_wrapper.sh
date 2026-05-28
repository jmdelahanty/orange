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
ORANGE_GUI_CLIP_SECONDS, crop-recorder controls, YOLO/PTP diagnostics, and
known path roots.

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
      [[ "$value" =~ ^(real|external_ipc|in_process|none|disabled|preprocess_only)$ ]] || {
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
    ORANGE_YOLO_PERF_LOG|ORANGE_CROP_COPY_TIMING|ORANGE_CROP_STAGE_SOURCE|ORANGE_ANALYTICS_EARLY_OWNED_FRAME|ORANGE_YOLO_DETACH_INPUT|ORANGE_YOLO_READY_EVENT_FASTPATH|ORANGE_RECORDING_DETECT_PRIORITY|ORANGE_GUI_SHOW_SPEED_GRAPHS|ORANGE_GUI_AUTORUN|ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE|ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW|ORANGE_GUI_AUTORUN_ENABLE_STREAM|ORANGE_GUI_AUTORUN_ENABLE_RECORD|ORANGE_GUI_AUTORUN_ENABLE_YOLO|ORANGE_GUI_AUTORUN_ENABLE_CROP|ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU|ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT|ORANGE_CROP_PREVIEW_DISABLE|ORANGE_LOCAL_CONTROL_DISABLE|ORANGE_GUI_LOCAL_CONTROL_DISABLE)
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
    ORANGE_PTP_REGISTER_READ_DECIMATE|ORANGE_GUI_STREAM_DOWNSAMPLE|ORANGE_DISPLAY_PREVIEW_MAX_FPS|ORANGE_GUI_SWAP_INTERVAL|ORANGE_GUI_FRAME_MAX_FPS|ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS|ORANGE_GUI_AUTORUN_RECORD_SECONDS|ORANGE_GUI_RECORD_FOR_SECONDS|ORANGE_GUI_CLIP_SECONDS|ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH|ORANGE_CROP_PREVIEW_MAX_FPS|ORANGE_CROP_FRAME_POOL_SIZE|ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID|ORANGE_YOLO_RT_PRIORITY)
      is_nonnegative_integer "$value" || {
        echo "$key must be a non-negative integer" >&2
        return 2
      }
      ;;
    ORANGE_YOLO_AFFINITY_CAM_*|ORANGE_YOLO_RT_PRIORITY_CAM_*)
      is_nonnegative_integer "$value" || {
        echo "$key must be a non-negative integer" >&2
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

exec "$ORANGE_BIN"
