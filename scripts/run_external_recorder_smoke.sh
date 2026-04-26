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
  --shard-gpu-ids <csv>      Diagnostic multi-shard recorder GPUs, e.g. 5,6.
  --duration <sec>           Headless run duration. Default 6.
  --warmup <sec>             Headless warmup. Default 2.
  --encode-fps <int>         External encode cap and nominal MP4 FPS. Default 60.
  --queue-depth <int>        External recorder-owned frame slots. Default 8.
  --socket <path>            Unix socket path. Default /tmp/orange_external_recorder_<serial>.sock.
                             Non-default paths require matching client env outside this script.
  --output-dir <path>        External recorder artifact root. Default /tmp.
  --bitstream-out <path>     Optional raw elementary stream output.
  --skip-video-sanity        Do not decode/check external MP4 content.
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
SHARD_GPU_IDS=""
DURATION=6
WARMUP=2
ENCODE_FPS=60
QUEUE_DEPTH=8
SOCKET_PATH=""
OUTPUT_DIR="/tmp"
BITSTREAM_OUT=""
SKIP_VIDEO_SANITY=0
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
    --shard-gpu-ids)
      shift
      [[ $# -gt 0 ]] || { echo "--shard-gpu-ids requires a value." >&2; exit 2; }
      SHARD_GPU_IDS="$1"
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
    --skip-video-sanity)
      SKIP_VIDEO_SANITY=1
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
if [[ -n "$SHARD_GPU_IDS" ]]; then
  [[ "$SHARD_GPU_IDS" =~ ^[0-9]+(,[0-9]+)+$ ]] || {
    echo "SHARD_GPU_IDS must be a comma-separated list of at least two GPU ids." >&2
    exit 2
  }
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
GOP_ROUTING_CSV="$RUN_DIR/external_gop_routing.csv"
SUMMARY_JSON="$RUN_DIR/external_recorder_summary.json"
VIDEO_SANITY_JSON="$RUN_DIR/external_video_sanity.json"
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

ROUTING_POLICY="single_shard"
if [[ -n "$SHARD_GPU_IDS" ]]; then
  ROUTING_POLICY="gop_modulo"
  SKIP_VIDEO_SANITY=1
fi

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
  --gop-routing-csv "$GOP_ROUTING_CSV"
  --summary-json "$SUMMARY_JSON"
  --session-id "$EXPERIMENT_ID"
  --stream-id "$CAMERA_SERIAL"
  --shard-id 0
  --routing-policy "$ROUTING_POLICY"
)
if [[ -n "$SHARD_GPU_IDS" ]]; then
  RECORDER_ARGS+=(--shard-gpu-ids "$SHARD_GPU_IDS")
fi
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

echo "[external-recorder] starting headless external_ipc run camera=$CAMERA_SERIAL analytics_gpu=$ANALYTICS_GPU_ID recorder_gpu=$RECORDER_GPU_ID shard_gpu_ids=${SHARD_GPU_IDS:-none} encode_fps=$ENCODE_FPS"
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
echo "  gop_routing_csv=$GOP_ROUTING_CSV"
echo "  summary_json=$SUMMARY_JSON"
echo "  video_sanity_json=$VIDEO_SANITY_JSON"
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
print(f"  session_id={summary.get('session_id')} assigned_gpu_id={summary.get('assigned_gpu_id')} assigned_shard_id={summary.get('assigned_shard_id')} routing_policy={summary.get('routing_policy')}")
print(f"  encode_enqueued={summary.get('encode_enqueued')} encode_skipped={summary.get('encode_skipped')} encode_dropped={summary.get('encode_dropped')} frames_encoded={summary.get('frames_encoded')}")
print(f"  detach_copy_p95_ms={summary.get('detach_timing', {}).get('copy_p95_ms')} encode_total_p95_ms={enc.get('encode_total_p95_ms')} lock_bitstream_p95_ms={enc.get('lock_bitstream_p95_ms')}")
print(f"  mp4_bytes={summary.get('output_file_sizes', {}).get('mp4_bytes')} worker_failed={summary.get('worker_failed')}")
for shard in summary.get("external_encode_shards", []):
    print(
        "  shard "
        f"id={shard.get('assigned_shard_id')} gpu={shard.get('assigned_gpu_id')} "
        f"frames_encoded={shard.get('frames_encoded')} mp4={shard.get('mp4')}"
    )
PY
fi

if [[ "$SKIP_VIDEO_SANITY" -eq 0 ]]; then
  python3 - "$MP4_OUT" "$VIDEO_SANITY_JSON" <<'PY'
import json
import math
import subprocess
import sys
from pathlib import Path

mp4_path = Path(sys.argv[1])
summary_path = Path(sys.argv[2])

def fail(status, detail):
    result = {
        "schema_version": 1,
        "video_path": str(mp4_path),
        "content_checked": True,
        "content_valid": False,
        "status": status,
        "detail": detail,
        "sampled_frames": [],
    }
    summary_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[external-recorder] video_sanity status={status} detail={detail}")
    raise SystemExit(1)

