# Unified timing and diagnostics: design (2026-10-11)

Status: proposal for Jeremy's review. Branch `agent/diagnostics/unified-timing-20261011`,
worktree `/home/jeremy/orange-timing-unify-20261011`, based on d9a9142. No code changed yet.

## 1. Goal

Orange writes about two dozen per-frame and per-event timing, diagnostic and status files.
They made the latency, frame-loss and recorder work possible, and all of them stay. What
this design changes is how they are produced:

- one clock module, so every timestamp names its time base and unit;
- one writer core, so ownership, headers, queueing, drop counting, flush and close are
  written once instead of a dozen times;
- one declaration per recording that lists every log file, its columns, and the clock and
  unit of each time column.

Hard rule from Jeremy (2026-10-11): **every timestamp keeps the time base it has today.**
No column changes clock. Where a name is ambiguous, the declaration and the code types make
it unambiguous; a renamed column is only ever an additional, versioned change agreed with
the readers.

## 2. The time bases Orange uses

There are exactly four clocks behind every number Orange writes, plus derived strings.
The table is the vocabulary for the rest of this document and for the code.

| Name in this design | What it is | Source in code today | Epoch, unit | Comparable across | Use it for |
|---|---|---|---|---|---|
| **camera** | The Emergent camera's own timestamp for the frame | `Emergent::CEmergentFrame::timestamp`, read after `EVT_CameraGetFrame` (`src/acquire_frames.cpp:1936`, stored at `:2440`) | ns. Epoch is not stated by the SDK; Orange classifies it per recording (`orange_camera_clock_classification_v3`, `src/session/recording_session.cpp:1440-1610`) as IEEE 1588 TAI when PTP lock evidence holds and camera minus CLOCK_REALTIME is about the UTC offset (37 s), otherwise "untraceable" or "device defined" | All PTP-locked cameras in one recording | Frame ordering and inter-camera sync |
| **camera latched** | The same camera clock, read from the `GevTimestampValue` registers after `GevTimestampControlLatch` | `src/acquire_frames.cpp:1143-1157`, `ptp_state.ptp_time` | Same as camera | Same as camera | PTP health only (latch minus frame) |
| **host realtime** | The host's POSIX wall clock, `CLOCK_REALTIME` | `clock_gettime(CLOCK_REALTIME)` (`acquire_frames.cpp:2001`), `std::chrono::system_clock` (identical on Linux) | ns since 1970-01-01 UTC (POSIX, leap seconds smeared or stepped). **Can jump** when the host clock is stepped | Other machines, other recordings, Citrus, Palette, the host monitor scripts | Placing an event on the calendar; joining with other processes |
| **host monotonic** | The host's monotonic clock, `CLOCK_MONOTONIC` | `std::chrono::steady_clock` (libstdc++ uses `CLOCK_MONOTONIC`), `clock_gettime(CLOCK_MONOTONIC)`, Python `time.monotonic_ns()` | ns since an unspecified point near boot. Never jumps. **Meaningless across a reboot or another machine** | Every process on this host since boot (Orange, the recorder, Python monitors) | Every duration and latency |
| GPU elapsed | Time between two CUDA events | `cudaEventElapsedTime` | ms, a duration only | n/a | GPU stage durations |

Derived values:

- **UTC strings** (`*_utc`, `started_at_utc`, `timestamp_utc`): host realtime formatted as
  `YYYY-MM-DDTHH:MM:SSZ`, one-second resolution. Labels for people, never for timing.
- **Local-time strings**: `get_current_date_time()` (folder names, `recording_id`) and
  `nvidia-smi dmon -o DT` use the host's local time zone, with no zone marker.
- **Elapsed seconds** (`*_elapsed_s`, `*_duration_s`): host monotonic differences.

Important property: Orange, the external recorder and the Python host monitors all read
the same `CLOCK_MONOTONIC` on pancake0, so their monotonic values can be compared directly
within one boot. No code relies on that today; the design makes it explicit.

### 2.1 How today's columns map to the time bases

These names stay as they are. The declaration (section 5.3) records the mapping.

