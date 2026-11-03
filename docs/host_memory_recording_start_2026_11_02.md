# Host memory and cross-CPU interrupts at recording start (2026-11-02)

Status, 2026-11-02:
- Measured on the 2026-11-02 demo recordings.
- One fix is committed on the dish branch, not installed: the recorders drop
  their written video from the page cache.
- Tracing who asks for the memory is set up but not yet run.
- Decisions are dated in the last section.

## What happened

Every recording start on 2026-11-02 interrupted Orange's isolated cores.
These are the cores that run the camera, detection and pose threads. During
the few seconds the recorder processes were starting:
- The isolated cores took up to **73,000 interrupts per second**. Normal is
  about 900 per second.
- Programs stalled waiting for memory hundreds of times.

Stream start (opening cameras, streaming, turning detection on) did none of
this. Only recording start did, in all three starts that night.

## How it is measured

The GUI wrapper (`/usr/local/bin/orange-gui-validation`) starts
`scripts/host_stall_monitor.py` beside every GUI session. Once a second it
reads the kernel's running totals:
- `/proc/vmstat`: allocation stalls, compaction stalls, page faults;
- `/proc/interrupts`: interrupts on the isolated cores, and the busiest
  interrupt kinds;
- `/proc/meminfo`, disk writes, load average, and the busiest processes other
  than Orange.

It also runs a heartbeat thread that sleeps 1 ms in a loop and logs any wake-up
more than 3 ms late, which would be a host-wide pause.

These are counts of events, not durations. An "interrupt-burst second" is one
of those one-second samples with more than 5,000 interrupts on the isolated
cores. "7 burst seconds" means 7 such samples, not a 7-second stall.

Since af512be (dish branch, not yet installed), each recording gets its own
summary for its own window. That window runs from the recording session's
start, so recorder start-up is included, to the stop request. The wrapper writes
it at GUI exit as `host_monitor_*` in the recording folder, and the validator
computes it from the session files if that copy is missing.
`scripts/host_monitor_window.py --prefix <monitor prefix> --recording-folder <folder>`
prints it by hand.

## Terms

### Pages and the page cache

The kernel manages RAM in 4 KB **pages**.

When a program writes a file, the data goes into RAM first, as *dirty* pages,
and the kernel writes it to disk shortly after. Once written, the pages are
*clean*, but the kernel keeps them in RAM in case someone reads the file again.
That is the **page cache**.

The page cache uses RAM nobody else wants. The kernel can drop clean pages any
time it needs memory, so `MemAvailable` counts them as available.

### Direct reclaim (an "allocation stall")

A program asks for memory and there is not enough free. Normally the kernel's
background thread (`kswapd`) keeps a reserve free. If the reserve runs out, the
program asking has to free page cache itself before it gets its memory. It
waits while that happens. That wait is an allocation stall (`allocstall_*` in
`/proc/vmstat`).

### Contiguous memory and compaction

Some requests need several *physically adjacent* pages: an "order-N" request
is 2^N pages, so order 4 is 64 KB. Examples are some driver buffers, kernel
structures, and huge pages.

Free memory can be plentiful but scattered in single pages between pages in
use. That is **fragmentation**, and `/proc/buddyinfo` shows it: counts of free
blocks of each size.

**Compaction** moves pages that are in use to make a contiguous free gap. When
the program asking does it itself, and waits, that is a **compaction stall**
(`compact_stall`). It can fail (`compact_fail`); the request then falls back to
something smaller or reclaims more.

### TLBs and TLB shootdowns

Programs use *virtual* addresses. Each CPU core keeps a small cache of recent
virtual-to-physical translations, the **TLB** (translation lookaside buffer),
so it does not walk the page tables on every memory access.

When the kernel changes a mapping, any core whose TLB still holds the old
translation must drop it before it can use a stale address. Mapping changes
include:
- moving a page during compaction;
- unmapping memory (`munmap`, freeing a large buffer);
- changing its protection.

