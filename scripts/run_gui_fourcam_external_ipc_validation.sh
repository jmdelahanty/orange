#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREVIEW_MODE="${ORANGE_GUI_FOURCAM_PREVIEW_MODE:-hidden}"

usage() {
  cat <<'EOF'
Usage:
  scripts/run_gui_fourcam_external_ipc_validation.sh [options]

Runs the local four-camera GUI validation profile:
  - config: /home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam
  - full-frame recording sink: external_ipc
  - crop recording sink: external_ipc
  - crop recorder GPUs: 2010093->4, 2010094->2, 2010095->8, 2010096->6
  - GUI frame pacing: swap_interval=0, frame_max_fps=60

Options:
  --hidden-crop-preview       Hide crop preview windows during autorun (default).
  --visible-crop-preview      Leave crop preview windows visible during autorun.
  --disable-crop-preview      Disable crop preview generation.
  --record-seconds <seconds>  Override autorun recording duration.
  --warmup-seconds <seconds>  Override autorun stream warmup duration.
  --validate-only             Run launcher preflight only.
  --print-exec-env-only       Print env values that would cross the privilege boundary.
  --help

Environment values set before invoking this script still override these profile
defaults unless an option above explicitly sets that value.
EOF
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_nonnegative_integer() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hidden-crop-preview)
      PREVIEW_MODE="hidden"
      shift
      ;;
    --visible-crop-preview)
      PREVIEW_MODE="visible"
      shift
      ;;
    --disable-crop-preview)
      PREVIEW_MODE="disabled"
      shift
      ;;
    --record-seconds)
      shift
      [[ $# -gt 0 ]] || { echo "--record-seconds requires a value" >&2; exit 2; }
      is_positive_integer "$1" || { echo "--record-seconds must be a positive integer" >&2; exit 2; }
      export ORANGE_GUI_AUTORUN_RECORD_SECONDS="$1"
      shift
      ;;
    --warmup-seconds)
      shift
      [[ $# -gt 0 ]] || { echo "--warmup-seconds requires a value" >&2; exit 2; }
      is_nonnegative_integer "$1" || { echo "--warmup-seconds must be a non-negative integer" >&2; exit 2; }
      export ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS="$1"
      shift
      ;;
    --validate-only)
      export ORANGE_GUI_VALIDATE_ONLY=1
      shift
      ;;
    --print-exec-env-only)
      export ORANGE_GUI_PRINT_EXEC_ENV_ONLY=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "${PREVIEW_MODE}" in
  hidden)
    export ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=1
    export ORANGE_CROP_PREVIEW_DISABLE="${ORANGE_CROP_PREVIEW_DISABLE:-0}"
    ;;
  visible)
    export ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=0
    export ORANGE_CROP_PREVIEW_DISABLE="${ORANGE_CROP_PREVIEW_DISABLE:-0}"
    ;;
  disabled)
    export ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=1
    export ORANGE_CROP_PREVIEW_DISABLE=1
    ;;
  *)
    echo "ORANGE_GUI_FOURCAM_PREVIEW_MODE must be hidden, visible, or disabled" >&2
    exit 2
    ;;
esac

export ORANGE_GUI_CONFIG_DIR="${ORANGE_GUI_CONFIG_DIR:-/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam}"
export ORANGE_GUI_EXPECT_CAMERAS="${ORANGE_GUI_EXPECT_CAMERAS:-2010093,2010094,2010095,2010096}"
export ORANGE_GUI_RECORDING_SINK_MODE="${ORANGE_GUI_RECORDING_SINK_MODE:-external_ipc}"
export ORANGE_CROP_RECORDING_SINK_MODE="${ORANGE_CROP_RECORDING_SINK_MODE:-external_ipc}"
export ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU="${ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU:-1}"
export ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010093="${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010093:-4}"
export ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010094="${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010094:-2}"
export ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095="${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095:-8}"
export ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010096="${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010096:-6}"
export ORANGE_PTP_REGISTER_READ_DECIMATE="${ORANGE_PTP_REGISTER_READ_DECIMATE:-100}"
export ORANGE_GUI_AUTORUN="${ORANGE_GUI_AUTORUN:-1}"
export ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS="${ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS:-2}"
export ORANGE_GUI_AUTORUN_RECORD_SECONDS="${ORANGE_GUI_AUTORUN_RECORD_SECONDS:-10}"
export ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE="${ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE:-1}"
export ORANGE_GUI_SWAP_INTERVAL="${ORANGE_GUI_SWAP_INTERVAL:-0}"
export ORANGE_GUI_FRAME_MAX_FPS="${ORANGE_GUI_FRAME_MAX_FPS:-60}"
export ORANGE_GUI_SHOW_SPEED_GRAPHS="${ORANGE_GUI_SHOW_SPEED_GRAPHS:-0}"

exec "${REPO_ROOT}/scripts/run_gui_aq_off_validation.sh"
