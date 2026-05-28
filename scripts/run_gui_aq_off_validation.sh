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
GUI_STREAM_DOWNSAMPLE="${ORANGE_GUI_STREAM_DOWNSAMPLE:-4}"
DISPLAY_PREVIEW_MAX_FPS="${ORANGE_DISPLAY_PREVIEW_MAX_FPS:-15}"
GUI_SWAP_INTERVAL="${ORANGE_GUI_SWAP_INTERVAL:-0}"
GUI_FRAME_MAX_FPS="${ORANGE_GUI_FRAME_MAX_FPS:-60}"
GUI_SHOW_SPEED_GRAPHS="${ORANGE_GUI_SHOW_SPEED_GRAPHS:-0}"
GUI_AUTORUN="${ORANGE_GUI_AUTORUN:-0}"
GUI_AUTORUN_STREAM_WARMUP_SECONDS="${ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS:-3}"
GUI_AUTORUN_RECORD_SECONDS="${ORANGE_GUI_AUTORUN_RECORD_SECONDS:-10}"
GUI_AUTORUN_EXIT_AFTER_FINALIZE="${ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE:-0}"
GUI_AUTORUN_HIDE_CROP_PREVIEW="${ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW:-0}"
GUI_AUTORUN_ENABLE_STREAM="${ORANGE_GUI_AUTORUN_ENABLE_STREAM:-1}"
GUI_AUTORUN_ENABLE_RECORD="${ORANGE_GUI_AUTORUN_ENABLE_RECORD:-1}"
GUI_AUTORUN_ENABLE_YOLO="${ORANGE_GUI_AUTORUN_ENABLE_YOLO:-1}"
GUI_AUTORUN_ENABLE_CROP="${ORANGE_GUI_AUTORUN_ENABLE_CROP:-1}"
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
DEFAULT_DETECT_ENGINE="/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a16_gpu5_trt100_fp16_bo5_avg32.engine"
DETECT_ENGINE="${ORANGE_GUI_DETECT_ENGINE:-${DEFAULT_DETECT_ENGINE}}"
APP_CONFIG_PATH="${ORANGE_GUI_APP_CONFIG_PATH:-${HOME}/orange_data/config/app/default.json}"

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
  EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS="--require-external-recorder-status"
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
if [[ -n "${CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER}" ]]; then
  EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS+=" --max-external-crop-encode-queue-high-water ${CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER}"
fi
if [[ -n "${CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS}" ]]; then
  EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS+=" --max-external-crop-enqueue-age-p95-ms ${CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS}"
fi
COMPARE_VALIDATION_FLAGS="--require-pass --require-zero-crop-drops --require-visible-samples --require-hidden-samples --require-matching-cameras --require-matching-display-config --require-matching-crop-config --min-gui-visible-p05-fps 45 --min-gui-hidden-p05-fps 45"
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
  COMPARE_VALIDATION_FLAGS+=" --require-external-recorder-status"
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
gui_record_for_seconds_raw = sys.argv[22]
gui_clip_seconds_raw = sys.argv[23]
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
  ORANGE_DEFAULT_DETECT_ENGINE=${DETECT_ENGINE}
  ORANGE_GUI_RECORDING_SINK_MODE=${ORANGE_GUI_RECORDING_SINK_MODE:-<app config/default>}
  ORANGE_GUI_STREAM_DOWNSAMPLE=${GUI_STREAM_DOWNSAMPLE}
  ORANGE_DISPLAY_PREVIEW_MAX_FPS=${DISPLAY_PREVIEW_MAX_FPS}
  ORANGE_GUI_SWAP_INTERVAL=${GUI_SWAP_INTERVAL}
  ORANGE_GUI_FRAME_MAX_FPS=${GUI_FRAME_MAX_FPS}
  ORANGE_GUI_SHOW_SPEED_GRAPHS=${GUI_SHOW_SPEED_GRAPHS}
  ORANGE_GUI_AUTORUN=${GUI_AUTORUN}
  ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=${GUI_AUTORUN_STREAM_WARMUP_SECONDS}
  ORANGE_GUI_AUTORUN_RECORD_SECONDS=${GUI_AUTORUN_RECORD_SECONDS}
  ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=${GUI_AUTORUN_EXIT_AFTER_FINALIZE}
  ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=${GUI_AUTORUN_HIDE_CROP_PREVIEW}
  ORANGE_GUI_AUTORUN_ENABLE_STREAM=${GUI_AUTORUN_ENABLE_STREAM}
  ORANGE_GUI_AUTORUN_ENABLE_RECORD=${GUI_AUTORUN_ENABLE_RECORD}
  ORANGE_GUI_AUTORUN_ENABLE_YOLO=${GUI_AUTORUN_ENABLE_YOLO}
  ORANGE_GUI_AUTORUN_ENABLE_CROP=${GUI_AUTORUN_ENABLE_CROP}
  ORANGE_GUI_RECORD_FOR_SECONDS=${GUI_RECORD_FOR_SECONDS:-<app config/disabled>}
  ORANGE_GUI_CLIP_SECONDS=${GUI_CLIP_SECONDS:-<app config/disabled>}
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
For a compact artifact health, crop fanout, and GUI timing summary, use:
  scripts/summarize_gui_validation.py --latest-complete
For full-frame GUI external IPC rolling runs, validate the external recorder
session and mirrored rolling manifest with:
  scripts/verify_external_recorder_session.py --analytics-root <recording_folder>

