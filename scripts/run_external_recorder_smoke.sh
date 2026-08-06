#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run_external_recorder_smoke.sh [options]

Runs a one-camera headless real-YOLO external-recorder smoke:
  orange_client supervises external_recorder_ipc_probe and owns the recording,
  finalization, manifest, and verification lifecycle.

Options:
  --spec <path>              Base experiment spec.
  --orange-client <path>     orange_client binary.
  --recorder-tool <path>     external_recorder_ipc_probe binary.
  --camera-serial <serial>   Camera serial. Default 2010096.
  --analytics-gpu-id <int>   GPU id selected for camera/YOLO. Default 5.
  --recorder-gpu-id <int>    GPU id used by external recorder. Default: analytics GPU.
  --shard-gpu-ids <csv>      Split-GOP recorder GPUs. Default 5,6 on the default rig.
  --duration <sec>           Timed recording duration. Default 3.
  --clip-seconds <sec>       Rolling clip duration. Default 0 (single clip).
  --warmup <sec>             Headless warmup before recording. Default 1.
  --encode-fps <int>         Source and nominal MP4 FPS. Default 100.
  --encode-max-fps <int>     External encode cap only. Use 0 for uncapped.
                             Default 0 (uncapped).
  --queue-depth <int>        External recorder-owned frame slots. Default 32.
  --prewarm-slots <int>      Prewarm encode detach slots per shard. Default 4.
  --prewarm-bytes <int|auto> Pre-listen prewarm byte size. Default auto from spec config.
  --no-prewarm-peer-copy     Do not warm the first source-to-shard peer copy.
  --direct-input-source      Legacy diagnostic option; rejected by this supervised runner.
  --deferred-source-release  Legacy diagnostic option; rejected by this supervised runner.
  --yolo-prewarm-iterations <int> Synthetic YOLO prewarm iterations. Default 3.
  --ptp-register-read-decimate <n> Read GevTimestampValue every n frames. Default 1.
  --socket <path>            Unix socket path. Default /tmp/orange_external_recorder_<serial>.sock.
                             Non-default paths require matching client env outside this script.
  --output-dir <path>        External recorder artifact root. Default /tmp.
  --bitstream-out <path>     Legacy diagnostic option; rejected by this supervised runner.
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

