#!/bin/bash

set -euo pipefail

echo "========================================"
echo "Starting build process..."
echo "========================================"
echo "build.sh is now a compatibility wrapper around CMake."

DEBUG=0
CLEAN=0
CUDA_DEBUG=0
NVTX_PROFILE=0
YOLO_PROFILE=0

for arg in "$@"; do
  case "$arg" in
    --debug)
      DEBUG=1
      ;;
    --clean)
      CLEAN=1
      ;;
    --cuda-debug)
      CUDA_DEBUG=1
      ;;
    --nvtx)
      NVTX_PROFILE=1
      ;;
    --yolo-profile)
      YOLO_PROFILE=1
      ;;
    --help)
      echo "Usage: $0 [options]"
      echo "Options:"
      echo "  --debug         Build in Debug mode"
      echo "  --clean         Remove the configured build directory before rebuilding"
      echo "  --cuda-debug    Enable CUDA debug logging macros"
      echo "  --nvtx          Enable NVTX profiling markers"
      echo "  --yolo-profile  Enable YOLO timing profile logging"
      echo "  --help          Show this help text"
      echo ""
      echo "Examples:"
      echo "  $0"
      echo "  $0 --debug --nvtx"
      echo "  cmake -S . -B targets/release"
      echo "  cmake --build targets/release -j\$(nproc)"
      exit 0
      ;;
    *)
      echo "Unknown option: $arg" >&2
      exit 1
      ;;
  esac
done

BUILD_TYPE="Release"
BUILD_VARIANT="release"
if [ "$DEBUG" -eq 1 ]; then
  BUILD_TYPE="Debug"
  BUILD_VARIANT="debug"
fi

if [ "$CUDA_DEBUG" -eq 1 ]; then
  if [ "$DEBUG" -eq 1 ]; then
    BUILD_VARIANT="debug_cuda"
  else
    BUILD_VARIANT="release_cuda"
  fi
fi

if [ "$NVTX_PROFILE" -eq 1 ]; then
  if [ "$CUDA_DEBUG" -eq 1 ] && [ "$DEBUG" -eq 1 ]; then
    BUILD_VARIANT="debug_cuda_nvtx"
  elif [ "$CUDA_DEBUG" -eq 1 ]; then
    BUILD_VARIANT="release_cuda_nvtx"
  elif [ "$DEBUG" -eq 1 ]; then
    BUILD_VARIANT="debug_nvtx"
  else
    BUILD_VARIANT="release_nvtx"
  fi
fi

BUILD_DIR="targets/${BUILD_VARIANT}"

echo "==> Build type: ${BUILD_TYPE}"
echo "==> Build directory: ${BUILD_DIR}"
echo "==> ORANGE_ENABLE_CUDA_DEBUG=${CUDA_DEBUG}"
echo "==> ORANGE_ENABLE_NVTX=${NVTX_PROFILE}"
echo "==> ORANGE_ENABLE_YOLO_PROFILE=${YOLO_PROFILE}"

if [ "$CLEAN" -eq 1 ]; then
  echo "========================================"
  echo "Cleaning build directory"
  echo "========================================"
  rm -rf "${BUILD_DIR}"
  rm -f targets/orange targets/orange_debug targets/orange_cuda_debug targets/orange_nvtx
fi

mkdir -p targets

CONFIGURE_ARGS=(
  -S .
  -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DORANGE_ENABLE_CUDA_DEBUG=$([ "$CUDA_DEBUG" -eq 1 ] && echo ON || echo OFF)
  -DORANGE_ENABLE_NVTX=$([ "$NVTX_PROFILE" -eq 1 ] && echo ON || echo OFF)
  -DORANGE_ENABLE_YOLO_PROFILE=$([ "$YOLO_PROFILE" -eq 1 ] && echo ON || echo OFF)
)

if [ -n "${ORANGE_EVT_ROOT:-}" ]; then
  CONFIGURE_ARGS+=(-DORANGE_EVT_ROOT="${ORANGE_EVT_ROOT}")
fi
if [ -n "${ORANGE_CUDA_ROOT:-}" ]; then
  CONFIGURE_ARGS+=(-DORANGE_CUDA_ROOT="${ORANGE_CUDA_ROOT}")
fi
if [ -n "${ORANGE_FFMPEG_ROOT:-}" ]; then
  CONFIGURE_ARGS+=(-DORANGE_FFMPEG_ROOT="${ORANGE_FFMPEG_ROOT}")
fi
if [ -n "${ORANGE_TENSORRT_ROOT:-}" ]; then
  CONFIGURE_ARGS+=(-DORANGE_TENSORRT_ROOT="${ORANGE_TENSORRT_ROOT}")
fi
if [ -n "${ORANGE_OPENCV_ROOT:-}" ]; then
  CONFIGURE_ARGS+=(-DORANGE_OPENCV_ROOT="${ORANGE_OPENCV_ROOT}")
fi
if [ -n "${ORANGE_CUDA_ARCHITECTURES:-}" ]; then
  CONFIGURE_ARGS+=(-DORANGE_CUDA_ARCHITECTURES="${ORANGE_CUDA_ARCHITECTURES}")
fi

echo "========================================"
echo "Configuring via CMake"
echo "========================================"
cmake "${CONFIGURE_ARGS[@]}"

echo "========================================"
echo "Building via CMake"
echo "========================================"
cmake --build "${BUILD_DIR}" --target orange -j"$(nproc)"

echo "========================================"
echo "Creating compatibility symlinks"
echo "========================================"
if [ "$NVTX_PROFILE" -eq 1 ]; then
  ln -sfn "${BUILD_VARIANT}/orange" targets/orange_nvtx
elif [ "$CUDA_DEBUG" -eq 1 ]; then
  ln -sfn "${BUILD_VARIANT}/orange" targets/orange_cuda_debug
elif [ "$DEBUG" -eq 1 ]; then
  ln -sfn "${BUILD_VARIANT}/orange" targets/orange_debug
else
  ln -sfn "${BUILD_VARIANT}/orange" targets/orange
fi

echo "========================================"
echo "Build completed successfully!"
echo "Binary location: ${BUILD_DIR}/orange"

if [ "$CUDA_DEBUG" -eq 1 ]; then
  echo ""
  echo "CUDA DEBUGGING ENABLED"
fi

if [ "$NVTX_PROFILE" -eq 1 ]; then
  echo ""
  echo "NVTX PROFILING ENABLED"
  echo "Profile with:"
  echo "  nsys profile --duration=30 --output=orange_profile targets/orange_nvtx"
fi

if [ "$YOLO_PROFILE" -eq 1 ]; then
  echo ""
  echo "YOLO PROFILE LOGGING ENABLED"
fi

echo "========================================"
echo "Direct CMake equivalent:"
echo "  cmake ${CONFIGURE_ARGS[*]}"
echo "  cmake --build ${BUILD_DIR} --target orange -j\$(nproc)"
