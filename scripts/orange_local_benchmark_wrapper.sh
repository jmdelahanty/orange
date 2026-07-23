#!/usr/bin/env bash
set -euo pipefail

ORANGE_ROOT="/home/jeremy/orange-jeremy"
DEFAULT_ORANGE_CLIENT="$ORANGE_ROOT/build/orange_client"
EXPERIMENT_ORANGE_ROOT="/home/jeremy/orange-gop-split-a16"
EXPERIMENT_ORANGE_CLIENT="$EXPERIMENT_ORANGE_ROOT/targets/release/orange_client"
ORANGE_CLIENT="$DEFAULT_ORANGE_CLIENT"
ACQUIRE_WORK_ENTRIES_MAX=""
ENCODER_ENTRY_POOL_SIZE=""
YOLO_PERF_LOG=""
YOLO_PERF_SAMPLE=""
ANALYTICS_EARLY_OWNED_FRAME=""
YOLO_READY_EVENT_FASTPATH=""
YOLO_DETACH_INPUT=""
PREPROCESS_DEFER_SOURCE_RELEASE=""
YOLO_SPATIAL_MASK_MODE=""
YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX=""
YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS=""
CITRUS_RECORDING_CANVAS_CONFIG_PATH=""
ALLOWED_SPEC_DIR_1="$ORANGE_ROOT/experiment_specs"
ALLOWED_SPEC_DIR_2="/tmp"
ALLOWED_SPEC_DIR_3="$EXPERIMENT_ORANGE_ROOT/experiment_specs"
ALLOWED_CITRUS_RIG_DIR="/home/jeremy/citrus/targets/rigs"
ALLOWED_CITRUS_TEST_DIR="/tmp"
EVT_PROFILE="/etc/profile.d/evt.sh"