SPEC="$REPO_ROOT/experiment_specs/2010096_headless_real_yolo_external_ipc_supervised_encode_smoke.json"
ORANGE_CLIENT="$REPO_ROOT/targets/release/orange_client"
RECORDER_TOOL="$REPO_ROOT/targets/release/external_recorder_ipc_probe"
CAMERA_SERIAL=2010096
ANALYTICS_GPU_ID=5
RECORDER_GPU_ID=""
SHARD_GPU_IDS=""
DURATION=3
CLIP_SECONDS=0
WARMUP=1
ENCODE_FPS=100
ENCODE_MAX_FPS=0
QUEUE_DEPTH=32
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
    --clip-seconds)
      shift
      [[ $# -gt 0 ]] || { echo "--clip-seconds requires a value." >&2; exit 2; }
      CLIP_SECONDS="$1"
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

for value_name in ANALYTICS_GPU_ID DURATION CLIP_SECONDS WARMUP ENCODE_FPS ENCODE_MAX_FPS QUEUE_DEPTH PREWARM_SLOTS YOLO_PREWARM_ITERATIONS PTP_REGISTER_READ_DECIMATE; do
  value="${!value_name}"
  [[ "$value" =~ ^[0-9]+$ ]] || { echo "$value_name must be a non-negative integer." >&2; exit 2; }
done
if [[ "$DURATION" -lt 1 || "$ENCODE_FPS" -lt 1 || "$QUEUE_DEPTH" -lt 1 ]]; then
  echo "DURATION, ENCODE_FPS, and QUEUE_DEPTH must be positive." >&2
  exit 2
fi
if [[ "$CLIP_SECONDS" -gt "$DURATION" ]]; then
  echo "CLIP_SECONDS must not exceed DURATION." >&2
  exit 2
fi
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
if [[ -z "$SHARD_GPU_IDS" && "$RECORDER_GPU_ID" == "5" ]]; then
  SHARD_GPU_IDS="5,6"
fi
if [[ -n "$SHARD_GPU_IDS" ]]; then
  [[ "$SHARD_GPU_IDS" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
    echo "SHARD_GPU_IDS must contain one or more comma-separated GPU ids." >&2
    exit 2
  }
  if [[ "${SHARD_GPU_IDS%%,*}" != "$RECORDER_GPU_ID" ]]; then
    echo "RECORDER_GPU_ID must match the first SHARD_GPU_IDS entry." >&2
    exit 2
  fi
fi
if [[ "$DIRECT_INPUT_SOURCE" != "0" || "$DEFERRED_SOURCE_RELEASE" != "0" ]]; then
  echo "Direct-input/deferred-release diagnostics are not supported by the supervised smoke runner." >&2
  exit 2
fi
if [[ -n "$BITSTREAM_OUT" ]]; then
  echo "Raw bitstream output is not supported by the supervised smoke runner." >&2
  exit 2
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
KEYFRAME_OUT="$RUN_DIR/Cam${CAMERA_SERIAL}_external_keyframes.json"
RECORDER_LOG="$RUN_DIR/external_recorder.log"
SESSION_CONTRACT_JSON="$RUN_DIR/external_recorder_session.json"

python3 - "$SPEC" "$TEMP_SPEC" "$SESSION_CONTRACT_JSON" "$STAMP" "$CAMERA_SERIAL" "$ANALYTICS_GPU_ID" "$RECORDER_GPU_ID" "$SHARD_GPU_IDS" "$DURATION" "$WARMUP" "$YOLO_PREWARM_ITERATIONS" "$PTP_REGISTER_READ_DECIMATE" "$RUN_DIR" "$SUMMARY_JSON" "$STATUS_JSON" "$VIDEO_SANITY_JSON" "$MP4_OUT" "$GOP_ROUTING_CSV" "$ENCODE_FPS" "$ENCODE_MAX_FPS" "$SKIP_VIDEO_SANITY" "$QUEUE_DEPTH" "$PREWARM_SLOTS" "$PREWARM_BYTES" "$PREWARM_PEER_COPY" "$RECORDER_TOOL" "$SOCKET_PATH" "$DETACH_CSV" "$ENCODE_CSV" "$KEYFRAME_OUT" "$RECORDER_LOG" "$CLIP_SECONDS" <<'PY'
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
queue_depth = int(sys.argv[22])
prewarm_slots = int(sys.argv[23])
prewarm_bytes_arg = sys.argv[24]
prewarm_peer_copy = bool(int(sys.argv[25]))
recorder_tool = Path(sys.argv[26])
socket_path = Path(sys.argv[27])
detach_csv = Path(sys.argv[28])
encode_csv = Path(sys.argv[29])
keyframe_out = Path(sys.argv[30])
recorder_log = Path(sys.argv[31])
clip_seconds = int(sys.argv[32])

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
fixed["duration_s"] = duration + warmup + 1
fixed["warmup_s"] = warmup
fixed["display"] = False
fixed["stream_only"] = False
fixed["recording_sink_mode"] = "external_ipc"
fixed["ptp_register_read_decimate"] = ptp_register_read_decimate
fixed["recording_control"] = {
    "record_for_seconds": duration,
    "clip_seconds": clip_seconds,
}
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
    "recorder_tool_path": str(recorder_tool),
    "supervise_processes": True,
    "artifact_root": str(run_dir),
    "session_id": spec["experiment_id"],
    "require_summary": True,
    "require_status": True,
    "require_storage_preflight": True,
    "require_protocol_hello": True,
    "require_video_sanity": skip_video_sanity == 0,
    "require_merged_mp4": clip_seconds == 0,
    "require_frame_identity_proof": True,
    "require_gop_routing": True,
    "preserve_shard_mp4s": False,
    "recording_control": {
        "record_for_seconds": duration,
        "clip_seconds": clip_seconds,
    },
    "streams": {
        camera_serial: {
            "stream_id": camera_serial,
            "camera_serial": camera_serial,
            "analytics_gpu_id": gpu_id,
            "recorder_gpu_id": recorder_gpu_id,
            "expected_shard_gpu_ids": expected_shard_gpu_ids,
            "routing_policy": routing_policy,
            "socket_path": str(socket_path),
            "summary_json": str(summary_json),
            "status_json": str(status_json),
            "video_sanity_json": str(video_sanity_json),
            "mp4": str(mp4_out),
            "mp4_keyframe": str(keyframe_out),
            "detach_csv": str(detach_csv),
            "encode_csv": str(encode_csv),
            "gop_routing_csv": str(gop_routing_csv),
            "recorder_log": str(recorder_log),
            "encode_fps": encode_fps,
            "encode_max_fps": encode_max_fps,
            "encode_queue_depth": queue_depth,
            "prewarm_slots": prewarm_slots,
            "prewarm_peer_copy": prewarm_peer_copy,
            "codec": "hevc",
            "preset": "p1",
            "tuning": "ll",
            "rate_control_mode": "vbr",
            "quality_value": 20,
            "gop": 25,
            "bitrate_bps": 150000000,
            "max_bitrate_bps": 150000000,
            "vbv_buffer_size": 150000000,
        }
    },
}
if prewarm_bytes_arg != "auto":
    contract["streams"][camera_serial]["prewarm_bytes"] = int(prewarm_bytes_arg)
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
echo "[external-recorder] starting supervised headless external_ipc run camera=$CAMERA_SERIAL analytics_gpu=$ANALYTICS_GPU_ID recorder_gpu=$RECORDER_GPU_ID shard_gpu_ids=${SHARD_GPU_IDS:-none} encode_fps=$ENCODE_FPS encode_max_fps=$ENCODE_MAX_FPS record_for_seconds=$DURATION clip_seconds=$CLIP_SECONDS"
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client "$ORANGE_CLIENT" \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  --analytics-early-owned-frame "$ANALYTICS_EARLY_OWNED_FRAME" \
  --yolo-ready-event-fastpath "$YOLO_READY_EVENT_FASTPATH" \
  --yolo-detach-input "$YOLO_DETACH_INPUT" \
  "$TEMP_SPEC"

echo "[external-recorder] analytics complete"
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
