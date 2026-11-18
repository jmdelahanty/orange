#!/usr/bin/env bash
# Installs the operator commands (docs/opt_install_plan_2026-11-14.md, 4f):
#   /usr/local/bin/orange-gui       the installed release's GUI launcher
#   /usr/local/bin/orange-headless  the installed release's headless spec runner
# Neither needs a sudoers entry: each runs as the user and the launcher calls
# the release's sudo wrapper (orange-gui-validation, orange-local-benchmark).
#   sudo scripts/install_orange_entry_points.sh [--bin-dir <dir>]
set -euo pipefail
# shellcheck source=orange_wrapper_install_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/orange_wrapper_install_lib.sh"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h) sed -n 2,7p "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  esac
  orange_wrapper_install_option "$@" || exit 2
  if (( ORANGE_WRAPPER_ARGS_CONSUMED > 0 )); then
    shift "$ORANGE_WRAPPER_ARGS_CONSUMED"
    continue
  fi
  echo "Unsupported argument: $1" >&2
  exit 2
done
if [[ "$ORANGE_WRAPPER_NO_ROOT" == "1" && "$ORANGE_WRAPPER_BIN_DIR" == "/usr/local/bin" ]]; then
  echo "--no-root needs a test --bin-dir" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for kind in gui headless; do
  source="$SCRIPT_DIR/orange_${kind}_entry.sh"
  target="$ORANGE_WRAPPER_BIN_DIR/orange-$kind"
  bash -n "$source"
  if [[ "$ORANGE_WRAPPER_NO_ROOT" == "1" ]]; then
    install -m 0755 "$source" "$target"
  else
    orange_wrapper_as_root install -o root -g root -m 0755 "$source" "$target"
  fi
  echo "[install-entry-points] $target -> $source"
done
