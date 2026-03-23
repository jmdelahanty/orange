#!/bin/bash

set -euo pipefail

DIR_FFMPEG="${ORANGE_FFMPEG_ROOT:-$HOME/nvidia/ffmpeg}"
DIR_TENSORRT="${ORANGE_TENSORRT_ROOT:-$HOME/nvidia/TensorRT}"

if [ -n "${ORANGE_BIN:-}" ]; then
  ORANGE_EXE="${ORANGE_BIN}"
else
  ORANGE_EXE=""
  for candidate in \
    "./targets/orange" \
    "./targets/release/orange" \
    "./targets/debug/orange" \
    "./targets/release_nvtx/orange" \
    "./targets/debug_nvtx/orange"; do
    if [ -x "$candidate" ]; then
      ORANGE_EXE="$candidate"
      break
    fi
  done
fi

if [ -z "${ORANGE_EXE}" ]; then
  echo "Unable to locate an orange binary." >&2
  echo "Set ORANGE_BIN or build one with ./build.sh or cmake --build --preset release." >&2
  exit 1
fi

sudo LD_LIBRARY_PATH=/usr/local/cuda/lib64:${DIR_FFMPEG}/build/lib:${DIR_TENSORRT}/lib "${ORANGE_EXE}"