The kernel tells those cores by sending each one an **inter-processor interrupt
(IPI)**. Each core stops what it is doing, flushes the entry, and continues.
That is a **TLB shootdown**: the `TLB:` line in `/proc/interrupts`.

Which cores get one depends on whose memory changed:
- a mapping of a process: every core that has run that process's threads;
- a mapping in the kernel's own address space (driver mappings set up with
  `vmap`/`ioremap`, for example): every core.

### Function-call interrupts

The `CAL:` line in `/proc/interrupts` counts the other kind of IPI: "run this
function on that core now" (`smp_call_function`). The kernel and drivers use it
for things like:
- flushing caches;
- synchronising instruction changes;
- some TLB and memory-attribute changes;
- collecting per-core state.

### Why the isolated cores still get them

`isolcpus` and `nohz_full` keep ordinary tasks and the timer tick off a core.
They do not stop IPIs: a core running Orange's threads must still be told when
memory it may have cached changes. Each IPI costs that core a few microseconds,
plus the cache and TLB misses afterwards. Tens of thousands of them per second
land directly on the acquisition and detection threads.

## Measurements, 2026-11-02

From the GUI session started at 19:26 (monitor prefix
`~/orange_data/diagnostics/host_stall/20261103T002623Z_3213346`), production
10a68c9.

| Recording | Window | Allocation stalls | Compaction stalls | Interrupt-burst s | Heartbeat gaps |
| --- | --- | --- | --- | --- | --- |
| 2026_11_02_19_42_54 | 264 s | 256 | 864 | 7 | 0 |
| 2026_11_02_20_02_54 | 453 s | 219 | 739 | 7 | 0 |

Almost all of it is in the recorder start-up seconds. For 2026_11_02_20_02_54,
against `recording_startup_audit.jsonl`, with 0 = recording enabled at
20:03:01:

| Time | Start-up step | Monitor |
| --- | --- | --- |
| -6.5 s | session begins | quiet |
| -6 to -3.7 s | full-frame recorders start, CUDA set-up, connect | TLB-shootdown bursts, 35k / 66k / 9k / 19k per s; no memory stalls |
| -2.9 s | staging slots imported and warmed | |
| -2.4 to -0.1 s | crop recorders start and prepare | 219 direct reclaims, 739 compactions (232 failed), 40k-73k IPIs per s |
| 0 | recording enabled | quiet again |
| +1.82 s | 2010093 loses one camera frame | quiet: normal interrupt rate, no stalls |

Other observations:
- The aborted start at 19:40:54 refused at pre-arm 1.5 s in. It still showed
  two seconds of TLB-shootdown bursts, and no memory stalls.
- No compiler ran, and the heartbeat saw no host-wide pause.
- The lost frame at +1.82 s came after the bursts had ended. These numbers do
  not explain it. It looks more like the old first-transfer start-up losses on
  card A: camera frame 28 at the first owner push, fixed by the recorder
  first-picture warm-up, 3f994dc, 2026-10-08.

### Memory at the time

| | |
| --- | --- |
| RAM | 256 GB, one NUMA node |
| Free | 5 GB, mostly in small blocks (`/proc/buddyinfo`) |
| Page cache | 187 GB (`MemAvailable` 202 GB) |
| Of that, recording video | about 85 GB (`fincore`: 2026_11_01_14_19_25, both 2026-11-02 recordings) |
| Of that, compression-test output | about 30 GB (`/mnt/Data2/orange_data/compression_tests`, 2026-11-01) |
| `vm.min_free_kbytes` | 67,584 (66 MB) |
| `vm.watermark_scale_factor` | 10 |
| `vm.compaction_proactiveness` | 0 (see 2026-09-23) |
| Transparent huge pages | `madvise`, defrag `madvise`; no huge-page faults in the window |

So most of the page cache was our own video, written once and never read again
on the rig. With 5 GB free and fragmented, whatever asks for contiguous memory
during recorder start-up has to reclaim and compact on the spot.

## History of the settings involved

