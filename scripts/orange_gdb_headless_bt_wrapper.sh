#!/usr/bin/env bash
# Passwordless-sudo wrapper: run the headless orange_client under gdb and print
# every thread's backtrace. Installed by scripts/install_orange_gdb_headless_bt_wrapper.sh
# (Jeremy runs it with sudo) as two commands, one per mode (see below):
#   /usr/local/bin/orange-gdb-headless-bt      the current installed release
#   /usr/local/bin/orange-dev-gdb-headless-bt  the development tree
# with sudoers entries /etc/sudoers.d/orange-gdb-headless-bt and
# orange-dev-gdb-headless-bt. Brought into the repo on 2026-11-14 from the
# hand-installed copy (2026-05-08) when the tree moved to /home/jeremy/orange.
#
# Usage: orange-[dev-]gdb-headless-bt [--dry-run] <experiment-spec.json>
#   spec under /home/jeremy/orange/experiment_specs or /tmp
set -euo pipefail
# Python started as root here (validators, the recorder verifier, the stall
# monitor) must not write __pycache__ into a root-owned /opt release (seen on
# the first release GUI run, 2026-11-18).
export PYTHONDONTWRITEBYTECODE=1

ORANGE_ROOT="/home/jeremy/orange"
DEV_CLIENT="$ORANGE_ROOT/targets/release/orange_client"

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

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  sed -n 2,12p "$0" | sed 's/^# \{0,1\}//'
  exit 0
fi
DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=1
  shift
fi
SPEC="$(realpath "${1:?spec path required}")"

case "$SPEC" in
  "$ORANGE_ROOT"/experiment_specs/*.json|/tmp/*.json) ;;
  *) echo "spec path not allowed: $SPEC" >&2; exit 2 ;;
esac

resolve_wrapper_binary orange_client "" "$DEV_CLIENT" || exit 2
CLIENT="$WRAPPER_BINARY"
echo "[gdb-wrapper] orange_client=$CLIENT release=${ORANGE_RELEASE_DIR:-none (development tree)}"

if [[ -f /etc/profile.d/evt.sh ]]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/evt.sh
fi
export EMERGENT_DIR="${EMERGENT_DIR:-/opt/EVT}"
export RIVERMAX_LOG_LEVEL="${RIVERMAX_LOG_LEVEL:-6}"
export VMA_TRACELEVEL="${VMA_TRACELEVEL:-0}"

CMD=(/usr/bin/gdb -batch
  -ex "set pagination off"
  -ex "run"
  -ex "thread apply all bt"
  --args "$CLIENT" --mode local --experiment-spec "$SPEC")
if [[ "$DRY_RUN" == "1" ]]; then
  printf '[dry-run] '; printf '%q ' "${CMD[@]}"; printf '\n'
  exit 0
fi
exec "${CMD[@]}"