| Column (file) | Time base | Unit | Notes |
|---|---|---|---|
| `timestamp` (`_meta.csv`, `_external_meta.csv`, `_crop_meta.csv`, `_yolo_perf.csv`, preenc index, aperture `frames.csv`) | camera | ns | Name says neither clock nor unit. Already declared for the meta CSVs in `recording_session.json` `timestamp_clock_contract` |
| `camera_timestamp_ns`, `timestamps.camera_timestamp` (cadence probe, master journal, event logs, startup timing) | camera | ns | |
| `latched_ptp_time_ns` (cadence probe), `last_latched_ptp_time_ns` (ptp summary) | camera latched | ns | |
| `timestamp_sys` (all meta CSVs, perf CSVs, IPC FRAME line), `timestamps.timestamp_sys_ns`, `capture_sys_ns` (owner push), `host_realtime_ns` (master journal) | host realtime | ns | One value, four names. See 2.2 for when it is taken |
| `timestamps.event_epoch_us` (event logs) | host realtime | µs | |
| `session_realtime_ns`, `receive_realtime_ns` (startup audit), `realtime_posix_ns` (NIC thermal), `t_ns` (host stall, writeback sampler) | host realtime | ns | |
| `receive_host_ns`, `*_steady_ns`, `*_host_ns`, `t_steady_ns`, `session_steady_ns`, `monotonic_elapsed_ns`, recorder status `steady_clock_ns` | host monotonic | ns | Most say "steady" or "host", none says "monotonic" |
| `timestamps.event_monotonic_us` (event logs) | host monotonic | µs | |
| `mono_ns` (host stall) | host monotonic | ns | Python `time.monotonic_ns()`, same clock as Orange's steady |
| every `*_ms` latency in the perf CSVs, except the GPU columns | host monotonic difference | ms | |
| `infer_ms`, `pre_ms`, `wait_ms`, `gap_ms`, `early_copy_ms` (yolo perf), `crop_copy_gpu_ms`, `device_stage_gpu`, `stage_copy_ms` (recorder) | GPU elapsed | ms | |
| `queue_ms` (yolo perf) | mixed: host `sync_ms` minus GPU `infer_ms` | ms | Mixed by construction; keep, but declare it |
| `age_at_push_ms` (owner push), `phase_wait_ms` (recorder) | host realtime difference | ms | A duration taken on the wall clock; see 4.2 |
| `sync.gate_times.start_ns/stop_ns` (snapshot) | camera | ns | Name does not say so |

### 2.2 When `timestamp_sys` is taken

`timestamp_sys` is read at `src/acquire_frames.cpp:2001`, after `PTP_timestamp_checking`
(`:1960-1968`), not at the return of `EVT_CameraGetFrame` (`:1936`, where `receive_host_ns`
is taken on the monotonic clock). With the default `ORANGE_PTP_LATCH_AFTER_FANOUT=1` the slow
register latch is deferred, so the gap is only the PTP check itself. With the latch inline
(`=0`) the 1.7-3.8 ms register round trip lands inside the gap. The value is a contract field
and keeps its position; the declaration records it as "host realtime, read on the
acquisition thread after the PTP check". If a receipt-exact wall time is wanted later, it is
a new, additional column.

## 3. What exists today

### 3.1 Writers

Contract level: **C** = pinned schema or delivered to Palette or Citrus; **M** = declared in
`recording_session.json` (`realtime_products`, `recording_backend`), diagnostic;
**I** = internal (scripts and validators only).

