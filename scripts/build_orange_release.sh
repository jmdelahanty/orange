#!/usr/bin/env bash
# Build a staged Orange release from a clean, pushed commit (plan
# docs/opt_install_plan_2026-11-14.md, step d). Runs as the developer, never
# as root; scripts/install_orange_release.sh (sudo) then installs the staged
# folder under /opt/orange/releases.
#
#   scripts/build_orange_release.sh [--tag orange-vYYYY.MM.DD[.N]] [--jobs N]
#                                   [--skip-fetch] [--keep-build] [--allow-unpushed]
#
# Refuses unless:
#   - the tracked tree is clean and every submodule is at its recorded commit;
#   - HEAD is on a branch of the fork (origin/*, after a fetch);
#   - no orange, orange_client or recorder process is running (builds on the
#     rig cost camera frames; send HOLD to the other sessions first).
# Then, in fresh build folders under targets/release-build/<tag>/:
#   - configures the main project with the production cache settings (Release,
#     sm_86, /opt/orange libs, TensorRT 10.0.1.6, host fast math) and builds
#     every shipped binary;
#   - runs ctest; any failure refuses the release;
#   - builds the CUDA 13 native recorder (tools/nvenc_native_probe).
# Stages targets/releases/<tag>/:
#   bin/              orange orange_client evt_stream_smoke evt_camera_ready
#                     evt_force_reboot physical_target_fit_cli
#                     external_recorder_ipc_probe_native native_nv12_write.ptx
#   share/orange/     fonts/ scripts/ docs/schemas/ contracts/ (exact committed
#                     bytes, from git archive)
#   INSTALLED.json    source, build settings, toolchain, ctest summary; the
#                     installer adds installed_at/installed_by
#   SHA256SUMS        every staged file except INSTALLED.json and itself
# Creates the local git tag <tag> at HEAD (pushing it needs Jeremy's word).
# --allow-unpushed (tests only): skip the fork check; INSTALLED.json then says
# source.on_fork false and install_orange_release.sh refuses it outside a
# --no-root test install.
set -euo pipefail

TAG=""
JOBS="${JOBS:-16}"
FETCH=1
KEEP_BUILD=0
ALLOW_UNPUSHED=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --tag) TAG="${2:?--tag needs a value}"; shift 2 ;;
    --jobs) JOBS="${2:?--jobs needs a value}"; shift 2 ;;
    --skip-fetch) FETCH=0; shift ;;
    --keep-build) KEEP_BUILD=1; shift ;;
    --allow-unpushed) ALLOW_UNPUSHED=1; shift ;;
    -h|--help) sed -n 2,35p "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "[release] $*" >&2; exit 1; }
[[ "${EUID}" -ne 0 ]] || die "run as the developer, not root (the installer is the sudo step)"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
git rev-parse --git-dir >/dev/null 2>&1 || die "not a git checkout: $ROOT"

# --- refusals -----------------------------------------------------------------
[[ -z "$(git status --porcelain --untracked-files=no)" ]] || die "tracked tree is not clean"
if git submodule status --recursive | grep -qE '^[-+U]'; then
  die "a submodule is missing or not at its recorded commit (git submodule update --init)"
fi
COMMIT="$(git rev-parse HEAD)"
if [[ "$FETCH" == 1 ]]; then git fetch -q origin || die "git fetch origin failed"; fi
ON_FORK="$(git branch -r --contains "$COMMIT" --format='%(refname:short)' | grep '^origin/' | head -1 || true)"
if [[ -z "$ON_FORK" ]]; then
  [[ "$ALLOW_UNPUSHED" == 1 ]] || die "HEAD $COMMIT is not on any origin/* branch; push it to the fork first"
  echo "[release] WARNING: HEAD is not on the fork; test release only (on_fork false)"
fi

ancestors=" "
pid=$$
while [[ "$pid" -gt 1 ]]; do
  ancestors+="$pid "
  pid="$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')"
  [[ -n "$pid" ]] || break
done
for match in $(pgrep -f "bin/orange_client|bin/orange( |$)|targets/release/orange_client|targets/release/orange( |$)|external_recorder_ipc_probe" 2>/dev/null); do
  [[ "$ancestors" == *" $match "* ]] && continue
  die "an Orange recording process is running (pid $match); build later"
done

if [[ -z "$TAG" ]]; then
  base="orange-v$(date +%Y.%m.%d)"
  TAG="$base"
  n=1
  while git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; do
    n=$((n + 1)); TAG="$base.$n"
  done
