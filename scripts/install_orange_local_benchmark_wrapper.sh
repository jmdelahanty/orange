#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  install_orange_local_benchmark_wrapper.sh
  install_orange_local_benchmark_wrapper.sh --target /usr/local/bin/orange-local-benchmark

Installs this repo's narrow sudo benchmark wrapper and verifies the installed
command exposes the expected headless YOLO perf and spatial-mask flags.
EOF
}

TARGET="/usr/local/bin/orange-local-benchmark"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --target)
      shift
      [[ $# -gt 0 ]] || { echo "--target requires a value." >&2; exit 2; }
      TARGET="$1"
      shift
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE="$REPO_ROOT/scripts/orange_local_benchmark_wrapper.sh"

if [[ ! -f "$SOURCE" ]]; then
  echo "Wrapper source not found: $SOURCE" >&2
  exit 1
fi

bash -n "$SOURCE"

echo "[install-wrapper] source=$SOURCE"
echo "[install-wrapper] target=$TARGET"

if [[ "${EUID}" -eq 0 ]]; then
  install -o root -g root -m 0755 "$SOURCE" "$TARGET"
else
  sudo install -o root -g root -m 0755 "$SOURCE" "$TARGET"
fi

HELP_TEXT="$("$TARGET" --help)"
for required in \
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
  "/home/jeremy/orange-gop-split-a16/experiment_specs"; do
  if [[ "$HELP_TEXT" != *"$required"* ]]; then
    echo "Installed wrapper help is missing expected text: $required" >&2
    exit 1
  fi
done

echo "[install-wrapper] installed and verified"
echo "[install-wrapper] try:"
echo "  sudo -n $TARGET --help"