| File | Writer and mechanism | Rows from | File I/O or formatting on a hot thread | Enabled by | Level |
|---|---|---|---|---|---|
| `Cam*_pipeline_perf.csv` | `PipelinePerfRecorder` (`acquire_frames.cpp:757`), plain ofstream, flush per row | acquisition thread, 1 Hz | **yes: write and flush; close does a `recording_snapshot.json` read-modify-write** | always while recording | M |
| `ptp_sync_summary.json` | `update_ptp_sync_summary_camera` (`project.cpp:6896`), read-modify-write under a global mutex | acquisition thread, 1 Hz | **yes** | `sync_camera` and recording | C (required artifact) |
| `Cam*_acquisition_cadence_probe.csv` | `AcquisitionCadenceProbeRecorder` on `AsyncLineSink` | acquisition thread | formatting (~70 fields, `ostringstream`, `gmtime`); open and close | frames 80-160 always; `ORANGE_ACQ_CADENCE_PROBE_ALL` | M |
| `Cam*_ring_release.csv` | `RingReleaseRecorder` on `AsyncLineSink` | acquisition thread | `snprintf`; open and close | `ORANGE_ACQ_RING_RELEASE_LOG` (off) | M |
| `Cam*_owner_push.csv` | `AsyncLineSink` (`recording_ingress.cpp:1958`) | handoff thread | `ostringstream`; open and close | owner push on | M |
| `Cam*_master_frames_v1.csv` + `.json` | `MasterFrameJournal`: lock-free SPSC ring, writer thread on configured CPUs, `O_EXCL`, fsync, create-once descriptor | acquisition thread (no I/O, no allocation) | no | `fixed.master_frame_journal` (off; required for crop-only) | C |
| `Cam*_yolo_perf.csv` | `YoloPerfLogger`: own thread and deque (8192), no flush per row | YOLO thread (fills a struct with three strings) | small allocations | `ORANGE_YOLO_PERF_LOG` (on), `ORANGE_YOLO_PERF_SAMPLE` | M (declared "so Palette can ingest") |
| `Cam*_yolo_events.jsonl` | `YoloEventLogger`: own thread, 8192 queue, JSON on the writer thread | YOLO thread (copies detections) | allocations | always | C (`orange.yolo_event` v2) |
| `Cam*_pose_events.jsonl` | `PoseEventLogger`: own thread, 8192 queue, flush 1 s | pose thread | allocations | with pose | C (`orange.pose_event` v2) |
| `Cam*_pose_perf.csv` | header at rotate, one summary row at close, ofstream under `recording_mutex_` | control thread | pose thread can wait on this mutex | with pose | M |
| `Cam*_crop_meta.csv` | ofstream on the crop-encode thread, flush per row | crop-encode thread | yes (not on the banned list) | crop pipeline | C |
| `Cam*_crop_perf.csv` | ofstream on the crop-encode thread, flush per row | crop-encode thread | yes | crop pipeline | M |
| `Cam*_crop_sidecar_perf.csv` | header at rotate, one summary row at close | control or crop thread | may wait up to 2 s for the preview worker | crop pipeline | M |
| `Cam*_meta.csv` (in-process) | `recording_metadata::write_row` on encoder threads | encoder threads | yes (in-process recorder is not production) | in-process recording | C |
| `Cam*_external_meta.csv` | `FrameMetadataCsvWriter` in the recorder, under the merged-output mutex at GOP release | recorder encode or harvest thread | yes | always | C |
| `Cam*_external_encode.csv` | ofstream on the recorder encode thread, flush every 60 | recorder encode thread (intake thread for `submit_direct_input`) | **yes, on the NVENC submit path** | supervisor always passes it | M |
| `Cam*_external_detach.csv`, `Cam*_external_gop_routing.csv` | ofstreams on the recorder intake thread after the ACK, flush every 60 | recorder intake thread | yes, between ACKs | always | M (routing is declared and read by the chroma repair) |
| `Cam*_external_status.json` | temp file + rename, `/proc/self/status`, `filesystem::space()` per output | recorder intake thread, ≥1 s | **yes, between one ACK and the next read** | always | M |
| `Cam*_external_summary.json`, keyframes, `.finalization.json`, content digest | end of run, writer thread, idle thread | not hot | | | C / M |
| `recording_startup_audit.jsonl` + summary | singleton, mutex-guarded vector, written at session end | acquisition, handoff, session threads | `Mark` builds strings under a mutex; the `camera_frame_gap` mark is not windowed | always | I |
| `gui_timing_windows.csv` | `GuiTimingSidecarWriter`: 1 s windows, bounded queue, own thread | GUI thread | no | always while recording | I (prose docs) |
| camera startup timing JSON | `GuiStartupTimingRecorder` | GUI thread | small | always | I (declares its clock) |
| `orange_local_control.events.jsonl` | append-open per event under a mutex | socket and GUI threads | no | local control on | C (local control contract) |
| `nic_thermal_monitor.csv` + summary | separate helper process | | no | `ORANGE_NIC_THERMAL_MONITOR_ENABLED` (on) | M |
| host stall monitor, writeback sampler, `nvidia_smi_dmon.csv` | Python, shell, nvidia-smi | | no | wrapper / headless client | I |

