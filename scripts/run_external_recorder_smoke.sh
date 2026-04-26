#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run_external_recorder_smoke.sh [options]

Runs a one-camera headless real-YOLO external-recorder smoke:
  Process A: orange_client with recording_sink_mode=external_ipc.
  Process B: external_recorder_ipc_probe detach + external NVENC + MP4 output.

Options:
  --spec <path>              Base experiment spec.
  --orange-client <path>     orange_client binary.
  --recorder-tool <path>     external_recorder_ipc_probe binary.
  --camera-serial <serial>   Camera serial. Default 2010096.
  --analytics-gpu-id <int>   GPU id selected for camera/YOLO. Default 5.
  --recorder-gpu-id <int>    GPU id used by external recorder. Default: analytics GPU.
  --duration <sec>           Headless run duration. Default 6.
  --warmup <sec>             Headless warmup. Default 2.
  --encode-fps <int>         External encode cap and nominal MP4 FPS. Default 60.
  --queue-depth <int>        External recorder-owned frame slots. Default 8.
  --socket <path>            Unix socket path. Default /tmp/orange_external_recorder_<serial>.sock.
                             Non-default paths require matching client env outside this script.
  --output-dir <path>        External recorder artifact root. Default /tmp.
  --bitstream-out <path>     Optional raw elementary stream output.
  --keep-temp-spec           Keep generated temp spec in output dir.
  --help
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SPEC="$REPO_ROOT/experiment_specs/2010096_headless_real_yolo_external_ipc_encode_smoke.json"
ORANGE_CLIENT="$REPO_ROOT/targets/release/orange_client"
RECORDER_TOOL="$REPO_ROOT/targets/release/external_recorder_ipc_probe"
CAMERA_SERIAL=2010096
ANALYTICS_GPU_ID=5
RECORDER_GPU_ID=""
DURATION=6
WARMUP=2
ENCODE_FPS=60
QUEUE_DEPTH=8
SOCKET_PATH=""
OUTPUT_DIR="/tmp"
BITSTREAM_OUT=""
KEEP_TEMP_SPEC=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --spec)
      shift
      [[ $# -gt 0 ]] || { echo "--spec requires a value." >&2; exit 2; }
      SPEC="$1"
      shift
      ;;
    --orange-client)
      shift
      [[ $# -gt 0 ]] || { echo "--orange-client requires a value." >&2; exit 2; }
      ORANGE_CLIENT="$1"
      shift
      ;;
    --recorder-tool)
      shift
      [[ $# -gt 0 ]] || { echo "--recorder-tool requires a value." >&2; exit 2; }
      RECORDER_TOOL="$1"
      shift
      ;;
    --camera-serial)
      shift
      [[ $# -gt 0 ]] || { echo "--camera-serial requires a value." >&2; exit 2; }
      CAMERA_SERIAL="$1"
      shift
      ;;
    --analytics-gpu-id)
      shift
      [[ $# -gt 0 ]] || { echo "--analytics-gpu-id requires a value." >&2; exit 2; }
      ANALYTICS_GPU_ID="$1"
      shift
      ;;
    --recorder-gpu-id)
      shift
      [[ $# -gt 0 ]] || { echo "--recorder-gpu-id requires a value." >&2; exit 2; }
      RECORDER_GPU_ID="$1"
      shift
      ;;
    --duration)
      shift
      [[ $# -gt 0 ]] || { echo "--duration requires a value." >&2; exit 2; }
      DURATION="$1"
      shift
      ;;
    --warmup)
      shift
      [[ $# -gt 0 ]] || { echo "--warmup requires a value." >&2; exit 2; }
      WARMUP="$1"
      shift
      ;;
    --encode-fps)
      shift
      [[ $# -gt 0 ]] || { echo "--encode-fps requires a value." >&2; exit 2; }
      ENCODE_FPS="$1"
      shift
      ;;
    --queue-depth)
      shift
      [[ $# -gt 0 ]] || { echo "--queue-depth requires a value." >&2; exit 2; }
      QUEUE_DEPTH="$1"
      shift
      ;;
    --socket)
      shift
      [[ $# -gt 0 ]] || { echo "--socket requires a value." >&2; exit 2; }
      SOCKET_PATH="$1"
      shift
      ;;
    --output-dir)
      shift
      [[ $# -gt 0 ]] || { echo "--output-dir requires a value." >&2; exit 2; }
      OUTPUT_DIR="$1"
      shift
      ;;
    --bitstream-out)
      shift
      [[ $# -gt 0 ]] || { echo "--bitstream-out requires a value." >&2; exit 2; }
      BITSTREAM_OUT="$1"
      shift
      ;;
    --keep-temp-spec)
      KEEP_TEMP_SPEC=1
      shift
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

for value_name in ANALYTICS_GPU_ID DURATION WARMUP ENCODE_FPS QUEUE_DEPTH; do
  value="${!value_name}"
  [[ "$value" =~ ^[0-9]+$ ]] || { echo "$value_name must be a non-negative integer." >&2; exit 2; }
done
if [[ -n "$RECORDER_GPU_ID" ]]; then
  [[ "$RECORDER_GPU_ID" =~ ^[0-9]+$ ]] || { echo "RECORDER_GPU_ID must be a non-negative integer." >&2; exit 2; }
else
  RECORDER_GPU_ID="$ANALYTICS_GPU_ID"
fi

SPEC="$(realpath -e "$SPEC")"
ORANGE_CLIENT="$(realpath -e "$ORANGE_CLIENT")"
RECORDER_TOOL="$(realpath -e "$RECORDER_TOOL")"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(realpath -m "$OUTPUT_DIR")"

STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$OUTPUT_DIR/orange_external_recorder_${CAMERA_SERIAL}_${STAMP}"
mkdir -p "$RUN_DIR"
if [[ -z "$SOCKET_PATH" ]]; then
  SOCKET_PATH="/tmp/orange_external_recorder_${CAMERA_SERIAL}.sock"
fi

TEMP_SPEC="$RUN_DIR/external_recorder_smoke_spec.json"
DETACH_CSV="$RUN_DIR/external_detach.csv"
ENCODE_CSV="$RUN_DIR/external_encode.csv"
SUMMARY_JSON="$RUN_DIR/external_recorder_summary.json"
MP4_OUT="$RUN_DIR/Cam${CAMERA_SERIAL}_external.mp4"
KEYFRAME_OUT="$RUN_DIR/Cam${CAMERA_SERIAL}_external_keyframes.csv"
RECORDER_LOG="$RUN_DIR/external_recorder.log"

python3 - "$SPEC" "$TEMP_SPEC" "$STAMP" "$CAMERA_SERIAL" "$ANALYTICS_GPU_ID" "$DURATION" "$WARMUP" <<'PY'
import json
import sys
from pathlib import Path

source = Path(sys.argv[1])
dest = Path(sys.argv[2])
stamp = sys.argv[3]
camera_serial = sys.argv[4]
gpu_id = int(sys.argv[5])
duration = int(sys.argv[6])
warmup = int(sys.argv[7])

with source.open("r", encoding="utf-8") as f:
    spec = json.load(f)

base_id = spec.get("experiment_id") or "external_recorder_smoke"
spec["experiment_id"] = f"{base_id}_{camera_serial}_{stamp}"
spec["notes"] = (
    spec.get("notes", "") +
    " Generated by run_external_recorder_smoke.sh."
).strip()
spec.setdefault("selection", {})["camera_serials"] = [camera_serial]
spec.setdefault("selection", {})["gpu_ids"] = [gpu_id]
fixed = spec.setdefault("fixed", {})
fixed["duration_s"] = duration
fixed["warmup_s"] = warmup
fixed["display"] = False
fixed["stream_only"] = False
fixed["recording_sink_mode"] = "external_ipc"

with dest.open("w", encoding="utf-8") as f:
    json.dump(spec, f, indent=2)
    f.write("\n")
PY

mapfile -t SPEC_INFO < <(python3 - "$TEMP_SPEC" <<'PY'
import json
import sys
from pathlib import Path

with Path(sys.argv[1]).open("r", encoding="utf-8") as f:
    spec = json.load(f)
print(spec["experiment_id"])
print(spec.get("fixed", {}).get("output_root", ""))
PY
)
EXPERIMENT_ID="${SPEC_INFO[0]}"
ANALYTICS_ROOT="${SPEC_INFO[1]}/$EXPERIMENT_ID"

cleanup() {
  if [[ -n "${RECORDER_PID:-}" ]] && kill -0 "$RECORDER_PID" >/dev/null 2>&1; then
    echo "[external-recorder] stopping recorder pid=$RECORDER_PID"
    kill -TERM "$RECORDER_PID" >/dev/null 2>&1 || true
    wait "$RECORDER_PID" || true
  fi
  rm -f "$SOCKET_PATH"
  if [[ "$KEEP_TEMP_SPEC" -eq 0 ]]; then
    rm -f "$TEMP_SPEC"
  fi
}
trap cleanup EXIT

echo "[external-recorder] run_dir=$RUN_DIR"
echo "[external-recorder] analytics_root=$ANALYTICS_ROOT"
echo "[external-recorder] socket=$SOCKET_PATH"
echo "[external-recorder] mp4_out=$MP4_OUT"
echo "[external-recorder] summary_json=$SUMMARY_JSON"

RECORDER_ARGS=(
  --socket "$SOCKET_PATH"
  --gpu-id "$RECORDER_GPU_ID"
  --csv "$DETACH_CSV"
  --encode
  --encode-max-fps "$ENCODE_FPS"
  --encode-queue-depth "$QUEUE_DEPTH"
  --fps "$ENCODE_FPS"
  --codec hevc
  --preset p1
  --tuning ll
  --gop 25
  --bitrate-bps 150000000
  --max-bitrate-bps 150000000
  --vbv-buffer-size 150000000
  --mp4-out "$MP4_OUT"
  --mp4-keyframe "$KEYFRAME_OUT"
  --encode-csv "$ENCODE_CSV"
  --summary-json "$SUMMARY_JSON"
)
if [[ -n "$BITSTREAM_OUT" ]]; then
  RECORDER_ARGS+=(--bitstream-out "$BITSTREAM_OUT")
fi

"$RECORDER_TOOL" "${RECORDER_ARGS[@]}" >"$RECORDER_LOG" 2>&1 &
RECORDER_PID=$!

for _ in {1..100}; do
  if [[ -S "$SOCKET_PATH" ]]; then
    break
  fi
  if ! kill -0 "$RECORDER_PID" >/dev/null 2>&1; then
    echo "external recorder exited before creating socket." >&2
    cat "$RECORDER_LOG" >&2 || true
    exit 1
  fi
  sleep 0.05
done
[[ -S "$SOCKET_PATH" ]] || { echo "external recorder socket was not created: $SOCKET_PATH" >&2; exit 1; }

echo "[external-recorder] starting headless external_ipc run camera=$CAMERA_SERIAL analytics_gpu=$ANALYTICS_GPU_ID recorder_gpu=$RECORDER_GPU_ID encode_fps=$ENCODE_FPS"
if [[ "$SOCKET_PATH" != "/tmp/orange_external_recorder_${CAMERA_SERIAL}.sock" ]]; then
  echo "Custom socket paths require passing ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_${CAMERA_SERIAL} to the benchmark process." >&2
  echo "Use the default socket path for the sudo -n smoke runner." >&2
  exit 2
fi
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client "$ORANGE_CLIENT" \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  "$TEMP_SPEC"

echo "[external-recorder] analytics complete"
wait "$RECORDER_PID"
trap - EXIT
rm -f "$SOCKET_PATH"
if [[ "$KEEP_TEMP_SPEC" -eq 0 ]]; then
  rm -f "$TEMP_SPEC"
fi

echo "[external-recorder] outputs:"
echo "  run_dir=$RUN_DIR"
echo "  analytics_root=$ANALYTICS_ROOT"
echo "  recorder_log=$RECORDER_LOG"
echo "  detach_csv=$DETACH_CSV"
echo "  encode_csv=$ENCODE_CSV"
echo "  summary_json=$SUMMARY_JSON"
echo "  mp4_out=$MP4_OUT"
echo "  keyframe_out=$KEYFRAME_OUT"
if [[ -n "$BITSTREAM_OUT" ]]; then
  echo "  bitstream_out=$BITSTREAM_OUT"
fi

if [[ -f "$SUMMARY_JSON" ]]; then
  python3 - "$SUMMARY_JSON" <<'PY'
import json
import sys
from pathlib import Path

summary = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
enc = summary.get("external_encode", {})
print("[external-recorder] summary:")
print(f"  frames_received={summary.get('frames_received')} acks_sent={summary.get('acks_sent')} detach_copied={summary.get('detach_copied')}")
print(f"  encode_enqueued={summary.get('encode_enqueued')} encode_skipped={summary.get('encode_skipped')} encode_dropped={summary.get('encode_dropped')} frames_encoded={summary.get('frames_encoded')}")
print(f"  detach_copy_p95_ms={summary.get('detach_timing', {}).get('copy_p95_ms')} encode_total_p95_ms={enc.get('encode_total_p95_ms')} lock_bitstream_p95_ms={enc.get('lock_bitstream_p95_ms')}")
print(f"  mp4_bytes={summary.get('output_file_sizes', {}).get('mp4_bytes')} worker_failed={summary.get('worker_failed')}")
PY
fi
