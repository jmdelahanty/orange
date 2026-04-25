#!/usr/bin/env bash
set -euo pipefail

repo_dir="/home/jeremy/orange-gop-split-a16"
cd "$repo_dir"

nsys_bin="${ORANGE_NSYS_BIN:-/usr/local/cuda/bin/nsys}"
app_bin="${ORANGE_NSYS_APP:-./targets/release_nvtx/orange}"
output_base="${1:-${ORANGE_NSYS_OUTPUT:-/tmp/orange_yolo_detach_nsys_$(date +%Y%m%d_%H%M%S)}}"

output_base="${output_base%.sqlite}"
output_base="${output_base%.nsys-rep}"

if [[ -z "${XAUTHORITY:-}" ]]; then
  export XAUTHORITY="$HOME/.Xauthority"
fi

if [[ ! -x "$nsys_bin" ]]; then
  echo "Nsight Systems CLI not found or not executable: $nsys_bin" >&2
  exit 1
fi

if [[ ! -x "$app_bin" ]]; then
  echo "NVTX orange binary not found or not executable: $app_bin" >&2
  echo "Build it with: cmake --preset release_nvtx && cmake --build targets/release_nvtx -j 8" >&2
  exit 1
fi

mkdir -p "$(dirname "$output_base")"

nsys_profile_args=(
  --trace=cuda,nvtx,osrt,nvvideo
  --cpuctxsw=process-tree
  --export=sqlite
  --stats=false
  --force-overwrite=true
)

if [[ "${ORANGE_NSYS_HEAVY:-0}" == "1" ]]; then
  nsys_profile_args+=(
    --sample="${ORANGE_NSYS_SAMPLE:-process-tree}"
    --backtrace="${ORANGE_NSYS_BACKTRACE:-lbr}"
    --cudabacktrace="${ORANGE_NSYS_CUDA_BACKTRACE:-kernel:1000000,memory:1000000,sync:1000000,other:1000000}"
    --osrt-threshold="${ORANGE_NSYS_OSRT_THRESHOLD_NS:-1000}"
    --osrt-backtrace-threshold="${ORANGE_NSYS_OSRT_BACKTRACE_THRESHOLD_NS:-100000}"
  )
  echo "Heavy Nsight mode enabled: CPU samples plus CUDA/OSRT backtraces."
else
  nsys_profile_args+=(--sample=none)
fi

if [[ -n "${ORANGE_NSYS_DURATION:-}" ]]; then
  nsys_profile_args+=(--duration="${ORANGE_NSYS_DURATION}")
fi

echo "Writing Nsight report base: $output_base"
echo "Expected SQLite after exit: ${output_base}.sqlite"

sudo env DISPLAY="${DISPLAY:-}" XAUTHORITY="${XAUTHORITY:-}" \
  ORANGE_GUI_RECORDING_SINK_MODE="${ORANGE_GUI_RECORDING_SINK_MODE:-}" \
  ORANGE_RECORDING_DETECT_PRIORITY=1 \
  ORANGE_YOLO_DETACH_INPUT=1 \
  ORANGE_YOLO_READY_EVENT_FASTPATH=1 \
  ORANGE_YOLO_PERF_LOG=1 \
  ORANGE_YOLO_PERF_SAMPLE=1 \
  ORANGE_CROP_COPY_TIMING=0 \
  ORANGE_CROP_STAGE_SOURCE=1 \
  ORANGE_CROP_PREVIEW_DISABLE="${ORANGE_CROP_PREVIEW_DISABLE:-0}" \
  ORANGE_DISPLAY_PREVIEW_MAX_FPS="${ORANGE_DISPLAY_PREVIEW_MAX_FPS:-}" \
  ORANGE_ANALYTICS_EARLY_OWNED_FRAME=1 \
  ORANGE_NVENC_EXTRA_OUTPUT_DELAY="${ORANGE_NVENC_EXTRA_OUTPUT_DELAY:-}" \
  ORANGE_NVENC_EXTRA_OUTPUT_DELAY_CAM_2010095="${ORANGE_NVENC_EXTRA_OUTPUT_DELAY_CAM_2010095:-}" \
  ORANGE_NVENC_EXTRA_OUTPUT_DELAY_CAM_2010096="${ORANGE_NVENC_EXTRA_OUTPUT_DELAY_CAM_2010096:-}" \
  ORANGE_NVENC_SPLIT_HARVEST="${ORANGE_NVENC_SPLIT_HARVEST:-0}" \
  ORANGE_YOLO_AFFINITY_CAM_2010095=2 \
  ORANGE_YOLO_AFFINITY_CAM_2010096=4 \
  ORANGE_YOLO_RT_PRIORITY="${ORANGE_YOLO_RT_PRIORITY:-0}" \
  ORANGE_YOLO_RT_POLICY="${ORANGE_YOLO_RT_POLICY:-fifo}" \
  "$nsys_bin" profile \
    "${nsys_profile_args[@]}" \
    --output="$output_base" \
    "$app_bin"
