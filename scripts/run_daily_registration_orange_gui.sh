#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORANGE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ORANGE_BIN="${ORANGE_BIN:-${ORANGE_ROOT}/targets/release/orange}"
ORANGE_GUI_WRAPPER="${ORANGE_GUI_WRAPPER:-/usr/local/bin/orange-gui-validation}"
ORANGE_CONFIG_DIR="${ORANGE_CONFIG_DIR:-/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam}"
CITRUS_CONTROL_SOCKET="/tmp/citrus_local_control.sock"

USER_ID="$(id -u)"
DISPLAY_VALUE="${DISPLAY:-:1}"
XDG_RUNTIME_DIR_VALUE="${XDG_RUNTIME_DIR:-/run/user/${USER_ID}}"
XAUTHORITY_VALUE="${XAUTHORITY:-${XDG_RUNTIME_DIR_VALUE}/gdm/Xauthority}"

if [[ "${1:-}" == "--help" ]]; then
  cat <<'EOF'
Usage: scripts/run_daily_registration_orange_gui.sh

Starts the four-camera Orange GUI for manual daily registration using the
current release build and a required healthy PTP stack. Streaming is enabled;
recording, YOLO, and crop production are disabled.

Start scripts/run_daily_registration_citrus_gui.sh first in another pane.

Optional environment overrides:
  ORANGE_BIN, ORANGE_GUI_WRAPPER, ORANGE_CONFIG_DIR
  DISPLAY, XAUTHORITY, XDG_RUNTIME_DIR
EOF
  exit 0
fi
if [[ $# -ne 0 ]]; then
  echo "Unexpected argument: $1 (use --help)" >&2
  exit 2
fi

[[ -x "${ORANGE_BIN}" ]] || {
  echo "Orange executable is missing or not executable: ${ORANGE_BIN}" >&2
  exit 1
}
[[ -x "${ORANGE_GUI_WRAPPER}" ]] || {
  echo "Orange GUI privilege wrapper is missing or not executable: ${ORANGE_GUI_WRAPPER}" >&2
  exit 1
}
[[ -d "${ORANGE_CONFIG_DIR}" ]] || {
  echo "Orange camera configuration is missing: ${ORANGE_CONFIG_DIR}" >&2
  exit 1
}
[[ -f "${XAUTHORITY_VALUE}" ]] || {
  echo "X authority file is missing: ${XAUTHORITY_VALUE}" >&2
  exit 1
}
[[ -S "${CITRUS_CONTROL_SOCKET}" ]] || {
  echo "Citrus is not listening on ${CITRUS_CONTROL_SOCKET}." >&2
  echo "Start scripts/run_daily_registration_citrus_gui.sh first." >&2
  exit 1
}

echo "Starting Orange daily-registration GUI"
echo "  binary=${ORANGE_BIN}"
echo "  config=${ORANGE_CONFIG_DIR}"
echo "  citrus_control_socket=${CITRUS_CONTROL_SOCKET}"
echo "  streaming=enabled recording=disabled yolo=disabled crop=disabled"

exec sudo -n "${ORANGE_GUI_WRAPPER}" \
  --orange-bin "${ORANGE_BIN}" \
  --ptp-stack-mode require \
  --env DISPLAY="${DISPLAY_VALUE}" \
  --env XAUTHORITY="${XAUTHORITY_VALUE}" \
  --env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR_VALUE}" \
  --env XDG_SESSION_TYPE=x11 \
  --env ORANGE_GUI_CONFIG_DIR="${ORANGE_CONFIG_DIR}" \
  --env ORANGE_GUI_AUTORUN=1 \
  --env ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=2 \
  --env ORANGE_GUI_AUTORUN_ENABLE_STREAM=1 \
  --env ORANGE_GUI_AUTORUN_ENABLE_RECORD=0 \
  --env ORANGE_GUI_AUTORUN_ENABLE_YOLO=0 \
  --env ORANGE_GUI_AUTORUN_ENABLE_CROP=0 \
  --env ORANGE_GUI_AUTORUN_START_RECORDING=0 \
  --env ORANGE_GUI_RECORDING_SINK_MODE=none \
  --env ORANGE_CROP_RECORDING_SINK_MODE=none