fi
[[ "$TAG" =~ ^orange-v[0-9]{4}\.[0-9]{2}\.[0-9]{2}(\.[0-9]+)?$ ]] || die "tag must look like orange-vYYYY.MM.DD[.N]: $TAG"
if existing="$(git rev-parse -q --verify "refs/tags/$TAG^{commit}")"; then
  [[ "$existing" == "$COMMIT" ]] || die "tag $TAG already names $existing, not HEAD $COMMIT"
fi

STAGE="$ROOT/targets/releases/$TAG"
BUILD="$ROOT/targets/release-build/$TAG"
[[ ! -e "$STAGE" ]] || die "staged release already exists: $STAGE (remove it to rebuild)"
rm -rf "$BUILD"
mkdir -p "$BUILD"
echo "[release] tag=$TAG commit=$COMMIT fork_branch=$ON_FORK"

# --- main project ---------------------------------------------------------------
CMAKE_SETTINGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DORANGE_CUDA_ARCHITECTURES=86
  -DORANGE_ROOT=/opt/orange
  -DORANGE_CUDA_ROOT=/usr/local/cuda
  -DORANGE_EVT_ROOT=/opt/EVT/eSDK
  -DORANGE_FFMPEG_ROOT=/opt/orange/lib/ffmpeg-nvidia
  -DORANGE_OPENCV_ROOT=/opt/orange/lib/opencv
  -DORANGE_TENSORRT_ROOT=/usr/local/TensorRT-10.0.1.6
  -DORANGE_USE_FAST_MATH=ON
  -DORANGE_ENABLE_CUDA_DEBUG=OFF
  -DORANGE_ENABLE_NVTX=OFF
  -DORANGE_ENABLE_TSAN=OFF
  -DORANGE_ENABLE_YOLO_PROFILE=OFF
)
SHIPPED=(orange orange_client evt_stream_smoke evt_camera_ready evt_force_reboot physical_target_fit_cli)
cmake -S "$ROOT" -B "$BUILD/main" "${CMAKE_SETTINGS[@]}" > "$BUILD/configure.log" 2>&1 \
  || { tail -20 "$BUILD/configure.log" >&2; die "configure failed ($BUILD/configure.log)"; }
nice -n 10 cmake --build "$BUILD/main" -j "$JOBS" > "$BUILD/build_all.log" 2>&1 \
  || { tail -30 "$BUILD/build_all.log" >&2; die "build failed ($BUILD/build_all.log)"; }
nice -n 10 cmake --build "$BUILD/main" -j "$JOBS" --target "${SHIPPED[@]}" > "$BUILD/build_shipped.log" 2>&1 \
  || { tail -30 "$BUILD/build_shipped.log" >&2; die "shipped targets failed ($BUILD/build_shipped.log)"; }
echo "[release] built main project"

( cd "$BUILD/main" && nice -n 10 ctest -j 8 --output-on-failure ) > "$BUILD/ctest.log" 2>&1 \
  || { grep -E "tests passed|Failed|\*\*\*" "$BUILD/ctest.log" | tail -20 >&2; die "ctest failed ($BUILD/ctest.log)"; }
CTEST_LINE="$(grep -E "tests passed" "$BUILD/ctest.log" | tail -1)"
echo "[release] ctest: $CTEST_LINE"

# --- native recorder ------------------------------------------------------------
CUDA_13_ROOT="${CUDA_13_ROOT:-$HOME/.local/opt/cuda-13.1.1-nvenc}"
NVENC_13_INTERFACE_DIR="${NVENC_13_INTERFACE_DIR:-$HOME/.local/opt/nvenc-interface-13.1.15/Video_Codec_Interface_13.1.15/Interface}"
for d in "$CUDA_13_ROOT" "$NVENC_13_INTERFACE_DIR"; do [[ -d "$d" ]] || die "missing directory: $d"; done
cmake -S "$ROOT/tools/nvenc_native_probe" -B "$BUILD/native" \
  -DORANGE_SOURCE_ROOT="$ROOT" -DCUDA_13_ROOT="$CUDA_13_ROOT" \
  -DNVENC_13_INTERFACE_DIR="$NVENC_13_INTERFACE_DIR" \
  -DBUILD_NATIVE_RECORDER=ON -DBUILD_NATIVE_KERNEL=ON > "$BUILD/native_configure.log" 2>&1 \
  || { tail -20 "$BUILD/native_configure.log" >&2; die "native configure failed"; }
