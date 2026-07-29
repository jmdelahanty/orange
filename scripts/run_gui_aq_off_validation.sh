#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORANGE_BIN="${ORANGE_BIN:-${REPO_ROOT}/targets/release/orange}"
CONFIG_DIR="${ORANGE_GUI_CONFIG_DIR:-/home/jeremy/orange_data/config/local/100_cam4_ptp}"
CONFIG_NAME="${ORANGE_GUI_CONFIG_NAME:-$(basename "${CONFIG_DIR}")}"
EXPECT_SYNC_MODE="${ORANGE_GUI_EXPECT_SYNC_MODE:-ptp_gate}"
EXPECT_PTP_ENABLED="${ORANGE_GUI_EXPECT_PTP_ENABLED:-1}"
EXPECT_CAMERAS="${ORANGE_GUI_EXPECT_CAMERAS:-}"
PTP_REGISTER_READ_DECIMATE="${ORANGE_PTP_REGISTER_READ_DECIMATE:-100}"
YOLO_DETACH_INPUT="${ORANGE_YOLO_DETACH_INPUT:-1}"
YOLO_AFFINITY_CAM_2010095="${ORANGE_YOLO_AFFINITY_CAM_2010095:-10}"
YOLO_AFFINITY_CAM_2010096="${ORANGE_YOLO_AFFINITY_CAM_2010096:-12}"
GUI_STREAM_DOWNSAMPLE="${ORANGE_GUI_STREAM_DOWNSAMPLE:-4}"
DISPLAY_PREVIEW_MAX_FPS="${ORANGE_DISPLAY_PREVIEW_MAX_FPS:-15}"
GUI_SWAP_INTERVAL="${ORANGE_GUI_SWAP_INTERVAL:-0}"
GUI_FRAME_MAX_FPS="${ORANGE_GUI_FRAME_MAX_FPS:-60}"
GUI_SHOW_SPEED_GRAPHS="${ORANGE_GUI_SHOW_SPEED_GRAPHS:-0}"
GUI_CAMERA_STARTUP_CONCURRENCY="${ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY:-1}"
GUI_AUTORUN="${ORANGE_GUI_AUTORUN:-0}"
GUI_AUTORUN_STREAM_WARMUP_SECONDS="${ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS:-3}"
GUI_AUTORUN_RECORD_SECONDS="${ORANGE_GUI_AUTORUN_RECORD_SECONDS:-10}"
GUI_AUTORUN_EXIT_AFTER_FINALIZE="${ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE:-0}"
GUI_AUTORUN_HIDE_CROP_PREVIEW="${ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW:-0}"
GUI_AUTORUN_ENABLE_STREAM="${ORANGE_GUI_AUTORUN_ENABLE_STREAM:-1}"
GUI_AUTORUN_ENABLE_RECORD="${ORANGE_GUI_AUTORUN_ENABLE_RECORD:-1}"
GUI_AUTORUN_ENABLE_YOLO="${ORANGE_GUI_AUTORUN_ENABLE_YOLO:-1}"
GUI_AUTORUN_ENABLE_CROP="${ORANGE_GUI_AUTORUN_ENABLE_CROP:-1}"
GUI_AUTORUN_START_RECORDING="${ORANGE_GUI_AUTORUN_START_RECORDING:-1}"
GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP="${ORANGE_GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP:-0}"
GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS="${ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS:--1}"
GUI_RECORD_FOR_SECONDS="${ORANGE_GUI_RECORD_FOR_SECONDS:-}"
GUI_CLIP_SECONDS="${ORANGE_GUI_CLIP_SECONDS:-}"
if [[ -n "${ORANGE_GUI_PTP_STACK_MODE:-}" ]]; then
  GUI_PTP_STACK_MODE="${ORANGE_GUI_PTP_STACK_MODE}"
elif [[ "${EXPECT_SYNC_MODE}" == "ptp_gate" && "${GUI_AUTORUN}" == "1" ]]; then
  GUI_PTP_STACK_MODE="auto"
else
  GUI_PTP_STACK_MODE="off"
fi
DISPLAY_ENV="${DISPLAY:-}"
XAUTHORITY_ENV="${XAUTHORITY:-${HOME}/.Xauthority}"
WAYLAND_DISPLAY_ENV="${WAYLAND_DISPLAY:-}"
XDG_RUNTIME_DIR_ENV="${XDG_RUNTIME_DIR:-}"
XDG_SESSION_TYPE_ENV="${XDG_SESSION_TYPE:-}"
GUI_USE_PRIVILEGE_WRAPPER="${ORANGE_GUI_USE_PRIVILEGE_WRAPPER:-auto}"
GUI_PRIVILEGE_WRAPPER="${ORANGE_GUI_PRIVILEGE_WRAPPER:-/usr/local/bin/orange-gui-validation}"
CROP_PREVIEW_VALIDATION_MAX_FPS="${ORANGE_CROP_PREVIEW_MAX_FPS:-15}"
CROP_RECORDING_SINK_MODE="${ORANGE_CROP_RECORDING_SINK_MODE:-in_process}"
CROP_EXTERNAL_ENCODE_QUEUE_DEPTH="${ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH:-64}"
CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER="${ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER:-}"
CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS="${ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS:-}"
CROP_EXTERNAL_REQUIRE_SEPARATE_GPU="${ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU:-0}"
CROP_FRAME_POOL_SIZE="${ORANGE_CROP_FRAME_POOL_SIZE:-}"
SOURCE_VERSION_REQUIRED="${ORANGE_GUI_REQUIRE_SOURCE_VERSION:-0}"
SOURCE_GIT_COMMAND_USER_MODE="${ORANGE_GUI_EXPECT_SOURCE_GIT_COMMAND_USER_MODE:-}"
SOURCE_DIRTY_TRACKED_EXPECTATION="${ORANGE_GUI_EXPECT_SOURCE_DIRTY_TRACKED:-}"
ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS="${ORANGE_GUI_ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS:-}"
REQUIRED_ISOLATED_CPUS="${ORANGE_GUI_REQUIRE_ISOLATED_CPUS:-}"
REQUIRED_KERNEL_CMDLINE_CPUS="${ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS:-}"
REQUIRED_KERNEL_CMDLINE_OPTIONS="${ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_OPTIONS:-}"
YOLO_MAX_ACQ_WORKER_P95_MS="${ORANGE_GUI_MAX_YOLO_ACQUISITION_TO_WORKER_START_P95_MS:-}"
YOLO_MAX_ENQUEUE_DEQUEUE_P95_MS="${ORANGE_GUI_MAX_YOLO_ENQUEUE_TO_DEQUEUE_P95_MS:-}"
YOLO_MAX_DEQUEUE_WORKER_P95_MS="${ORANGE_GUI_MAX_YOLO_DEQUEUE_TO_WORKER_START_P95_MS:-}"
YOLO_MAX_SERVICE_GAP_P95_MS="${ORANGE_GUI_MAX_YOLO_SAME_CAMERA_SERVICE_GAP_P95_MS:-}"
DEFAULT_DETECT_ENGINE="/home/jeremy/orange_data/detect/detect_all_available_detect_training_v004_yolo11n_trt_20260520_a16_gpu5_trt100_fp16_bo5_avg32.engine"
DETECT_ENGINE="${ORANGE_GUI_DETECT_ENGINE:-${DEFAULT_DETECT_ENGINE}}"
APP_CONFIG_PATH="${ORANGE_APP_CONFIG_PATH:-${ORANGE_GUI_APP_CONFIG_PATH:-${HOME}/orange_data/config/app/default.json}}"
APP_CONFIG_ENV_KEY=""
APP_CONFIG_ENV_VALUE=""
if [[ -n "${ORANGE_APP_CONFIG_PATH:-}" ]]; then
  APP_CONFIG_ENV_KEY="ORANGE_APP_CONFIG_PATH"
  APP_CONFIG_ENV_VALUE="${ORANGE_APP_CONFIG_PATH}"
elif [[ -n "${ORANGE_GUI_APP_CONFIG_PATH:-}" ]]; then
  APP_CONFIG_ENV_KEY="ORANGE_GUI_APP_CONFIG_PATH"
  APP_CONFIG_ENV_VALUE="${ORANGE_GUI_APP_CONFIG_PATH}"
elif [[ -f "${APP_CONFIG_PATH}" ]]; then
  APP_CONFIG_ENV_KEY="ORANGE_GUI_APP_CONFIG_PATH"
  APP_CONFIG_ENV_VALUE="${APP_CONFIG_PATH}"
fi
APP_CONFIG_ENV_DISPLAY="<not forwarded>"
if [[ -n "${APP_CONFIG_ENV_KEY}" ]]; then
  APP_CONFIG_ENV_DISPLAY="${APP_CONFIG_ENV_KEY}=${APP_CONFIG_ENV_VALUE}"
fi

is_nonnegative_integer() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

