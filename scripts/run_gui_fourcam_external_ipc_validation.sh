#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREVIEW_MODE="${ORANGE_GUI_FOURCAM_PREVIEW_MODE:-hidden}"
DISPLAY_PROFILE="${ORANGE_GUI_FOURCAM_DISPLAY_PROFILE:-fast}"
MANUAL_LOCAL_CONTROL_MODE="none"

usage() {
  cat <<'EOF'
Usage:
  scripts/run_gui_fourcam_external_ipc_validation.sh [options]

Runs the local four-camera GUI validation profile:
  - config: /home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam
  - full-frame recording sink: external_ipc
  - crop recording sink: external_ipc
  - crop external encode queue depth: 128
  - crop recorder GPUs: 2010093->4, 2010094->2, 2010095->8, 2010096->6
  - YOLO CPU affinity: 2010093->6, 2010094->8, 2010095->10, 2010096->12
  - required isolated CPUs: 6,8,10,12 plus SMT siblings 38,40,42,44
  - required boot CPU options: isolcpus, nohz_full, rcu_nocbs
  - GUI display profile: fast (swap_interval=0, frame_max_fps=60, preview=15)

Options:
  --hidden-crop-preview       Hide crop preview windows during autorun (default).
  --visible-crop-preview      Leave crop preview windows visible during autorun.
  --disable-crop-preview      Disable crop preview generation.
  --fast-display              Use fast validation display pacing (default).
  --citrus-display-safe       Reduce Orange display pressure for Citrus stimulus runs.
  --display-preview-max-fps <fps>
                             Override full-frame GUI display preview cadence.
  --swap-interval <interval>  Override GLFW swap interval, in [0,4].
  --gui-frame-max-fps <fps>   Override GUI loop frame cap; 0 disables cap.
  --record-seconds <seconds>  Override autorun recording duration.
  --warmup-seconds <seconds>  Override autorun stream warmup duration.
  --startup-lifecycle-only   Start all four streams, warm up, explicitly stop
                             streaming, then close without recording or YOLO.
  --cancel-stream-startup-after-ms <ms>
                             Request the normal startup-cancel path after the
                             given delay, wait for rollback, then close.
  --clip-seconds <seconds>    Enable GUI rolling clips with this clip duration.
  --manual-local-control      Disable autorun, keep the GUI operator-owned,
                              and enable generic stop_recording plus Citrus
                              completion-stop requests.
  --manual-citrus-completion-control
                             Disable autorun, keep the GUI operator-owned,
                             and enable only Citrus completion-stop requests.
  --allow-main-video-content-failure <serials>
                             Treat listed no-lens/invalid-content cameras as
                             allowed main-video content failures in printed
                             validation commands.
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

require_nonnegative_integer_in_range() {
  local value="$1"
  local label="$2"
  local max_value="$3"
  is_nonnegative_integer "${value}" || { echo "${label} must be a non-negative integer" >&2; exit 2; }
  (( value <= max_value )) || { echo "${label} must be <= ${max_value}" >&2; exit 2; }
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
    --fast-display)
      DISPLAY_PROFILE="fast"
      shift
      ;;
    --citrus-display-safe)
      DISPLAY_PROFILE="citrus_safe"
      shift
      ;;
    --display-preview-max-fps)
      shift
      [[ $# -gt 0 ]] || { echo "--display-preview-max-fps requires a value" >&2; exit 2; }
      require_nonnegative_integer_in_range "$1" "--display-preview-max-fps" 10000
      export ORANGE_DISPLAY_PREVIEW_MAX_FPS="$1"
      shift
      ;;
    --swap-interval)
      shift
      [[ $# -gt 0 ]] || { echo "--swap-interval requires a value" >&2; exit 2; }
      require_nonnegative_integer_in_range "$1" "--swap-interval" 4
      export ORANGE_GUI_SWAP_INTERVAL="$1"
      shift
      ;;
    --gui-frame-max-fps)
      shift
      [[ $# -gt 0 ]] || { echo "--gui-frame-max-fps requires a value" >&2; exit 2; }
      require_nonnegative_integer_in_range "$1" "--gui-frame-max-fps" 1000
      export ORANGE_GUI_FRAME_MAX_FPS="$1"
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
    --startup-lifecycle-only)
      export ORANGE_GUI_AUTORUN=1
      export ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=1
      export ORANGE_GUI_AUTORUN_ENABLE_STREAM=1
      export ORANGE_GUI_AUTORUN_ENABLE_RECORD=0
      export ORANGE_GUI_AUTORUN_ENABLE_YOLO=0
      export ORANGE_GUI_AUTORUN_ENABLE_CROP=0
      export ORANGE_GUI_AUTORUN_START_RECORDING=0
      export ORANGE_GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP=1
      export ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS=-1
      shift
      ;;
    --cancel-stream-startup-after-ms)
      shift
      [[ $# -gt 0 ]] || { echo "--cancel-stream-startup-after-ms requires a value" >&2; exit 2; }
      is_nonnegative_integer "$1" || {
        echo "--cancel-stream-startup-after-ms must be a non-negative integer" >&2
        exit 2
      }
      export ORANGE_GUI_AUTORUN=1
      export ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=1
      export ORANGE_GUI_AUTORUN_ENABLE_STREAM=1
      export ORANGE_GUI_AUTORUN_ENABLE_RECORD=0
      export ORANGE_GUI_AUTORUN_ENABLE_YOLO=0
      export ORANGE_GUI_AUTORUN_ENABLE_CROP=0
      export ORANGE_GUI_AUTORUN_START_RECORDING=0
      export ORANGE_GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP=0
      export ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS="$1"
      shift
      ;;
    --clip-seconds)
      shift
      [[ $# -gt 0 ]] || { echo "--clip-seconds requires a value" >&2; exit 2; }
      is_positive_integer "$1" || { echo "--clip-seconds must be a positive integer" >&2; exit 2; }
      export ORANGE_GUI_CLIP_SECONDS="$1"
      shift
      ;;
    --manual-local-control)
      if [[ "${MANUAL_LOCAL_CONTROL_MODE}" != "none" ]]; then
        echo "--manual-local-control conflicts with --manual-citrus-completion-control" >&2
        exit 2
      fi
      MANUAL_LOCAL_CONTROL_MODE="full"
      shift
      ;;
    --manual-citrus-completion-control)
      if [[ "${MANUAL_LOCAL_CONTROL_MODE}" != "none" ]]; then
        echo "--manual-citrus-completion-control conflicts with --manual-local-control" >&2
        exit 2
      fi
      MANUAL_LOCAL_CONTROL_MODE="citrus_completion"
      shift
      ;;
    --allow-main-video-content-failure)
      shift
      [[ $# -gt 0 ]] || { echo "--allow-main-video-content-failure requires a value" >&2; exit 2; }
      export ORANGE_GUI_ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS="$1"
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

case "${DISPLAY_PROFILE}" in
  fast)
    export ORANGE_DISPLAY_PREVIEW_MAX_FPS="${ORANGE_DISPLAY_PREVIEW_MAX_FPS:-15}"
    export ORANGE_GUI_SWAP_INTERVAL="${ORANGE_GUI_SWAP_INTERVAL:-0}"
    export ORANGE_GUI_FRAME_MAX_FPS="${ORANGE_GUI_FRAME_MAX_FPS:-60}"
    ;;
  citrus_safe)
    export ORANGE_DISPLAY_PREVIEW_MAX_FPS="${ORANGE_DISPLAY_PREVIEW_MAX_FPS:-10}"
    export ORANGE_GUI_SWAP_INTERVAL="${ORANGE_GUI_SWAP_INTERVAL:-1}"
    export ORANGE_GUI_FRAME_MAX_FPS="${ORANGE_GUI_FRAME_MAX_FPS:-30}"
    ;;
  *)
    echo "ORANGE_GUI_FOURCAM_DISPLAY_PROFILE must be fast or citrus_safe" >&2
    exit 2
    ;;
esac

case "${MANUAL_LOCAL_CONTROL_MODE}" in
  none)
    ;;
  full)
    export ORANGE_GUI_AUTORUN=0
    export ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=0
    export ORANGE_GUI_AUTORUN_ENABLE_STREAM=0
    export ORANGE_GUI_AUTORUN_ENABLE_RECORD=0
    export ORANGE_GUI_AUTORUN_ENABLE_YOLO=0
    export ORANGE_GUI_AUTORUN_ENABLE_CROP=0
    export ORANGE_GUI_AUTORUN_START_RECORDING=0
    export ORANGE_GUI_LOCAL_CONTROL_DISABLE=0
    export ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=0
    export ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=1
    export ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1
    export ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=0
    ;;
  citrus_completion)
    export ORANGE_GUI_AUTORUN=0
    export ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=0
    export ORANGE_GUI_AUTORUN_ENABLE_STREAM=0
    export ORANGE_GUI_AUTORUN_ENABLE_RECORD=0
    export ORANGE_GUI_AUTORUN_ENABLE_YOLO=0
    export ORANGE_GUI_AUTORUN_ENABLE_CROP=0
    export ORANGE_GUI_AUTORUN_START_RECORDING=0
    export ORANGE_GUI_LOCAL_CONTROL_DISABLE=0
    export ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=0
    export ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=0
    export ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=1
    export ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=0
    ;;