nice -n 10 cmake --build "$BUILD/native" --target external_recorder_ipc_probe_native native_nv12_write_ptx -j "$JOBS" \
  > "$BUILD/native_build.log" 2>&1 \
  || { tail -30 "$BUILD/native_build.log" >&2; die "native recorder build failed"; }
echo "[release] built native recorder"

# --- stage ------------------------------------------------------------------------
mkdir -p "$STAGE.partial/bin" "$STAGE.partial/share/orange"
trap 'rm -rf "$STAGE.partial"' EXIT
for t in "${SHIPPED[@]}"; do
  install -m 0755 "$BUILD/main/$t" "$STAGE.partial/bin/$t"
done
install -m 0755 "$BUILD/native/external_recorder_ipc_probe_native" "$STAGE.partial/bin/"
install -m 0644 "$BUILD/native/native_nv12_write.ptx" "$STAGE.partial/bin/"
git archive --format=tar HEAD fonts scripts docs/schemas contracts | tar -x -C "$STAGE.partial/share/orange"

for b in "$STAGE.partial"/bin/*; do
  [[ "$b" == *.ptx ]] && continue
  if ldd "$b" | grep -q "not found"; then
    ldd "$b" | grep "not found" >&2; die "unresolved libraries in $(basename "$b")"
  fi
done
for b in orange orange_client; do
  strings "$STAGE.partial/bin/$b" | grep -q "$COMMIT" || die "$b does not carry the build stamp $COMMIT"
done

DRIVER="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1 || true)"
NVCC="$(/usr/local/cuda/bin/nvcc --version 2>/dev/null | grep -o 'release [0-9.]*' | head -1 || true)"
NVCC13="$("$CUDA_13_ROOT/bin/nvcc" --version 2>/dev/null | grep -o 'release [0-9.]*' | head -1 || true)"
python3 - "$STAGE.partial/INSTALLED.json" <<EOF
import json, sys
doc = {
    "schema_id": "orange.installed_release",
    "schema_version": 1,
    "release": "$TAG",
    "tag": "$TAG",
    "source": {
        "repo": "jmdelahanty/orange",
        "fork_branch": "$ON_FORK",
        "branch": "$(git rev-parse --abbrev-ref HEAD)",
        "commit": "$COMMIT",
        "describe": "$(git describe --tags --always --dirty 2>/dev/null)",
        "tracked_tree_clean": True,
        "on_fork": $([[ -n "$ON_FORK" ]] && echo True || echo False),
    },
    "build": {
        "built_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
        "built_by": "$(id -un)",
        "host": "$(hostname)",
        "cmake_settings": "${CMAKE_SETTINGS[*]}".split(),
        "native_recorder_settings": {
            "CUDA_13_ROOT": "$CUDA_13_ROOT",
            "NVENC_13_INTERFACE_DIR": "$NVENC_13_INTERFACE_DIR",
        },
        "toolchain": {
            "cuda_nvcc": "$NVCC",
            "cuda_13_nvcc": "$NVCC13",
            "tensorrt_root": "/usr/local/TensorRT-10.0.1.6",
            "evt_sdk_root": "/opt/EVT/eSDK",
            "nvidia_driver": "$DRIVER",
            "cxx": "$(c++ --version | head -1)",
        },
        "ctest": "$CTEST_LINE",
    },
    "installed_at": None,
    "installed_by": None,
}
open(sys.argv[1], "w").write(json.dumps(doc, indent=2, sort_keys=True) + "\n")
EOF
( cd "$STAGE.partial" && find . -type f ! -name INSTALLED.json ! -name SHA256SUMS -printf '%P\n' | LC_ALL=C sort \
    | xargs -d '\n' sha256sum > SHA256SUMS )
mv "$STAGE.partial" "$STAGE"
trap - EXIT

git tag "$TAG" "$COMMIT" 2>/dev/null || true
[[ "$KEEP_BUILD" == 1 ]] || rm -rf "$BUILD"

echo "[release] staged $STAGE"
echo "[release]   orange        $(sha256sum "$STAGE/bin/orange" | cut -c1-16)"
echo "[release]   orange_client $(sha256sum "$STAGE/bin/orange_client" | cut -c1-16)"
echo "[release]   files         $(wc -l < "$STAGE/SHA256SUMS")"
echo "[release] local tag $TAG -> $COMMIT (push needs Jeremy's word)"
echo "[release] install (HOLD first; Jeremy runs sudo):"
echo "  sudo $ROOT/scripts/install_orange_release.sh --release-dir $STAGE"
