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
  --encode-max-fps <int>     External encode cap only. Use 0 for uncapped.
                             Default: same as --encode-fps.
  --queue-depth <int>        External recorder-owned frame slots. Default 8.
  --prewarm-slots <int>      Prewarm encode detach slots per shard. Default 4.
  --prewarm-bytes <int|auto> Pre-listen prewarm byte size. Default auto from spec config.
  --no-prewarm-peer-copy     Do not warm the first source-to-shard peer copy.
  --direct-input-source      Copy IPC source directly into NVENC input. Experimental.
  --deferred-source-release  ACK accepted work, then RELEASE after source consumption. Experimental.
  --yolo-prewarm-iterations <int> Synthetic YOLO prewarm iterations. Default 3.
  --ptp-register-read-decimate <n> Read GevTimestampValue every n frames. Default 1.
  --socket <path>            Unix socket path. Default /tmp/orange_external_recorder_<serial>.sock.
                             Non-default paths require matching client env outside this script.
  --output-dir <path>        External recorder artifact root. Default /tmp.
  --bitstream-out <path>     Optional raw elementary stream output.
  --skip-video-sanity        Do not decode/check external MP4 content.
  --max-encode-queue-high-water <int>
                             Verifier threshold for recorder encode queue high-water.
  --max-enqueue-age-p95-ms <float>
                             Verifier threshold for recorder enqueue-age p95.
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
ENCODE_MAX_FPS=""
QUEUE_DEPTH=8
PREWARM_SLOTS=4
PREWARM_BYTES=auto
PREWARM_PEER_COPY=1
DIRECT_INPUT_SOURCE="${ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT:-0}"
DEFERRED_SOURCE_RELEASE="${ORANGE_EXTERNAL_RECORDER_DEFERRED_RELEASE:-0}"
YOLO_PREWARM_ITERATIONS=3
PTP_REGISTER_READ_DECIMATE=1
ANALYTICS_EARLY_OWNED_FRAME="${ORANGE_ANALYTICS_EARLY_OWNED_FRAME:-1}"
YOLO_READY_EVENT_FASTPATH="${ORANGE_YOLO_READY_EVENT_FASTPATH:-1}"
YOLO_DETACH_INPUT="${ORANGE_YOLO_DETACH_INPUT:-1}"
SOCKET_PATH=""
OUTPUT_DIR="/tmp"
BITSTREAM_OUT=""
SKIP_VIDEO_SANITY=0
KEEP_TEMP_SPEC=0
VERIFY_MAX_ENCODE_QUEUE_HIGH_WATER=""
VERIFY_MAX_ENQUEUE_AGE_P95_MS=""

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
    --encode-max-fps)
      shift
      [[ $# -gt 0 ]] || { echo "--encode-max-fps requires a value." >&2; exit 2; }
      ENCODE_MAX_FPS="$1"
      shift
      ;;
    --queue-depth)
      shift
      [[ $# -gt 0 ]] || { echo "--queue-depth requires a value." >&2; exit 2; }
      QUEUE_DEPTH="$1"
      shift
      ;;
    --prewarm-slots)
      shift
      [[ $# -gt 0 ]] || { echo "--prewarm-slots requires a value." >&2; exit 2; }
      PREWARM_SLOTS="$1"
      shift
      ;;
    --prewarm-bytes)
      shift
      [[ $# -gt 0 ]] || { echo "--prewarm-bytes requires a value." >&2; exit 2; }
      PREWARM_BYTES="$1"
      shift
      ;;
    --no-prewarm-peer-copy)
      PREWARM_PEER_COPY=0
      shift
      ;;
    --direct-input-source)
      DIRECT_INPUT_SOURCE=1
      shift
      ;;
    --deferred-source-release)
      DEFERRED_SOURCE_RELEASE=1
      shift
      ;;
    --yolo-prewarm-iterations)
      shift
      [[ $# -gt 0 ]] || { echo "--yolo-prewarm-iterations requires a value." >&2; exit 2; }
      YOLO_PREWARM_ITERATIONS="$1"
      shift
      ;;
    --ptp-register-read-decimate)
      shift
      [[ $# -gt 0 ]] || { echo "--ptp-register-read-decimate requires a value." >&2; exit 2; }
      PTP_REGISTER_READ_DECIMATE="$1"
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
    --max-encode-queue-high-water)
      shift
      [[ $# -gt 0 ]] || { echo "--max-encode-queue-high-water requires a value." >&2; exit 2; }
      VERIFY_MAX_ENCODE_QUEUE_HIGH_WATER="$1"
      shift
      ;;
    --max-enqueue-age-p95-ms)
      shift
      [[ $# -gt 0 ]] || { echo "--max-enqueue-age-p95-ms requires a value." >&2; exit 2; }
      VERIFY_MAX_ENQUEUE_AGE_P95_MS="$1"
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

for value_name in ANALYTICS_GPU_ID DURATION WARMUP ENCODE_FPS QUEUE_DEPTH PREWARM_SLOTS YOLO_PREWARM_ITERATIONS PTP_REGISTER_READ_DECIMATE; do
  value="${!value_name}"
  [[ "$value" =~ ^[0-9]+$ ]] || { echo "$value_name must be a non-negative integer." >&2; exit 2; }
done
if [[ "$PTP_REGISTER_READ_DECIMATE" -lt 1 ]]; then
  echo "PTP_REGISTER_READ_DECIMATE must be >= 1." >&2
  exit 2
fi
if [[ "$PREWARM_BYTES" != "auto" ]]; then
  [[ "$PREWARM_BYTES" =~ ^[0-9]+$ ]] || { echo "PREWARM_BYTES must be auto or a non-negative integer." >&2; exit 2; }
fi
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
if [[ -n "$VERIFY_MAX_ENCODE_QUEUE_HIGH_WATER" ]]; then
  [[ "$VERIFY_MAX_ENCODE_QUEUE_HIGH_WATER" =~ ^[0-9]+$ ]] || {
    echo "VERIFY_MAX_ENCODE_QUEUE_HIGH_WATER must be a non-negative integer." >&2
    exit 2
  }
fi
if [[ -n "$VERIFY_MAX_ENQUEUE_AGE_P95_MS" ]]; then
  [[ "$VERIFY_MAX_ENQUEUE_AGE_P95_MS" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
    echo "VERIFY_MAX_ENQUEUE_AGE_P95_MS must be a non-negative number." >&2
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
if [[ -z "$ENCODE_MAX_FPS" ]]; then
  ENCODE_MAX_FPS="$ENCODE_FPS"
fi
if [[ -z "$SOCKET_PATH" ]]; then
  SOCKET_PATH="/tmp/orange_external_recorder_${CAMERA_SERIAL}.sock"
fi

TEMP_SPEC="$RUN_DIR/external_recorder_smoke_spec.json"
DETACH_CSV="$RUN_DIR/external_detach.csv"
ENCODE_CSV="$RUN_DIR/external_encode.csv"
GOP_ROUTING_CSV="$RUN_DIR/external_gop_routing.csv"
SUMMARY_JSON="$RUN_DIR/external_recorder_summary.json"
STATUS_JSON="$RUN_DIR/external_recorder_status.json"
VIDEO_SANITY_JSON="$RUN_DIR/external_video_sanity.json"
MP4_OUT="$RUN_DIR/Cam${CAMERA_SERIAL}_external.mp4"
KEYFRAME_OUT="$RUN_DIR/Cam${CAMERA_SERIAL}_external_keyframes.csv"
RECORDER_LOG="$RUN_DIR/external_recorder.log"
SESSION_CONTRACT_JSON="$RUN_DIR/external_recorder_session.json"

python3 - "$SPEC" "$TEMP_SPEC" "$SESSION_CONTRACT_JSON" "$STAMP" "$CAMERA_SERIAL" "$ANALYTICS_GPU_ID" "$RECORDER_GPU_ID" "$SHARD_GPU_IDS" "$DURATION" "$WARMUP" "$YOLO_PREWARM_ITERATIONS" "$PTP_REGISTER_READ_DECIMATE" "$RUN_DIR" "$SUMMARY_JSON" "$STATUS_JSON" "$VIDEO_SANITY_JSON" "$MP4_OUT" "$GOP_ROUTING_CSV" "$ENCODE_FPS" "$ENCODE_MAX_FPS" "$SKIP_VIDEO_SANITY" <<'PY'
import json
import sys
from pathlib import Path

source = Path(sys.argv[1])
dest = Path(sys.argv[2])
contract_dest = Path(sys.argv[3])
stamp = sys.argv[4]
camera_serial = sys.argv[5]
gpu_id = int(sys.argv[6])
recorder_gpu_id = int(sys.argv[7])
shard_gpu_ids = sys.argv[8]
duration = int(sys.argv[9])
warmup = int(sys.argv[10])
yolo_prewarm_iterations = int(sys.argv[11])
ptp_register_read_decimate = int(sys.argv[12])
run_dir = Path(sys.argv[13])
summary_json = Path(sys.argv[14])
status_json = Path(sys.argv[15])
video_sanity_json = Path(sys.argv[16])
mp4_out = Path(sys.argv[17])
gop_routing_csv = Path(sys.argv[18])
encode_fps = int(sys.argv[19])
encode_max_fps = int(sys.argv[20])
skip_video_sanity = int(sys.argv[21])

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
fixed["ptp_register_read_decimate"] = ptp_register_read_decimate
if isinstance(fixed.get("yolo_worker"), dict):
    fixed["yolo_worker"]["prewarm_iterations"] = yolo_prewarm_iterations
expected_shard_gpu_ids = (
    [int(item) for item in shard_gpu_ids.split(",") if item]
    if shard_gpu_ids
    else [recorder_gpu_id]
)
routing_policy = "gop_modulo" if len(expected_shard_gpu_ids) > 1 else "single_shard"
contract = {
    "schema_id": "orange.external_recorder.contract",
    "schema_version": 1,
    "mode": "diagnostic_ipc_v1",
    "artifact_root": str(run_dir),
    "session_id": spec["experiment_id"],
    "require_summary": True,
    "require_status": True,
    "require_storage_preflight": True,
    "require_video_sanity": skip_video_sanity == 0,
    "require_merged_mp4": len(expected_shard_gpu_ids) > 1,
    "require_gop_routing": True,
    "preserve_shard_mp4s": False,
    "streams": {
        camera_serial: {
            "stream_id": camera_serial,
            "camera_serial": camera_serial,
            "analytics_gpu_id": gpu_id,
            "recorder_gpu_id": recorder_gpu_id,
            "expected_shard_gpu_ids": expected_shard_gpu_ids,
            "routing_policy": routing_policy,
            "summary_json": str(summary_json),
            "status_json": str(status_json),
            "video_sanity_json": str(video_sanity_json),
            "mp4": str(mp4_out),
            "gop_routing_csv": str(gop_routing_csv),
            "encode_fps": encode_fps,
            "encode_max_fps": encode_max_fps,
        }
    },
}
fixed["external_recorder_contract"] = contract

with dest.open("w", encoding="utf-8") as f:
    json.dump(spec, f, indent=2)
    f.write("\n")
with contract_dest.open("w", encoding="utf-8") as f:
    json.dump(contract, f, indent=2)
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
if [[ "$PREWARM_BYTES" == "auto" ]]; then
  PREWARM_BYTES="$(python3 - "$TEMP_SPEC" "$CAMERA_SERIAL" <<'PY'
import json
import sys
from pathlib import Path

spec = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
camera_serial = sys.argv[2]
config_folder = Path(spec.get("fixed", {}).get("config_folder", ""))
path = config_folder / f"{camera_serial}.json"
try:
    data = json.loads(path.read_text(encoding="utf-8"))
    width = int(data.get("width", 0))
    height = int(data.get("height", 0))
    pixel_format = str(data.get("pixel_format", "Mono8"))
    bytes_per_pixel = 1 if pixel_format == "Mono8" else 0
    print(width * height * bytes_per_pixel)
except Exception:
    print(0)
PY
)"
fi

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
echo "[external-recorder] status_json=$STATUS_JSON"
echo "[external-recorder] prewarm_slots=$PREWARM_SLOTS prewarm_bytes=$PREWARM_BYTES prewarm_peer_copy=$PREWARM_PEER_COPY"
echo "[external-recorder] direct_input_source=$DIRECT_INPUT_SOURCE deferred_source_release=$DEFERRED_SOURCE_RELEASE"
echo "[external-recorder] yolo_prewarm_iterations=$YOLO_PREWARM_ITERATIONS"
echo "[external-recorder] ptp_register_read_decimate=$PTP_REGISTER_READ_DECIMATE"
echo "[external-recorder] analytics_early_owned_frame=$ANALYTICS_EARLY_OWNED_FRAME yolo_ready_event_fastpath=$YOLO_READY_EVENT_FASTPATH yolo_detach_input=$YOLO_DETACH_INPUT"

ROUTING_POLICY="single_shard"
if [[ -n "$SHARD_GPU_IDS" ]]; then
  ROUTING_POLICY="gop_modulo"
fi

RECORDER_ARGS=(
  --socket "$SOCKET_PATH"
  --gpu-id "$RECORDER_GPU_ID"
  --csv "$DETACH_CSV"
  --encode
  --encode-max-fps "$ENCODE_MAX_FPS"
  --encode-queue-depth "$QUEUE_DEPTH"
  --prewarm-slots "$PREWARM_SLOTS"
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
  --status-json "$STATUS_JSON"
  --session-id "$EXPERIMENT_ID"
  --stream-id "$CAMERA_SERIAL"
  --shard-id 0
  --routing-policy "$ROUTING_POLICY"
)
if [[ "$PREWARM_BYTES" =~ ^[0-9]+$ && "$PREWARM_BYTES" -gt 0 ]]; then
  RECORDER_ARGS+=(--prewarm-bytes "$PREWARM_BYTES")
fi
if [[ "$PREWARM_PEER_COPY" -eq 1 ]]; then
  RECORDER_ARGS+=(--prewarm-peer-copy)
fi
if [[ "$DIRECT_INPUT_SOURCE" != "0" ]]; then
  RECORDER_ARGS+=(--direct-input-source)
fi
if [[ "$DEFERRED_SOURCE_RELEASE" != "0" ]]; then
  RECORDER_ARGS+=(--deferred-source-release)
fi
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

echo "[external-recorder] starting headless external_ipc run camera=$CAMERA_SERIAL analytics_gpu=$ANALYTICS_GPU_ID recorder_gpu=$RECORDER_GPU_ID shard_gpu_ids=${SHARD_GPU_IDS:-none} encode_fps=$ENCODE_FPS encode_max_fps=$ENCODE_MAX_FPS"
if [[ "$SOCKET_PATH" != "/tmp/orange_external_recorder_${CAMERA_SERIAL}.sock" ]]; then
  echo "Custom socket paths require passing ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_${CAMERA_SERIAL} to the benchmark process." >&2
  echo "Use the default socket path for the sudo -n smoke runner." >&2
  exit 2
fi
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client "$ORANGE_CLIENT" \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  --analytics-early-owned-frame "$ANALYTICS_EARLY_OWNED_FRAME" \
  --yolo-ready-event-fastpath "$YOLO_READY_EVENT_FASTPATH" \
  --yolo-detach-input "$YOLO_DETACH_INPUT" \
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
print(f"  direct_input_source={summary.get('direct_input_source')} deferred_source_release={summary.get('deferred_source_release')}")
print(f"  session_id={summary.get('session_id')} assigned_gpu_id={summary.get('assigned_gpu_id')} assigned_shard_id={summary.get('assigned_shard_id')} routing_policy={summary.get('routing_policy')}")
print(f"  encode_enqueued={summary.get('encode_enqueued')} encode_skipped={summary.get('encode_skipped')} encode_dropped={summary.get('encode_dropped')} frames_encoded={summary.get('frames_encoded')}")
print(f"  source_releases_sent={enc.get('source_releases_sent')} source_release_failures={enc.get('source_release_failures')}")
print(f"  detach_copy_p95_ms={summary.get('detach_timing', {}).get('copy_p95_ms')} encode_total_p95_ms={enc.get('encode_total_p95_ms')} lock_bitstream_p95_ms={enc.get('lock_bitstream_p95_ms')}")
print(f"  prewarm_slots={enc.get('prewarm_slots')} prewarm_ms={enc.get('prewarm_ms')} prewarm_peer_copy={enc.get('prewarm_peer_copy')}")
print(f"  mp4_bytes={summary.get('output_file_sizes', {}).get('mp4_bytes')} worker_failed={summary.get('worker_failed')}")
for shard in summary.get("external_encode_shards", []):
    retention = shard.get("mp4_retention", {})
    print(
        "  shard "
        f"id={shard.get('assigned_shard_id')} gpu={shard.get('assigned_gpu_id')} "
        f"frames_encoded={shard.get('frames_encoded')} "
        f"prewarm_ms={shard.get('prewarm_ms')} "
        f"mp4_retention={retention.get('status')} mp4={shard.get('mp4')}"
    )
PY
fi

if [[ "$SKIP_VIDEO_SANITY" -eq 0 ]]; then
  VIDEO_SANITY_MP4="$(python3 - "$SUMMARY_JSON" "$MP4_OUT" <<'PY'
import json
import sys
from pathlib import Path

summary_path = Path(sys.argv[1])
fallback = sys.argv[2]
try:
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    rolling = summary.get("rolling_output") or {}
    clips = rolling.get("clips") or [] if rolling.get("enabled") else []
    print(clips[0].get("mp4") if clips else fallback)
except Exception:
    print(fallback)
PY
)"
  python3 - "$VIDEO_SANITY_MP4" "$VIDEO_SANITY_JSON" <<'PY'
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
fi

VERIFY_ARGS=(
  "$RUN_DIR"
  --analytics-root "$ANALYTICS_ROOT"
  --expect-encode-queue-depth "$QUEUE_DEPTH"
)
if [[ -n "$VERIFY_MAX_ENCODE_QUEUE_HIGH_WATER" ]]; then
  VERIFY_ARGS+=(--max-encode-queue-high-water "$VERIFY_MAX_ENCODE_QUEUE_HIGH_WATER")
fi
if [[ -n "$VERIFY_MAX_ENQUEUE_AGE_P95_MS" ]]; then
  VERIFY_ARGS+=(--max-enqueue-age-p95-ms "$VERIFY_MAX_ENQUEUE_AGE_P95_MS")
fi
"$REPO_ROOT/scripts/verify_external_recorder_session.py" "${VERIFY_ARGS[@]}"
