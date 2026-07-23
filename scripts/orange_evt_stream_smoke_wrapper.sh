#!/usr/bin/env bash
set -euo pipefail

EXPERIMENT_ORANGE_ROOT="/home/jeremy/orange-gop-split-a16"
EVT_STREAM_SMOKE_BIN="$EXPERIMENT_ORANGE_ROOT/targets/release/evt_stream_smoke"
EVT_PROFILE="/etc/profile.d/evt.sh"
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage:
  orange_evt_stream_smoke_wrapper.sh [options]

Runs the Orange EVT stream-smoke diagnostic with the narrow root privileges
needed by the Emergent/NIC stack. When installed with the sudoers rule, this
command can be invoked as a normal user; it re-execs itself through sudo -n.

Allowed diagnostic binary:
  /home/jeremy/orange-gop-split-a16/targets/release/evt_stream_smoke

Options:
  --config-dir <dir>       Camera config directory. Defaults to ORANGE_GUI_CONFIG_DIR before sudo.
  --serial <serial>        Probe one camera serial. May be repeated.
  --serials <csv>          Probe comma-separated serials. Defaults to ORANGE_GUI_EXPECT_CAMERAS before sudo.
  --all                    Probe every discovered camera with a usable config/default.
  --list-only              List discovered cameras and config match state only.
  --frames <n>             After stream open, acquire n frames before close.
  --measure-seconds <s>    Timed raw acquisition FPS measurement after stream open.
  --buffer-count <n>       EVT frame buffers when grabbing frames.
  --timeout-ms <ms>        Frame wait timeout when grabbing frames.
  --frame-rate <fps>       Diagnostic override for configured FrameRate.
  --gpu-direct <0|1>       Diagnostic override for configured GPUDirect.
  --frame-stats            Print Mono8 brightness statistics for explicitly acquired frames.
  --dry-run                Validate wrapper arguments and print the command.
  --help                   Show this message.

Allowed config roots:
  /home/jeremy/orange_data/config/local
  /home/jeremy/orange-gop-split-a16/config/local
  /tmp
EOF
}

arg_present() {
  local needle="$1"
  shift
  local arg
  for arg in "$@"; do
    [[ "$arg" == "$needle" ]] && return 0
  done
  return 1
}

selection_arg_present() {
  local arg
  for arg in "$@"; do
    case "$arg" in
      --serial|--serials|--all)
        return 0
        ;;
    esac
  done
  return 1
}

is_nonnegative_integer() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_nonnegative_decimal() {
  [[ "$1" =~ ^([0-9]+([.][0-9]+)?|[.][0-9]+)$ ]]
}

decimal_leq() {
  awk -v v="$1" -v max="$2" 'BEGIN { exit !(v <= max) }'
}

validate_serial() {
  [[ "$1" =~ ^(Cam|cam)?[A-Za-z0-9_.-]+$ ]]
}