### 3.2 Time helpers

About 30 functions return the current time, most of them private copies:
`steady_clock_now_ns` (`latency_stats.h:41`), five copies of `steady_now_ns`,
`steady_now_ns_diag`, `steady_time_now_ns`, `helper_host_now_ns`, `recording_ingress_now_ns`,
`steady_ns`/`realtime_ns` (startup audit, NIC thermal, replay probe),
`GuiStartupTimingRecorder::NowNs`, three copies each of `epoch_time_us`/`steady_time_us`,
`get_steady_time_us`/`get_epoch_time_us`/`get_time_us` (`shaman.h`),
`get_current_utc_timestamp` (about 106 call sites in 30 files), four more `utc_now` copies,
`CurrentUtcTimestamp`, `gui_shadow_utc_now`, the local-time `get_current_date_time` family,
`thread.h tick()`, `now_ms` lambdas, `MonotonicNs`. Direct calls: `steady_clock::now` 294 in
`src/` and 118 in `tools/`, `system_clock::now` 16 and 4, `clock_gettime(CLOCK_REALTIME)` 8.

## 4. Problems the survey found

These are fixed as small, separate commits on this branch, each with its own note, whether
or not the rest of the design goes ahead.

### 4.1 Correctness

1. `get_current_utc_timestamp()` (`project.cpp:4200`) calls `std::gmtime`, which returns a
   shared static buffer. Several acquisition threads call it at once (the cadence probe,
   `pipeline_perf` rows), so two threads can swap each other's strings. Fix: `gmtime_r`.
2. `thread.h tick()` returns `CLOCK_MONOTONIC` as a `float` in seconds. After 4 days of uptime
   (about 3.7×10^5 s) a float resolves only about 0.03 s. The headless loop uses it
   (`orange_headless_client.cpp:12378-12398`). Fix: `double` or integer ns.
3. The pose event log stamps `event_epoch_us`/`event_monotonic_us` on the writer thread when
   the line is written (`pose_event_log.cpp:257-261`); the YOLO log stamps them on the YOLO
   thread when the result exists. Same field names, different meanings. This is a contract
   field (`orange.pose_event` v2): either document "time the line was written", or move the
   stamp to result time under a schema note agreed with Citrus and Palette. Recommendation:
   move it (it is what the name says) and announce it.
4. Missing stamps are written three ways: −1 (YOLO), 0.0 (pose `elapsed_ms`, crop CPU
   columns), −1 (`crop_copy_gpu_ms`). A real 0.0 ms latency and "not measured" look the same
   in the pose log. The core will write one sentinel per file, declared in the index;
   existing files keep theirs and the index records which one each uses.
5. `scripts/analyze_gop_boundary_phase.py:18` globs `Cam<serial>_gop_routing.csv`; the file is
   `Cam<serial>_external_gop_routing.csv`, and the result is never used. It falls back to
   `(recording_frame_id - 1) % 50`, which matches production only because the routing
   offset is 0 and GOPs are 25 frames. Fix: read the real file.

### 4.2 Misleading values (documented, not changed)

6. Recorder detach `copy_ms` is the CPU time of `detach_and_enqueue` (queueing an async copy),
   not a copy duration. The GPU copy is `stage_copy_ms` in the encode CSV.
7. Recorder encode CSV `returned_packets`, `returned_bytes`, `lock_bitstream_ms` and
   `bitstream_fetch_ms` describe packets of earlier frames (`extra_output_delay`). In
   split-submit mode the completion columns are always 0 in the CSV.
8. `age_at_push_ms` (owner push) and the recorder `phase_wait_ms` are durations measured on
   the wall clock, so a host clock step would show up as a jump. The phase hold needs the
   wall clock (it aligns to `timestamp_sys`); `age_at_push_ms` could use monotonic, but it
   keeps its time base per the rule above, and the index declares it.
