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
GUI_SHOW_SPEED_GRAPHS="${ORANGE_GUI_SHOW_SPEED_GRAPHS:-0}"
CROP_PREVIEW_VALIDATION_MAX_FPS="${ORANGE_CROP_PREVIEW_MAX_FPS:-15}"
CROP_RECORDING_SINK_MODE="${ORANGE_CROP_RECORDING_SINK_MODE:-in_process}"
CROP_EXTERNAL_ENCODE_QUEUE_DEPTH="${ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH:-256}"
DEFAULT_DETECT_ENGINE="/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a16_gpu5_trt100_fp16_bo5_avg32.engine"
DETECT_ENGINE="${ORANGE_GUI_DETECT_ENGINE:-${DEFAULT_DETECT_ENGINE}}"
APP_CONFIG_PATH="${ORANGE_GUI_APP_CONFIG_PATH:-${HOME}/orange_data/config/app/default.json}"

if [[ ! -x "${ORANGE_BIN}" ]]; then
  echo "Missing executable: ${ORANGE_BIN}" >&2
  echo "Build it first: cmake --build ${REPO_ROOT}/targets/release -j 8" >&2
  exit 1
fi

python3 - "${CONFIG_DIR}" "${EXPECT_SYNC_MODE}" "${EXPECT_PTP_ENABLED}" "${PTP_REGISTER_READ_DECIMATE}" "${DETECT_ENGINE}" "${APP_CONFIG_PATH}" "${EXPECT_CAMERAS}" <<'PY'
import json
import sys
from pathlib import Path

config_dir = Path(sys.argv[1])
expect_sync_mode = sys.argv[2]
expect_ptp_enabled_raw = sys.argv[3]
ptp_register_read_decimate_raw = sys.argv[4]
detect_engine = sys.argv[5]
app_config_path = Path(sys.argv[6]).expanduser()
expect_cameras_raw = sys.argv[7]
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

try:
    ptp_register_read_decimate = int(ptp_register_read_decimate_raw)
    if ptp_register_read_decimate < 1:
        errors.append("ORANGE_PTP_REGISTER_READ_DECIMATE must be >= 1")
except ValueError:
    errors.append("ORANGE_PTP_REGISTER_READ_DECIMATE must be an integer")

if detect_engine:
    detect_engine_path = Path(detect_engine).expanduser()
    if not detect_engine_path.is_file():
        errors.append(f"detect engine does not exist: {detect_engine_path}")

if app_config_path.exists():
    try:
        app_config = json.loads(app_config_path.read_text())
        app_engine = app_config.get("models", {}).get("default_detect_engine", "")
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
  ORANGE_GUI_SHOW_SPEED_GRAPHS=${GUI_SHOW_SPEED_GRAPHS}
  ORANGE_CROP_PREVIEW_MAX_FPS=${ORANGE_CROP_PREVIEW_MAX_FPS:-<camera config/default>}
  ORANGE_CROP_PREVIEW_DISABLE=${ORANGE_CROP_PREVIEW_DISABLE:-0}
  ORANGE_CROP_FRAME_POOL_SIZE=${ORANGE_CROP_FRAME_POOL_SIZE:-<orange default>}
  ORANGE_CROP_RECORDING_SINK_MODE=${CROP_RECORDING_SINK_MODE}
  ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH}

After recording, validate the artifact with:
  scripts/validate_gui_ptp_recording.py <recording_folder>
Or validate the newest artifact with:
  scripts/validate_gui_ptp_recording.py --latest
Or validate the newest real recording artifact with:
  scripts/validate_gui_ptp_recording.py --latest-complete
For a compact artifact health, crop fanout, and GUI timing summary, use:
  scripts/summarize_gui_validation.py --latest-complete

For crop-recording plus crop-preview validation, use:
  scripts/validate_gui_ptp_recording.py --latest-complete --require-crop-recording-artifacts --require-crop-preview-counters --require-crop-preview-sampling --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 0 --expect-crop-preview-display-enabled 1 --min-crop-frame-pool-size 32 --expect-external-crop-encode-queue-depth ${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --min-gui-crop-preview-visible-fps-p05 45 --json-out /tmp/orange_gui_crop_visible_validation.json
For a run where crop preview windows were hidden at finalization, use:
  scripts/validate_gui_ptp_recording.py --latest-complete --require-crop-recording-artifacts --require-crop-preview-counters --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 0 --expect-crop-preview-display-enabled 0 --min-crop-frame-pool-size 32 --expect-external-crop-encode-queue-depth ${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --min-gui-crop-preview-hidden-fps-p05 45 --json-out /tmp/orange_gui_crop_hidden_validation.json
For a run with ORANGE_CROP_PREVIEW_DISABLE=1, use:
  scripts/validate_gui_ptp_recording.py --latest-complete --require-crop-recording-artifacts --require-crop-preview-counters --expect-crop-preview-max-fps ${CROP_PREVIEW_VALIDATION_MAX_FPS} --expect-crop-preview-disabled 1 --min-crop-frame-pool-size 32 --expect-external-crop-encode-queue-depth ${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH} --expect-gui-stream-downsample ${GUI_STREAM_DOWNSAMPLE} --expect-display-preview-max-fps ${DISPLAY_PREVIEW_MAX_FPS} --expect-yolo-speed-graphs-enabled ${GUI_SHOW_SPEED_GRAPHS} --require-gui-timing-telemetry --json-out /tmp/orange_gui_crop_disabled_validation.json
Then compare visible and hidden runs with:
  scripts/compare_gui_crop_preview_validation.py visible=/tmp/orange_gui_crop_visible_validation.json hidden=/tmp/orange_gui_crop_hidden_validation.json --require-pass --require-zero-crop-drops
EOF

if [[ "${ORANGE_GUI_VALIDATE_ONLY:-0}" == "1" ]]; then
  exit 0
fi

cd "${REPO_ROOT}"
ENV_ARGS=(
  "DISPLAY=${DISPLAY:-}"
  "XAUTHORITY=${XAUTHORITY:-${HOME}/.Xauthority}"
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
  "ORANGE_GUI_STREAM_DOWNSAMPLE=${GUI_STREAM_DOWNSAMPLE}"
  "ORANGE_DISPLAY_PREVIEW_MAX_FPS=${DISPLAY_PREVIEW_MAX_FPS}"
  "ORANGE_GUI_SHOW_SPEED_GRAPHS=${GUI_SHOW_SPEED_GRAPHS}"
  "ORANGE_CROP_RECORDING_SINK_MODE=${CROP_RECORDING_SINK_MODE}"
  "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=${CROP_EXTERNAL_ENCODE_QUEUE_DEPTH}"
)
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
if [[ -n "${ORANGE_CROP_FRAME_POOL_SIZE:-}" ]]; then
  ENV_ARGS+=("ORANGE_CROP_FRAME_POOL_SIZE=${ORANGE_CROP_FRAME_POOL_SIZE}")
fi

if [[ "${ORANGE_GUI_PRINT_EXEC_ENV_ONLY:-0}" == "1" ]]; then
  printf '%s\n' "${ENV_ARGS[@]}"
  exit 0
fi

exec sudo env "${ENV_ARGS[@]}" "${ORANGE_BIN}"
