#!/usr/bin/env bash
# Raw Mono8 frame capture for INT8 calibration (run with sudo).
#
#   scripts/evt_dump_frames.sh <serial|all> <set> [seconds] [every] [max]
#     all      capture the four cameras at once; the IR-light camera (2010096) starts first
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
SERIAL="${1:?serial, or all}"; SET="${2:?set name (empty|fish)}"
if [[ "$SET" == "empty" ]]; then D_SECS=20; D_EVERY=20; D_MAX=100; else D_SECS=100; D_EVERY=10; D_MAX=300; fi
SECS="${3:-$D_SECS}"; EVERY="${4:-$D_EVERY}"; MAX="${5:-$D_MAX}"
SET_ROOT="$CALIB_ROOT/calibration_raw_$(date +%Y%m%d)"
LIGHT_CAMERA="${ORANGE_LIGHT_CAMERA:-2010096}"   # its GPO drives the IR lights; it must stream whenever any other camera does
ALL_CAMERAS="${ORANGE_CALIB_CAMERAS:-2010096 2010093 2010094 2010095}"

capture_one() {
  local serial="$1" secs="$2" out="$SET_ROOT/$SET/Cam$1"
  mkdir -p "$out"
  stdbuf -oL "$BIN" --config-dir "$CONFIG_DIR" --serial "$serial" --measure-seconds "$secs" --buffer-count 8 \
    --dump-dir "$out" --dump-every "$EVERY" --dump-max "$MAX" 2>&1 | sed -u "s/^/[$serial] /" | grep -E 'DUMP|MEASURE|RESULT|FAIL|rror'
  return "${PIPESTATUS[0]}"
}

set +e
if [[ "$SERIAL" == "all" ]]; then
  # All four at once. The light camera starts first and streams a little
  # longer so the IR lights are on for the whole capture on every camera;
  # the others start as soon as it reports its stream open (about 8 s).
  capture_one "$LIGHT_CAMERA" "$((SECS + 15))" &
  light_pid=$!
  sleep 12
  pids=()
  for cam in $ALL_CAMERAS; do
    [[ "$cam" == "$LIGHT_CAMERA" ]] && continue
    capture_one "$cam" "$SECS" &
    pids+=($!)
  done
  status=0
  for p in "${pids[@]}" "$light_pid"; do wait "$p" || status=1; done
else
  capture_one "$SERIAL" "$SECS"
  status=$?
fi
set -e
# Same handoff as orange_local_benchmark_wrapper.sh: give the capture tree
# back to the invoking user when run through sudo.
if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" && -d "$SET_ROOT" ]]; then
  chown -R "${SUDO_UID}:${SUDO_GID}" "$SET_ROOT"
fi
exit "$status"