For crop-recording plus crop-preview validation, use:
  scripts/validate_gui_ptp_recording.py --latest-complete --require-crop-recording-artifacts --require-crop-preview-counters --require-crop-preview-sampling --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 0 --expect-crop-preview-display-enabled 1 --min-crop-frame-pool-size ${CROP_FRAME_POOL_VALIDATION_MIN} ${EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS} ${EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-gui-swap-interval ${GUI_SWAP_INTERVAL} --expect-gui-frame-max-fps ${GUI_FRAME_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --min-gui-crop-preview-visible-fps-p05 45 --json-out /tmp/orange_gui_crop_visible_validation.json
For a run where crop preview windows were hidden at finalization, use:
  scripts/validate_gui_ptp_recording.py --latest-complete --require-crop-recording-artifacts --require-crop-preview-counters --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 0 --expect-crop-preview-display-enabled 0 --min-crop-frame-pool-size ${CROP_FRAME_POOL_VALIDATION_MIN} ${EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS} ${EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-gui-swap-interval ${GUI_SWAP_INTERVAL} --expect-gui-frame-max-fps ${GUI_FRAME_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --min-gui-crop-preview-hidden-fps-p05 45 --json-out /tmp/orange_gui_crop_hidden_validation.json
For a run with ORANGE_CROP_PREVIEW_DISABLE=1, use:
  scripts/validate_gui_ptp_recording.py --latest-complete --require-crop-recording-artifacts --require-crop-preview-counters --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 1 --min-crop-frame-pool-size ${CROP_FRAME_POOL_VALIDATION_MIN} ${EXTERNAL_CROP_QUEUE_VALIDATION_FLAGS} ${EXTERNAL_RECORDER_STATUS_VALIDATION_FLAGS} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-gui-swap-interval ${GUI_SWAP_INTERVAL} --expect-gui-frame-max-fps ${GUI_FRAME_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --json-out /tmp/orange_gui_crop_disabled_validation.json
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
  "ORANGE_YOLO_AFFINITY_CAM_2010095=${ORANGE_YOLO_AFFINITY_CAM_2010095:-2}"
  "ORANGE_YOLO_AFFINITY_CAM_2010096=${ORANGE_YOLO_AFFINITY_CAM_2010096:-4}"
  "ORANGE_YOLO_READY_EVENT_FASTPATH=${ORANGE_YOLO_READY_EVENT_FASTPATH:-1}"
  "ORANGE_PTP_REGISTER_READ_DECIMATE=${PTP_REGISTER_READ_DECIMATE}"
  "ORANGE_DEFAULT_DETECT_ENGINE=${DETECT_ENGINE}"
  "ORANGE_GUI_CONFIG_DIR=${CONFIG_DIR}"
  "ORANGE_GUI_STREAM_DOWNSAMPLE=${GUI_STREAM_DOWNSAMPLE}"
  "ORANGE_DISPLAY_PREVIEW_MAX_FPS=${DISPLAY_PREVIEW_MAX_FPS}"
  "ORANGE_GUI_SWAP_INTERVAL=${GUI_SWAP_INTERVAL}"
  "ORANGE_GUI_FRAME_MAX_FPS=${GUI_FRAME_MAX_FPS}"
  "ORANGE_GUI_SHOW_SPEED_GRAPHS=${GUI_SHOW_SPEED_GRAPHS}"
  "ORANGE_GUI_AUTORUN=${GUI_AUTORUN}"
  "ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=${GUI_AUTORUN_STREAM_WARMUP_SECONDS}"
  "ORANGE_GUI_AUTORUN_RECORD_SECONDS=${GUI_AUTORUN_RECORD_SECONDS}"
  "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=${GUI_AUTORUN_EXIT_AFTER_FINALIZE}"
  "ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=${GUI_AUTORUN_HIDE_CROP_PREVIEW}"
  "ORANGE_CROP_RECORDING_SINK_MODE=${CROP_RECORDING_SINK_MODE}"
  "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH}"
  "ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=${CROP_EXTERNAL_REQUIRE_SEPARATE_GPU}"
)
if [[ -n "${GUI_RECORD_FOR_SECONDS}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_RECORD_FOR_SECONDS=${GUI_RECORD_FOR_SECONDS}")
fi
if [[ -n "${GUI_CLIP_SECONDS}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_CLIP_SECONDS=${GUI_CLIP_SECONDS}")
fi
if [[ -n "${ORANGE_GUI_AUTORUN_ENABLE_STREAM:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_AUTORUN_ENABLE_STREAM=${GUI_AUTORUN_ENABLE_STREAM}")
fi
if [[ -n "${ORANGE_GUI_AUTORUN_ENABLE_RECORD:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_AUTORUN_ENABLE_RECORD=${GUI_AUTORUN_ENABLE_RECORD}")
fi
if [[ -n "${ORANGE_GUI_AUTORUN_ENABLE_YOLO:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_AUTORUN_ENABLE_YOLO=${GUI_AUTORUN_ENABLE_YOLO}")
fi
if [[ -n "${ORANGE_GUI_AUTORUN_ENABLE_CROP:-}" ]]; then
  ENV_ARGS+=("ORANGE_GUI_AUTORUN_ENABLE_CROP=${GUI_AUTORUN_ENABLE_CROP}")
fi
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
  if ! "${GUI_PRIVILEGE_WRAPPER}" \
      --dry-run \
      --orange-bin "${ORANGE_BIN}" \
      --env "ORANGE_GUI_FRAME_MAX_FPS=${GUI_FRAME_MAX_FPS}" >/dev/null; then
    cat >&2 <<EOF
Installed GUI privilege wrapper does not support ORANGE_GUI_FRAME_MAX_FPS.

Reinstall it before running capped GUI validation:

  sudo scripts/install_orange_gui_validation_wrapper.sh --install-sudoers
EOF
    exit 1
  fi
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