- **2026-09-21:** `vm.dirty_background_bytes` 64 MB and `vm.dirty_bytes`
  512 MB (`/etc/sysctl.d/90-orange-writeback.conf`).
  - Without them, the kernel flushed about 3 GB of recorder pages every 31 s at
    6 GB/s on host bridge 20, and that cost camera frames.
  - The MP4 writer also paces its own writeback (`sync_file_range` every
    32 MB).
- **2026-09-23:** `vm.compaction_proactiveness = 0` (same file; its source is
  pancakebatter `configs/sysctl/90-orange-writeback.conf`).
  - Background compaction (`kcompactd`) migrated pages during recordings. Each
    migration sent TLB shootdowns to Orange's isolated cores: more than 5,000
    IPIs/s in 15 of 18 seconds on the 2026-09-23 soaks.
  - The cost of turning it off: fragmentation is no longer cleaned up in the
    background. It builds up until a request needs contiguous memory, and
    that request then compacts on the spot. This is what recorder start-up
    hits now.
- **2026-11-02:** this document.

## Decisions, 2026-11-02 (Jeremy)

- **Drop written video from the page cache: approved, built.**
  - Commit edcc97d on the dish branch. At each 32 MB writeback tick the MP4
    writer drops the pages written back since the previous tick
    (`posix_fadvise(DONTNEED)`), and all clean pages at close.
    `ORANGE_MP4_DROP_WRITTEN_PAGES=0` turns it off.
  - The writer test measures 100 % of a file cached without it and 0.7 % with
    it.
  - The recording itself is on disk either way. This only stops the kernel
    keeping a second copy in RAM.
  - It reaches recordings only after the native recorder is rebuilt and
    installed, with the dish promotion.
  - Check afterwards with `fincore` on a new recording's MP4s, and with the
    per-recording host summary.
- **Find out who asks for the contiguous memory: approved, tool ready.**
  - `scripts/trace_record_start_memory.sh` (sudo) traces one record start.
  - It catches direct reclaim and direct compaction, with the process,
    allocation order and kernel stack; IPI sends, with sender, kernel function
    and callback; and remote TLB shootdowns on the isolated cores.
  - `scripts/summarize_memory_trace.py` reports them. See "Running the trace"
    below.
- **Larger free-memory reserve: wait for the trace** (pancake-batter,
  2026-11-02; nothing changed).
  - The 5 GB free was in small blocks, so this is fragmentation more than too
    little free memory. The kernel's watermarks count free pages, not block
    sizes, so a bigger reserve may not help.
  - If tuning is needed after the trace, use `vm.watermark_scale_factor`, not
    `vm.min_free_kbytes`.
  - `vm.compaction_proactiveness = 0` does not stop `kswapd` from waking
    `kcompactd` after a high-order reclaim. `compact_daemon_wake` stood at
    2.59 million since boot, so background compaction still runs on demand.
  - The setting belongs in pancakebatter `configs/sysctl/90-orange-writeback.conf`.
    That is the source of the installed `/etc/sysctl.d` file, and
    `check_kernel_tuning.py` and `hosts/pancake0/config.yml` already read it.
  - pancake-batter agrees the recorder page-cache drop is the right first step.
- **Starting the recorders before streaming: deferred.** If the recorders started
  before the cameras streamed, their start-up could not overlap live
  acquisition. It is a larger change.

## Running the trace

With the GUI open and streaming, and not recording:

```bash
cd /home/jeremy/orange-per-camera-dish-20261011
sudo scripts/trace_record_start_memory.sh --seconds 45
```

1. Press Record within the first few seconds.
2. Stop the recording whenever you like. A short test recording is enough, and
   no fish are needed.
3. Read the output in
   `~/orange_data/diagnostics/memory_trace/<UTC time>/summary.txt`.

What it costs:
- It uses a private tracefs instance, so it doesn't use or disturb `perf` or
  other tracing.
- It holds 4 MB of ring buffer per CPU (256 MB) until it exits.
- It records kernel stacks only for reclaim and compaction events.

It is a recording, so send the usual hold and clear messages to the Citrus
sessions.

`perf` on this host (6.5.13) was built without libtraceevent and cannot record
tracepoints, which is why the script uses tracefs directly.
