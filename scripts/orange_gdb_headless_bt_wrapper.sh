#!/usr/bin/env bash
# Passwordless-sudo wrapper: run the headless orange_client under gdb and print
# every thread's backtrace. Installed (by Jeremy) as
# /usr/local/bin/orange-gdb-headless-bt:
#   sudo install -o root -g root -m 0755 scripts/orange_gdb_headless_bt_wrapper.sh /usr/local/bin/orange-gdb-headless-bt
# The sudoers entry is /etc/sudoers.d/orange-gdb-headless-bt. Brought into the
# repo on 2026-11-14 from the hand-installed copy (2026-05-08) when the tree
# moved to /home/jeremy/orange.
set -euo pipefail

ORANGE_ROOT="/home/jeremy/orange"
CLIENT="$ORANGE_ROOT/targets/release/orange_client"
SPEC="$(realpath "${1:?spec path required}")"

case "$SPEC" in
  "$ORANGE_ROOT"/experiment_specs/*.json|/tmp/*.json) ;;
  *) echo "spec path not allowed: $SPEC" >&2; exit 2 ;;
esac

if [[ -f /etc/profile.d/evt.sh ]]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/evt.sh
fi
export EMERGENT_DIR="${EMERGENT_DIR:-/opt/EVT}"
export RIVERMAX_LOG_LEVEL="${RIVERMAX_LOG_LEVEL:-6}"
export VMA_TRACELEVEL="${VMA_TRACELEVEL:-0}"

exec /usr/bin/gdb -batch \
  -ex "set pagination off" \
  -ex "run" \
  -ex "thread apply all bt" \
  --args "$CLIENT" --mode local --experiment-spec "$SPEC"
