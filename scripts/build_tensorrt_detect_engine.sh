#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  build_tensorrt_detect_engine.sh --onnx <path> --onnx-manifest <path> --run-id <id> --device <gpu-id> [options]

Builds an Orange TensorRT detect engine from a Palette ONNX export.

The build runs in a staging directory first. Only the final .engine and runtime
manifest are copied into the detect runtime directory.

Required:
  --onnx <path>                    Palette ONNX export.
  --onnx-manifest <path>           ONNX export manifest JSON.
  --run-id <id>                    Palette run_id.
  --device <gpu-id>                Explicit CUDA device for trtexec.

Options:
  --precision <fp16>               Build precision. Default fp16. INT8 is intentionally not implemented here yet.
  --target-hardware-class <name>   Hardware class label. Default A16.
  --trtexec <path>                 trtexec path.
                                   Default /usr/local/TensorRT-10.0.1.6/targets/x86_64-linux-gnu/bin/trtexec
  --trt-tag <tag>                  Naming tag for TensorRT version. Default trt100.
  --builder-optimization-level <n> Default 5.
  --avg-timing <n>                 Default 32.
  --profiling-verbosity <value>    Default detailed.
  --benchmark-duration <sec>       Default 10.
  --benchmark-warmup-ms <ms>       Default 1000.
  --output-engine-dir <path>       Default /home/jeremy/orange_data/detect.
  --staging-root <path>            Default /home/jeremy/orange_data/trt_builds/detect.
  --palette-registry <path>        Default /nvme1/palette_registry.sqlite.
  --build-id <id>                  Override build id used in filenames.
  --build-stamp <stamp>            Override staging timestamp. Default UTC YYYYmmdd_HHMMSS.
  --status <candidate|validated>   Manifest status. Default candidate.
  --create-handoff                 Write a Palette handoff tar.zst under --handoff-output-dir.
  --handoff-output-dir <path>      Default /tmp.
  --force                          Allow overwriting existing runtime/staging artifacts.
  --dry-run                        Print the resolved plan without running trtexec or writing files.
  --help

Example:
  scripts/stage_detect_onnx_export.sh \
    --onnx /path/to/model.onnx \
    --onnx-manifest /path/to/model.onnx.manifest.json

  scripts/build_tensorrt_detect_engine.sh \
    --onnx /home/jeremy/orange_data/model_sources/detect/<run_id>/source.onnx \
    --onnx-manifest /home/jeremy/orange_data/model_sources/detect/<run_id>/source.onnx.manifest.json \
    --run-id detect_all_available_detect_training_v004_yolo11n_trt_20260520 \
    --device 5 \
    --create-handoff
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MANIFEST_WRITER="$SCRIPT_DIR/write_tensorrt_engine_manifest.py"

