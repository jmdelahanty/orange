#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run_external_recorder_two_camera_ptp_smoke.sh [options]

Runs a two-camera headless real-YOLO PTP external-recorder smoke:
  Process A: orange_client with recording_sink_mode=external_ipc.
  Process B/C: one external_recorder_ipc_probe per camera.

Defaults match the local A16 PTP pairing:
  2010095 -> analytics GPU 5, recorder shards 5,6
  2010096 -> analytics GPU 7, recorder shards 7,8

Options:
  --spec <path>                    Base experiment spec.
  --orange-client <path>           orange_client binary.
  --recorder-tool <path>           external_recorder_ipc_probe binary.
  --camera-serials <csv>           Camera serials. Default 2010095,2010096.
  --analytics-gpu-ids <csv>        Analytics GPU ids. Default 5,7.
  --shard-gpu-ids-per-camera <;>   Shard groups. Default '5,6;7,8'.
  --duration <sec>                 Headless run duration. Default 3.
  --warmup <sec>                   Headless warmup. Default 1.
  --encode-fps <int>               Nominal MP4 FPS. Default 100.
  --encode-max-fps <int>           External encode cap. Use 0 for uncapped. Default 0.
  --queue-depth <int>              External recorder-owned frame slots. Default 32.
  --prewarm-slots <int>            Prewarm encode detach slots per shard. Default 4.
  --prewarm-bytes <int|auto>       Pre-listen prewarm byte size. Default auto from config.
  --no-prewarm-peer-copy           Do not warm the first source-to-shard peer copy.
  --yolo-prewarm-iterations <int>  Synthetic YOLO prewarm iterations. Default 3.
  --ptp-register-read-decimate <n> Read GevTimestampValue every n frames. Default 1.
  --steady-state-after-frame <n>   Report steady-state metrics after this frame. Default 50.
  --config-folder <path>           Camera config folder. Default local/100_cam4_ptp.
  --output-dir <path>              External recorder artifact root. Default /tmp.
  --skip-video-sanity              Do not decode/check external MP4 content.
  --max-encode-queue-high-water <int>
                                   Verifier threshold for recorder encode queue high-water.
  --max-enqueue-age-p95-ms <float>
                                   Verifier threshold for recorder enqueue-age p95.
  --keep-temp-spec                 Keep generated temp spec in output dir.
  --help
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SPEC="$REPO_ROOT/experiment_specs/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp.json"
ORANGE_CLIENT="$REPO_ROOT/targets/release/orange_client"
RECORDER_TOOL="$REPO_ROOT/targets/release/external_recorder_ipc_probe"
CAMERA_SERIALS="2010095,2010096"
ANALYTICS_GPU_IDS="5,7"
SHARD_GPU_IDS_PER_CAMERA="5,6;7,8"
DURATION=3
WARMUP=1
ENCODE_FPS=100
ENCODE_MAX_FPS=0
QUEUE_DEPTH=32
PREWARM_SLOTS=4
PREWARM_BYTES=auto
PREWARM_PEER_COPY=1
YOLO_PREWARM_ITERATIONS=3
PTP_REGISTER_READ_DECIMATE=1
ANALYTICS_EARLY_OWNED_FRAME="${ORANGE_ANALYTICS_EARLY_OWNED_FRAME:-1}"
YOLO_READY_EVENT_FASTPATH="${ORANGE_YOLO_READY_EVENT_FASTPATH:-1}"
YOLO_DETACH_INPUT="${ORANGE_YOLO_DETACH_INPUT:-1}"
STEADY_STATE_AFTER_FRAME=50
CONFIG_FOLDER="/home/jeremy/orange_data/config/local/100_cam4_ptp"
OUTPUT_DIR="/tmp"
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
    --camera-serials)
      shift
      [[ $# -gt 0 ]] || { echo "--camera-serials requires a value." >&2; exit 2; }
      CAMERA_SERIALS="$1"
      shift
      ;;
    --analytics-gpu-ids)
      shift
      [[ $# -gt 0 ]] || { echo "--analytics-gpu-ids requires a value." >&2; exit 2; }
      ANALYTICS_GPU_IDS="$1"
      shift
      ;;
    --shard-gpu-ids-per-camera)
      shift
      [[ $# -gt 0 ]] || { echo "--shard-gpu-ids-per-camera requires a value." >&2; exit 2; }
      SHARD_GPU_IDS_PER_CAMERA="$1"
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
    --steady-state-after-frame)
      shift
      [[ $# -gt 0 ]] || { echo "--steady-state-after-frame requires a value." >&2; exit 2; }
      STEADY_STATE_AFTER_FRAME="$1"
      shift
      ;;
    --config-folder)
      shift
      [[ $# -gt 0 ]] || { echo "--config-folder requires a value." >&2; exit 2; }
      CONFIG_FOLDER="$1"
      shift
      ;;
    --output-dir)
      shift
      [[ $# -gt 0 ]] || { echo "--output-dir requires a value." >&2; exit 2; }
      OUTPUT_DIR="$1"
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

for value_name in DURATION WARMUP ENCODE_FPS ENCODE_MAX_FPS QUEUE_DEPTH PREWARM_SLOTS YOLO_PREWARM_ITERATIONS PTP_REGISTER_READ_DECIMATE STEADY_STATE_AFTER_FRAME; do
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

IFS=',' read -r -a CAMERAS <<<"$CAMERA_SERIALS"
IFS=',' read -r -a ANALYTICS_GPUS <<<"$ANALYTICS_GPU_IDS"
IFS=';' read -r -a SHARD_GROUPS <<<"$SHARD_GPU_IDS_PER_CAMERA"

CAMERA_COUNT="${#CAMERAS[@]}"
if [[ "$CAMERA_COUNT" -lt 1 ]]; then
  echo "At least one camera is required." >&2
  exit 2
fi
if [[ "${#ANALYTICS_GPUS[@]}" -ne "$CAMERA_COUNT" ]]; then
  echo "--analytics-gpu-ids must have one entry per camera." >&2
  exit 2
fi
if [[ "${#SHARD_GROUPS[@]}" -ne "$CAMERA_COUNT" ]]; then
  echo "--shard-gpu-ids-per-camera must have one semicolon-separated group per camera." >&2
  exit 2
fi
for i in "${!CAMERAS[@]}"; do
  [[ "${CAMERAS[$i]}" =~ ^[0-9]+$ ]] || { echo "Invalid camera serial: ${CAMERAS[$i]}" >&2; exit 2; }
  [[ "${ANALYTICS_GPUS[$i]}" =~ ^[0-9]+$ ]] || { echo "Invalid analytics GPU id: ${ANALYTICS_GPUS[$i]}" >&2; exit 2; }
  [[ "${SHARD_GROUPS[$i]}" =~ ^[0-9]+(,[0-9]+)*$ ]] || {
    echo "Invalid shard GPU group: ${SHARD_GROUPS[$i]}" >&2
    exit 2
  }
done

SPEC="$(realpath -e "$SPEC")"
ORANGE_CLIENT="$(realpath -e "$ORANGE_CLIENT")"
RECORDER_TOOL="$(realpath -e "$RECORDER_TOOL")"
CONFIG_FOLDER="$(realpath -e "$CONFIG_FOLDER")"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(realpath -m "$OUTPUT_DIR")"

STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$OUTPUT_DIR/orange_external_recorder_ptp_${STAMP}"
mkdir -p "$RUN_DIR"
TEMP_SPEC="$RUN_DIR/external_recorder_two_camera_ptp_spec.json"
SESSION_CONTRACT_JSON="$RUN_DIR/external_recorder_session.json"

python3 - "$SPEC" "$TEMP_SPEC" "$SESSION_CONTRACT_JSON" "$STAMP" "$CAMERA_SERIALS" "$ANALYTICS_GPU_IDS" "$SHARD_GPU_IDS_PER_CAMERA" "$DURATION" "$WARMUP" "$CONFIG_FOLDER" "$YOLO_PREWARM_ITERATIONS" "$PTP_REGISTER_READ_DECIMATE" "$RUN_DIR" "$ENCODE_FPS" "$ENCODE_MAX_FPS" "$SKIP_VIDEO_SANITY" <<'PY'
import json
import sys
from pathlib import Path

source = Path(sys.argv[1])
dest = Path(sys.argv[2])
contract_dest = Path(sys.argv[3])
stamp = sys.argv[4]
camera_serials = [item for item in sys.argv[5].split(",") if item]
gpu_ids = [int(item) for item in sys.argv[6].split(",") if item]
shard_groups = [item for item in sys.argv[7].split(";") if item]
duration = int(sys.argv[8])
warmup = int(sys.argv[9])
config_folder = sys.argv[10]
yolo_prewarm_iterations = int(sys.argv[11])
ptp_register_read_decimate = int(sys.argv[12])
run_dir = Path(sys.argv[13])
encode_fps = int(sys.argv[14])
encode_max_fps = int(sys.argv[15])
skip_video_sanity = int(sys.argv[16])

with source.open("r", encoding="utf-8") as f:
    spec = json.load(f)

base_id = spec.get("experiment_id") or "external_recorder_two_camera_ptp"
spec["experiment_id"] = f"{base_id}_external_ipc_{stamp}"
spec["notes"] = (
    spec.get("notes", "") +
    " Generated by run_external_recorder_two_camera_ptp_smoke.sh."
).strip()
spec.setdefault("selection", {})["camera_serials"] = camera_serials
spec.setdefault("selection", {})["gpu_ids"] = gpu_ids
fixed = spec.setdefault("fixed", {})
fixed["duration_s"] = duration
fixed["warmup_s"] = warmup
fixed["display"] = False
fixed["stream_only"] = False
fixed["sync_mode"] = "ptp_gate"
fixed["recording_sink_mode"] = "external_ipc"
fixed["config_folder"] = config_folder
fixed["ptp_register_read_decimate"] = ptp_register_read_decimate
if isinstance(fixed.get("yolo_worker"), dict):
    fixed["yolo_worker"]["prewarm_iterations"] = yolo_prewarm_iterations
streams = {}
for serial, analytics_gpu_id, shard_group in zip(camera_serials, gpu_ids, shard_groups):
    shard_gpu_ids = [int(item) for item in shard_group.split(",") if item]
    routing_policy = "gop_modulo" if len(shard_gpu_ids) > 1 else "single_shard"
    streams[serial] = {
        "stream_id": serial,
        "camera_serial": serial,
        "analytics_gpu_id": analytics_gpu_id,
        "recorder_gpu_id": shard_gpu_ids[0],
        "expected_shard_gpu_ids": shard_gpu_ids,
        "routing_policy": routing_policy,
        "summary_json": str(run_dir / f"Cam{serial}_external_summary.json"),
        "status_json": str(run_dir / f"Cam{serial}_external_status.json"),
        "video_sanity_json": str(run_dir / f"Cam{serial}_external_video_sanity.json"),
        "mp4": str(run_dir / f"Cam{serial}_external.mp4"),
        "gop_routing_csv": str(run_dir / f"Cam{serial}_external_gop_routing.csv"),
        "encode_fps": encode_fps,
        "encode_max_fps": encode_max_fps,
    }
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
    "require_merged_mp4": True,
    "require_gop_routing": True,
    "streams": streams,
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

declare -a RECORDER_PIDS=()
declare -a SOCKETS=()
declare -a SUMMARY_JSONS=()
declare -a VIDEO_SANITY_JSONS=()
declare -a MP4_OUTS=()
declare -a RECORDER_LOGS=()

cleanup() {
  for pid in "${RECORDER_PIDS[@]:-}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
      echo "[external-recorder-ptp] stopping recorder pid=$pid"
      kill -TERM "$pid" >/dev/null 2>&1 || true
      wait "$pid" || true
    fi
  done
  for socket in "${SOCKETS[@]:-}"; do
    rm -f "$socket"
  done
  if [[ "$KEEP_TEMP_SPEC" -eq 0 ]]; then
    rm -f "$TEMP_SPEC"
  fi
}
trap cleanup EXIT

echo "[external-recorder-ptp] run_dir=$RUN_DIR"
echo "[external-recorder-ptp] analytics_root=$ANALYTICS_ROOT"
echo "[external-recorder-ptp] cameras=$CAMERA_SERIALS analytics_gpus=$ANALYTICS_GPU_IDS shard_groups=$SHARD_GPU_IDS_PER_CAMERA"
echo "[external-recorder-ptp] encode_fps=$ENCODE_FPS encode_max_fps=$ENCODE_MAX_FPS queue_depth=$QUEUE_DEPTH"
echo "[external-recorder-ptp] prewarm_slots=$PREWARM_SLOTS prewarm_bytes=$PREWARM_BYTES prewarm_peer_copy=$PREWARM_PEER_COPY"
echo "[external-recorder-ptp] yolo_prewarm_iterations=$YOLO_PREWARM_ITERATIONS"
echo "[external-recorder-ptp] ptp_register_read_decimate=$PTP_REGISTER_READ_DECIMATE"
echo "[external-recorder-ptp] analytics_early_owned_frame=$ANALYTICS_EARLY_OWNED_FRAME yolo_ready_event_fastpath=$YOLO_READY_EVENT_FASTPATH yolo_detach_input=$YOLO_DETACH_INPUT"
echo "[external-recorder-ptp] steady_state_after_frame=$STEADY_STATE_AFTER_FRAME"
echo "[external-recorder-ptp] config_folder=$CONFIG_FOLDER"

for i in "${!CAMERAS[@]}"; do
  serial="${CAMERAS[$i]}"
  analytics_gpu="${ANALYTICS_GPUS[$i]}"
  shard_group="${SHARD_GROUPS[$i]}"
  recorder_gpu="${shard_group%%,*}"
  socket="/tmp/orange_external_recorder_${serial}.sock"
  detach_csv="$RUN_DIR/Cam${serial}_external_detach.csv"
  encode_csv="$RUN_DIR/Cam${serial}_external_encode.csv"
  gop_routing_csv="$RUN_DIR/Cam${serial}_external_gop_routing.csv"
  summary_json="$RUN_DIR/Cam${serial}_external_summary.json"
  status_json="$RUN_DIR/Cam${serial}_external_status.json"
  video_sanity_json="$RUN_DIR/Cam${serial}_external_video_sanity.json"
  mp4_out="$RUN_DIR/Cam${serial}_external.mp4"
  keyframe_out="$RUN_DIR/Cam${serial}_external_keyframes.csv"
  recorder_log="$RUN_DIR/Cam${serial}_external_recorder.log"
  routing_policy="single_shard"
  if [[ "$shard_group" == *,* ]]; then
    routing_policy="gop_modulo"
  fi
  camera_prewarm_bytes="$PREWARM_BYTES"
  if [[ "$camera_prewarm_bytes" == "auto" ]]; then
    camera_prewarm_bytes="$(python3 - "$CONFIG_FOLDER/$serial.json" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
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

  rm -f "$socket"
  SOCKETS+=("$socket")
  SUMMARY_JSONS+=("$summary_json")
  VIDEO_SANITY_JSONS+=("$video_sanity_json")
  MP4_OUTS+=("$mp4_out")
  RECORDER_LOGS+=("$recorder_log")

  RECORDER_ARGS=(
    --socket "$socket" \
    --gpu-id "$recorder_gpu" \
    --csv "$detach_csv" \
    --encode \
    --encode-max-fps "$ENCODE_MAX_FPS" \
    --encode-queue-depth "$QUEUE_DEPTH" \
    --prewarm-slots "$PREWARM_SLOTS" \
    --fps "$ENCODE_FPS" \
    --codec hevc \
    --preset p1 \
    --tuning ll \
    --gop 25 \
    --bitrate-bps 150000000 \
    --max-bitrate-bps 150000000 \
    --vbv-buffer-size 150000000 \
    --mp4-out "$mp4_out" \
    --mp4-keyframe "$keyframe_out" \
    --encode-csv "$encode_csv" \
    --gop-routing-csv "$gop_routing_csv" \
    --summary-json "$summary_json" \
    --status-json "$status_json" \
    --session-id "$EXPERIMENT_ID" \
    --stream-id "$serial" \
    --shard-id 0 \
    --routing-policy "$routing_policy"
  )
  if [[ "$camera_prewarm_bytes" =~ ^[0-9]+$ && "$camera_prewarm_bytes" -gt 0 ]]; then
    RECORDER_ARGS+=(--prewarm-bytes "$camera_prewarm_bytes")
  fi
  if [[ "$PREWARM_PEER_COPY" -eq 1 ]]; then
    RECORDER_ARGS+=(--prewarm-peer-copy)
  fi
  if [[ "$shard_group" == *,* ]]; then
    RECORDER_ARGS+=(--shard-gpu-ids "$shard_group")
  fi

  "$RECORDER_TOOL" "${RECORDER_ARGS[@]}" >"$recorder_log" 2>&1 &
  pid=$!
  RECORDER_PIDS+=("$pid")
  echo "[external-recorder-ptp] recorder camera=$serial analytics_gpu=$analytics_gpu recorder_gpu=$recorder_gpu shards=$shard_group prewarm_bytes=$camera_prewarm_bytes pid=$pid socket=$socket"
done

for i in "${!SOCKETS[@]}"; do
  socket="${SOCKETS[$i]}"
  pid="${RECORDER_PIDS[$i]}"
  log="${RECORDER_LOGS[$i]}"
  for _ in {1..100}; do
    if [[ -S "$socket" ]]; then
      break
    fi
    if ! kill -0 "$pid" >/dev/null 2>&1; then
      echo "external recorder exited before creating socket: $socket" >&2
      cat "$log" >&2 || true
      exit 1
    fi
    sleep 0.05
  done
  [[ -S "$socket" ]] || { echo "external recorder socket was not created: $socket" >&2; exit 1; }
done

echo "[external-recorder-ptp] starting headless external_ipc PTP benchmark"
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client "$ORANGE_CLIENT" \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  --analytics-early-owned-frame "$ANALYTICS_EARLY_OWNED_FRAME" \
  --yolo-ready-event-fastpath "$YOLO_READY_EVENT_FASTPATH" \
  --yolo-detach-input "$YOLO_DETACH_INPUT" \
  "$TEMP_SPEC"

echo "[external-recorder-ptp] analytics complete"
for pid in "${RECORDER_PIDS[@]}"; do
  wait "$pid"
done
trap - EXIT
for socket in "${SOCKETS[@]}"; do
  rm -f "$socket"
done
if [[ "$KEEP_TEMP_SPEC" -eq 0 ]]; then
  rm -f "$TEMP_SPEC"
fi

echo "[external-recorder-ptp] outputs:"
echo "  run_dir=$RUN_DIR"
echo "  analytics_root=$ANALYTICS_ROOT"

for i in "${!CAMERAS[@]}"; do
  serial="${CAMERAS[$i]}"
  summary_json="${SUMMARY_JSONS[$i]}"
  mp4_out="${MP4_OUTS[$i]}"
  video_sanity_json="${VIDEO_SANITY_JSONS[$i]}"
  recorder_log="${RECORDER_LOGS[$i]}"
  echo "  camera=$serial recorder_log=$recorder_log"
  echo "  camera=$serial summary_json=$summary_json"
  echo "  camera=$serial mp4_out=$mp4_out"
  echo "  camera=$serial video_sanity_json=$video_sanity_json"
  if [[ -f "$summary_json" ]]; then
    python3 - "$summary_json" <<'PY'
import json
import sys
from pathlib import Path

summary = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
enc = summary.get("external_encode", {})
merged = summary.get("merged_output", {})
print(
    "[external-recorder-ptp] summary "
    f"stream={summary.get('stream_id')} "
    f"received={summary.get('frames_received')} acked={summary.get('acks_sent')} "
    f"encoded={summary.get('frames_encoded')} skipped={summary.get('encode_skipped')} "
    f"dropped={summary.get('encode_dropped')} failed={summary.get('worker_failed')} "
    f"lock_p95_ms={enc.get('lock_bitstream_p95_ms')} "
    f"prewarm_ms={enc.get('prewarm_ms')} "
    f"merged_packets={merged.get('packets_written')} pending_gops={merged.get('pending_gops')}"
)
for shard in summary.get("external_encode_shards", []):
    print(
        "[external-recorder-ptp] shard "
        f"stream={summary.get('stream_id')} "
        f"id={shard.get('assigned_shard_id')} gpu={shard.get('assigned_gpu_id')} "
        f"encoded={shard.get('frames_encoded')} dropped={shard.get('frames_dropped')} "
        f"prewarm_ms={shard.get('prewarm_ms')}"
    )
PY
  fi
done

if [[ "$SKIP_VIDEO_SANITY" -eq 0 ]]; then
  python3 - "$RUN_DIR" "${MP4_OUTS[@]}" "${VIDEO_SANITY_JSONS[@]}" <<'PY'
import json
import math
import subprocess
import sys
from pathlib import Path

run_dir = Path(sys.argv[1])
arg_count = len(sys.argv) - 2
half = arg_count // 2
mp4_paths = [Path(p) for p in sys.argv[2:2 + half]]
summary_paths = [Path(p) for p in sys.argv[2 + half:]]

def write_result(path, result):
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

for mp4_path, summary_path in zip(mp4_paths, summary_paths):
    result = {
        "schema_version": 1,
        "video_path": str(mp4_path),
        "content_checked": True,
        "content_valid": False,
        "status": "unknown",
        "sampled_frames": [],
    }
    if not mp4_path.exists() or mp4_path.stat().st_size == 0:
        result["status"] = "missing_video"
        write_result(summary_path, result)
        raise SystemExit(f"missing MP4 output: {mp4_path}")

    probe = subprocess.run(
        [
            "ffprobe", "-v", "error", "-select_streams", "v:0",
            "-show_entries", "stream=width,height,nb_frames,duration",
            "-show_entries", "format=size,duration", "-of", "json",
            str(mp4_path),
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    metadata = json.loads(probe.stdout)
    streams = metadata.get("streams") or []
    if not streams:
        result["status"] = "no_video_stream"
        write_result(summary_path, result)
        raise SystemExit(f"no video stream: {mp4_path}")

    stream = streams[0]
    width = int(stream.get("width") or 0)
    height = int(stream.get("height") or 0)
    try:
        frame_count = int(stream.get("nb_frames") or 0)
    except ValueError:
        frame_count = 0
    if width <= 0 or height <= 0:
        result["status"] = "invalid_dimensions"
        write_result(summary_path, result)
        raise SystemExit(f"invalid dimensions: {mp4_path}")

    sample_indices = [0]
    if frame_count > 1:
        sample_indices = sorted({0, frame_count // 2, frame_count - 1})
    select_expr = "+".join(f"eq(n\\,{index})" for index in sample_indices)
    decoded = subprocess.run(
        [
            "ffmpeg", "-v", "error", "-i", str(mp4_path),
            "-vf", f"select='{select_expr}'", "-vsync", "0",
            "-pix_fmt", "gray", "-f", "rawvideo", "-",
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout

    frame_bytes = width * height
    decoded_frames = len(decoded) // frame_bytes if frame_bytes else 0
    if decoded_frames == 0:
        result["status"] = "decode_empty"
        write_result(summary_path, result)
        raise SystemExit(f"decode returned no frames: {mp4_path}")

    measurements = []
    for i in range(decoded_frames):
        frame = decoded[i * frame_bytes:(i + 1) * frame_bytes]
        hist = [0] * 256
        for value in frame:
            hist[value] += 1
        pixels = sum(hist)
        total = sum(value * count for value, count in enumerate(hist))
        total_sq = sum(value * value * count for value, count in enumerate(hist))
        mean = total / pixels
        variance = max(0.0, total_sq / pixels - mean * mean)
        measurements.append({
            "requested_frame_index": sample_indices[min(i, len(sample_indices) - 1)],
            "mean": mean,
            "stddev": math.sqrt(variance),
            "black_fraction_lt8": sum(hist[:8]) / pixels,
            "decoded_bytes": pixels,
        })

    max_black = max(item["black_fraction_lt8"] for item in measurements)
    max_stddev = max(item["stddev"] for item in measurements)
    mean_luma = sum(item["mean"] for item in measurements) / len(measurements)
    content_valid = max_black < 0.98 and max_stddev >= 5.0
    result.update({
        "content_valid": content_valid,
        "status": "pass" if content_valid else ("black_frame" if max_black >= 0.98 else "flat_frame"),
        "width": width,
        "height": height,
        "nb_frames": frame_count,
        "container": metadata.get("format", {}),
        "sampled_frame_count": len(measurements),
        "mean_luma": mean_luma,
        "max_stddev": max_stddev,
        "max_black_fraction_lt8": max_black,
        "sampled_frames": measurements,
    })
    write_result(summary_path, result)
    print(
        "[external-recorder-ptp] video_sanity "
        f"path={mp4_path} status={result['status']} frames={frame_count} "
        f"mean_luma={mean_luma:.3f} max_stddev={max_stddev:.3f} "
        f"max_black_fraction_lt8={max_black:.6f}"
    )
    if not content_valid:
        raise SystemExit(f"video sanity failed: {mp4_path}")

aggregate = {
    "schema_version": 1,
    "videos_checked": len(mp4_paths),
    "status": "pass",
    "mp4_paths": [str(path) for path in mp4_paths],
}
write_result(run_dir / "external_video_sanity_summary.json", aggregate)
PY
fi

python3 - "$RUN_DIR" "$ANALYTICS_ROOT" "$CAMERA_SERIALS" "$STEADY_STATE_AFTER_FRAME" <<'PY'
import csv
import json
import sys
from pathlib import Path

run_dir = Path(sys.argv[1])
analytics_root = Path(sys.argv[2])
cameras = [item for item in sys.argv[3].split(",") if item]
steady_after = int(sys.argv[4])

yolo_fields = [
    "acquisition_to_detect_done_ms",
    "acquisition_to_worker_start_ms",
    "cpu_preprocess_ms",
    "cpu_pre_sync_ms",
    "total_ms",
]
detach_fields = [
    "total_ms",
    "open_handle_ms",
    "copy_ms",
]

def percentile(sorted_values, pct):
    if not sorted_values:
        return None
    index = int((pct / 100.0) * (len(sorted_values) - 1))
    return sorted_values[index]

def summarize_rows(rows, field_names):
    out = {"rows": len(rows), "fields": {}}
    for field in field_names:
        values = []
        max_row = None
        max_value = None
        for row in rows:
            value = row.get(field)
            if value in ("", None):
                continue
            try:
                parsed = float(value)
            except ValueError:
                continue
            values.append(parsed)
            if max_value is None or parsed > max_value:
                max_value = parsed
                max_row = row
        values.sort()
        out["fields"][field] = {
            "count": len(values),
            "p50": percentile(values, 50.0),
            "p95": percentile(values, 95.0),
            "p99": percentile(values, 99.0),
            "max": values[-1] if values else None,
            "max_frame_id": max_row.get("frame_id") if max_row else max_row,
            "max_recording_frame_id": max_row.get("recording_frame_id") if max_row else max_row,
        }
    return out

def load_csv(path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))

analytics_run_dirs = sorted(
    [path for path in analytics_root.glob("run_*") if path.is_dir()])
analytics_run_dir = analytics_run_dirs[0] if analytics_run_dirs else None

summary = {
    "schema_version": 1,
    "run_dir": str(run_dir),
    "analytics_root": str(analytics_root),
    "analytics_run_dir": str(analytics_run_dir) if analytics_run_dir else "",
    "steady_state_after_frame": steady_after,
    "cameras": {},
}

for serial in cameras:
    camera_out = {}
    yolo_rows = []
    if analytics_run_dir:
        yolo_rows = load_csv(analytics_run_dir / f"Cam{serial}_yolo_perf.csv")
    detach_rows = load_csv(run_dir / f"Cam{serial}_external_detach.csv")

    def frame_id(row):
        try:
            return int(row.get("frame_id") or row.get("recording_frame_id") or 0)
        except ValueError:
            return 0

    def recording_frame_id(row):
        try:
            return int(row.get("recording_frame_id") or 0)
        except ValueError:
            return 0

    steady_yolo = [row for row in yolo_rows if frame_id(row) > steady_after]
    steady_detach = [row for row in detach_rows if recording_frame_id(row) > steady_after]
    camera_out["yolo_all"] = summarize_rows(yolo_rows, yolo_fields)
    camera_out["yolo_steady_state"] = summarize_rows(steady_yolo, yolo_fields)
    camera_out["external_detach_all"] = summarize_rows(detach_rows, detach_fields)
    camera_out["external_detach_steady_state"] = summarize_rows(steady_detach, detach_fields)
    summary["cameras"][serial] = camera_out

summary_path = run_dir / "external_latency_summary.json"
summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(f"[external-recorder-ptp] latency_summary={summary_path}")
for serial, camera in summary["cameras"].items():
    yolo_all = camera["yolo_all"]["fields"].get("acquisition_to_detect_done_ms", {})
    yolo_steady = camera["yolo_steady_state"]["fields"].get("acquisition_to_detect_done_ms", {})
    yolo_total_steady = camera["yolo_steady_state"]["fields"].get("total_ms", {})
    detach_steady = camera["external_detach_steady_state"]["fields"].get("copy_ms", {})
    print(
        "[external-recorder-ptp] latency "
        f"camera={serial} "
        f"detect_all_p95={yolo_all.get('p95')} detect_all_max={yolo_all.get('max')} "
        f"detect_steady_p95={yolo_steady.get('p95')} detect_steady_max={yolo_steady.get('max')} "
        f"yolo_total_steady_p95={yolo_total_steady.get('p95')} "
        f"detach_copy_steady_p95={detach_steady.get('p95')} detach_copy_steady_max={detach_steady.get('max')}"
    )
PY

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
