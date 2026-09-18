#!/usr/bin/env bash
# Raw Mono8 frame capture for INT8 calibration (run with sudo).
#
#   scripts/evt_dump_frames.sh <serial> <set> [seconds] [every] [max]
#     set      subfolder name, e.g. empty or fish
#     seconds  stream time (default 20 for empty, 100 otherwise)
#     every    dump period in frames (default 20 for empty, 10 otherwise)
#     max      frames to write (default 100 for empty, 300 otherwise)
#
# Frames land in $ORANGE_CALIB_ROOT/calibration_raw_<today>/<set>/Cam<serial>/ as PGM.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/targets/release/evt_stream_smoke"
CONFIG_DIR="${ORANGE_CALIB_CONFIG_DIR:-/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam}"
CALIB_ROOT="${ORANGE_CALIB_ROOT:-/home/jeremy/orange_data/model_sources/detect/detect_all_available_detect_training_v004_yolo11n_trt_20260520}"
SERIAL="${1:?serial}"; SET="${2:?set name (empty|fish)}"
if [[ "$SET" == "empty" ]]; then D_SECS=20; D_EVERY=20; D_MAX=100; else D_SECS=100; D_EVERY=10; D_MAX=300; fi
SECS="${3:-$D_SECS}"; EVERY="${4:-$D_EVERY}"; MAX="${5:-$D_MAX}"
OUT="$CALIB_ROOT/calibration_raw_$(date +%Y%m%d)/$SET/Cam$SERIAL"
mkdir -p "$OUT"
# Keep the files owned by the invoking user when run through sudo.
exec stdbuf -oL "$BIN" --config-dir "$CONFIG_DIR" --serial "$SERIAL" --measure-seconds "$SECS" --buffer-count 8 \
  --dump-dir "$OUT" --dump-every "$EVERY" --dump-max "$MAX" 2>&1 | grep -E 'DUMP|MEASURE|RESULT|FAIL|rror'
