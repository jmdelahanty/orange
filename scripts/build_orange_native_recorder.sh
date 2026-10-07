#!/usr/bin/env bash
# Configure and build the CUDA 13 native full-frame recorder
# (external_recorder_ipc_probe_native + its NV12 write kernel) from this tree,
# then optionally install it to /opt/orange/bin.
#
# Usage: scripts/build_orange_native_recorder.sh [--install] [--jobs N]
#   --install   run scripts/install_orange_native_recorder.sh afterwards (sudo prompt)
#
# Inputs (override with environment variables):
#   CUDA_13_ROOT            default $HOME/.local/opt/cuda-13.1.1-nvenc
#   NVENC_13_INTERFACE_DIR  default $HOME/.local/opt/nvenc-interface-13.1.15/Video_Codec_Interface_13.1.15/Interface
# Build directory: targets/native (untracked, never committed).
# Do not run this while a recording or soak is in progress on the rig.
set -euo pipefail
INSTALL=0
JOBS="${JOBS:-16}"
while [ $# -gt 0 ]; do
    case "$1" in
        --install) INSTALL=1; shift ;;
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help) sed -n 2,13p "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CUDA_13_ROOT="${CUDA_13_ROOT:-$HOME/.local/opt/cuda-13.1.1-nvenc}"
NVENC_13_INTERFACE_DIR="${NVENC_13_INTERFACE_DIR:-$HOME/.local/opt/nvenc-interface-13.1.15/Video_Codec_Interface_13.1.15/Interface}"
for d in "$CUDA_13_ROOT" "$NVENC_13_INTERFACE_DIR"; do
    [ -d "$d" ] || { echo "missing directory: $d" >&2; exit 2; }
done
if pgrep -f "targets/release/orange_client|targets/release/orange( |$)|external_recorder_ipc_probe" >/dev/null 2>&1; then
    echo "an Orange recording process is running; build later (builds on the rig cost camera frames)" >&2
    exit 3
fi
echo "[native] source $ROOT ($(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown))"
cmake -S "$ROOT/tools/nvenc_native_probe" -B "$ROOT/targets/native" \
    -DORANGE_SOURCE_ROOT="$ROOT" \
    -DCUDA_13_ROOT="$CUDA_13_ROOT" \
    -DNVENC_13_INTERFACE_DIR="$NVENC_13_INTERFACE_DIR" \
    -DBUILD_NATIVE_RECORDER=ON -DBUILD_NATIVE_KERNEL=ON
cmake --build "$ROOT/targets/native" --target external_recorder_ipc_probe_native -j "$JOBS"
BIN="$ROOT/targets/native/external_recorder_ipc_probe_native"
[ -x "$BIN" ] || { echo "native recorder not built: $BIN" >&2; exit 1; }
echo "[native] built $BIN"
"$BIN" --help 2>&1 | grep -q -- "--rolling-clip-root" && echo "[native] --rolling-clip-root supported" || echo "[native] WARNING: --rolling-clip-root not in --help"
if [ "$INSTALL" = 1 ]; then
    "$ROOT/scripts/install_orange_native_recorder.sh"
else
    echo "[native] install with: scripts/install_orange_native_recorder.sh   (sudo; -> /opt/orange/bin)"
fi
