#!/bin/bash
# nsys trace of the four-camera owner-push run around GOP boundaries (2026-09-20).
# Run as root (sudo): nsys must launch the wrapper so its CUDA injection reaches orange_client.
# Collection starts 25 s in (past warmup and the push slot handshake) and lasts 5 s (~20 GOP cycles per camera);
# the application runs to its normal 60 s end (--kill=none).
set -e
OUT=/home/jeremy/orange_data/diagnostics/gop_boundary_$(date +%Y%m%d_%H%M%S)
mkdir -p $(dirname $OUT)
exec /home/jeremy/.local/opt/nsight-systems-2025.5.2-nvenc/bin/nsys profile -o "$OUT" --trace=cuda --sample=none --cpuctxsw=none --delay=25 --duration=5 --kill=none --force-overwrite=true \
  /usr/local/bin/orange-local-benchmark --orange-client /home/jeremy/orange-device-roi-20260912/targets/release/orange_client \
  --yolo-perf-log --yolo-perf-sample 1 /tmp/fourcam_fused_recorder_realfish_int8_192_native_ownerpush_ring_nsys_20260920_011933.json
