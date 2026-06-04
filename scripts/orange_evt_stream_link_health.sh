#!/usr/bin/env bash
set -euo pipefail

CAMERA_IP=""
CONFIG_DIR="${ORANGE_GUI_CONFIG_DIR:-}"
SERIAL=""
MEASURE_SECONDS="5"
BUFFER_COUNT="64"
FRAME_RATE=""
GPU_DIRECT=""
OUT_DIR=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage:
  orange_evt_stream_link_health.sh --camera-ip <ip> --serial <serial> --config-dir <dir> [options]

Runs the installed orange-evt-stream-smoke diagnostic and captures before/after
NIC link counters for the Linux interface used to reach the camera.

Options:
  --camera-ip <ip>         Camera IPv4 address.
  --serial <serial>        Camera serial passed to orange-evt-stream-smoke.
  --config-dir <dir>       Camera config directory. Defaults to ORANGE_GUI_CONFIG_DIR.
  --measure-seconds <s>    Measurement duration. Default 5.
  --buffer-count <n>       EVT buffer count. Default 64.
  --frame-rate <fps>       Optional FrameRate override.
  --gpu-direct <0|1>       Optional GPUDirect override.
  --out-dir <dir>          Output directory. Default /tmp/orange_evt_link_health_<timestamp>.
  --dry-run                Print commands without running them.
  --help                   Show this message.
EOF
}

require_value() {
  local flag="$1"
  shift
  [[ $# -gt 0 ]] || { echo "$flag requires a value." >&2; exit 2; }
  printf '%s\n' "$1"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --camera-ip)
      shift
      CAMERA_IP="$(require_value --camera-ip "$@")"
      shift
      ;;
    --serial)
      shift
      SERIAL="$(require_value --serial "$@")"
      shift
      ;;
    --config-dir)
      shift
      CONFIG_DIR="$(require_value --config-dir "$@")"
      shift
      ;;
    --measure-seconds)
      shift
      MEASURE_SECONDS="$(require_value --measure-seconds "$@")"
      shift
      ;;
    --buffer-count)
      shift
      BUFFER_COUNT="$(require_value --buffer-count "$@")"
      shift
      ;;
    --frame-rate)
      shift
      FRAME_RATE="$(require_value --frame-rate "$@")"
      shift
      ;;
    --gpu-direct)
      shift
      GPU_DIRECT="$(require_value --gpu-direct "$@")"
      shift
      ;;
    --out-dir)
      shift
      OUT_DIR="$(require_value --out-dir "$@")"
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -n "$CAMERA_IP" ]] || { echo "--camera-ip is required." >&2; exit 2; }
[[ -n "$SERIAL" ]] || { echo "--serial is required." >&2; exit 2; }
[[ -n "$CONFIG_DIR" ]] || { echo "--config-dir or ORANGE_GUI_CONFIG_DIR is required." >&2; exit 2; }

if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="/tmp/orange_evt_link_health_$(date +%Y%m%d_%H%M%S)"
fi

iface="$(ip route get "$CAMERA_IP" | awk '/ dev / { for (i = 1; i <= NF; ++i) if ($i == "dev") { print $(i + 1); exit } }')"
[[ -n "$iface" ]] || { echo "Could not resolve interface for $CAMERA_IP." >&2; exit 1; }

SMOKE_CMD=(
  /usr/local/bin/orange-evt-stream-smoke
  --config-dir "$CONFIG_DIR"
  --serial "$SERIAL"
  --measure-seconds "$MEASURE_SECONDS"
  --buffer-count "$BUFFER_COUNT"
)
if [[ -n "$FRAME_RATE" ]]; then
  SMOKE_CMD+=(--frame-rate "$FRAME_RATE")
fi
if [[ -n "$GPU_DIRECT" ]]; then
  SMOKE_CMD+=(--gpu-direct "$GPU_DIRECT")
fi

if [[ "$DRY_RUN" == "1" ]]; then
  echo "[link-health] camera_ip=$CAMERA_IP interface=$iface out_dir=$OUT_DIR"
  printf '[link-health] smoke:'
  printf ' %q' "${SMOKE_CMD[@]}"
  printf '\n'
  exit 0
fi

mkdir -p "$OUT_DIR"

{
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "camera_ip=$CAMERA_IP"
  echo "serial=$SERIAL"
  echo "interface=$iface"
  echo "config_dir=$CONFIG_DIR"
  echo "measure_seconds=$MEASURE_SECONDS"
  echo "buffer_count=$BUFFER_COUNT"
  echo "frame_rate_override=$FRAME_RATE"
  echo "gpu_direct_override=$GPU_DIRECT"
} > "$OUT_DIR/summary.env"

ip -br addr > "$OUT_DIR/ip_addr_before.txt" 2>&1 || true
ip -s link show dev "$iface" > "$OUT_DIR/ip_link_before.txt" 2>&1 || true
ethtool "$iface" > "$OUT_DIR/ethtool_before.txt" 2>&1 || true
ethtool --show-fec "$iface" > "$OUT_DIR/fec_before.txt" 2>&1 || true
ethtool -S "$iface" > "$OUT_DIR/stats_before.txt" 2>&1 || true

set +e
"${SMOKE_CMD[@]}" > "$OUT_DIR/stream_smoke.log" 2>&1
status=$?
set -e

ip -s link show dev "$iface" > "$OUT_DIR/ip_link_after.txt" 2>&1 || true
ethtool -S "$iface" > "$OUT_DIR/stats_after.txt" 2>&1 || true

{
  echo "stream_smoke_status=$status"
  echo "out_dir=$OUT_DIR"
  echo
  echo "[MEASURE]"
  grep '^\[MEASURE\]' "$OUT_DIR/stream_smoke.log" || true
  echo
  echo "[PHY before/after selected counters]"
  for counter in \
    rx_crc_errors_phy \
    rx_pcs_symbol_err_phy \
    rx_corrected_bits_phy \
    rx_err_lane_0_phy \
    rx_err_lane_1_phy \
    rx_fragments_phy \
    rx_discards_phy \
    rx_out_of_buffer; do
    before="$(awk -v key="$counter:" '$1 == key { print $2; exit }' "$OUT_DIR/stats_before.txt" 2>/dev/null || true)"
    after="$(awk -v key="$counter:" '$1 == key { print $2; exit }' "$OUT_DIR/stats_after.txt" 2>/dev/null || true)"
    if [[ -n "$before" || -n "$after" ]]; then
      delta="NA"
      if [[ "$before" =~ ^[0-9]+$ && "$after" =~ ^[0-9]+$ ]]; then
        delta="$((after - before))"
      fi
      echo "$counter before=${before:-NA} after=${after:-NA} delta=$delta"
    fi
  done
} | tee "$OUT_DIR/summary.txt"

exit "$status"
