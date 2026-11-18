#!/usr/bin/env bash
set -euo pipefail
# shellcheck source=orange_wrapper_install_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/orange_wrapper_install_lib.sh"

usage() {
  cat <<'EOF'
Usage:
  install_orange_local_benchmark_wrapper.sh [options]

Installs this repo's narrow sudo benchmark wrapper and verifies the installed
commands expose the expected headless YOLO perf and spatial-mask flags.
Installs two commands from scripts/orange_local_benchmark_wrapper.sh; the installed name sets the mode:
  /usr/local/bin/orange-local-benchmark      installed releases only (default /opt/orange/current)
  /usr/local/bin/orange-dev-local-benchmark  the development tree only (/home/jeremy/orange)

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
SOURCE="$REPO_ROOT/scripts/orange_local_benchmark_wrapper.sh"
RELEASE_TARGET="$ORANGE_WRAPPER_BIN_DIR/orange-local-benchmark"
DEV_TARGET="$ORANGE_WRAPPER_BIN_DIR/orange-dev-local-benchmark"

orange_wrapper_install install-wrapper "$SOURCE" local-benchmark

for target in "$RELEASE_TARGET" "$DEV_TARGET"; do
  orange_wrapper_require_help "$target" \
  "--dry-run" \
  "/home/jeremy/orange/targets/release/orange_client" \
  "--run-stamp" \
  "--yolo-perf-log" \
  "--yolo-perf-sample" \
  "--analytics-early-owned-frame" \
  "--yolo-ready-event-fastpath" \
  "--yolo-detach-input" \
  "--preprocess-defer-source-release" \
  "--yolo-spatial-mask-mode" \
  "--yolo-spatial-mask-input-context-outset-px" \
  "--yolo-spatial-mask-apply-timeout-ms" \
  "--citrus-recording-canvas-config-path" \
  "/home/jeremy/orange/experiment_specs"
done

echo "[install-wrapper] installed and verified"
echo "[install-wrapper] try:"
echo "  sudo -n $RELEASE_TARGET --help"
echo "  sudo -n $DEV_TARGET --help"