ONNX=""
ONNX_MANIFEST=""
RUN_ID=""
DEVICE=""
PRECISION="fp16"
TARGET_HARDWARE_CLASS="A16"
TRTEXEC="/usr/local/TensorRT-10.0.1.6/targets/x86_64-linux-gnu/bin/trtexec"
TRT_TAG="trt100"
BUILDER_OPTIMIZATION_LEVEL=5
AVG_TIMING=32
PROFILING_VERBOSITY="detailed"
BENCHMARK_DURATION=10
BENCHMARK_WARMUP_MS=1000
OUTPUT_ENGINE_DIR="/home/jeremy/orange_data/detect"
STAGING_ROOT="/home/jeremy/orange_data/trt_builds/detect"
PALETTE_REGISTRY="/nvme1/palette_registry.sqlite"
BUILD_ID=""
BUILD_STAMP="$(date -u +%Y%m%d_%H%M%S)"
STATUS="candidate"
CREATE_HANDOFF=0
HANDOFF_OUTPUT_DIR="/tmp"
FORCE=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --onnx)
      shift
      [[ $# -gt 0 ]] || { echo "--onnx requires a value." >&2; exit 2; }
      ONNX="$1"
      shift
      ;;
    --onnx-manifest)
      shift
      [[ $# -gt 0 ]] || { echo "--onnx-manifest requires a value." >&2; exit 2; }
      ONNX_MANIFEST="$1"
      shift
      ;;
    --run-id)
      shift
      [[ $# -gt 0 ]] || { echo "--run-id requires a value." >&2; exit 2; }
      RUN_ID="$1"
      shift
      ;;
    --device)
      shift
      [[ $# -gt 0 ]] || { echo "--device requires a value." >&2; exit 2; }
      DEVICE="$1"
      shift
      ;;
    --precision)
      shift
      [[ $# -gt 0 ]] || { echo "--precision requires a value." >&2; exit 2; }
      PRECISION="$1"
      shift
      ;;
    --target-hardware-class)
      shift
      [[ $# -gt 0 ]] || { echo "--target-hardware-class requires a value." >&2; exit 2; }
      TARGET_HARDWARE_CLASS="$1"
      shift
      ;;
    --trtexec)
      shift
      [[ $# -gt 0 ]] || { echo "--trtexec requires a value." >&2; exit 2; }
      TRTEXEC="$1"
      shift
      ;;
    --trt-tag)
      shift
      [[ $# -gt 0 ]] || { echo "--trt-tag requires a value." >&2; exit 2; }
      TRT_TAG="$1"
      shift
      ;;
    --builder-optimization-level)
      shift
      [[ $# -gt 0 ]] || { echo "--builder-optimization-level requires a value." >&2; exit 2; }
      BUILDER_OPTIMIZATION_LEVEL="$1"
      shift
      ;;
    --avg-timing)
      shift
      [[ $# -gt 0 ]] || { echo "--avg-timing requires a value." >&2; exit 2; }
      AVG_TIMING="$1"
      shift
      ;;
    --profiling-verbosity)
      shift
      [[ $# -gt 0 ]] || { echo "--profiling-verbosity requires a value." >&2; exit 2; }
      PROFILING_VERBOSITY="$1"
      shift
      ;;
    --benchmark-duration)
      shift
      [[ $# -gt 0 ]] || { echo "--benchmark-duration requires a value." >&2; exit 2; }
      BENCHMARK_DURATION="$1"
      shift
      ;;
    --benchmark-warmup-ms)
      shift
      [[ $# -gt 0 ]] || { echo "--benchmark-warmup-ms requires a value." >&2; exit 2; }
      BENCHMARK_WARMUP_MS="$1"
      shift
      ;;
    --output-engine-dir)
      shift
      [[ $# -gt 0 ]] || { echo "--output-engine-dir requires a value." >&2; exit 2; }
      OUTPUT_ENGINE_DIR="$1"
      shift
      ;;
    --staging-root)
      shift
      [[ $# -gt 0 ]] || { echo "--staging-root requires a value." >&2; exit 2; }
      STAGING_ROOT="$1"
      shift
      ;;
    --palette-registry)
      shift
      [[ $# -gt 0 ]] || { echo "--palette-registry requires a value." >&2; exit 2; }
      PALETTE_REGISTRY="$1"
      shift
      ;;
    --build-id)
      shift
      [[ $# -gt 0 ]] || { echo "--build-id requires a value." >&2; exit 2; }
      BUILD_ID="$1"
      shift
      ;;
    --build-stamp)
      shift
      [[ $# -gt 0 ]] || { echo "--build-stamp requires a value." >&2; exit 2; }
      BUILD_STAMP="$1"
      shift
      ;;
    --status)
      shift
      [[ $# -gt 0 ]] || { echo "--status requires a value." >&2; exit 2; }
      STATUS="$1"
      shift
      ;;
    --create-handoff)
      CREATE_HANDOFF=1
      shift
      ;;
    --handoff-output-dir)
      shift
      [[ $# -gt 0 ]] || { echo "--handoff-output-dir requires a value." >&2; exit 2; }
      HANDOFF_OUTPUT_DIR="$1"
      shift
      ;;
    --force)
      FORCE=1
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

[[ -n "$ONNX" ]] || { echo "--onnx is required." >&2; exit 2; }
[[ -n "$ONNX_MANIFEST" ]] || { echo "--onnx-manifest is required." >&2; exit 2; }
[[ -n "$RUN_ID" ]] || { echo "--run-id is required." >&2; exit 2; }
[[ -n "$DEVICE" ]] || { echo "--device is required." >&2; exit 2; }

for value_name in DEVICE BUILDER_OPTIMIZATION_LEVEL AVG_TIMING BENCHMARK_DURATION BENCHMARK_WARMUP_MS; do
  value="${!value_name}"
  [[ "$value" =~ ^[0-9]+$ ]] || { echo "$value_name must be a non-negative integer." >&2; exit 2; }
done

PRECISION="$(printf '%s' "$PRECISION" | tr '[:upper:]' '[:lower:]')"
TARGET_HARDWARE_LOWER="$(printf '%s' "$TARGET_HARDWARE_CLASS" | tr '[:upper:]' '[:lower:]')"
if [[ "$PRECISION" != "fp16" ]]; then
  echo "Only --precision fp16 is implemented in this wrapper. INT8 needs a calibration cache/data contract first." >&2
  exit 2
fi
if [[ "$STATUS" != "candidate" && "$STATUS" != "validated" ]]; then
  echo "--status must be candidate or validated." >&2
  exit 2
fi

ONNX="$(realpath -e "$ONNX")"
ONNX_MANIFEST="$(realpath -e "$ONNX_MANIFEST")"
TRTEXEC="$(realpath -e "$TRTEXEC")"
OUTPUT_ENGINE_DIR="$(realpath -m "$OUTPUT_ENGINE_DIR")"
STAGING_ROOT="$(realpath -m "$STAGING_ROOT")"
HANDOFF_OUTPUT_DIR="$(realpath -m "$HANDOFF_OUTPUT_DIR")"

if [[ -z "$BUILD_ID" ]]; then
  BUILD_ID="${TARGET_HARDWARE_LOWER}_gpu${DEVICE}_${TRT_TAG}_${PRECISION}_bo${BUILDER_OPTIMIZATION_LEVEL}_avg${AVG_TIMING}"
fi

ENGINE_STEM="${RUN_ID}_${BUILD_ID}"
STAGING_DIR="$STAGING_ROOT/$RUN_ID/${BUILD_ID}_${BUILD_STAMP}"
STAGED_ONNX="$STAGING_DIR/source.onnx"
STAGED_ONNX_MANIFEST="$STAGING_DIR/source.onnx.manifest.json"
STAGED_ENGINE="$STAGING_DIR/${ENGINE_STEM}.engine"
BUILD_LOG="$STAGING_DIR/${ENGINE_STEM}_trtexec.log"
PROFILE_JSON="$STAGING_DIR/${ENGINE_STEM}_trtexec_profile.json"
LAYER_INFO_JSON="$STAGING_DIR/${ENGINE_STEM}_trtexec_layer_info.json"
BUILD_TIMES_JSON="$STAGING_DIR/${ENGINE_STEM}_trtexec_times.json"
BENCHMARK_LOG="$STAGING_DIR/${ENGINE_STEM}_benchmark.log"
BENCHMARK_TIMES_JSON="$STAGING_DIR/${ENGINE_STEM}_benchmark_times.json"
BUILD_MANIFEST="$STAGING_DIR/build_manifest.json"
RUNTIME_ENGINE="$OUTPUT_ENGINE_DIR/${ENGINE_STEM}.engine"
RUNTIME_MANIFEST="$OUTPUT_ENGINE_DIR/${ENGINE_STEM}.manifest.json"

BUILD_CMD=(
  "$TRTEXEC"
  "--device=$DEVICE"
  "--onnx=$STAGED_ONNX"
  "--saveEngine=$STAGED_ENGINE"
  "--fp16"
  "--builderOptimizationLevel=$BUILDER_OPTIMIZATION_LEVEL"
  "--avgTiming=$AVG_TIMING"
  "--profilingVerbosity=$PROFILING_VERBOSITY"
  "--exportTimes=$BUILD_TIMES_JSON"
  "--exportProfile=$PROFILE_JSON"
  "--exportLayerInfo=$LAYER_INFO_JSON"
)
BENCHMARK_CMD=(
  "$TRTEXEC"
  "--device=$DEVICE"
  "--loadEngine=$STAGED_ENGINE"
  "--duration=$BENCHMARK_DURATION"
  "--warmUp=$BENCHMARK_WARMUP_MS"
  "--exportTimes=$BENCHMARK_TIMES_JSON"
)

echo "[trt-build] run_id=$RUN_ID"
echo "[trt-build] build_id=$BUILD_ID"
echo "[trt-build] staging_dir=$STAGING_DIR"
echo "[trt-build] runtime_engine=$RUNTIME_ENGINE"
echo "[trt-build] runtime_manifest=$RUNTIME_MANIFEST"

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf '[trt-build] build command:'
  printf ' %q' "${BUILD_CMD[@]}"
  printf '\n'
  printf '[trt-build] benchmark command:'
  printf ' %q' "${BENCHMARK_CMD[@]}"
  printf '\n'
  exit 0
fi

if [[ -e "$RUNTIME_ENGINE" && "$FORCE" -ne 1 ]]; then
  echo "runtime engine already exists: $RUNTIME_ENGINE (use --force to overwrite)" >&2
  exit 1
fi
if [[ -e "$RUNTIME_MANIFEST" && "$FORCE" -ne 1 ]]; then
  echo "runtime manifest already exists: $RUNTIME_MANIFEST (use --force to overwrite)" >&2
  exit 1
fi
if [[ -d "$STAGING_DIR" && "$FORCE" -ne 1 ]]; then
  echo "staging directory already exists: $STAGING_DIR (use --force to overwrite)" >&2
  exit 1
fi

mkdir -p "$STAGING_DIR" "$OUTPUT_ENGINE_DIR"
cp "$ONNX" "$STAGED_ONNX"
cp "$ONNX_MANIFEST" "$STAGED_ONNX_MANIFEST"

echo "[trt-build] building TensorRT engine"
"${BUILD_CMD[@]}" 2>&1 | tee "$BUILD_LOG"
[[ -s "$STAGED_ENGINE" ]] || { echo "trtexec did not write a non-empty engine: $STAGED_ENGINE" >&2; exit 1; }

echo "[trt-build] running standalone benchmark"
"${BENCHMARK_CMD[@]}" 2>&1 | tee "$BENCHMARK_LOG"

cp "$STAGED_ENGINE" "$RUNTIME_ENGINE"

python3 "$MANIFEST_WRITER" \
  --run-id "$RUN_ID" \
  --build-id "$BUILD_ID" \
  --build-stamp "$BUILD_STAMP" \
  --source-onnx "$STAGED_ONNX" \
  --source-onnx-manifest "$STAGED_ONNX_MANIFEST" \
  --source-input-onnx "$ONNX" \
  --source-input-onnx-manifest "$ONNX_MANIFEST" \
  --staged-engine "$STAGED_ENGINE" \
  --runtime-engine "$RUNTIME_ENGINE" \
  --runtime-manifest "$RUNTIME_MANIFEST" \
  --build-manifest "$BUILD_MANIFEST" \
  --staging-dir "$STAGING_DIR" \
  --output-engine-dir "$OUTPUT_ENGINE_DIR" \
  --trtexec "$TRTEXEC" \
  --device "$DEVICE" \
  --target-hardware-class "$TARGET_HARDWARE_CLASS" \
  --deployment-runtime orange \
  --precision "$PRECISION" \
  --trt-tag "$TRT_TAG" \
  --builder-optimization-level "$BUILDER_OPTIMIZATION_LEVEL" \
  --avg-timing "$AVG_TIMING" \
  --profiling-verbosity "$PROFILING_VERBOSITY" \
  --build-log "$BUILD_LOG" \
  --profile-json "$PROFILE_JSON" \
  --layer-info-json "$LAYER_INFO_JSON" \
  --build-times-json "$BUILD_TIMES_JSON" \
  --benchmark-log "$BENCHMARK_LOG" \
  --benchmark-times-json "$BENCHMARK_TIMES_JSON" \
  --benchmark-duration "$BENCHMARK_DURATION" \
  --benchmark-warmup-ms "$BENCHMARK_WARMUP_MS" \
  --palette-registry "$PALETTE_REGISTRY" \
  --status "$STATUS"

if [[ "$CREATE_HANDOFF" -eq 1 ]]; then
  mkdir -p "$HANDOFF_OUTPUT_DIR"
  HANDOFF_DIR="$HANDOFF_OUTPUT_DIR/orange_${TARGET_HARDWARE_LOWER}_detect_engine_${RUN_ID}_${BUILD_ID}_${BUILD_STAMP}"
  HANDOFF_TARBALL="${HANDOFF_DIR}.tar.zst"
  if [[ -e "$HANDOFF_DIR" && "$FORCE" -ne 1 ]]; then
    echo "handoff directory already exists: $HANDOFF_DIR (use --force or a new --build-stamp)" >&2
    exit 1
  fi
  mkdir -p "$HANDOFF_DIR"
  cp "$STAGING_DIR/README.md" "$HANDOFF_DIR/README.md"
  cp "$RUNTIME_ENGINE" "$RUNTIME_MANIFEST" "$HANDOFF_DIR/"
  for path in "$BUILD_LOG" "$BENCHMARK_LOG" "$PROFILE_JSON" "$LAYER_INFO_JSON" "$BENCHMARK_TIMES_JSON"; do
    [[ -e "$path" ]] && cp "$path" "$HANDOFF_DIR/"
  done
  (
    cd "$HANDOFF_DIR"
    sha256sum ./* > SHA256SUMS
  )
  tar --zstd -cf "$HANDOFF_TARBALL" -C "$HANDOFF_OUTPUT_DIR" "$(basename "$HANDOFF_DIR")"
  echo "[trt-build] handoff_tarball=$HANDOFF_TARBALL"
  sha256sum "$HANDOFF_TARBALL"
fi

echo "[trt-build] complete"
echo "[trt-build] engine=$RUNTIME_ENGINE"
echo "[trt-build] manifest=$RUNTIME_MANIFEST"
echo "[trt-build] staging=$STAGING_DIR"