usage() {
  cat <<'EOF'
Usage:
  orange_local_benchmark_wrapper.sh [--orange-client <path>] [options] <experiment-spec.json>
  orange_local_benchmark_wrapper.sh [--orange-client <path>] [options] --stream-only --config-folder <path> --camera <serial|all> [stream-options]

Options:
  --orange-client <path>             Use an allowed orange_client binary.
  --acquire-work-entries-max <n>     Export ORANGE_ACQUIRE_WORK_ENTRIES_MAX.
  --encoder-entry-pool-size <n>      Export ORANGE_ENCODER_ENTRY_POOL_SIZE.
  --yolo-perf-log                    Export ORANGE_YOLO_PERF_LOG=1.
  --no-yolo-perf-log                 Export ORANGE_YOLO_PERF_LOG=0.
  --yolo-perf-sample <n>             Export ORANGE_YOLO_PERF_SAMPLE=<n>; also enables YOLO perf logging unless explicitly disabled.
  --analytics-early-owned-frame <0|1>
                                      Export ORANGE_ANALYTICS_EARLY_OWNED_FRAME.
  --yolo-ready-event-fastpath <0|1>  Export ORANGE_YOLO_READY_EVENT_FASTPATH.
  --yolo-detach-input <0|1>          Export ORANGE_YOLO_DETACH_INPUT.
  --preprocess-defer-source-release <0|1>
                                      Export ORANGE_PREPROCESS_DEFER_SOURCE_RELEASE.
  --yolo-spatial-mask-mode <mode>    Export ORANGE_YOLO_SPATIAL_MASK_MODE. Modes:
                                      off, audit, gate_only, gate_and_input_mask.
  --yolo-spatial-mask-input-context-outset-px <px>
                                      Export ORANGE_YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX.
  --yolo-spatial-mask-apply-timeout-ms <ms>
                                      Export ORANGE_YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS (50-10000).
  --citrus-recording-canvas-config-path <path>
                                      Select an existing Citrus canvas JSON under
                                      /home/jeremy/citrus/targets/rigs or /tmp.

Behavior:
  - Runs orange_client in local experiment mode as root.
  - Only accepts orange_client binaries at:
      /home/jeremy/orange-jeremy/build/orange_client
      /home/jeremy/orange-gop-split-a16/targets/release/orange_client
  - Only accepts spec files under:
      /home/jeremy/orange-jeremy/experiment_specs
      /home/jeremy/orange-gop-split-a16/experiment_specs
      /tmp
  - Stream-only mode only accepts config folders under:
      /home/jeremy/orange_data/config/local
      /tmp
  - After the run, chowns the experiment output folder back to the invoking user
    when launched via sudo.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "This wrapper must be run as root (typically via sudo)." >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required to parse the experiment spec." >&2
  exit 1
fi

# Restore the EVT SDK environment that the installer normally provides in
# interactive shells. sudo env_reset commonly strips these variables.
if [[ -f "$EVT_PROFILE" ]]; then
  # shellcheck disable=SC1090
  source "$EVT_PROFILE"
fi
export EMERGENT_DIR="${EMERGENT_DIR:-/opt/EVT}"
export RIVERMAX_LOG_LEVEL="${RIVERMAX_LOG_LEVEL:-6}"
export VMA_TRACELEVEL="${VMA_TRACELEVEL:-0}"

if [[ $# -eq 0 ]]; then
  usage >&2
  exit 2
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --orange-client)
      shift
      [[ $# -gt 0 ]] || { echo "--orange-client requires a value." >&2; exit 2; }
      ORANGE_CLIENT="$(realpath -e "$1")"
      case "$ORANGE_CLIENT" in
        "$DEFAULT_ORANGE_CLIENT"|"$EXPERIMENT_ORANGE_CLIENT")
          ;;
        *)
          echo "Refusing to use orange_client outside allowed binaries: $ORANGE_CLIENT" >&2
          exit 2
          ;;
      esac
      shift
      ;;
    --acquire-work-entries-max)
      shift
      [[ $# -gt 0 ]] || { echo "--acquire-work-entries-max requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[0-9]+$ ]] || { echo "--acquire-work-entries-max must be a non-negative integer." >&2; exit 2; }
      ACQUIRE_WORK_ENTRIES_MAX="$1"
      shift
      ;;
    --encoder-entry-pool-size)
      shift
      [[ $# -gt 0 ]] || { echo "--encoder-entry-pool-size requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[0-9]+$ ]] || { echo "--encoder-entry-pool-size must be a non-negative integer." >&2; exit 2; }
      ENCODER_ENTRY_POOL_SIZE="$1"
      shift
      ;;
    --yolo-perf-log)
      YOLO_PERF_LOG="1"
      shift
      ;;
    --no-yolo-perf-log)
      YOLO_PERF_LOG="0"
      shift
      ;;
    --yolo-perf-sample)
      shift
      [[ $# -gt 0 ]] || { echo "--yolo-perf-sample requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[1-9][0-9]*$ ]] || { echo "--yolo-perf-sample must be a positive integer." >&2; exit 2; }
      YOLO_PERF_SAMPLE="$1"
      if [[ -z "$YOLO_PERF_LOG" ]]; then
        YOLO_PERF_LOG="1"
      fi
      shift
      ;;
    --analytics-early-owned-frame)
      shift
      [[ $# -gt 0 ]] || { echo "--analytics-early-owned-frame requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[01]$ ]] || { echo "--analytics-early-owned-frame must be 0 or 1." >&2; exit 2; }
      ANALYTICS_EARLY_OWNED_FRAME="$1"
      shift
      ;;
    --yolo-ready-event-fastpath)
      shift
      [[ $# -gt 0 ]] || { echo "--yolo-ready-event-fastpath requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[01]$ ]] || { echo "--yolo-ready-event-fastpath must be 0 or 1." >&2; exit 2; }
      YOLO_READY_EVENT_FASTPATH="$1"
      shift
      ;;
    --yolo-detach-input)
      shift
      [[ $# -gt 0 ]] || { echo "--yolo-detach-input requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[01]$ ]] || { echo "--yolo-detach-input must be 0 or 1." >&2; exit 2; }
      YOLO_DETACH_INPUT="$1"
      shift
      ;;
    --preprocess-defer-source-release)
      shift
      [[ $# -gt 0 ]] || { echo "--preprocess-defer-source-release requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[01]$ ]] || { echo "--preprocess-defer-source-release must be 0 or 1." >&2; exit 2; }
      PREPROCESS_DEFER_SOURCE_RELEASE="$1"
      shift
      ;;
    --yolo-spatial-mask-mode)
      shift
      [[ $# -gt 0 ]] || { echo "--yolo-spatial-mask-mode requires a value." >&2; exit 2; }
      case "$1" in
        off|audit|gate_only|gate_and_input_mask)
          YOLO_SPATIAL_MASK_MODE="$1"
          ;;
        *)
          echo "--yolo-spatial-mask-mode must be off, audit, gate_only, or gate_and_input_mask." >&2
          exit 2
          ;;
      esac
      shift
      ;;
    --yolo-spatial-mask-input-context-outset-px)
      shift
      [[ $# -gt 0 ]] || { echo "--yolo-spatial-mask-input-context-outset-px requires a value." >&2; exit 2; }
      [[ "$1" =~ ^([0-9]+([.][0-9]*)?|[.][0-9]+)$ ]] || {
        echo "--yolo-spatial-mask-input-context-outset-px must be a non-negative finite decimal." >&2
        exit 2
      }
      YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX="$1"
      shift
      ;;
    --yolo-spatial-mask-apply-timeout-ms)
      shift
      [[ $# -gt 0 ]] || { echo "--yolo-spatial-mask-apply-timeout-ms requires a value." >&2; exit 2; }
      [[ "$1" =~ ^[0-9]+$ ]] || { echo "--yolo-spatial-mask-apply-timeout-ms must be an integer." >&2; exit 2; }
      if (( 10#$1 < 50 || 10#$1 > 10000 )); then
        echo "--yolo-spatial-mask-apply-timeout-ms must be within [50,10000]." >&2
        exit 2
      fi
      YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS="$1"
      shift
      ;;
    --citrus-recording-canvas-config-path)
      shift
      [[ $# -gt 0 ]] || { echo "--citrus-recording-canvas-config-path requires a value." >&2; exit 2; }
      CITRUS_RECORDING_CANVAS_CONFIG_PATH="$(realpath -e "$1")"
      if [[ ! -f "$CITRUS_RECORDING_CANVAS_CONFIG_PATH" ||
            "$CITRUS_RECORDING_CANVAS_CONFIG_PATH" != *.json ]]; then
        echo "Citrus recording canvas config must be an existing JSON file." >&2
        exit 2
      fi
      case "$CITRUS_RECORDING_CANVAS_CONFIG_PATH" in
        "$ALLOWED_CITRUS_RIG_DIR"/*|"$ALLOWED_CITRUS_TEST_DIR"/*)
          ;;
        *)
          echo "Refusing Citrus canvas config outside allowed roots: $CITRUS_RECORDING_CANVAS_CONFIG_PATH" >&2
          exit 2
          ;;
      esac
      shift
      ;;
    *)
      break
      ;;
  esac
done

if [[ $# -eq 0 ]]; then
  usage >&2
  exit 2
fi

if [[ ! -x "$ORANGE_CLIENT" ]]; then
  echo "orange_client not found or not executable at $ORANGE_CLIENT" >&2
  exit 1
fi

export_optional_runtime_env() {
  if [[ -n "$ACQUIRE_WORK_ENTRIES_MAX" ]]; then
    echo "[sudo-wrapper] acquire_work_entries_max=$ACQUIRE_WORK_ENTRIES_MAX"
    export ORANGE_ACQUIRE_WORK_ENTRIES_MAX="$ACQUIRE_WORK_ENTRIES_MAX"
  fi
  if [[ -n "$ENCODER_ENTRY_POOL_SIZE" ]]; then
    echo "[sudo-wrapper] encoder_entry_pool_size=$ENCODER_ENTRY_POOL_SIZE"
    export ORANGE_ENCODER_ENTRY_POOL_SIZE="$ENCODER_ENTRY_POOL_SIZE"
  fi
  if [[ -n "$YOLO_PERF_LOG" ]]; then
    echo "[sudo-wrapper] yolo_perf_log=$YOLO_PERF_LOG"
    export ORANGE_YOLO_PERF_LOG="$YOLO_PERF_LOG"
  fi
  if [[ -n "$YOLO_PERF_SAMPLE" ]]; then
    echo "[sudo-wrapper] yolo_perf_sample=$YOLO_PERF_SAMPLE"
    export ORANGE_YOLO_PERF_SAMPLE="$YOLO_PERF_SAMPLE"
  fi
  if [[ -n "$ANALYTICS_EARLY_OWNED_FRAME" ]]; then
    echo "[sudo-wrapper] analytics_early_owned_frame=$ANALYTICS_EARLY_OWNED_FRAME"
    export ORANGE_ANALYTICS_EARLY_OWNED_FRAME="$ANALYTICS_EARLY_OWNED_FRAME"
  fi
  if [[ -n "$YOLO_READY_EVENT_FASTPATH" ]]; then
    echo "[sudo-wrapper] yolo_ready_event_fastpath=$YOLO_READY_EVENT_FASTPATH"
    export ORANGE_YOLO_READY_EVENT_FASTPATH="$YOLO_READY_EVENT_FASTPATH"
  fi
  if [[ -n "$YOLO_DETACH_INPUT" ]]; then
    echo "[sudo-wrapper] yolo_detach_input=$YOLO_DETACH_INPUT"
    export ORANGE_YOLO_DETACH_INPUT="$YOLO_DETACH_INPUT"
  fi
  if [[ -n "$PREPROCESS_DEFER_SOURCE_RELEASE" ]]; then
    echo "[sudo-wrapper] preprocess_defer_source_release=$PREPROCESS_DEFER_SOURCE_RELEASE"
    export ORANGE_PREPROCESS_DEFER_SOURCE_RELEASE="$PREPROCESS_DEFER_SOURCE_RELEASE"
  fi
  if [[ -n "$YOLO_SPATIAL_MASK_MODE" ]]; then
    echo "[sudo-wrapper] yolo_spatial_mask_mode=$YOLO_SPATIAL_MASK_MODE"
    export ORANGE_YOLO_SPATIAL_MASK_MODE="$YOLO_SPATIAL_MASK_MODE"
  fi
  if [[ -n "$YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX" ]]; then
    echo "[sudo-wrapper] yolo_spatial_mask_input_context_outset_px=$YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX"
    export ORANGE_YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX="$YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX"
  fi
  if [[ -n "$YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS" ]]; then
    echo "[sudo-wrapper] yolo_spatial_mask_apply_timeout_ms=$YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS"
    export ORANGE_YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS="$YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS"
  fi
  if [[ -n "$CITRUS_RECORDING_CANVAS_CONFIG_PATH" ]]; then
    echo "[sudo-wrapper] citrus_recording_canvas_config_path=$CITRUS_RECORDING_CANVAS_CONFIG_PATH"
    export ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH="$CITRUS_RECORDING_CANVAS_CONFIG_PATH"
  fi
}

if [[ "$1" == "--stream-only" ]]; then
  shift
  CONFIG_FOLDER=""
  CAMERAS=()
  GPU_IDS=()
  DURATION=""
  STREAM_START_DELAY=""
  ALLOWED_CONFIG_DIR_1="/home/jeremy/orange_data/config/local"
  ALLOWED_CONFIG_DIR_2="/tmp"
  CMD=("$ORANGE_CLIENT" "--mode" "local" "--stream-only")

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --config-folder)
        shift
        [[ $# -gt 0 ]] || { echo "--config-folder requires a value." >&2; exit 2; }
        CONFIG_FOLDER="$(realpath -e "$1")"
        case "$CONFIG_FOLDER" in
          "$ALLOWED_CONFIG_DIR_1"/*|"$ALLOWED_CONFIG_DIR_1"|"$ALLOWED_CONFIG_DIR_2"/*|"$ALLOWED_CONFIG_DIR_2")
            ;;
          *)
            echo "Refusing to use config folder outside allowed roots: $CONFIG_FOLDER" >&2
            exit 2
            ;;
        esac
        CMD+=("--config-folder" "$CONFIG_FOLDER")
        ;;
      --camera)
        shift
        [[ $# -gt 0 ]] || { echo "--camera requires a value." >&2; exit 2; }
        CAMERAS+=("$1")
        CMD+=("--camera" "$1")
        ;;
      --gpu-id)
        shift
        [[ $# -gt 0 ]] || { echo "--gpu-id requires a value." >&2; exit 2; }
        [[ "$1" =~ ^[0-9]+$ ]] || { echo "--gpu-id must be a non-negative integer." >&2; exit 2; }
        GPU_IDS+=("$1")
        CMD+=("--gpu-id" "$1")
        ;;
      --duration)
        shift
        [[ $# -gt 0 ]] || { echo "--duration requires a value." >&2; exit 2; }
        [[ "$1" =~ ^[0-9]+$ ]] || { echo "--duration must be a non-negative integer." >&2; exit 2; }
        DURATION="$1"
        CMD+=("--duration" "$1")
        ;;
      --stream-start-delay)
        shift
        [[ $# -gt 0 ]] || { echo "--stream-start-delay requires a value." >&2; exit 2; }
        [[ "$1" =~ ^[0-9]+$ ]] || { echo "--stream-start-delay must be a non-negative integer." >&2; exit 2; }
        STREAM_START_DELAY="$1"
        CMD+=("--stream-start-delay" "$1")
        ;;
      *)
        echo "Unsupported stream-only argument: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
    shift
  done

  if [[ -z "$CONFIG_FOLDER" ]]; then
    echo "--stream-only requires --config-folder." >&2
    exit 2
  fi
  if [[ "${#CAMERAS[@]}" -eq 0 ]]; then
    echo "--stream-only requires at least one --camera." >&2
    exit 2
  fi

  echo "[sudo-wrapper] running stream-only local check"
  echo "[sudo-wrapper] orange_client=$ORANGE_CLIENT"
  echo "[sudo-wrapper] config_folder=$CONFIG_FOLDER"
  echo "[sudo-wrapper] cameras=${CAMERAS[*]}"
  if [[ "${#GPU_IDS[@]}" -gt 0 ]]; then
    echo "[sudo-wrapper] gpu_ids=${GPU_IDS[*]}"
  fi
  if [[ -n "$DURATION" ]]; then
    echo "[sudo-wrapper] duration_s=$DURATION"
  fi
  if [[ -n "$STREAM_START_DELAY" ]]; then
    echo "[sudo-wrapper] stream_start_delay_s=$STREAM_START_DELAY"
  fi
  export_optional_runtime_env

  exec "${CMD[@]}"
fi

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 2
fi

SPEC_PATH="$(realpath -e "$1")"
case "$SPEC_PATH" in
  "$ALLOWED_SPEC_DIR_1"/*|"$ALLOWED_SPEC_DIR_2"/*|"$ALLOWED_SPEC_DIR_3"/*)
    ;;
  *)
    echo "Refusing to use experiment spec outside allowed roots: $SPEC_PATH" >&2
    exit 2
    ;;
esac

mapfile -t SPEC_FIELDS < <(python3 - "$SPEC_PATH" <<'PY'
import json
import sys
from pathlib import Path

spec_path = Path(sys.argv[1])
with spec_path.open("r", encoding="utf-8") as f:
    spec = json.load(f)

experiment_id = spec.get("experiment_id", "")
fixed = spec.get("fixed", {})
output_root = fixed.get("output_root", "")
if not experiment_id or not output_root:
    raise SystemExit("experiment spec must define experiment_id and fixed.output_root")

print(experiment_id)
print(output_root)
PY
)

if [[ "${#SPEC_FIELDS[@]}" -ne 2 ]]; then
  echo "Failed to resolve experiment_id/output_root from $SPEC_PATH" >&2
  exit 2
fi

EXPERIMENT_ID="${SPEC_FIELDS[0]}"
OUTPUT_ROOT="${SPEC_FIELDS[1]}"
EXPERIMENT_ROOT="$(realpath -m "$OUTPUT_ROOT/$EXPERIMENT_ID")"

echo "[sudo-wrapper] running experiment_id=$EXPERIMENT_ID"
echo "[sudo-wrapper] orange_client=$ORANGE_CLIENT"
echo "[sudo-wrapper] spec=$SPEC_PATH"
echo "[sudo-wrapper] output_root=$EXPERIMENT_ROOT"
export_optional_runtime_env

set +e
"$ORANGE_CLIENT" --mode local --experiment-spec "$SPEC_PATH"
STATUS=$?
set -e

if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" && -d "$EXPERIMENT_ROOT" ]]; then
  chown -R "${SUDO_UID}:${SUDO_GID}" "$EXPERIMENT_ROOT"
fi

exit "$STATUS"
