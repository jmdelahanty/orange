#!/usr/bin/env bash
# 10 Hz host sampler for correlating pipeline stalls with page-cache writeback
# (2026-09-21): Dirty/Writeback from /proc/meminfo, nvme0n1 sectors written,
# io_ticks and in-flight from /proc/diskstats, procs_blocked and ctxt from
# /proc/stat. Usage: host_writeback_sampler.sh <out.csv> <stop-file>
# Stops when <stop-file> exists. Correlate with Cam*_yolo_perf.csv timestamp_sys
# (ns since epoch): the 30-min fish endurance showed 25-57 ms stalls on a
# 31.2 s lattice = the kernel's periodic writeback of ~3 GB of recorder MP4
# pages at ~6 GB/s (NVMe shares host bridge 20 with A16 card A).
set -u
OUT="${1:?out csv}"; STOP="${2:?stop file}"
echo "t_ns,dirty_kb,writeback_kb,nvme_wr_sectors,nvme_io_ticks_ms,nvme_inflight,procs_blocked,ctxt" > "$OUT"
while [ ! -f "$STOP" ]; do
  t=$(date +%s%N); m=$(awk '/^Dirty:/{d=$2} /^Writeback:/{w=$2} END{print d","w}' /proc/meminfo)
  n=$(awk '$3=="nvme0n1"{print $10","$13","$12}' /proc/diskstats); p=$(awk '/^procs_blocked/{pb=$2} /^ctxt/{c=$2} END{print pb","c}' /proc/stat)
  echo "$t,$m,$n,$p" >> "$OUT"; sleep 0.1
done