if not mp4_path.exists() or mp4_path.stat().st_size == 0:
    fail("missing_video", "MP4 output is missing or empty")

probe_cmd = [
    "ffprobe",
    "-v",
    "error",
    "-select_streams",
    "v:0",
    "-show_entries",
    "stream=width,height,nb_frames,avg_frame_rate,duration",
    "-show_entries",
    "format=size,duration",
    "-of",
    "json",
    str(mp4_path),
]
try:
    probe = subprocess.run(
        probe_cmd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
except (subprocess.CalledProcessError, FileNotFoundError) as exc:
    fail("ffprobe_failed", str(exc))

metadata = json.loads(probe.stdout)
streams = metadata.get("streams") or []
if not streams:
    fail("no_video_stream", "ffprobe found no video stream")

stream = streams[0]
width = int(stream.get("width") or 0)
height = int(stream.get("height") or 0)
if width <= 0 or height <= 0:
    fail("invalid_dimensions", f"width={width} height={height}")

nb_frames_value = stream.get("nb_frames")
try:
    frame_count = int(nb_frames_value)
except (TypeError, ValueError):
    frame_count = 0

if frame_count > 0:
    sample_indices = sorted({
        0,
        max(0, frame_count // 4),
        max(0, frame_count // 2),
        max(0, (3 * frame_count) // 4),
        max(0, frame_count - 1),
    })
else:
    sample_indices = [0]

select_expr = "+".join(f"eq(n\\,{index})" for index in sample_indices)
decode_cmd = [
    "ffmpeg",
    "-v",
    "error",
    "-i",
    str(mp4_path),
    "-vf",
    f"select='{select_expr}'",
    "-vsync",
    "0",
    "-pix_fmt",
    "gray",
    "-f",
    "rawvideo",
    "-",
]
try:
    decoded = subprocess.run(
        decode_cmd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout
except (subprocess.CalledProcessError, FileNotFoundError) as exc:
    fail("decode_failed", str(exc))

frame_bytes = width * height
decoded_frames = len(decoded) // frame_bytes if frame_bytes > 0 else 0
if decoded_frames == 0:
    fail("decode_empty", "ffmpeg returned no decoded sample frames")

measurements = []
for i in range(decoded_frames):
    frame = decoded[i * frame_bytes:(i + 1) * frame_bytes]
    hist = [0] * 256
    for value in frame:
        hist[value] += 1
    pixel_count = sum(hist)
    total = sum(value * count for value, count in enumerate(hist))
    total_sq = sum(value * value * count for value, count in enumerate(hist))
    mean = total / pixel_count
    variance = max(0.0, total_sq / pixel_count - mean * mean)
    min_value = next(value for value, count in enumerate(hist) if count)
    max_value = 255 - next(value for value, count in enumerate(reversed(hist)) if count)
    measurements.append({
        "requested_frame_index": sample_indices[min(i, len(sample_indices) - 1)],
        "mean": mean,
        "stddev": math.sqrt(variance),
        "min": min_value,
        "max": max_value,
        "black_fraction_lt8": sum(hist[:8]) / pixel_count,
        "white_fraction_gt247": sum(hist[248:]) / pixel_count,
        "decoded_bytes": pixel_count,
    })

max_black_fraction = max(item["black_fraction_lt8"] for item in measurements)
max_stddev = max(item["stddev"] for item in measurements)
mean_luma = sum(item["mean"] for item in measurements) / len(measurements)
content_valid = max_black_fraction < 0.98 and max_stddev >= 5.0
if max_black_fraction >= 0.98:
    status = "black_frame"
elif max_stddev < 5.0:
    status = "flat_frame"
else:
    status = "pass"

result = {
    "schema_version": 1,
    "video_path": str(mp4_path),
    "content_checked": True,
    "content_valid": content_valid,
    "status": status,
    "width": width,
    "height": height,
    "nb_frames": frame_count,
    "container": metadata.get("format", {}),
    "sampled_frame_count": len(measurements),
    "mean_luma": mean_luma,
    "max_stddev": max_stddev,
    "max_black_fraction_lt8": max_black_fraction,
    "thresholds": {
        "max_black_fraction_lt8": 0.98,
        "min_max_stddev": 5.0,
    },
    "sampled_frames": measurements,
}
summary_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(
    "[external-recorder] video_sanity "
    f"status={status} frames={frame_count} samples={len(measurements)} "
    f"mean_luma={mean_luma:.3f} max_stddev={max_stddev:.3f} "
    f"max_black_fraction_lt8={max_black_fraction:.6f}"
)
if not content_valid:
    raise SystemExit(1)
PY
elif [[ -n "$SHARD_GPU_IDS" ]]; then
  echo "[external-recorder] video_sanity skipped for multi-shard diagnostic; inspect per-shard MP4 paths in summary_json."
fi