validate_serials_csv() {
  [[ "$1" =~ ^(Cam|cam)?[A-Za-z0-9_.-]+(,(Cam|cam)?[A-Za-z0-9_.-]+)*$ ]]
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

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

SELF="$(realpath -e "$0" 2>/dev/null || printf '%s\n' "$0")"
if [[ "${EUID}" -ne 0 ]]; then
  ROOT_ARGS=()
  if ! arg_present "--config-dir" "$@" && [[ -n "${ORANGE_GUI_CONFIG_DIR:-}" ]]; then
    ROOT_ARGS+=("--config-dir" "$ORANGE_GUI_CONFIG_DIR")
  fi
  if ! selection_arg_present "$@" && [[ -n "${ORANGE_GUI_EXPECT_CAMERAS:-}" ]]; then
    ROOT_ARGS+=("--serials" "$ORANGE_GUI_EXPECT_CAMERAS")
  fi
  ROOT_ARGS+=("$@")

  if arg_present "--dry-run" "$@"; then
    set -- "${ROOT_ARGS[@]}"
  else
    # Do not use `sudo -v` here. A deliberately narrow NOPASSWD rule authorizes
    # this exact wrapper command but does not authorize a generic credential
    # refresh, so `sudo -n -v` incorrectly fails even when execution is allowed.
    exec sudo -n "$SELF" "${ROOT_ARGS[@]}"
  fi
fi

if [[ -f "$EVT_PROFILE" ]]; then
  # shellcheck disable=SC1090
  source "$EVT_PROFILE"
fi
export EMERGENT_DIR="${EMERGENT_DIR:-/opt/EVT}"
export RIVERMAX_LOG_LEVEL="${RIVERMAX_LOG_LEVEL:-6}"
export VMA_TRACELEVEL="${VMA_TRACELEVEL:-0}"

CMD=("$EVT_STREAM_SMOKE_BIN")

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config-dir)
      shift
      [[ $# -gt 0 ]] || { echo "--config-dir requires a value." >&2; exit 2; }
      config_dir="$(validate_existing_path_under_allowed_roots "$1" \
        "/home/jeremy/orange_data/config/local" \
        "$EXPERIMENT_ORANGE_ROOT/config/local" \
        "/tmp")" || {
        echo "Config dir is outside allowed roots or missing: $1" >&2
        exit 2
      }
      CMD+=("--config-dir" "$config_dir")
      shift
      ;;
    --serial)
      shift
      [[ $# -gt 0 ]] || { echo "--serial requires a value." >&2; exit 2; }
      validate_serial "$1" || { echo "Invalid serial: $1" >&2; exit 2; }
      CMD+=("--serial" "$1")
      shift
      ;;
    --serials)
      shift
      [[ $# -gt 0 ]] || { echo "--serials requires a value." >&2; exit 2; }
      validate_serials_csv "$1" || { echo "Invalid serial CSV: $1" >&2; exit 2; }
      CMD+=("--serials" "$1")
      shift
      ;;
    --all|--list-only|--frame-stats)
      CMD+=("$1")
      shift
      ;;
    --frames)
      shift
      [[ $# -gt 0 ]] || { echo "--frames requires a value." >&2; exit 2; }
      is_nonnegative_integer "$1" || { echo "--frames must be a non-negative integer." >&2; exit 2; }
      (( 10#$1 <= 1000000 )) || { echo "--frames is capped at 1000000." >&2; exit 2; }
      CMD+=("--frames" "$1")
      shift
      ;;
    --measure-seconds)
      shift
      [[ $# -gt 0 ]] || { echo "--measure-seconds requires a value." >&2; exit 2; }
      is_nonnegative_decimal "$1" || { echo "--measure-seconds must be non-negative." >&2; exit 2; }
      decimal_leq "$1" 120 || { echo "--measure-seconds is capped at 120." >&2; exit 2; }
      CMD+=("--measure-seconds" "$1")
      shift
      ;;
    --buffer-count)
      shift
      [[ $# -gt 0 ]] || { echo "--buffer-count requires a value." >&2; exit 2; }
      is_positive_integer "$1" || { echo "--buffer-count must be a positive integer." >&2; exit 2; }
      (( 10#$1 <= 4096 )) || { echo "--buffer-count is capped at 4096." >&2; exit 2; }
      CMD+=("--buffer-count" "$1")
      shift
      ;;
    --timeout-ms)
      shift
      [[ $# -gt 0 ]] || { echo "--timeout-ms requires a value." >&2; exit 2; }
      is_positive_integer "$1" || { echo "--timeout-ms must be a positive integer." >&2; exit 2; }
      (( 10#$1 <= 60000 )) || { echo "--timeout-ms is capped at 60000." >&2; exit 2; }
      CMD+=("--timeout-ms" "$1")
      shift
      ;;
    --frame-rate)
      shift
      [[ $# -gt 0 ]] || { echo "--frame-rate requires a value." >&2; exit 2; }
      is_positive_integer "$1" || { echo "--frame-rate must be a positive integer." >&2; exit 2; }
      (( 10#$1 <= 1000000 )) || { echo "--frame-rate is capped at 1000000." >&2; exit 2; }
      CMD+=("--frame-rate" "$1")
      shift
      ;;
    --gpu-direct)
      shift
      [[ $# -gt 0 ]] || { echo "--gpu-direct requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[01]$ ]] || { echo "--gpu-direct must be 0 or 1." >&2; exit 2; }
      CMD+=("--gpu-direct" "$1")
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

if [[ "$DRY_RUN" == "1" ]]; then
  printf '[evt-stream-smoke-wrapper] command:'
  printf ' %q' "${CMD[@]}"
  printf '\n'
  exit 0
fi

if [[ ! -x "$EVT_STREAM_SMOKE_BIN" ]]; then
  echo "evt_stream_smoke binary not found or not executable: $EVT_STREAM_SMOKE_BIN" >&2
  echo "Build it with: cmake --build $EXPERIMENT_ORANGE_ROOT/targets/release --target evt_stream_smoke -j 8" >&2
  exit 1
fi

exec "${CMD[@]}"