case "${GUI_USE_PRIVILEGE_WRAPPER}" in
  auto|0|1)
    ;;
  *)
    echo "ORANGE_GUI_USE_PRIVILEGE_WRAPPER must be auto, 0, or 1" >&2
    exit 2
    ;;
esac
case "${GUI_PTP_STACK_MODE}" in
  off|require|auto)
    ;;
  *)
    echo "ORANGE_GUI_PTP_STACK_MODE must be off, require, or auto" >&2
    exit 2
    ;;
esac
if [[ -n "${CROP_FRAME_POOL_SIZE}" ]]; then
  if ! is_nonnegative_integer "${CROP_FRAME_POOL_SIZE}" ||
      (( CROP_FRAME_POOL_SIZE < 1 || CROP_FRAME_POOL_SIZE > 512 )); then
    echo "ORANGE_CROP_FRAME_POOL_SIZE must be an integer in [1,512]" >&2
    exit 2
  fi
fi
if ! is_nonnegative_integer "${GUI_SWAP_INTERVAL}" || (( GUI_SWAP_INTERVAL > 4 )); then
  echo "ORANGE_GUI_SWAP_INTERVAL must be an integer in [0,4]" >&2
  exit 2
fi
if ! is_nonnegative_integer "${GUI_FRAME_MAX_FPS}" || (( GUI_FRAME_MAX_FPS > 1000 )); then
  echo "ORANGE_GUI_FRAME_MAX_FPS must be an integer in [0,1000]" >&2
  exit 2
fi
case "${GUI_CAMERA_STARTUP_CONCURRENCY}" in
  1|2|4)
    ;;
  *)
    echo "ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY must be 1, 2, or 4" >&2
    exit 2
    ;;
esac
case "${SOURCE_VERSION_REQUIRED}" in
  0|1)
    ;;
  *)
    echo "ORANGE_GUI_REQUIRE_SOURCE_VERSION must be 0 or 1" >&2
    exit 2
    ;;
esac
case "${SOURCE_GIT_COMMAND_USER_MODE}" in
  ""|process_euid|sudo_invoking_user)
    ;;
  *)
    echo "ORANGE_GUI_EXPECT_SOURCE_GIT_COMMAND_USER_MODE must be process_euid or sudo_invoking_user" >&2
    exit 2
    ;;
esac
case "${SOURCE_DIRTY_TRACKED_EXPECTATION}" in
  ""|auto|0|1)
    ;;
  *)
    echo "ORANGE_GUI_EXPECT_SOURCE_DIRTY_TRACKED must be auto, 0, or 1" >&2
    exit 2
    ;;
esac

CROP_FRAME_POOL_SIZE_DISPLAY="${CROP_FRAME_POOL_SIZE:-<orange default>}"
CROP_FRAME_POOL_VALIDATION_MIN=32
if [[ -z "${CROP_FRAME_POOL_SIZE}" &&
      "${CROP_RECORDING_SINK_MODE}" == "external_ipc" &&
      "${GUI_AUTORUN_ENABLE_CROP}" == "1" &&
      "${GUI_AUTORUN_ENABLE_YOLO}" == "1" ]] &&
      is_nonnegative_integer "${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH}" &&
      (( CROP_EXTERNAL_ENCODE_QUEUE_DEPTH > 0 )); then
  CROP_FRAME_POOL_SIZE=$((CROP_EXTERNAL_ENCODE_QUEUE_DEPTH * 2))
  if (( CROP_FRAME_POOL_SIZE < 64 )); then
    CROP_FRAME_POOL_SIZE=64
  elif (( CROP_FRAME_POOL_SIZE > 512 )); then
    CROP_FRAME_POOL_SIZE=512
  fi
  CROP_FRAME_POOL_SIZE_DISPLAY="${CROP_FRAME_POOL_SIZE} (auto for external_ipc)"
fi
if [[ -n "${CROP_FRAME_POOL_SIZE}" ]]; then
  CROP_FRAME_POOL_VALIDATION_MIN="${CROP_FRAME_POOL_SIZE}"
fi

EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS="--expect-external-crop-encode-queue-depth ${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH}"
EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS=""
if [[ "${ORANGE_GUI_RECORDING_SINK_MODE:-}" == "external_ipc" ||
      "${CROP_RECORDING_SINK_MODE}" == "external_ipc" ]]; then
  EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS="--require-external-recorder-status --require-external-recorder-storage-preflight --require-external-recorder-protocol-hello"
fi
PER_CAMERA_GPU_DISPLAY_ITEMS=()
if [[ "${CROP_RECORDING_SINK_MODE}" == "external_ipc" ]]; then
  EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS+=" --require-external-crop-backend-metadata"
fi
if [[ "${CROP_EXTERNAL_REQUIRE_SEPARATE_GPU}" == "1" ]]; then
  EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS+=" --require-external-crop-recorder-gpu-separate-from-analytics"
fi
if [[ -n "${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID:-}" ]] && is_nonnegative_integer "${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID}"; then
  EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS+=" --expect-external-crop-recorder-gpu-id ${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID}"
fi
while IFS= read -r var_name; do
  [[ -n "${var_name}" ]] || continue
  var_value="${!var_name}"
  serial="${var_name#ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_}"
  PER_CAMERA_GPU_DISPLAY_ITEMS+=("${serial}=${var_value}")
  if is_nonnegative_integer "${var_value}"; then
    EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS+=" --expect-external-crop-recorder-gpu ${serial}=${var_value}"
  fi
