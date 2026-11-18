#!/usr/bin/env bash
set -euo pipefail
# shellcheck source=orange_wrapper_install_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/orange_wrapper_install_lib.sh"

usage() {
  cat <<'EOF'
Usage:
  install_orange_gdb_headless_bt_wrapper.sh [options]

Installs the passwordless gdb backtrace wrapper for the headless orange_client.
Installs two commands from scripts/orange_gdb_headless_bt_wrapper.sh; the installed name sets the mode:
  /usr/local/bin/orange-gdb-headless-bt      installed releases only (default /opt/orange/current)
  /usr/local/bin/orange-dev-gdb-headless-bt  the development tree only (/home/jeremy/orange)

Options:
EOF
  orange_wrapper_install_options_help
  cat <<'EOF'
  --help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
  esac
  orange_wrapper_install_option "$@" || exit 2
  if (( ORANGE_WRAPPER_ARGS_CONSUMED > 0 )); then
    shift "$ORANGE_WRAPPER_ARGS_CONSUMED"
    continue
  fi
  echo "Unsupported argument: $1" >&2
  usage >&2
  exit 2
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE="$REPO_ROOT/scripts/orange_gdb_headless_bt_wrapper.sh"
RELEASE_TARGET="$ORANGE_WRAPPER_BIN_DIR/orange-gdb-headless-bt"
DEV_TARGET="$ORANGE_WRAPPER_BIN_DIR/orange-dev-gdb-headless-bt"

orange_wrapper_install install-gdb-wrapper "$SOURCE" gdb-headless-bt

for target in "$RELEASE_TARGET" "$DEV_TARGET"; do
  orange_wrapper_require_help "$target" "gdb" "--dry-run" "/home/jeremy/orange/experiment_specs"
done

echo "[install-gdb-wrapper] installed and verified"
echo "[install-gdb-wrapper] try:"
echo "  sudo -n $RELEASE_TARGET --help"
echo "  sudo -n $DEV_TARGET --help"
