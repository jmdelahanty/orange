#!/usr/bin/env bash
# Install the CUDA 13 native NV12 external recorder at a stable path so app
# configs and experiment specs stop pointing into a worktree build directory.
#
#   scripts/install_orange_native_recorder.sh [--prefix /opt/orange]
#
# Copies targets/native/external_recorder_ipc_probe_native and its PTX kernel
# to <prefix>/bin, keeps the binary's RUNPATH (CUDA 13.1 runtime under
# ~/.local/opt/cuda-13.1.1-nvenc/lib and the ffmpeg-nvidia libs under
# /opt/orange/lib), and verifies every shared library resolves. Build first:
#   cmake -S tools/nvenc_native_probe -B targets/native -DORANGE_SOURCE_ROOT=$PWD \
#     -DCUDA_13_ROOT=$HOME/.local/opt/cuda-13.1.1-nvenc \
#     -DNVENC_13_INTERFACE_DIR=$HOME/.local/opt/nvenc-interface-13.1.15/Video_Codec_Interface_13.1.15/Interface \
#     -DBUILD_NATIVE_RECORDER=ON -DBUILD_NATIVE_KERNEL=ON
#   cmake --build targets/native --target external_recorder_ipc_probe_native -j 16
set -euo pipefail
PREFIX="/opt/orange"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) shift; [[ $# -gt 0 ]] || { echo "--prefix requires a value" >&2; exit 2; }; PREFIX="$1"; shift ;;
    --help) sed -n 2,14p "$0"; exit 0 ;;
    *) echo "Unsupported argument: $1" >&2; exit 2 ;;
  esac
done
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$REPO_ROOT/targets/native/external_recorder_ipc_probe_native"
PTX="$REPO_ROOT/targets/native/native_nv12_write.ptx"
[[ -x "$BIN" ]] || { echo "native recorder not built: $BIN" >&2; exit 1; }
if ldd "$BIN" | grep -q "not found"; then
  echo "unresolved shared libraries:" >&2; ldd "$BIN" | grep "not found" >&2; exit 1
fi
SUDO=""; [[ "${EUID}" -eq 0 ]] || SUDO="sudo"
$SUDO install -d -m 0755 "$PREFIX/bin"
$SUDO install -m 0755 "$BIN" "$PREFIX/bin/external_recorder_ipc_probe_native"
[[ -f "$PTX" ]] && $SUDO install -m 0644 "$PTX" "$PREFIX/bin/native_nv12_write.ptx"
INSTALLED="$PREFIX/bin/external_recorder_ipc_probe_native"
if ldd "$INSTALLED" | grep -q "not found"; then
  echo "installed binary has unresolved libraries:" >&2; ldd "$INSTALLED" | grep "not found" >&2; exit 1
fi
"$INSTALLED" --help > /dev/null 2>&1 || { echo "installed binary does not run" >&2; exit 1; }
echo "[install-native-recorder] installed $INSTALLED ($(stat -c %s "$INSTALLED") bytes)"
echo "[install-native-recorder] point recording.external_ipc.recorder_tool_path and spec recorder_tool_path at it"
