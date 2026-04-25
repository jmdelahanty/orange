#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORANGE_BIN="${ORANGE_BIN:-${REPO_ROOT}/targets/release/orange}"
CONFIG_DIR="${ORANGE_GUI_CONFIG_DIR:-/home/jeremy/orange_data/config/local/100_cam4_ptp}"
CONFIG_NAME="${ORANGE_GUI_CONFIG_NAME:-$(basename "${CONFIG_DIR}")}"
EXPECT_SYNC_MODE="${ORANGE_GUI_EXPECT_SYNC_MODE:-ptp_gate}"
EXPECT_PTP_ENABLED="${ORANGE_GUI_EXPECT_PTP_ENABLED:-1}"

if [[ ! -x "${ORANGE_BIN}" ]]; then
  echo "Missing executable: ${ORANGE_BIN}" >&2
  echo "Build it first: cmake --build ${REPO_ROOT}/targets/release -j 8" >&2
  exit 1
fi

python3 - "${CONFIG_DIR}" "${EXPECT_SYNC_MODE}" "${EXPECT_PTP_ENABLED}" <<'PY'
import json
import sys
from pathlib import Path

config_dir = Path(sys.argv[1])
expect_sync_mode = sys.argv[2]
expect_ptp_enabled_raw = sys.argv[3]
expect_ptp_enabled = None
if expect_ptp_enabled_raw:
    expect_ptp_enabled = expect_ptp_enabled_raw not in {"0", "false", "False", "no", "No"}
expected = ["2010095.json", "2010096.json"]
errors = []

if not config_dir.is_dir():
    errors.append(f"config folder does not exist: {config_dir}")
else:
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

if errors:
    for error in errors:
        print(error, file=sys.stderr)
    sys.exit(1)
PY

cat <<EOF
Launching Orange GUI for AQ-off validation.

Before opening cameras in the GUI:
  1. In the Local panel, select: ${CONFIG_NAME}
  2. Open cameras.
  3. Confirm recording defaults show: AQ off, temporal AQ off.
  4. Confirm sync mode is PTP gate if the GUI displays it.
  5. Run the normal two-camera recording test.

Validated config folder:
  ${CONFIG_DIR}
EOF

if [[ "${ORANGE_GUI_VALIDATE_ONLY:-0}" == "1" ]]; then
  exit 0
fi

cd "${REPO_ROOT}"
exec sudo env \
  DISPLAY="${DISPLAY:-}" \
  XAUTHORITY="${XAUTHORITY:-${HOME}/.Xauthority}" \
  ORANGE_YOLO_PERF_LOG="${ORANGE_YOLO_PERF_LOG:-1}" \
  ORANGE_YOLO_PERF_SAMPLE="${ORANGE_YOLO_PERF_SAMPLE:-1}" \
  ORANGE_CROP_COPY_TIMING="${ORANGE_CROP_COPY_TIMING:-0}" \
  ORANGE_CROP_STAGE_SOURCE="${ORANGE_CROP_STAGE_SOURCE:-1}" \
  ORANGE_ANALYTICS_EARLY_OWNED_FRAME="${ORANGE_ANALYTICS_EARLY_OWNED_FRAME:-1}" \
  ORANGE_YOLO_AFFINITY_CAM_2010095="${ORANGE_YOLO_AFFINITY_CAM_2010095:-2}" \
  ORANGE_YOLO_AFFINITY_CAM_2010096="${ORANGE_YOLO_AFFINITY_CAM_2010096:-4}" \
  ORANGE_YOLO_READY_EVENT_FASTPATH="${ORANGE_YOLO_READY_EVENT_FASTPATH:-1}" \
  "${ORANGE_BIN}"