done < <(compgen -e ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_ | sort)
if ((${#PER_CAMERA_GPU_DISPLAY_ITEMS[@]})); then
  CROP_EXTERNAL_RECORDER_GPU_PER_CAMERA_DISPLAY="$(IFS=,; echo "${PER_CAMERA_GPU_DISPLAY_ITEMS[*]}")"
else
  CROP_EXTERNAL_RECORDER_GPU_PER_CAMERA_DISPLAY="<none>"
fi
declare -A YOLO_AFFINITY_DISPLAY_MAP=(
  ["2010095"]="${YOLO_AFFINITY_CAM_2010095}"
  ["2010096"]="${YOLO_AFFINITY_CAM_2010096}"
)
while IFS= read -r var_name; do
  [[ -n "${var_name}" ]] || continue
  serial="${var_name#ORANGE_YOLO_AFFINITY_CAM_}"
  YOLO_AFFINITY_DISPLAY_MAP["${serial}"]="${!var_name}"
done < <(compgen -e ORANGE_YOLO_AFFINITY_CAM_ | sort)
YOLO_AFFINITY_DISPLAY_ITEMS=()
while IFS= read -r serial; do
  [[ -n "${serial}" ]] || continue
  YOLO_AFFINITY_DISPLAY_ITEMS+=("${serial}=${YOLO_AFFINITY_DISPLAY_MAP[${serial}]}")
done < <(printf '%s\n' "${!YOLO_AFFINITY_DISPLAY_MAP[@]}" | sort)
YOLO_AFFINITY_PER_CAMERA_DISPLAY="$(IFS=,; echo "${YOLO_AFFINITY_DISPLAY_ITEMS[*]}")"
YOLO_AFFINITY_VALIDATION_FLAGS=""
for item in "${YOLO_AFFINITY_DISPLAY_ITEMS[@]}"; do
  YOLO_AFFINITY_VALIDATION_FLAGS+=" --expect-yolo-affinity ${item}"
done
CPU_ISOLATION_VALIDATION_FLAGS=""
if [[ -n "${REQUIRED_ISOLATED_CPUS}" ]]; then
  CPU_ISOLATION_VALIDATION_FLAGS=" --require-isolated-cpus ${REQUIRED_ISOLATED_CPUS}"
fi
if [[ -n "${REQUIRED_KERNEL_CMDLINE_CPUS}" ]]; then
  IFS=',' read -r -a REQUIRED_KERNEL_CMDLINE_OPTION_LIST <<< "${REQUIRED_KERNEL_CMDLINE_OPTIONS:-isolcpus,nohz_full,rcu_nocbs}"
  for option_name in "${REQUIRED_KERNEL_CMDLINE_OPTION_LIST[@]}"; do
    option_name="${option_name//[[:space:]]/}"
    [[ -n "${option_name}" ]] || continue
    CPU_ISOLATION_VALIDATION_FLAGS+=" --require-kernel-cmdline-cpus ${option_name}=${REQUIRED_KERNEL_CMDLINE_CPUS}"
  done
fi
YOLO_LATENCY_VALIDATION_FLAGS=""
if [[ -n "${YOLO_MAX_ACQ_WORKER_P95_MS}" ]]; then
  YOLO_LATENCY_VALIDATION_FLAGS+=" --max-yolo-acquisition-to-worker-start-p95-ms ${YOLO_MAX_ACQ_WORKER_P95_MS}"
fi
if [[ -n "${YOLO_MAX_ENQUEUE_DEQUEUE_P95_MS}" ]]; then
  YOLO_LATENCY_VALIDATION_FLAGS+=" --max-yolo-enqueue-to-dequeue-p95-ms ${YOLO_MAX_ENQUEUE_DEQUEUE_P95_MS}"
fi
if [[ -n "${YOLO_MAX_DEQUEUE_WORKER_P95_MS}" ]]; then
  YOLO_LATENCY_VALIDATION_FLAGS+=" --max-yolo-dequeue-to-worker-start-p95-ms ${YOLO_MAX_DEQUEUE_WORKER_P95_MS}"
fi
if [[ -n "${YOLO_MAX_SERVICE_GAP_P95_MS}" ]]; then
  YOLO_LATENCY_VALIDATION_FLAGS+=" --max-yolo-same-camera-service-gap-p95-ms ${YOLO_MAX_SERVICE_GAP_P95_MS}"
fi
if [[ -n "${CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER}" ]]; then
  EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS+=" --max-external-crop-encode-queue-high-water ${CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER}"
fi
if [[ -n "${CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS}" ]]; then
  EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS+=" --max-external-crop-enqueue-age-p95-ms ${CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS}"
fi
SOURCE_VERSION_VALIDATION_FLAGS=""
SOURCE_DIRTY_TRACKED_DISPLAY="${SOURCE_DIRTY_TRACKED_EXPECTATION:-<not set>}"
if [[ "${SOURCE_VERSION_REQUIRED}" == "1" ]]; then
  SOURCE_VERSION_VALIDATION_FLAGS+=" --require-source-version"
fi
if [[ -n "${SOURCE_GIT_COMMAND_USER_MODE}" ]]; then
  SOURCE_VERSION_VALIDATION_FLAGS+=" --expect-source-git-command-user-mode ${SOURCE_GIT_COMMAND_USER_MODE}"
fi
if [[ "${SOURCE_DIRTY_TRACKED_EXPECTATION}" == "auto" ]]; then
  if git_status="$(git -C "${REPO_ROOT}" status --porcelain --untracked-files=no 2>/dev/null)"; then
    if [[ -n "${git_status}" ]]; then
      SOURCE_DIRTY_TRACKED_EXPECTATION="1"
    else
      SOURCE_DIRTY_TRACKED_EXPECTATION="0"
    fi
    SOURCE_DIRTY_TRACKED_DISPLAY="${SOURCE_DIRTY_TRACKED_EXPECTATION} (auto)"
  else
    SOURCE_DIRTY_TRACKED_EXPECTATION=""
    SOURCE_DIRTY_TRACKED_DISPLAY="auto (git status unavailable)"
  fi
fi
if [[ "${SOURCE_DIRTY_TRACKED_EXPECTATION}" == "0" ||
      "${SOURCE_DIRTY_TRACKED_EXPECTATION}" == "1" ]]; then
  SOURCE_VERSION_VALIDATION_FLAGS+=" --expect-source-dirty-tracked ${SOURCE_DIRTY_TRACKED_EXPECTATION}"
fi
MAIN_VIDEO_CONTENT_VALIDATION_FLAGS=""
if [[ -n "${ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS}" ]]; then
  MAIN_VIDEO_CONTENT_VALIDATION_FLAGS+=" --allow-main-video-content-failure ${ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS}"
fi
RECORDING_MODE_VALIDATION_FLAGS=""
if [[ -n "${GUI_CLIP_SECONDS}" ]] &&
      is_nonnegative_integer "${GUI_CLIP_SECONDS}" &&
      (( GUI_CLIP_SECONDS > 0 )); then
  ROLLING_RECORD_FOR_SECONDS="${GUI_RECORD_FOR_SECONDS}"
  if [[ -z "${ROLLING_RECORD_FOR_SECONDS}" && "${GUI_AUTORUN}" == "1" ]]; then
    ROLLING_RECORD_FOR_SECONDS="${GUI_AUTORUN_RECORD_SECONDS}"
  fi
  RECORDING_MODE_VALIDATION_FLAGS+=" --expect-recording-mode rolling_clips"
  if [[ -n "${ROLLING_RECORD_FOR_SECONDS}" ]] &&
        is_nonnegative_integer "${ROLLING_RECORD_FOR_SECONDS}" &&
        (( ROLLING_RECORD_FOR_SECONDS > 0 )); then
    RECORDING_MODE_VALIDATION_FLAGS+=" --expect-record-for-seconds ${ROLLING_RECORD_FOR_SECONDS}"
  fi
  RECORDING_MODE_VALIDATION_FLAGS+=" --expect-clip-seconds ${GUI_CLIP_SECONDS}"
fi
COMPARE_VALIDATION_FLAGS="--require-pass --require-zero-crop-drops --require-visible-samples --require-hidden-samples --require-matching-cameras --require-matching-display-config --require-matching-crop-config --require-matching-yolo-runtime-config --require-imgui-glfw-size-cache --min-gui-visible-p05-fps 45 --min-gui-hidden-p05-fps 45"
if [[ -n "${CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER}" ]]; then
  COMPARE_VALIDATION_FLAGS+=" --max-external-crop-queue-high-water ${CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER}"
fi
if [[ -n "${CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS}" ]]; then
  COMPARE_VALIDATION_FLAGS+=" --max-external-crop-enqueue-age-p95-ms ${CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS}"
fi
if [[ "${CROP_EXTERNAL_REQUIRE_SEPARATE_GPU}" == "1" ]]; then
  COMPARE_VALIDATION_FLAGS+=" --require-external-crop-recorder-gpu-separate-from-analytics"
fi
if [[ -n "${EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS}" ]]; then
  COMPARE_VALIDATION_FLAGS+=" --require-external-recorder-status --require-external-recorder-storage-preflight --require-external-recorder-protocol-hello"
fi

if [[ ! -x "${ORANGE_BIN}" ]]; then
  echo "Missing executable: ${ORANGE_BIN}" >&2
  echo "Build it first: cmake --build ${REPO_ROOT}/targets/release -j 8" >&2
  exit 1
fi

python3 - \
  "${CONFIG_DIR}" \
  "${EXPECT_SYNC_MODE}" \
  "${EXPECT_PTP_ENABLED}" \
  "${PTP_REGISTER_READ_DECIMATE}" \
  "${DETECT_ENGINE}" \
  "${APP_CONFIG_PATH}" \
  "${EXPECT_CAMERAS}" \
  "${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH}" \
  "${CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER}" \
  "${CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS}" \
  "${CROP_EXTERNAL_REQUIRE_SEPARATE_GPU}" \
  "${CROP_RECORDING_SINK_MODE}" \
  "${GUI_AUTORUN}" \
  "${GUI_AUTORUN_STREAM_WARMUP_SECONDS}" \
  "${GUI_AUTORUN_RECORD_SECONDS}" \
  "${GUI_AUTORUN_EXIT_AFTER_FINALIZE}" \
  "${GUI_AUTORUN_HIDE_CROP_PREVIEW}" \
  "${GUI_AUTORUN_ENABLE_STREAM}" \
  "${GUI_AUTORUN_ENABLE_RECORD}" \
  "${GUI_AUTORUN_ENABLE_YOLO}" \
  "${GUI_AUTORUN_ENABLE_CROP}" \
  "${GUI_AUTORUN_START_RECORDING}" \
  "${GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP}" \
  "${GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS}" \
  "${GUI_RECORD_FOR_SECONDS}" \
  "${GUI_CLIP_SECONDS}" <<'PY'
import json
import os
import sys
from pathlib import Path

config_dir = Path(sys.argv[1])
expect_sync_mode = sys.argv[2]
expect_ptp_enabled_raw = sys.argv[3]
ptp_register_read_decimate_raw = sys.argv[4]
detect_engine = sys.argv[5]
app_config_path = Path(sys.argv[6]).expanduser()
expect_cameras_raw = sys.argv[7]
crop_external_queue_depth_raw = sys.argv[8]
crop_external_max_queue_high_water_raw = sys.argv[9]
crop_external_max_enqueue_age_p95_ms_raw = sys.argv[10]
crop_external_require_separate_gpu_raw = sys.argv[11]
crop_recording_sink_mode = sys.argv[12]
gui_autorun_raw = sys.argv[13]
gui_autorun_stream_warmup_seconds_raw = sys.argv[14]
gui_autorun_record_seconds_raw = sys.argv[15]
gui_autorun_exit_after_finalize_raw = sys.argv[16]
gui_autorun_hide_crop_preview_raw = sys.argv[17]
gui_autorun_enable_stream_raw = sys.argv[18]
gui_autorun_enable_record_raw = sys.argv[19]
gui_autorun_enable_yolo_raw = sys.argv[20]
gui_autorun_enable_crop_raw = sys.argv[21]
gui_autorun_start_recording_raw = sys.argv[22]
gui_autorun_stop_streaming_after_warmup_raw = sys.argv[23]
gui_autorun_cancel_stream_startup_after_ms_raw = sys.argv[24]
gui_record_for_seconds_raw = sys.argv[25]
gui_clip_seconds_raw = sys.argv[26]
expect_ptp_enabled = None
if expect_ptp_enabled_raw:
    expect_ptp_enabled = expect_ptp_enabled_raw not in {"0", "false", "False", "no", "No"}
errors = []
notes = []

def expected_config_names(config_dir: Path, raw: str) -> list[str]:
    if raw.strip():
        names = []
        for item in raw.split(","):
            item = item.strip()
            if not item:
                continue
            names.append(item if item.endswith(".json") else f"{item}.json")
        return sorted(set(names))
    return sorted(path.name for path in config_dir.glob("*.json"))

def optional_int(value) -> int | None:
    if isinstance(value, bool):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None

def resolved_crop_recorder_gpu(serial: str, analytics_gpu_id: int) -> int | None:
    per_camera_name = f"ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_{serial}"
    per_camera_raw = os.environ.get(per_camera_name, "")
    if per_camera_raw:
        return optional_int(per_camera_raw)
    global_raw = os.environ.get("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID", "")
    if global_raw:
        return optional_int(global_raw)
    return analytics_gpu_id

try:
    ptp_register_read_decimate = int(ptp_register_read_decimate_raw)
    if ptp_register_read_decimate < 1:
        errors.append("ORANGE_PTP_REGISTER_READ_DECIMATE must be >= 1")
except ValueError:
    errors.append("ORANGE_PTP_REGISTER_READ_DECIMATE must be an integer")

crop_external_queue_depth = None
try:
    crop_external_queue_depth = int(crop_external_queue_depth_raw)
    if crop_external_queue_depth < 1:
        errors.append("ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH must be >= 1")
except ValueError:
    errors.append("ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH must be an integer")

if crop_external_max_queue_high_water_raw:
    try:
        crop_external_max_queue_high_water = int(crop_external_max_queue_high_water_raw)
        if crop_external_max_queue_high_water < 0:
            errors.append("ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER must be >= 0")
        if (
            crop_external_queue_depth is not None
            and crop_external_max_queue_high_water > crop_external_queue_depth
        ):
            notes.append(
                "ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER is higher than "
                "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH; the validation gate "
                "will not constrain queue occupancy"
            )
    except ValueError:
        errors.append("ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER must be an integer")

if crop_external_max_enqueue_age_p95_ms_raw:
    try:
        crop_external_max_enqueue_age_p95_ms = float(crop_external_max_enqueue_age_p95_ms_raw)
        if crop_external_max_enqueue_age_p95_ms < 0.0:
            errors.append("ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS must be >= 0")
    except ValueError:
        errors.append("ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS must be a number")

if crop_external_require_separate_gpu_raw not in {"0", "1"}:
    errors.append("ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU must be 0 or 1")

if gui_autorun_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN must be 0 or 1")
if gui_autorun_exit_after_finalize_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE must be 0 or 1")
if gui_autorun_hide_crop_preview_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW must be 0 or 1")
if gui_autorun_enable_stream_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN_ENABLE_STREAM must be 0 or 1")
if gui_autorun_enable_record_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN_ENABLE_RECORD must be 0 or 1")
if gui_autorun_enable_yolo_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN_ENABLE_YOLO must be 0 or 1")
if gui_autorun_enable_crop_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN_ENABLE_CROP must be 0 or 1")
if gui_autorun_start_recording_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN_START_RECORDING must be 0 or 1")
if gui_autorun_stop_streaming_after_warmup_raw not in {"0", "1"}:
    errors.append("ORANGE_GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP must be 0 or 1")
gui_autorun_cancel_stream_startup_after_ms = -1
try:
    gui_autorun_cancel_stream_startup_after_ms = int(
        gui_autorun_cancel_stream_startup_after_ms_raw
    )
    if gui_autorun_cancel_stream_startup_after_ms < -1:
        errors.append(
            "ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS must be >= -1"
        )
except ValueError:
    errors.append(
        "ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS must be an integer"
    )
if gui_autorun_cancel_stream_startup_after_ms >= 0:
    if gui_autorun_raw != "1":
        errors.append(
            "startup cancellation requires ORANGE_GUI_AUTORUN=1"
        )
    if gui_autorun_enable_stream_raw != "1":
        errors.append(
            "startup cancellation requires ORANGE_GUI_AUTORUN_ENABLE_STREAM=1"
        )
    if gui_autorun_start_recording_raw != "0":
        errors.append(
            "startup cancellation requires ORANGE_GUI_AUTORUN_START_RECORDING=0"
        )
try:
    gui_autorun_stream_warmup_seconds = int(gui_autorun_stream_warmup_seconds_raw)
    if gui_autorun_stream_warmup_seconds < 0:
        errors.append("ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS must be >= 0")
except ValueError:
    errors.append("ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS must be an integer")
gui_autorun_record_seconds = 0
try:
    gui_autorun_record_seconds = int(gui_autorun_record_seconds_raw)
    if gui_autorun_record_seconds < 1:
        errors.append("ORANGE_GUI_AUTORUN_RECORD_SECONDS must be >= 1")
except ValueError:
    errors.append("ORANGE_GUI_AUTORUN_RECORD_SECONDS must be an integer")

if gui_autorun_raw == "1" and not str(config_dir):
    errors.append("ORANGE_GUI_CONFIG_DIR is required when ORANGE_GUI_AUTORUN=1")

def validate_optional_gpu_env(name: str) -> None:
    raw = os.environ.get(name, "")
    if not raw:
        return
    try:
        value = int(raw)
    except ValueError:
        errors.append(f"{name} must be an integer")
        return
    if value < 0:
        errors.append(f"{name} must be >= 0")

validate_optional_gpu_env("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID")
for name in sorted(os.environ):
    if name.startswith("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_"):
        validate_optional_gpu_env(name)

if detect_engine:
    detect_engine_path = Path(detect_engine).expanduser()
    if not detect_engine_path.is_file():
        errors.append(f"detect engine does not exist: {detect_engine_path}")

app_record_for_seconds = 0
app_clip_seconds = 0
if app_config_path.exists():
    try:
        app_config = json.loads(app_config_path.read_text())
        app_engine = app_config.get("models", {}).get("default_detect_engine", "")
        app_recording_control = app_config.get("recording", {}).get("recording_control", {})
        app_record_for_seconds = optional_int(app_recording_control.get("record_for_seconds"))
        app_clip_seconds = optional_int(app_recording_control.get("clip_seconds"))
        if app_recording_control and (
            app_record_for_seconds is None or app_clip_seconds is None
        ):
            notes.append(
                "app config recording_control is present but not fully numeric; "
                "Orange will validate it on startup"
            )
        if app_engine and detect_engine and app_engine != detect_engine:
            notes.append(
                "app config default_detect_engine differs; this launcher will "
                "override the GUI selection with ORANGE_DEFAULT_DETECT_ENGINE "
                "for this run"
            )
        elif not app_engine and detect_engine:
            notes.append(
                "app config has no default detect engine; this launcher will "
                "select one with ORANGE_DEFAULT_DETECT_ENGINE for this run"
            )
    except Exception as exc:
        notes.append(f"could not inspect app config {app_config_path}: {exc}")
else:
    notes.append(f"app config not found at {app_config_path}; using launcher detect-engine override")

def parse_optional_nonnegative_int(raw: str, name: str) -> int | None:
    if raw == "":
        return None
    try:
        value = int(raw)
    except ValueError:
        errors.append(f"{name} must be an integer")
        return None
    if value < 0:
        errors.append(f"{name} must be >= 0")
        return None
    return value

env_record_for_seconds = parse_optional_nonnegative_int(
    gui_record_for_seconds_raw,
    "ORANGE_GUI_RECORD_FOR_SECONDS",
)
env_clip_seconds = parse_optional_nonnegative_int(
    gui_clip_seconds_raw,
    "ORANGE_GUI_CLIP_SECONDS",
)
effective_record_for_seconds = (
    env_record_for_seconds
    if env_record_for_seconds is not None
    else (app_record_for_seconds or 0)
)
effective_clip_seconds = (
    env_clip_seconds
    if env_clip_seconds is not None
    else (app_clip_seconds or 0)
)
if effective_clip_seconds > 0 and effective_record_for_seconds <= 0:
    if gui_autorun_raw == "1":
        effective_record_for_seconds = gui_autorun_record_seconds
        notes.append(
            "GUI recording_control will use ORANGE_GUI_AUTORUN_RECORD_SECONDS "
            "as record_for_seconds for this rolling autorun"
        )
    else:
        errors.append(
            "GUI rolling recording_control requires ORANGE_GUI_RECORD_FOR_SECONDS "
            "or recording.recording_control.record_for_seconds when "
            "ORANGE_GUI_CLIP_SECONDS/clip_seconds is > 0"
        )

if not config_dir.is_dir():
    errors.append(f"config folder does not exist: {config_dir}")
else:
    expected = expected_config_names(config_dir, expect_cameras_raw)
    if not expected:
        errors.append(f"no camera JSON files found in config folder: {config_dir}")
    for name in expected:
        path = config_dir / name
        if not path.exists():
            errors.append(f"missing config: {path}")
            continue
        try:
            data = json.loads(path.read_text())
        except Exception as exc:
            errors.append(f"invalid JSON {path}: {exc}")
            continue
        encode = data.get("recording", {}).get("encode", {})
        if data.get("schema_version") != 4:
            errors.append(f"{path}: schema_version is not 4")
        if encode.get("aq") != "off":
            errors.append(f"{path}: recording.encode.aq is not 'off'")
        if encode.get("temporal_aq") != "off":
            errors.append(f"{path}: recording.encode.temporal_aq is not 'off'")
        if expect_sync_mode and data.get("sync_mode") != expect_sync_mode:
            errors.append(f"{path}: sync_mode is not {expect_sync_mode!r}")
        if expect_ptp_enabled is not None:
            ptp_enabled = bool(data.get("ptp", {}).get("enabled", False))
            if ptp_enabled != expect_ptp_enabled:
                errors.append(f"{path}: ptp.enabled is not {expect_ptp_enabled}")
        mapped_nir_strobes = [
            item
            for item in data.get("rig_io", {}).get("connections", [])
            if isinstance(item, dict)
            and item.get("purpose") == "nir_strobe_trigger"
            and item.get("direction") == "output"
        ]
        if data.get("gpio_pinout_access") == "exposed" and mapped_nir_strobes:
            gpio_nodes = {
                item.get("name"): item
                for item in data.get("gpio", {}).get("nodes", [])
                if isinstance(item, dict) and isinstance(item.get("name"), str)
            }
            required_strobe_nodes = {
                "GPO_0_Polarity": ("bool", False),
                "GPO_0_Mode": ("enum", "Exposure"),
            }
            for node_name, (node_type, node_value) in required_strobe_nodes.items():
                node = gpio_nodes.get(node_name)
                if (
                    not node
                    or node.get("type") != node_type
                    or node.get("value") != node_value
                ):
                    errors.append(
                        f"{path}: exposed mapped NIR strobe requires gpio.nodes "
                        f"{node_name}={node_value!r} ({node_type})"
                    )
        if (
            crop_recording_sink_mode == "external_ipc"
            and crop_external_require_separate_gpu_raw == "1"
        ):
            serial = path.stem
            analytics_gpu_id = optional_int(data.get("source_gpu_id"))
            if analytics_gpu_id is None:
                errors.append(
                    f"{path}: source_gpu_id missing; cannot validate "
                    "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=1"
                )
            else:
                recorder_gpu_id = resolved_crop_recorder_gpu(serial, analytics_gpu_id)
                if recorder_gpu_id is None:
                    errors.append(
                        f"{path}: external crop recorder GPU override is invalid for {serial}"
                    )
                elif recorder_gpu_id == analytics_gpu_id:
                    errors.append(
                        f"{path}: external crop recorder GPU would be {recorder_gpu_id}, "
                        f"matching source_gpu_id {analytics_gpu_id}; set "
                        "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID or "
                        f"ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_{serial} to a different GPU"
                    )
    if expected:
        notes.append(
            "validated camera configs: "
            + ", ".join(path.removesuffix(".json") for path in expected)
        )

if errors:
    for error in errors:
        print(error, file=sys.stderr)
    sys.exit(1)
for note in notes:
    print(f"note: {note}")
PY

LOCAL_CONTROL_RECORDING_START_DISPLAY="${ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START:-${ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START:-<app config/default>}}"
LOCAL_CONTROL_RECORDING_STOP_DISPLAY="${ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP:-${ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP:-<app config/default>}}"
LOCAL_CONTROL_CITRUS_STOP_DISPLAY="${ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP:-${ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP:-<app config/default>}}"

cat <<EOF
Launching Orange GUI for AQ-off validation.

Before opening cameras in the GUI:
  1. In the Local panel, select: ${CONFIG_NAME}
  2. Open cameras.
  3. Confirm recording defaults show: AQ off, temporal AQ off.
  4. Confirm sync mode is PTP gate if the GUI displays it.
  5. Run the normal recording test for the selected cameras.

Validated config folder:
  ${CONFIG_DIR}

Validation environment:
  ORANGE_GUI_EXPECT_CAMERAS=${EXPECT_CAMERAS:-<all JSON files in config folder>}
  ORANGE_PTP_REGISTER_READ_DECIMATE=${PTP_REGISTER_READ_DECIMATE}
  ORANGE_YOLO_DETACH_INPUT=${YOLO_DETACH_INPUT}
  ORANGE_YOLO_AFFINITY_CAM_*=${YOLO_AFFINITY_PER_CAMERA_DISPLAY}
  ORANGE_GUI_REQUIRE_ISOLATED_CPUS=${REQUIRED_ISOLATED_CPUS:-<not set>}
  ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_CPUS=${REQUIRED_KERNEL_CMDLINE_CPUS:-<not set>}
  ORANGE_GUI_REQUIRE_KERNEL_CMDLINE_OPTIONS=${REQUIRED_KERNEL_CMDLINE_OPTIONS:-<default if CPU list is set>}
  ORANGE_GUI_MAX_YOLO_ACQUISITION_TO_WORKER_START_P95_MS=${YOLO_MAX_ACQ_WORKER_P95_MS:-<not set>}
  ORANGE_GUI_MAX_YOLO_ENQUEUE_TO_DEQUEUE_P95_MS=${YOLO_MAX_ENQUEUE_DEQUEUE_P95_MS:-<not set>}
  ORANGE_GUI_MAX_YOLO_DEQUEUE_TO_WORKER_START_P95_MS=${YOLO_MAX_DEQUEUE_WORKER_P95_MS:-<not set>}
  ORANGE_GUI_MAX_YOLO_SAME_CAMERA_SERVICE_GAP_P95_MS=${YOLO_MAX_SERVICE_GAP_P95_MS:-<not set>}
  ORANGE_DEFAULT_DETECT_ENGINE=${DETECT_ENGINE}
  app config inspected path=${APP_CONFIG_PATH}
  app config forwarded env=${APP_CONFIG_ENV_DISPLAY}
  ORANGE_GUI_RECORDING_SINK_MODE=${ORANGE_GUI_RECORDING_SINK_MODE:-<app config/default>}
  ORANGE_GUI_STREAM_DOWNSAMPLE=${GUI_STREAM_DOWNSAMPLE}
  ORANGE_DISPLAY_PREVIEW_MAX_FPS=${DISPLAY_PREVIEW_MAX_FPS}
  ORANGE_GUI_SWAP_INTERVAL=${GUI_SWAP_INTERVAL}
  ORANGE_GUI_FRAME_MAX_FPS=${GUI_FRAME_MAX_FPS}
  ORANGE_GUI_SHOW_SPEED_GRAPHS=${GUI_SHOW_SPEED_GRAPHS}
  ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY=${GUI_CAMERA_STARTUP_CONCURRENCY}
  ORANGE_GUI_AUTORUN=${GUI_AUTORUN}
  ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=${GUI_AUTORUN_STREAM_WARMUP_SECONDS}
  ORANGE_GUI_AUTORUN_RECORD_SECONDS=${GUI_AUTORUN_RECORD_SECONDS}
  ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=${GUI_AUTORUN_EXIT_AFTER_FINALIZE}
  ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=${GUI_AUTORUN_HIDE_CROP_PREVIEW}
  ORANGE_GUI_AUTORUN_ENABLE_STREAM=${GUI_AUTORUN_ENABLE_STREAM}
  ORANGE_GUI_AUTORUN_ENABLE_RECORD=${GUI_AUTORUN_ENABLE_RECORD}
  ORANGE_GUI_AUTORUN_ENABLE_YOLO=${GUI_AUTORUN_ENABLE_YOLO}
  ORANGE_GUI_AUTORUN_ENABLE_CROP=${GUI_AUTORUN_ENABLE_CROP}
  ORANGE_GUI_AUTORUN_START_RECORDING=${GUI_AUTORUN_START_RECORDING}
  ORANGE_GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP=${GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP}
  ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS=${GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS}
  ORANGE_GUI_RECORD_FOR_SECONDS=${GUI_RECORD_FOR_SECONDS:-<app config/disabled>}
  ORANGE_GUI_CLIP_SECONDS=${GUI_CLIP_SECONDS:-<app config/disabled>}
  ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=${LOCAL_CONTROL_RECORDING_START_DISPLAY}
  ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=${LOCAL_CONTROL_RECORDING_STOP_DISPLAY}
  ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=${LOCAL_CONTROL_CITRUS_STOP_DISPLAY}
  ORANGE_GUI_PTP_STACK_MODE=${GUI_PTP_STACK_MODE}
  ORANGE_CROP_PREVIEW_MAX_FPS=${ORANGE_CROP_PREVIEW_MAX_FPS:-<camera config/default>}
  ORANGE_CROP_PREVIEW_DISABLE=${ORANGE_CROP_PREVIEW_DISABLE:-0}
  ORANGE_CROP_FRAME_POOL_SIZE=${CROP_FRAME_POOL_SIZE_DISPLAY}
  ORANGE_CROP_RECORDING_SINK_MODE=${CROP_RECORDING_SINK_MODE}
  ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH}
  ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID=${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID:-<camera GPU/default>}
  ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*=${CROP_EXTERNAL_RECORDER_GPU_PER_CAMERA_DISPLAY}
  ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=${CROP_EXTERNAL_REQUIRE_SEPARATE_GPU}
  ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER=${CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER:-<not set>}
  ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS=${CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS:-<not set>}
  ORANGE_GUI_REQUIRE_SOURCE_VERSION=${SOURCE_VERSION_REQUIRED}
  ORANGE_GUI_EXPECT_SOURCE_GIT_COMMAND_USER_MODE=${SOURCE_GIT_COMMAND_USER_MODE:-<not set>}
  ORANGE_GUI_EXPECT_SOURCE_DIRTY_TRACKED=${SOURCE_DIRTY_TRACKED_DISPLAY}
  ORANGE_GUI_ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS=${ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS:-<not set>}

Display session environment forwarded to the privileged GUI launch:
  DISPLAY=${DISPLAY_ENV:-<not set>}
  XAUTHORITY=${XAUTHORITY_ENV:-<not set>}
  WAYLAND_DISPLAY=${WAYLAND_DISPLAY_ENV:-<not set>}
  XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR_ENV:-<not set>}
  XDG_SESSION_TYPE=${XDG_SESSION_TYPE_ENV:-<not set>}
  ORANGE_GUI_USE_PRIVILEGE_WRAPPER=${GUI_USE_PRIVILEGE_WRAPPER}
  ORANGE_GUI_PRIVILEGE_WRAPPER=${GUI_PRIVILEGE_WRAPPER}

After recording, validate the artifact with:
  scripts/validate_gui_ptp_recording.py <recording_folder>
Or validate the newest artifact with:
  scripts/validate_gui_ptp_recording.py --latest
Or validate the newest real recording artifact with:
  scripts/validate_gui_ptp_recording.py --latest-complete
Before launching Orange, preflight the manual Citrus completion app-config with:
  scripts/check_gui_citrus_completion_ready.py --check-socket
After Orange is running and before starting Citrus, require live socket gates with:
  scripts/check_gui_citrus_completion_ready.py --require-manual-citrus-ready --wait-seconds 120
To capture the exact Orange artifact path for post-run validation:
  ORANGE_CITRUS_HANDOFF=/tmp/orange_manual_citrus_completion_handoff.json
  ORANGE_RECORDING_FOLDER=\$(scripts/check_gui_citrus_completion_ready.py --require-manual-citrus-ready --wait-seconds 120 --write-handoff "\${ORANGE_CITRUS_HANDOFF}" --print-recording-folder)
If Citrus /home/jeremy/citrus/system_config.yml already enables
  citrus_runtime.orange_completion.enabled=true
  citrus_runtime.orange_completion.socket_path=/tmp/orange_local_control.sock
  citrus_runtime.orange_completion.grace_seconds=10
the handoff export is optional. To override/check the Citrus completion-notify
environment from that verified handoff:
  eval "\$(scripts/validate_gui_citrus_completion_recording.py --handoff "\${ORANGE_CITRUS_HANDOFF}" --print-citrus-env)"
For a manual GUI run stopped by Citrus STOP ALL, validate the stop handoff
and Orange GUI-thread lifecycle evidence with:
  scripts/validate_gui_citrus_completion_recording.py --stop-all
Or target the exact folder captured before Citrus:
  scripts/validate_gui_citrus_completion_recording.py "\${ORANGE_RECORDING_FOLDER}" --stop-all
Or use the handoff JSON captured before Citrus:
  scripts/validate_gui_citrus_completion_recording.py --handoff "\${ORANGE_CITRUS_HANDOFF}" --stop-all
Or print the exact handoff-stored STOP ALL validation command:
  scripts/validate_gui_citrus_completion_recording.py --handoff "\${ORANGE_CITRUS_HANDOFF}" --print-validation-command --stop-all
Equivalent expanded command:
  scripts/validate_gui_ptp_recording.py --latest-complete --expect-local-control-stop-method citrus_completion --expect-local-control-stop-command-source citrus --expect-local-control-stop-terminal-state stopped --expect-local-control-stop-reason stopped_by_local_control --expect-local-control-stop-ack-state executed --expect-local-control-generic-stop-enabled 0 --expect-local-control-citrus-stop-enabled 1 --require-orange-local-control-event-log
For a manual GUI run stopped by natural Citrus protocol completion, validate
the stop handoff and Orange GUI-thread lifecycle evidence with:
  scripts/validate_gui_citrus_completion_recording.py --natural-completion
Or target the exact folder captured before Citrus:
  scripts/validate_gui_citrus_completion_recording.py "\${ORANGE_RECORDING_FOLDER}" --natural-completion
Or use the handoff JSON captured before Citrus:
  scripts/validate_gui_citrus_completion_recording.py --handoff "\${ORANGE_CITRUS_HANDOFF}" --natural-completion
Or print the exact handoff-stored natural-completion validation command:
  scripts/validate_gui_citrus_completion_recording.py --handoff "\${ORANGE_CITRUS_HANDOFF}" --print-validation-command --natural-completion
Equivalent expanded command:
  scripts/validate_gui_ptp_recording.py --latest-complete --expect-local-control-stop-method citrus_completion --expect-local-control-stop-command-source citrus --expect-local-control-stop-terminal-state completed --expect-local-control-stop-reason protocol_finished --expect-local-control-stop-ack-state executed --expect-local-control-generic-stop-enabled 0 --expect-local-control-citrus-stop-enabled 1 --require-orange-local-control-event-log
For a compact artifact health, crop fanout, and GUI timing summary, use:
  scripts/summarize_gui_validation.py --latest-complete
For full-frame GUI external IPC rolling runs, validate the external recorder
session and mirrored rolling manifest with:
  scripts/verify_external_recorder_session.py --analytics-root <recording_folder>

For crop-recording plus crop-preview validation, use:
  scripts/validate_gui_ptp_recording.py --latest-complete ${RECORDING_MODE_VALIDATION_FLAGS} --require-crop-recording-artifacts --require-crop-preview-counters --require-crop-preview-sampling --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 0 --expect-crop-preview-display-enabled 1 --min-crop-frame-pool-size ${CROP_FRAME_POOL_VALIDATION_MIN} ${EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS} ${EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS} ${SOURCE_VERSION_VALIDATION_FLAGS} ${MAIN_VIDEO_CONTENT_VALIDATION_FLAGS} ${YOLO_AFFINITY_VALIDATION_FLAGS} ${CPU_ISOLATION_VALIDATION_FLAGS} ${YOLO_LATENCY_VALIDATION_FLAGS} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-gui-swap-interval ${GUI_SWAP_INTERVAL} --expect-gui-frame-max-fps ${GUI_FRAME_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --require-imgui-glfw-size-cache --min-gui-crop-preview-visible-fps-p05 45 --json-out /tmp/orange_gui_crop_visible_validation.json
For a run where crop preview windows were hidden at finalization, use:
  scripts/validate_gui_ptp_recording.py --latest-complete ${RECORDING_MODE_VALIDATION_FLAGS} --require-crop-recording-artifacts --require-crop-preview-counters --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 0 --expect-crop-preview-display-enabled 0 --min-crop-frame-pool-size ${CROP_FRAME_POOL_VALIDATION_MIN} ${EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS} ${EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS} ${SOURCE_VERSION_VALIDATION_FLAGS} ${MAIN_VIDEO_CONTENT_VALIDATION_FLAGS} ${YOLO_AFFINITY_VALIDATION_FLAGS} ${CPU_ISOLATION_VALIDATION_FLAGS} ${YOLO_LATENCY_VALIDATION_FLAGS} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-gui-swap-interval ${GUI_SWAP_INTERVAL} --expect-gui-frame-max-fps ${GUI_FRAME_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --require-imgui-glfw-size-cache --min-gui-crop-preview-hidden-fps-p05 45 --json-out /tmp/orange_gui_crop_hidden_validation.json
For a run with ORANGE_CROP_PREVIEW_DISABLE=1, use:
  scripts/validate_gui_ptp_recording.py --latest-complete ${RECORDING_MODE_VALIDATION_FLAGS} --require-crop-recording-artifacts --require-crop-preview-counters --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 1 --min-crop-frame-pool-size ${CROP_FRAME_POOL_VALIDATION_MIN} ${EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS} ${EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS} ${SOURCE_VERSION_VALIDATION_FLAGS} ${MAIN_VIDEO_CONTENT_VALIDATION_FLAGS} ${YOLO_AFFINITY_VALIDATION_FLAGS} ${CPU_ISOLATION_VALIDATION_FLAGS} ${YOLO_LATENCY_VALIDATION_FLAGS} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-gui-swap-interval ${GUI_SWAP_INTERVAL} --expect-gui-frame-max-fps ${GUI_FRAME_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --require-imgui-glfw-size-cache --json-out /tmp/orange_gui_crop_disabled_validation.json
Then compare visible and hidden runs with:
  scripts/compare_gui_crop_preview_validation.py visible=/tmp/orange_gui_crop_visible_validation.json hidden=/tmp/orange_gui_crop_hidden_validation.json ${COMPARE_VALIDATION_FLAGS}
EOF

if [[ "${ORANGE_GUI_VALIDATE_ONLY:-0}" == "1" ]]; then
  exit 0
fi

cd "${REPO_ROOT}"
ENV_ARGS=(
  "DISPLAY=${DISPLAY_ENV}"
  "XAUTHORITY=${XAUTHORITY_ENV}"
  "ORANGE_YOLO_PERF_LOG=${ORANGE_YOLO_PERF_LOG:-1}"
  "ORANGE_YOLO_PERF_SAMPLE=${ORANGE_YOLO_PERF_SAMPLE:-1}"
  "ORANGE_CROP_COPY_TIMING=${ORANGE_CROP_COPY_TIMING:-0}"
  "ORANGE_CROP_STAGE_SOURCE=${ORANGE_CROP_STAGE_SOURCE:-1}"
  "ORANGE_ANALYTICS_EARLY_OWNED_FRAME=${ORANGE_ANALYTICS_EARLY_OWNED_FRAME:-1}"
  "ORANGE_YOLO_DETACH_INPUT=${YOLO_DETACH_INPUT}"
  "ORANGE_YOLO_AFFINITY_CAM_2010095=${YOLO_AFFINITY_CAM_2010095}"
  "ORANGE_YOLO_AFFINITY_CAM_2010096=${YOLO_AFFINITY_CAM_2010096}"
  "ORANGE_YOLO_READY_EVENT_FASTPATH=${ORANGE_YOLO_READY_EVENT_FASTPATH:-1}"
  "ORANGE_PTP_REGISTER_READ_DECIMATE=${PTP_REGISTER_READ_DECIMATE}"
  "ORANGE_DEFAULT_DETECT_ENGINE=${DETECT_ENGINE}"
  "ORANGE_GUI_CONFIG_DIR=${CONFIG_DIR}"
  "ORANGE_GUI_STREAM_DOWNSAMPLE=${GUI_STREAM_DOWNSAMPLE}"
  "ORANGE_DISPLAY_PREVIEW_MAX_FPS=${DISPLAY_PREVIEW_MAX_FPS}"
  "ORANGE_GUI_SWAP_INTERVAL=${GUI_SWAP_INTERVAL}"
  "ORANGE_GUI_FRAME_MAX_FPS=${GUI_FRAME_MAX_FPS}"
  "ORANGE_GUI_SHOW_SPEED_GRAPHS=${GUI_SHOW_SPEED_GRAPHS}"
  "ORANGE_GUI_CAMERA_STARTUP_CONCURRENCY=${GUI_CAMERA_STARTUP_CONCURRENCY}"
  "ORANGE_GUI_AUTORUN=${GUI_AUTORUN}"
  "ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=${GUI_AUTORUN_STREAM_WARMUP_SECONDS}"
  "ORANGE_GUI_AUTORUN_RECORD_SECONDS=${GUI_AUTORUN_RECORD_SECONDS}"
  "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=${GUI_AUTORUN_EXIT_AFTER_FINALIZE}"
  "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=${GUI_AUTORUN_HIDE_CROP_PREVIEW}"
  "ORANGE_CROP_RECORDING_SINK_MODE=${CROP_RECORDING_SINK_MODE}"
  "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH}"
  "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=${CROP_EXTERNAL_REQUIRE_SEPARATE_GPU}"
)
while IFS= read -r var_name; do
  [[ -n "${var_name}" ]] || continue
  case "${var_name}" in
    ORANGE_YOLO_AFFINITY_CAM_2010095|ORANGE_YOLO_AFFINITY_CAM_2010096)
      continue
      ;;
  esac
  ENV_ARGS+=("${var_name}=${!var_name}")
done < <(compgen -e ORANGE_YOLO_AFFINITY_CAM_ | sort)
if [[ -n "${ORANGE_YOLO_RT_PRIORITY:-}" ]]; then
  ENV_ARGS+=("ORANGE_YOLO_RT_PRIORITY=${ORANGE_YOLO_RT_PRIORITY}")
fi
if [[ -n "${ORANGE_YOLO_RT_POLICY:-}" ]]; then
  ENV_ARGS+=("ORANGE_YOLO_RT_POLICY=${ORANGE_YOLO_RT_POLICY}")
fi
while IFS= read -r var_name; do
  [[ -n "${var_name}" ]] || continue
  ENV_ARGS+=("${var_name}=${!var_name}")
done < <(compgen -e ORANGE_YOLO_RT_PRIORITY_CAM_ | sort)
if [[ -n "${ORANGE_RECORDING_DETECT_PRIORITY:-}" ]]; then
  ENV_ARGS+=("ORANGE_RECORDING_DETECT_PRIORITY=${ORANGE_RECORDING_DETECT_PRIORITY}")
fi
if [[ -n "${APP_CONFIG_ENV_KEY}" ]]; then
  ENV_ARGS+=("${APP_CONFIG_ENV_KEY}=${APP_CONFIG_ENV_VALUE}")
fi
if [[ -n "${GUI_RECORD_FOR_SECONDS}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_RECORD_FOR_SECONDS=${GUI_RECORD_FOR_SECONDS}")
fi
if [[ -n "${GUI_CLIP_SECONDS}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_CLIP_SECONDS=${GUI_CLIP_SECONDS}")
fi
ENV_ARGS+=("ORANGE_GUI_AUTORUN_ENABLE_STREAM=${GUI_AUTORUN_ENABLE_STREAM}")
ENV_ARGS+=("ORANGE_GUI_AUTORUN_ENABLE_RECORD=${GUI_AUTORUN_ENABLE_RECORD}")
ENV_ARGS+=("ORANGE_GUI_AUTORUN_ENABLE_YOLO=${GUI_AUTORUN_ENABLE_YOLO}")
ENV_ARGS+=("ORANGE_GUI_AUTORUN_ENABLE_CROP=${GUI_AUTORUN_ENABLE_CROP}")
ENV_ARGS+=("ORANGE_GUI_AUTORUN_START_RECORDING=${GUI_AUTORUN_START_RECORDING}")
ENV_ARGS+=("ORANGE_GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP=${GUI_AUTORUN_STOP_STREAMING_AFTER_WARMUP}")
ENV_ARGS+=("ORANGE_GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS=${GUI_AUTORUN_CANCEL_STREAM_STARTUP_AFTER_MS}")
if [[ -n "${WAYLAND_DISPLAY_ENV}" ]]; then
  ENV_ARGS+=("WAYLAND_DISPLAY=${WAYLAND_DISPLAY_ENV}")
fi
if [[ -n "${XDG_RUNTIME_DIR_ENV}" ]]; then
  ENV_ARGS+=("XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR_ENV}")
fi
if [[ -n "${XDG_SESSION_TYPE_ENV}" ]]; then
  ENV_ARGS+=("XDG_SESSION_TYPE=${XDG_SESSION_TYPE_ENV}")
fi
if [[ -n "${ORANGE_GUI_RECORDING_SINK_MODE:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_RECORDING_SINK_MODE=${ORANGE_GUI_RECORDING_SINK_MODE}")
fi
if [[ -n "${ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT=${ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT}")
fi
if [[ -n "${ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT_PATH:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT_PATH=${ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT_PATH}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_SOCKET:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_SOCKET=${ORANGE_LOCAL_CONTROL_SOCKET}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_SOCKET:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_SOCKET=${ORANGE_GUI_LOCAL_CONTROL_SOCKET}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_LOG:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_LOG=${ORANGE_LOCAL_CONTROL_LOG}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_LOG:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_LOG=${ORANGE_GUI_LOCAL_CONTROL_LOG}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_DISABLE:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_DISABLE=${ORANGE_LOCAL_CONTROL_DISABLE}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_DISABLE:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_DISABLE=${ORANGE_GUI_LOCAL_CONTROL_DISABLE}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START=${ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=${ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP=${ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=${ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP=${ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP=${ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_EXIT_AFTER_FINALIZE:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=${ORANGE_LOCAL_CONTROL_EXIT_AFTER_FINALIZE}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE=${ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS=${ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS=${ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS}")
fi
if [[ -n "${ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS:-}" ]]; then
  ENV_ARGS+=("ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS=${ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS}")
fi
if [[ -n "${ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS=${ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS}")
fi
while IFS= read -r var_name; do
  [[ -n "${var_name}" ]] || continue
  ENV_ARGS+=("${var_name}=${!var_name}")
done < <(compgen -e ORANGE_GUI_GUIDED_CAPTURE_ | sort)
while IFS= read -r var_name; do
  [[ -n "${var_name}" ]] || continue
  ENV_ARGS+=("${var_name}=${!var_name}")
done < <(compgen -e ORANGE_GUI_ARENA_CENTERING_ | sort)
for var_name in \
  ORANGE_GUI_OPERATOR_MONITOR \
  ORANGE_GUI_RESERVED_MONITOR \
  ORANGE_CITRUS_EXPECTED_STIMULUS_MONITOR \
  ORANGE_GUI_PROJECTED_SURFACE_SCALE_TARGETS_READY \
  ORANGE_GUI_ACCEPT_PROJECTED_SURFACE_SCALES_ARMED \
  ORANGE_GUI_GROUP_CAPTURE_POST_PRESENTATION_SETTLE_MS; do
  if [[ -n "${!var_name:-}" ]]; then
    ENV_ARGS+=("${var_name}=${!var_name}")
  fi
done
if [[ -n "${ORANGE_CROP_PREVIEW_MAX_FPS:-}" ]]; then
  ENV_ARGS+=("ORANGE_CROP_PREVIEW_MAX_FPS=${ORANGE_CROP_PREVIEW_MAX_FPS}")
fi
if [[ -n "${ORANGE_CROP_PREVIEW_DISABLE:-}" ]]; then
  ENV_ARGS+=("ORANGE_CROP_PREVIEW_DISABLE=${ORANGE_CROP_PREVIEW_DISABLE}")
fi
if [[ -n "${CROP_FRAME_POOL_SIZE}" ]]; then
  ENV_ARGS+=("ORANGE_CROP_FRAME_POOL_SIZE=${CROP_FRAME_POOL_SIZE}")
fi
if [[ -n "${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID:-}" ]]; then
  ENV_ARGS+=("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID=${ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID}")
fi
while IFS= read -r var_name; do
  [[ -n "${var_name}" ]] || continue
  ENV_ARGS+=("${var_name}=${!var_name}")
done < <(compgen -e ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_ | sort)

if [[ "${ORANGE_GUI_PRINT_EXEC_ENV_ONLY:-0}" == "1" ]]; then
  printf '%s\n' "${ENV_ARGS[@]}"
  exit 0
fi

if [[ -z "${DISPLAY_ENV}" && -z "${WAYLAND_DISPLAY_ENV}" && "${ORANGE_GUI_ALLOW_NO_DISPLAY:-0}" != "1" ]]; then
  cat >&2 <<'EOF'
No GUI display session detected: DISPLAY and WAYLAND_DISPLAY are both unset.

Run this launcher from a graphical terminal, or refresh the tmux environment
from one before launching Orange. For X11/XWayland sessions, also allow the
root-launched Orange process to connect to the display:

  xhost +SI:localuser:root

Useful tmux refresh commands from the graphical terminal:

  tmux set-environment -g DISPLAY "$DISPLAY"
  tmux set-environment -g XAUTHORITY "${XAUTHORITY:-$HOME/.Xauthority}"
  tmux set-environment -g WAYLAND_DISPLAY "$WAYLAND_DISPLAY"
  tmux set-environment -g XDG_RUNTIME_DIR "$XDG_RUNTIME_DIR"
  tmux set-environment -g XDG_SESSION_TYPE "$XDG_SESSION_TYPE"

Set ORANGE_GUI_ALLOW_NO_DISPLAY=1 only for non-performance smoke diagnostics.
EOF
  exit 1
fi

if [[ "${GUI_USE_PRIVILEGE_WRAPPER}" == "1" || ( "${GUI_USE_PRIVILEGE_WRAPPER}" == "auto" && -x "${GUI_PRIVILEGE_WRAPPER}" ) ]]; then
  if [[ ! -x "${GUI_PRIVILEGE_WRAPPER}" ]]; then
    echo "Requested GUI privilege wrapper is not executable: ${GUI_PRIVILEGE_WRAPPER}" >&2
    echo "Install it with: scripts/install_orange_gui_validation_wrapper.sh --install-sudoers" >&2
    exit 1
  fi
  WRAPPER_SUPPORTS_PTP_STACK=0
  if "${GUI_PRIVILEGE_WRAPPER}" --help | grep -q -- "--ptp-stack-mode"; then
    WRAPPER_SUPPORTS_PTP_STACK=1
  fi
  if [[ "${GUI_PTP_STACK_MODE}" != "off" && "${WRAPPER_SUPPORTS_PTP_STACK}" != "1" ]]; then
    cat >&2 <<EOF
Installed GUI privilege wrapper does not support --ptp-stack-mode.

Reinstall it before running autorun PTP validation:

  sudo scripts/install_orange_gui_validation_wrapper.sh --install-sudoers
EOF
    exit 1
  fi
  for env_arg in "${ENV_ARGS[@]}"; do
    env_key="${env_arg%%=*}"
    env_label="${env_key}"
    case "${env_key}" in
      ORANGE_YOLO_AFFINITY_CAM_*)
        env_label="ORANGE_YOLO_AFFINITY_CAM_*"
        ;;
      ORANGE_YOLO_RT_PRIORITY_CAM_*)
        env_label="ORANGE_YOLO_RT_PRIORITY_CAM_*"
        ;;
      ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*)
        env_label="ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_*"
        ;;
    esac
    if ! "${GUI_PRIVILEGE_WRAPPER}" \
        --dry-run \
        --orange-bin "${ORANGE_BIN}" \
        --env "${env_arg}" >/dev/null; then
      cat >&2 <<EOF
Installed GUI privilege wrapper does not support ${env_label}.

Reinstall it before running GUI validation:

  sudo scripts/install_orange_gui_validation_wrapper.sh --install-sudoers
EOF
      exit 1
    fi
  done
  WRAPPER_ARGS=("--orange-bin" "${ORANGE_BIN}")
  if [[ "${WRAPPER_SUPPORTS_PTP_STACK}" == "1" ]]; then
    WRAPPER_ARGS+=("--ptp-stack-mode" "${GUI_PTP_STACK_MODE}")
  fi
  for env_arg in "${ENV_ARGS[@]}"; do
    WRAPPER_ARGS+=("--env" "${env_arg}")
  done
  exec sudo -n "${GUI_PRIVILEGE_WRAPPER}" "${WRAPPER_ARGS[@]}"
fi

if [[ "${GUI_PTP_STACK_MODE}" != "off" ]]; then
  PTP_STACK_SCRIPT="${REPO_ROOT}/scripts/ptp_stack.sh"
  if [[ ! -x "${PTP_STACK_SCRIPT}" ]]; then
    echo "PTP stack script is missing or not executable: ${PTP_STACK_SCRIPT}" >&2
    exit 1
  fi
  PTP_STATUS="$(sudo -n "${PTP_STACK_SCRIPT}" status 2>&1)" || {
    echo "Failed to query host PTP stack with sudo -n." >&2
    printf '%s\n' "${PTP_STATUS}" >&2
    echo "Install/use the GUI privilege wrapper or start PTP manually from an interactive shell." >&2
    exit 1
  }
  PTP_HEALTHY=0
  if [[ "${PTP_STATUS}" != *"(no ptp4l/phc2sys process)"* &&
        "${PTP_STATUS}" == *"ptp4l"* &&
        "${PTP_STATUS}" == *"phc2sys"* &&
        "${PTP_STATUS}" != *"(socket "*" not found)"* &&
        "${PTP_STATUS}" == *"sending: GET TIME_STATUS_NP"* ]]; then
    PTP_HEALTHY=1
  fi
  if [[ "${PTP_HEALTHY}" != "1" && "${GUI_PTP_STACK_MODE}" == "require" ]]; then
    echo "Host PTP stack is not healthy and ORANGE_GUI_PTP_STACK_MODE=require was used." >&2
    printf '%s\n' "${PTP_STATUS}" >&2
    exit 1
  fi
  if [[ "${PTP_HEALTHY}" != "1" ]]; then
    printf '%s\n' "${PTP_STATUS}"
    sudo -n "${PTP_STACK_SCRIPT}" start
    PTP_STATUS_AFTER="$(sudo -n "${PTP_STACK_SCRIPT}" status 2>&1)" || {
      echo "Failed to recheck host PTP stack with sudo -n after start." >&2
      printf '%s\n' "${PTP_STATUS_AFTER}" >&2
      exit 1
    }
    if [[ "${PTP_STATUS_AFTER}" == *"(no ptp4l/phc2sys process)"* ||
          "${PTP_STATUS_AFTER}" != *"ptp4l"* ||
          "${PTP_STATUS_AFTER}" != *"phc2sys"* ||
          "${PTP_STATUS_AFTER}" == *"(socket "*" not found)"* ||
          "${PTP_STATUS_AFTER}" != *"sending: GET TIME_STATUS_NP"* ]]; then
      echo "Host PTP stack is still not healthy after start." >&2
      printf '%s\n' "${PTP_STATUS_AFTER}" >&2
      exit 1
    fi
  fi
fi

exec sudo env "${ENV_ARGS[@]}" "${ORANGE_BIN}"