9. `queue_ms` (YOLO perf) is a host duration minus a GPU duration.

### 4.3 Hot-thread work that should move

10. `pipeline_perf` writes and flushes on the acquisition thread every second, and its close
    rewrites `recording_snapshot.json` there.
11. `ptp_sync_summary.json` is read, modified and rewritten on the acquisition thread every
    second.
12. `AsyncLineSink::Open` and `Close` (truncate, header, thread start and join) run on the
    acquisition and handoff threads at folder rotation. Per-row formatting stays on those
    threads for the cadence probe, ring release and owner push.
13. The recorder rewrites its status JSON on the intake thread (with a free-space query per
    output) and writes the encode CSV on the NVENC submit thread.
14. `YoloPerfLogger`, `YoloEventLogger` and `PoseEventLogger` threads inherit the creator's
    scheduling and CPU affinity (possibly an isolated core); `AsyncLineSink` and the master
    journal do not.

## 5. Design

### 5.1 `src/timing/clock.h`: one clock vocabulary

Header-only, no dependencies, used by Orange, the recorder and the tools.

```cpp
namespace orange::timing {
// Strong types: the compiler refuses to mix them.
struct CameraNs     { uint64_t value; };  // Emergent frame timestamp (camera / PTP clock)
struct RealtimeNs   { uint64_t value; };  // CLOCK_REALTIME, ns since 1970 UTC; can step
struct MonotonicNs  { uint64_t value; };  // CLOCK_MONOTONIC (= std::chrono::steady_clock)
struct DurationNs   { int64_t  value; };  // difference of two values of ONE time base

MonotonicNs monotonic_now();
RealtimeNs  realtime_now();
DurationNs  between(MonotonicNs from, MonotonicNs to);   // only same-base overloads exist
DurationNs  between(RealtimeNs from, RealtimeNs to);
DurationNs  between(CameraNs from, CameraNs to);

std::string utc_iso8601(RealtimeNs t, int fractional_digits = 0);  // gmtime_r, thread-safe
std::string local_folder_stamp(RealtimeNs t);  // today's YYYY_MM_DD_HH_MM_SS, local time

// A pair read back to back, for placing monotonic stamps on the calendar.
struct ClockAnchor { MonotonicNs monotonic; RealtimeNs realtime; };
ClockAnchor anchor_now();  // monotonic, realtime, monotonic; keeps the tighter pair
}
```

- Every current-time helper listed in 3.2 becomes a call to `monotonic_now()` or
  `realtime_now()`. The values are identical, because they read the same clocks.