esac

export ORANGE_GUI_CONFIG_DIR="${ORANGE_GUI_CONFIG_DIR:-/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam}"
export ORANGE_GUI_EXPECT_CAMERAS="${ORANGE_GUI_EXPECT_CAMERAS:-2010093,2010094,2010095,2010096}"
export ORANGE_GUI_RECORDING_SINK_MODE="${ORANGE_GUI_RECORDING_SINK_MODE:-external_ipc}"
export ORANGE_CROP_RECORDING_SINK_MODE="${ORANGE_CROP_RECORDING_SINK_MODE:-external_ipc}"
export ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH="${ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH:-128}"
export ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU="${ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU:-1}"
# GOP-parity crop interleave (2026-09-04): the crop contract lists [other die,
# detect die] and routes each crop GOP to the idle encoder; the per-camera
# GPU ids below name the other die.
export ORANGE_CROP_EXTERNAL_INTERLEAVE="${ORANGE_CROP_EXTERNAL_INTERLEAVE:-1}"
export ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010093="${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010093:-4}"
export ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010094="${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010094:-2}"
export ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095="${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095:-8}"
export ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010096="${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010096:-6}"
export ORANGE_YOLO_AFFINITY_CAM_2010093="${ORANGE_YOLO_AFFINITY_CAM_2010093:-6}"
export ORANGE_YOLO_AFFINITY_CAM_2010094="${ORANGE_YOLO_AFFINITY_CAM_2010094:-8}"
export ORANGE_YOLO_AFFINITY_CAM_2010095="${ORANGE_YOLO_AFFINITY_CAM_2010095:-10}"
export ORANGE_YOLO_AFFINITY_CAM_2010096="${ORANGE_YOLO_AFFINITY_CAM_2010096:-12}"
if [[ ! -v ORANGE_GUI_REQUIRE_ISOLATED_CPUS ]]; then
  export ORANGE_GUI_REQUIRE_ISOLATED_CPUS="6,8,10,12,38,40,42,44"
