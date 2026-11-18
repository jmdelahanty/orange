#!/usr/bin/env bash
set -euo pipefail
# shellcheck source=orange_wrapper_install_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/orange_wrapper_install_lib.sh"

usage() {
  cat <<'EOF'
Usage:
  install_orange_evt_stream_smoke_wrapper.sh [options]

Installs this repo's narrow EVT stream-smoke wrapper. With --install-sudoers,
the installed commands can be run as a normal user without typing sudo.
Installs two commands from scripts/orange_evt_stream_smoke_wrapper.sh; the installed name sets the mode:
  /usr/local/bin/orange-evt-stream-smoke      installed releases only (default /opt/orange/current)
  /usr/local/bin/orange-dev-evt-stream-smoke  the development tree only (/home/jeremy/orange)

Options:
EOF
  orange_wrapper_install_options_help
  cat <<'EOF'
  --help

After installation:
  orange-evt-stream-smoke --help
  orange-dev-evt-stream-smoke --config-dir /home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam --all --frames 5
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
SOURCE="$REPO_ROOT/scripts/orange_evt_stream_smoke_wrapper.sh"
RELEASE_TARGET="$ORANGE_WRAPPER_BIN_DIR/orange-evt-stream-smoke"
DEV_TARGET="$ORANGE_WRAPPER_BIN_DIR/orange-dev-evt-stream-smoke"

orange_wrapper_install install-evt-stream-smoke-wrapper "$SOURCE" evt-stream-smoke

for target in "$RELEASE_TARGET" "$DEV_TARGET"; do
  orange_wrapper_require_help "$target" \
  "--config-dir" \
  "--serials" \
  "--measure-seconds" \
  "--frame-rate" \
  "--gpu-direct" \
  "--frame-stats" \
  "evt_stream_smoke" \
  "/home/jeremy/orange/targets/release/evt_stream_smoke"
done
"$DEV_TARGET" --dry-run --all >/dev/null
if orange_wrapper_release_installed; then
  "$RELEASE_TARGET" --dry-run --all >/dev/null
else
  echo "[install-evt-stream-smoke-wrapper] no /opt/orange/current yet: release command dry run skipped"
fi

echo "[install-evt-stream-smoke-wrapper] installed and verified"
echo "[install-evt-stream-smoke-wrapper] try:"
echo "  sudo -n $RELEASE_TARGET --help"
echo "  sudo -n $DEV_TARGET --help"