- The format of every existing string stays identical (one-second `...Z` strings stay
  one-second; `utc_iso8601(t, 0)` produces exactly today's format).
- `WORKER_ENTRY::timestamp` and `timestamp_sys` become `CameraNs` and `RealtimeNs` inside
  the code; their file and IPC representation does not change.

### 5.2 `src/timing/channel.h`: one writer core

Generalises `AsyncLineSink` and the master journal's ring:

- **Hot side:** `channel.emit(record)` copies a fixed-size, trivially copyable record into a
  bounded single-producer ring. No allocation, no formatting, no lock, no syscall. A full
  ring increments `dropped` and returns.
- **Writer side:** one low-priority thread per process (or per camera when a channel needs
  it) drains all channels, formats rows, writes, and flushes on a timer. It sets its own
  scheduling and stays off the isolated cores (the rule for every helper thread).
- **Lifecycle off the hot thread:** open (create, `ScopedFsuid`, header), rotate and close
  run on the writer thread. Hot threads only publish "the folder changed at frame N".
- **Per channel:** name, file pattern, columns with their declarations, schema id and
  version, missing-value sentinel, enable switch, queue size, flush policy, and counters
  (`offered`, `written`, `dropped`, `queue_high_water`).
- **Formats:** CSV and JSONL. A channel whose bytes are pinned by a contract supplies its own
  row formatter, so its output stays byte-identical.

The master journal's create-once, `O_EXCL`, fsync and digest gate stay as they are: it moves
onto the shared ring and writer, not onto weaker guarantees.

### 5.3 The declaration: `diagnostics_index.json` per recording

Written at finalize (create-once) and declared in `recording_session.json`, next to
`realtime_products`:

```json
{
  "schema_id": "orange.recording.diagnostics_index", "schema_version": 1,
  "clock_anchors": [ {"label": "recording_start", "monotonic_ns": 0, "realtime_ns": 0} ],
  "files": [
    {
      "path": "Cam2010093_yolo_perf.csv", "channel": "yolo_perf", "format": "csv",
      "schema": {"id": null, "version": null, "header_sha256": "..."},
      "rows_written": 0, "rows_dropped": 0, "missing_value": "-1",
      "columns": {
        "timestamp":     {"time_base": "camera",        "unit": "ns"},
        "timestamp_sys": {"time_base": "host_realtime", "unit": "ns",
                          "taken": "acquisition thread, after the PTP check"},
        "infer_ms":      {"time_base": "gpu_elapsed",   "unit": "ms", "kind": "duration"},
        "queue_ms":      {"time_base": "mixed",         "unit": "ms", "kind": "duration",
                          "note": "host sync_ms minus GPU infer_ms, clamped at 0"}
      }
    }
  ]
}
```

- The time-base names are the five in section 2 (`camera`, `camera_latched`,
  `host_realtime`, `host_monotonic`, `gpu_elapsed`) plus `mixed`, `utc_string` and
  `local_time_string`.
- `clock_anchors`: monotonic and realtime read back to back at recording start, at stop and
  once a minute, so any monotonic value in any file can be placed on the calendar.
- It extends the existing `timestamp_clock_contract` (which covers only the meta CSVs) to
  every file, and it is what the validators and `realtime_products` read, instead of
  searching the folder for known names.
- The recorder writes its own index fragment; the finalizer merges it.

### 5.4 Switches

Each channel's switch moves into the app config (`diagnostics.channels.<name>`), bridged to
today's environment variable names so existing scripts keep working. The switches are then
sealed in `session.effective_configuration` like every other app-config knob.

## 6. Migration, in order

Each step is its own commit, built and tested; each step that touches a hot path passes the
headless gate (60 s round, then the 30-minute endurance) with detect p95/p99, zero camera
frame-id gaps, zero recorder drops and unchanged NIC counters before it goes further.

| Step | What | Output change | Risk |
|---|---|---|---|
| 0 | Fixes 4.1 (1, 2, 5) | none, apart from the fixed bugs | low |
| 1 | `clock.h`; replace the private helpers | **none** (same clocks, same formats); a unit test pins every string format | low |
| 2 | `channel.h` core; move the diagnostic-only files: pipeline perf, ptp summary RMW, cadence probe, ring release, owner push, yolo perf, startup audit, GUI timing | none in content; rows may arrive at the file a little later | hot path, gated |
| 3 | `diagnostics_index.json`, clock anchors, app-config switches | adds one file and one manifest block | low |
| 4 | Recorder: status JSON and encode/detach/routing CSVs onto the core | none in content | recorder hot path, gated |
| 5 | Contract files (`_meta.csv`, `_external_meta.csv`, `_crop_meta.csv`, event logs, master journal) onto the core with their own formatters | **byte-identical**, proven by golden-file tests that feed fixed records through old and new writers | contract, gated |
| 6 | Optional, later: versioned column names that say their clock (for example `host_realtime_ns`), only as a schema version agreed with Citrus and Palette | yes, versioned | coordination |

Fix 4.1 (3), the pose event-time stamp, needs Jeremy's choice (document versus move) and a
note to Citrus and Palette before it changes.

## 7. Questions for Jeremy

1. Pose `event_epoch_us`/`event_monotonic_us`: move the stamp to pose-result time (matches
   YOLO and the field name) or document it as write time?
2. Should `diagnostics_index.json` go to Palette with the recording (it is small and makes
   every diagnostic file self-describing), or stay Orange-internal at first?
3. Step 6 (renaming columns to say their clock): worth doing at all, or is the declaration
   enough?
4. One writer thread per process, or one per camera? Per process is simpler; per camera
   isolates a slow disk from the other cameras' logs.
