#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run_process_isolation_discriminator.sh [options]

Runs the first process-isolation discriminator:
  Process A: headless real-YOLO analytics with preprocess_only recording.
  Process B: separate synthetic NVENC stress process.

Options:
  --spec <path>              Process A experiment spec.
  --orange-client <path>     orange_client binary for Process A.
  --nvenc-tool <path>        nvenc_stress_load binary for Process B.
  --gpu-id <int>             Process A selected GPU id. Default: keep spec.
  --nvenc-gpu-id <int>       Process B GPU id. Default 5.
  --nvenc-fps <int>          Process B target FPS. Default 60.
  --nvenc-duration <sec>     Process B duration. Default 45.
  --nvenc-width <int>        Process B width. Default 4512.
  --nvenc-height <int>       Process B height. Default 4512.
  --nvenc-codec <hevc|h264>  Process B codec. Default hevc.
  --nvenc-preset <p1|p3|p5|p7>
                              Process B preset. Default p1.
  --nvenc-tuning <ll|ull|hq|lossless>
                              Process B tuning. Default ll.
  --nvenc-gop <int>          Process B GOP length. Default 25.
  --nvenc-pattern <solid|host-noise>
                              Process B input pattern. Default host-noise.
  --output-dir <path>        Directory for temp spec and stress logs. Default /tmp.
  --stress-warmup <sec>      Seconds to let Process B run before Process A. Default 2.
  --help
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SPEC="$REPO_ROOT/experiment_specs/2010096_headless_real_yolo_preprocessonly_a16_gpu5.json"
ORANGE_CLIENT="$REPO_ROOT/targets/release/orange_client"
NVENC_TOOL="$REPO_ROOT/targets/release/nvenc_stress_load"
GPU_ID=""
NVENC_GPU_ID=5
NVENC_FPS=60
NVENC_DURATION=45
NVENC_WIDTH=4512
NVENC_HEIGHT=4512
NVENC_CODEC="hevc"
NVENC_PRESET="p1"
NVENC_TUNING="ll"
NVENC_GOP=25
NVENC_PATTERN="host-noise"
OUTPUT_DIR="/tmp"
STRESS_WARMUP=2

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
    --nvenc-tool)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-tool requires a value." >&2; exit 2; }
      NVENC_TOOL="$1"
      shift
      ;;
    --gpu-id)
      shift
      [[ $# -gt 0 ]] || { echo "--gpu-id requires a value." >&2; exit 2; }
      GPU_ID="$1"
      shift
      ;;
    --nvenc-gpu-id)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-gpu-id requires a value." >&2; exit 2; }
      NVENC_GPU_ID="$1"
      shift
      ;;
    --nvenc-fps)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-fps requires a value." >&2; exit 2; }
      NVENC_FPS="$1"
      shift
      ;;
    --nvenc-duration)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-duration requires a value." >&2; exit 2; }
      NVENC_DURATION="$1"
      shift
      ;;
    --nvenc-width)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-width requires a value." >&2; exit 2; }
      NVENC_WIDTH="$1"
      shift
      ;;
    --nvenc-height)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-height requires a value." >&2; exit 2; }
      NVENC_HEIGHT="$1"
      shift
      ;;
    --nvenc-codec)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-codec requires a value." >&2; exit 2; }
      NVENC_CODEC="$1"
      shift
      ;;
    --nvenc-preset)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-preset requires a value." >&2; exit 2; }
      NVENC_PRESET="$1"
      shift
      ;;
    --nvenc-tuning)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-tuning requires a value." >&2; exit 2; }
      NVENC_TUNING="$1"
      shift
      ;;
    --nvenc-gop)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-gop requires a value." >&2; exit 2; }
      NVENC_GOP="$1"
      shift
      ;;
    --nvenc-pattern)
      shift
      [[ $# -gt 0 ]] || { echo "--nvenc-pattern requires a value." >&2; exit 2; }
      NVENC_PATTERN="$1"
      shift
      ;;
    --output-dir)
      shift
      [[ $# -gt 0 ]] || { echo "--output-dir requires a value." >&2; exit 2; }
      OUTPUT_DIR="$1"
      shift
      ;;
    --stress-warmup)
      shift
      [[ $# -gt 0 ]] || { echo "--stress-warmup requires a value." >&2; exit 2; }
      STRESS_WARMUP="$1"
      shift
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

for value_name in NVENC_GPU_ID NVENC_FPS NVENC_DURATION NVENC_WIDTH NVENC_HEIGHT NVENC_GOP STRESS_WARMUP; do
  value="${!value_name}"
  [[ "$value" =~ ^[0-9]+$ ]] || { echo "$value_name must be a non-negative integer." >&2; exit 2; }
done
if [[ -n "$GPU_ID" ]]; then
  [[ "$GPU_ID" =~ ^[0-9]+$ ]] || { echo "--gpu-id must be a non-negative integer." >&2; exit 2; }
fi

SPEC="$(realpath -e "$SPEC")"
ORANGE_CLIENT="$(realpath -e "$ORANGE_CLIENT")"
NVENC_TOOL="$(realpath -e "$NVENC_TOOL")"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(realpath -m "$OUTPUT_DIR")"

STAMP="$(date +%Y%m%d_%H%M%S)"
TEMP_SPEC="$OUTPUT_DIR/orange_process_isolation_${STAMP}.json"
STRESS_CSV="$OUTPUT_DIR/orange_nvenc_stress_${STAMP}.csv"
STRESS_LOG="$OUTPUT_DIR/orange_nvenc_stress_${STAMP}.log"

python3 - "$SPEC" "$TEMP_SPEC" "$STAMP" "$GPU_ID" <<'PY'
import json
import sys
from pathlib import Path

source = Path(sys.argv[1])
dest = Path(sys.argv[2])
stamp = sys.argv[3]
gpu_id = sys.argv[4]

with source.open("r", encoding="utf-8") as f:
    spec = json.load(f)

base_id = spec.get("experiment_id") or "orange_process_isolation"
spec["experiment_id"] = f"{base_id}_external_nvenc_{stamp}"
spec["notes"] = (
    spec.get("notes", "") +
    " Process-isolation discriminator: Process A is headless real YOLO; "
    "Process B is separate synthetic NVENC stress load."
).strip()
if gpu_id:
    spec.setdefault("selection", {})["gpu_ids"] = [int(gpu_id)]

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
OUTPUT_ROOT="${SPEC_INFO[1]}"
ANALYTICS_ROOT="$OUTPUT_ROOT/$EXPERIMENT_ID"

cleanup() {
  if [[ -n "${STRESS_PID:-}" ]] && kill -0 "$STRESS_PID" >/dev/null 2>&1; then
    echo "[process-isolation] stopping nvenc stress pid=$STRESS_PID"
    kill -TERM "$STRESS_PID" >/dev/null 2>&1 || true
    wait "$STRESS_PID" || true
  fi
}
trap cleanup EXIT

echo "[process-isolation] temp_spec=$TEMP_SPEC"
echo "[process-isolation] analytics_root=$ANALYTICS_ROOT"
echo "[process-isolation] nvenc_csv=$STRESS_CSV"
echo "[process-isolation] nvenc_log=$STRESS_LOG"
echo "[process-isolation] starting external nvenc stress on gpu=$NVENC_GPU_ID fps=$NVENC_FPS duration=$NVENC_DURATION pattern=$NVENC_PATTERN"

"$NVENC_TOOL" \
  --gpu-id "$NVENC_GPU_ID" \
  --width "$NVENC_WIDTH" \
  --height "$NVENC_HEIGHT" \
  --fps "$NVENC_FPS" \
  --duration "$NVENC_DURATION" \
  --codec "$NVENC_CODEC" \
  --preset "$NVENC_PRESET" \
  --tuning "$NVENC_TUNING" \
  --gop "$NVENC_GOP" \
  --pattern "$NVENC_PATTERN" \
  --csv "$STRESS_CSV" \
  >"$STRESS_LOG" 2>&1 &
STRESS_PID=$!

sleep "$STRESS_WARMUP"
if ! kill -0 "$STRESS_PID" >/dev/null 2>&1; then
  echo "NVENC stress process exited before analytics started." >&2
  cat "$STRESS_LOG" >&2 || true
  exit 1
fi

echo "[process-isolation] starting headless real-yolo analytics"
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client "$ORANGE_CLIENT" \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  "$TEMP_SPEC"

echo "[process-isolation] analytics complete"
cleanup
trap - EXIT

echo "[process-isolation] outputs:"
echo "  analytics_root=$ANALYTICS_ROOT"
echo "  temp_spec=$TEMP_SPEC"
echo "  nvenc_csv=$STRESS_CSV"
echo "  nvenc_log=$STRESS_LOG"
