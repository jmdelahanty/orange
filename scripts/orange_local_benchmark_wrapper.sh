#!/usr/bin/env bash
set -euo pipefail

ORANGE_ROOT="/home/jeremy/orange-jeremy"
DEFAULT_ORANGE_CLIENT="$ORANGE_ROOT/build/orange_client"
EXPERIMENT_ORANGE_CLIENT="/home/jeremy/orange-gop-split-a16/targets/release/orange_client"
ORANGE_CLIENT="$DEFAULT_ORANGE_CLIENT"
ACQUIRE_WORK_ENTRIES_MAX=""
ENCODER_ENTRY_POOL_SIZE=""
ALLOWED_SPEC_DIR_1="$ORANGE_ROOT/experiment_specs"
ALLOWED_SPEC_DIR_2="/tmp"
EVT_PROFILE="/etc/profile.d/evt.sh"

usage() {
  cat <<'EOF'
Usage:
  orange_local_benchmark_wrapper.sh [--orange-client <path>] <experiment-spec.json>
  orange_local_benchmark_wrapper.sh [--orange-client <path>] [--acquire-work-entries-max <n>] <experiment-spec.json>
  orange_local_benchmark_wrapper.sh [--orange-client <path>] [--acquire-work-entries-max <n>] [--encoder-entry-pool-size <n>] <experiment-spec.json>
  orange_local_benchmark_wrapper.sh [--orange-client <path>] [--acquire-work-entries-max <n>] [--encoder-entry-pool-size <n>] --stream-only --config-folder <path> --camera <serial|all> [options]

Behavior:
  - Runs orange_client in local experiment mode as root.
  - Only accepts orange_client binaries at:
      /home/jeremy/orange-jeremy/build/orange_client
      /home/jeremy/orange-gop-split-a16/targets/release/orange_client
  - Optionally exports ORANGE_ACQUIRE_WORK_ENTRIES_MAX for the launched process.
  - Optionally exports ORANGE_ENCODER_ENTRY_POOL_SIZE for the launched process.
  - Only accepts spec files under:
      /home/jeremy/orange-jeremy/experiment_specs
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
    *)
      break
      ;;
  esac
done

if [[ ! -x "$ORANGE_CLIENT" ]]; then
  echo "orange_client not found or not executable at $ORANGE_CLIENT" >&2
  exit 1
fi

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
  if [[ -n "$ACQUIRE_WORK_ENTRIES_MAX" ]]; then
    echo "[sudo-wrapper] acquire_work_entries_max=$ACQUIRE_WORK_ENTRIES_MAX"
    export ORANGE_ACQUIRE_WORK_ENTRIES_MAX="$ACQUIRE_WORK_ENTRIES_MAX"
  fi
  if [[ -n "$ENCODER_ENTRY_POOL_SIZE" ]]; then
    echo "[sudo-wrapper] encoder_entry_pool_size=$ENCODER_ENTRY_POOL_SIZE"
    export ORANGE_ENCODER_ENTRY_POOL_SIZE="$ENCODER_ENTRY_POOL_SIZE"
  fi

  exec "${CMD[@]}"
fi

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 2
fi

SPEC_PATH="$(realpath -e "$1")"
case "$SPEC_PATH" in
  "$ALLOWED_SPEC_DIR_1"/*|"$ALLOWED_SPEC_DIR_2"/*)
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
if [[ -n "$ACQUIRE_WORK_ENTRIES_MAX" ]]; then
  echo "[sudo-wrapper] acquire_work_entries_max=$ACQUIRE_WORK_ENTRIES_MAX"
  export ORANGE_ACQUIRE_WORK_ENTRIES_MAX="$ACQUIRE_WORK_ENTRIES_MAX"
fi
if [[ -n "$ENCODER_ENTRY_POOL_SIZE" ]]; then
  echo "[sudo-wrapper] encoder_entry_pool_size=$ENCODER_ENTRY_POOL_SIZE"
  export ORANGE_ENCODER_ENTRY_POOL_SIZE="$ENCODER_ENTRY_POOL_SIZE"
fi

set +e
"$ORANGE_CLIENT" --mode local --experiment-spec "$SPEC_PATH"
STATUS=$?
set -e

if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" && -d "$EXPERIMENT_ROOT" ]]; then
  chown -R "${SUDO_UID}:${SUDO_GID}" "$EXPERIMENT_ROOT"
fi

exit "$STATUS"
