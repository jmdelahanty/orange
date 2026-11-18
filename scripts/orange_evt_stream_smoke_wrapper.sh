#!/usr/bin/env bash
set -euo pipefail

EXPERIMENT_ORANGE_ROOT="/home/jeremy/orange"
DEV_EVT_STREAM_SMOKE_BIN="$EXPERIMENT_ORANGE_ROOT/targets/release/evt_stream_smoke"
EVT_STREAM_SMOKE_BIN=""  # --evt-stream-smoke-bin; empty: the mode's default (resolve_wrapper_binary)
EVT_PROFILE="/etc/profile.d/evt.sh"
DRY_RUN=0

# --- wrapper mode (identical in every Orange sudo wrapper; release_install_tests checks) ---
# Installed releases (docs/opt_install_plan_2026-11-14.md, step e): a binary
# under /opt/orange/releases/<tag>/bin is accepted only when every folder from
# /opt down to it, and the binary itself, is root-owned and not writable by
# group or others, and the release holds INSTALLED.json.
ORANGE_RELEASES_ROOT="/opt/orange/releases"
ORANGE_RELEASE_DIR=""
installed_release_dir_for_bin() {
  local bin="$1" name="$2" rel tag p perms
  case "$bin" in
    "$ORANGE_RELEASES_ROOT"/*/bin/"$name") ;;
    *) return 1 ;;
  esac
  rel="${bin%/bin/"$name"}"
  tag="${rel##*/}"
  [[ "$tag" =~ ^orange-v[0-9]{4}\.[0-9]{2}\.[0-9]{2}(\.[0-9]+)?$ ]] || return 1
  [[ "$rel" == "$ORANGE_RELEASES_ROOT/$tag" && -f "$rel/INSTALLED.json" ]] || return 1
  for p in /opt /opt/orange "$ORANGE_RELEASES_ROOT" "$rel" "$rel/bin" "$bin" \
           "$rel/share" "$rel/share/orange" "$rel/share/orange/scripts"; do
    [[ -e "$p" && ! -L "$p" ]] || return 1
    [[ "$(stat -c '%u' "$p")" == 0 ]] || return 1
    perms="$(stat -c '%a' "$p")"
    (( (8#$perms & 8#022) == 0 )) || return 1
  done
  printf '%s\n' "$rel"
}

# Wrapper mode (P3): the installed name decides, so each sudoers rule names
# exactly one mode.
#   orange-dev-<name>  development-tree binaries only (jeremy's sudoers rule);
#   any other name     installed releases only, by default
#                      /opt/orange/current/bin/<binary> (resolved once, so a
#                      run stays on its release if current flips).
ORANGE_CURRENT_BIN_DIR="/opt/orange/current/bin"
case "$(basename -- "$0")" in
  orange-dev-*) ORANGE_WRAPPER_MODE="dev" ;;
  *) ORANGE_WRAPPER_MODE="release" ;;
esac
WRAPPER_BINARY=""

# resolve_wrapper_binary <binary name> <requested path, or empty> <dev path>...
# Called once, after argument parsing. Sets WRAPPER_BINARY (physical path) and
# ORANGE_RELEASE_DIR, or says why this mode refuses the binary and returns 1.
resolve_wrapper_binary() {
  local name="$1" requested="$2" candidate dev_path
  shift 2
  if [[ "$ORANGE_WRAPPER_MODE" == "release" ]]; then
    candidate="${requested:-$ORANGE_CURRENT_BIN_DIR/$name}"
    if ! candidate="$(realpath -e -- "$candidate" 2>/dev/null)"; then
      echo "No installed $name at ${requested:-$ORANGE_CURRENT_BIN_DIR/$name}" >&2
      return 1
    fi
    if ! ORANGE_RELEASE_DIR="$(installed_release_dir_for_bin "$candidate" "$name")"; then
      ORANGE_RELEASE_DIR=""
      echo "Refusing $name: this wrapper runs only root-owned, read-only installed releases under $ORANGE_RELEASES_ROOT (development builds: the orange-dev-* wrapper): $candidate" >&2
      return 1
    fi
    WRAPPER_BINARY="$candidate"
    return 0
  fi
  candidate="${requested:-$1}"
  # A dev path that is not built yet stays as given (a dry run still works;
  # the run itself refuses a missing binary).
  candidate="$(realpath -e -- "$candidate" 2>/dev/null || printf '%s' "$candidate")"
  for dev_path in "$@"; do
    if [[ "$candidate" == "$dev_path" ]]; then
      ORANGE_RELEASE_DIR=""
      WRAPPER_BINARY="$candidate"
      return 0
    fi
  done
  echo "Refusing $name: this development wrapper runs only $(printf '%s\n' "$@" | sort -u | paste -sd ' ') (installed releases: the orange-* wrapper without dev-): $candidate" >&2
  return 1
}
# --- end wrapper mode ---

usage() {
  cat <<'EOF'
Usage:
  orange_evt_stream_smoke_wrapper.sh [options]

Runs the Orange EVT stream-smoke diagnostic with the narrow root privileges
needed by the Emergent/NIC stack. When installed with the sudoers rule, this
command can be invoked as a normal user; it re-execs itself through sudo -n.

Modes (by installed name):
  orange-evt-stream-smoke      installed releases only: a root-owned, read-only
                               /opt/orange/releases/<tag>/bin/evt_stream_smoke;
                               default /opt/orange/current/bin/evt_stream_smoke
  orange-dev-evt-stream-smoke  the development tree only:
                               /home/jeremy/orange/targets/release/evt_stream_smoke

Options:
  --evt-stream-smoke-bin <path>  evt_stream_smoke to run (default: the mode's).
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
  /home/jeremy/orange/config/local
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
    --evt-stream-smoke-bin)
      shift
      [[ $# -gt 0 ]] || { echo "--evt-stream-smoke-bin requires a value." >&2; exit 2; }
      EVT_STREAM_SMOKE_BIN="$1"
      shift
      ;;
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

resolve_wrapper_binary evt_stream_smoke "$EVT_STREAM_SMOKE_BIN" "$DEV_EVT_STREAM_SMOKE_BIN" || exit 2
EVT_STREAM_SMOKE_BIN="$WRAPPER_BINARY"
CMD[0]="$EVT_STREAM_SMOKE_BIN"

if [[ "$DRY_RUN" == "1" ]]; then
  printf '[evt-stream-smoke-wrapper] command:'
  printf ' %q' "${CMD[@]}"
  printf '\n'
  exit 0
fi

if [[ ! -x "$EVT_STREAM_SMOKE_BIN" ]]; then
  echo "evt_stream_smoke binary not found or not executable: $EVT_STREAM_SMOKE_BIN" >&2
  [[ -n "$ORANGE_RELEASE_DIR" ]] || echo "Build it with: cmake --build $EXPERIMENT_ORANGE_ROOT/targets/release --target evt_stream_smoke -j 8" >&2
  exit 1
fi

exec "${CMD[@]}"
