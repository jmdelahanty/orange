#!/usr/bin/env bash
# Trace the kernel memory work and cross-CPU interrupts around one recording
# start: who stalls in direct reclaim or compaction (process, request order,
# kernel stack), who sends the IPIs that reach the isolated cores (kernel
# function and callback), and which CPUs take TLB shootdowns.
# Background: docs/host_memory_recording_start_2026_11_02.md.
#
# Uses a private tracefs instance (no perf/bpftrace needed; this host's perf
# has no tracepoint support), so it never touches other tracing. Needs root:
#
#   sudo scripts/trace_record_start_memory.sh [--seconds 45] [--out DIR]
#
# Start it with the GUI streaming and not recording, press Record within the
# first few seconds, and let it run out. Then it writes:
#   <out>/trace.txt         raw tracefs output (with kernel stacks)
#   <out>/clock.json        tracefs mono clock -> realtime offset
#   <out>/summary.txt/.json scripts/summarize_memory_trace.py
# Default <out>: /home/jeremy/orange_data/diagnostics/memory_trace/<UTC time>.
#
# Cost while it runs: 4 MB of ring buffer per CPU (256 MB on pancake0),
# released at the end; kernel stacks only for reclaim and compaction events.
set -euo pipefail

SECONDS_TO_TRACE=45
OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --seconds) SECONDS_TO_TRACE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ "$SECONDS_TO_TRACE" =~ ^[0-9]+$ && "$SECONDS_TO_TRACE" -ge 5 && "$SECONDS_TO_TRACE" -le 600 ]] || {
  echo "--seconds must be 5..600" >&2; exit 2; }
[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 2; }

TRACEFS=/sys/kernel/tracing
[[ -d "$TRACEFS/instances" ]] || { echo "tracefs not mounted at $TRACEFS" >&2; exit 1; }
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-/home/jeremy/orange_data/diagnostics/memory_trace/$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT"
INSTANCE="$TRACEFS/instances/orange_memtrace_$$"

cleanup() {
  if [[ -d "$INSTANCE" ]]; then
    echo 0 > "$INSTANCE/tracing_on" 2>/dev/null || true
    for trig in compaction/mm_compaction_try_to_compact_pages vmscan/mm_vmscan_direct_reclaim_begin; do
      echo '!stacktrace' > "$INSTANCE/events/$trig/trigger" 2>/dev/null || true
    done
    echo 0 > "$INSTANCE/events/enable" 2>/dev/null || true
    rmdir "$INSTANCE" 2>/dev/null || echo "note: could not remove $INSTANCE" >&2
  fi
  if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" ]]; then
    chown -R "${SUDO_UID}:${SUDO_GID}" "$OUT" 2>/dev/null || true
  fi
}
trap cleanup EXIT

mkdir "$INSTANCE"
echo 0 > "$INSTANCE/tracing_on"
echo 4096 > "$INSTANCE/buffer_size_kb"
echo mono > "$INSTANCE/trace_clock"
ev="$INSTANCE/events"
echo 1 > "$ev/vmscan/mm_vmscan_direct_reclaim_begin/enable"
echo 1 > "$ev/compaction/mm_compaction_try_to_compact_pages/enable"
echo 1 > "$ev/compaction/mm_compaction_end/enable"
echo 1 > "$ev/kmem/mm_page_alloc_extfrag/enable"
echo 1 > "$ev/ipi/ipi_send_cpumask/enable"
echo 1 > "$ev/ipi/ipi_send_cpu/enable"
# Receiving side of a TLB shootdown only (reason 1 = TLB_REMOTE_SHOOTDOWN);
# task-switch flushes would swamp the buffer.
echo 'reason == 1' > "$ev/tlb/tlb_flush/filter"
echo 1 > "$ev/tlb/tlb_flush/enable"
echo stacktrace > "$ev/compaction/mm_compaction_try_to_compact_pages/trigger"
echo stacktrace > "$ev/vmscan/mm_vmscan_direct_reclaim_begin/trigger"

python3 - "$OUT/clock.json" <<'EOF'
import json, sys, time
mono = time.clock_gettime(time.CLOCK_MONOTONIC)
real = time.time()
json.dump({"mono_s": mono, "realtime_s": real, "realtime_minus_mono_s": real - mono,
           "isolated": open("/sys/devices/system/cpu/isolated").read().strip()},
          open(sys.argv[1], "w"), indent=2)
EOF

echo 1 > "$INSTANCE/tracing_on"
echo "[memtrace] tracing for ${SECONDS_TO_TRACE} s into $OUT"
echo "[memtrace] press Record in the GUI now"
for ((i = SECONDS_TO_TRACE; i > 0; i -= 5)); do
  sleep $(( i < 5 ? i : 5 ))
  echo "[memtrace] $(( i > 5 ? i - 5 : 0 )) s left"
done
echo 0 > "$INSTANCE/tracing_on"
cat "$INSTANCE/per_cpu"/cpu*/stats 2>/dev/null | awk '/overrun/ {o += $2} END {print "[memtrace] events lost to ring overrun: " o+0}'
cat "$INSTANCE/trace" > "$OUT/trace.txt"
echo "[memtrace] raw trace: $(wc -l < "$OUT/trace.txt") lines"
python3 "$REPO_ROOT/scripts/summarize_memory_trace.py" "$OUT" | tee "$OUT/summary.txt"