fi
if [[ ! -v ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS ]]; then
  export ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS="${ORANGE_GUI_REQUIRE_ISOLATED_CPUS}"
fi
if [[ ! -v ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_OPTIONS ]]; then
  export ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_OPTIONS="isolcpus,nohz_full,rcu_nocbs"
fi
export ORANGE_PTP_REGISTER_READ_DECIMATE="${ORANGE_PTP_REGISTER_READ_DECIMATE:-100}"
export ORANGE_GUI_AUTORUN="${ORANGE_GUI_AUTORUN:-1}"
export ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS="${ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS:-2}"
export ORANGE_GUI_AUTORUN_RECORD_SECONDS="${ORANGE_GUI_AUTORUN_RECORD_SECONDS:-10}"
export ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE="${ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE:-1}"
export ORANGE_GUI_SHOW_SPEED_GRAPHS="${ORANGE_GUI_SHOW_SPEED_GRAPHS:-0}"
export ORANGE_GUI_REQUIRE_SOURCE_VERSION="${ORANGE_GUI_REQUIRE_SOURCE_VERSION:-1}"
export ORANGE_GUI_EXPECT_SOURCE_GIT_COMMAND_USER_MODE="${ORANGE_GUI_EXPECT_SOURCE_GIT_COMMAND_USER_MODE:-sudo_invoking_user}"
export ORANGE_GUI_EXPECT_SOURCE_DIRTY_TRACKED="${ORANGE_GUI_EXPECT_SOURCE_DIRTY_TRACKED:-auto}"

exec "${REPO_ROOT}/scripts/run_gui_aq_off_validation.sh"
