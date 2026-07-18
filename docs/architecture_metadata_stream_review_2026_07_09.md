# Threading, Metadata, and Data-Stream Architecture Review, 2026-07-09

Branch: `exp/gop-split-a16`

Status: read-only static review; findings are open unless a later status note
explicitly marks them resolved.

## Scope

This review covers:

- worker and raw-thread lifecycle, synchronization, queueing, and shutdown
- frame, CUDA buffer, and external-recorder ownership
- acquisition, analytics, display, recording, and IPC data flow
- recording metadata, timestamps, manifests, provenance, and durability
- build, validation, and architectural practices relevant to those paths

This was not a runtime or hardware validation. No binaries were built, no tests
were run, and no repository files were changed as part of the review itself.
The review intentionally did not inspect or modify `build-gop-split/`.

Severity terms used below:

- `critical`: credible silent data corruption or unsafe buffer reuse in an
  active production-capable path
- `high`: undefined behavior, hangs, false-success artifacts, unbounded growth,
  or loss of an explicit recording guarantee
- `medium`: incorrect diagnostics/provenance, weak recovery behavior, or a
  structural best-practice gap that should be scheduled

## Executive Summary

The repository is stronger than average in normal-path frame ownership,
instrumentation, process isolation, validation, and performance engineering.
The CAS-based `WORKER_ENTRY` ownership core, bounded object pools, CUDA-event
fencing, split-GOP routing, recording lifecycle modeling, and artifact
validators show unusually deliberate design work.

The repository is not yet following best practices end to end. The largest
remaining risks are concentrated at failure and transition boundaries:

1. an in-process helper-GPU path can copy from a camera buffer after it has
   become eligible for SDK reuse;
2. shared recording/UI state contains definite C++ data races;
3. YOLO failure can strand the display worker in an uncancellable spin;
4. external deferred-release recovery can recycle a CUDA source whose ownership
   is still ambiguous;
5. queue overload behavior does not match the advertised drop/fail policy;
6. media and metadata write failures do not reliably make a session incomplete;
7. authoritative metadata is not durably committed and can contain false
   runtime provenance; and
8. the local external-recorder protocol lacks strict identity, framing, and
   peer validation.

The current architecture should therefore be described as **strong on the
healthy path and transitional at failure boundaries**, rather than fully
production-hardened.

## Priority Summary

| Priority | Finding | Primary impact |
| --- | --- | --- |
| P0 | Helper-GPU preprocess uses the wrong source pointer | Silent wrong/corrupt in-process recording frames |
| P0 | `CameraControl` and `CameraEachSelect` are shared without synchronization | C++ undefined behavior and torn lifecycle transitions |
| P0 | Display waits forever when YOLO does not publish completion | GUI shutdown hang and retained frame ownership |
| P0 | Deferred IPC timeout recovery releases ambiguous ownership | Recorder may read a recycled CUDA source |
| P1 | Blocking queue behavior conflicts with `fail_on_drop` | Acquisition stall and SDK-ring starvation instead of fail-fast |
| P1 | External writer and GOP queues are unbounded | Process memory growth under disk or ordering failure |
| P1 | Writer failures do not control session completion | Truncated media can be reported as `completed` |
| P1 | Canonical metadata commits are not durable | Partial or mutually inconsistent authoritative artifacts |
| P1 | External IPC identity/framing is permissive | Cross-session interference, hangs, and process-stability risk |
| P2 | Timestamp, continuity, pose provenance, and wrap accounting are inaccurate | Misleading diagnostics and archived provenance |

## Detailed Findings

### 1. Helper-GPU Split-GOP Can Read a Recycled Camera Buffer

Severity: `critical`

The default analytics-hybrid acquisition path stores two distinct pointers:

- `WORKER_ENTRY::d_image` remains the Emergent SDK camera buffer;
- `WORKER_ENTRY::d_analytics_image` is the Orange-owned delayed-consumer copy.

Acquisition records `analytics_ready_event` after producing the owned copy and
then permits the SDK frame to be requeued after the analytics/YOLO fences:

- `src/acquire_frames.cpp:1754-1767`
- `src/acquire_frames.cpp:1786-1808`
- `src/acquire_frames.cpp:1858-1873`

`WORKER_ENTRY::delayed_consumer_image()` correctly selects the owned image:

- `src/video_capture.h:96-115`

`EncoderPreprocessWorker` also computes the correct `input_source`, but the
cross-GPU peer-copy branch ignores it and passes `entry->d_image` to
`cudaMemcpyPeerAsync`:

- `src/encoder_preprocess_worker.cpp:1372-1385`
- `src/encoder_preprocess_worker.cpp:1440-1447`

In the early-owned path, that pointer can refer to an SDK buffer that is already
eligible for requeue/reuse. The result can be a wrong or corrupted encoded
frame without a crash. The exact defect is scoped to in-process split-GOP
helper-GPU preprocessing; external IPC uses the delayed-consumer accessor.

Recommended action:

- use `input_source` in the peer copy;
- add a regression test that reuses or poisons the original camera source after
  `analytics_ready_event`, then verifies that helper preprocessing still reads
  the owned copy.

### 2. Recording and UI Control State Has Definite Data Races

Severity: `high`

`CameraControl` stores lifecycle fields as plain booleans:

- `src/video_capture.h:136-162`

Those values are concurrently accessed by GUI/session, acquisition,
preprocess, and encoder threads. Examples include:

- session writes: `src/session/recording_session.cpp:2232` and `:2286-2298`
- acquisition reads: `src/acquire_frames.cpp:1448`, `:1646`, and `:1691`
- preprocess reads/writes: `src/encoder_preprocess_worker.cpp:850-874` and
  `:1298-1320`
- hardware worker reads/writes: `src/encoder_hw_worker.cpp:1181-1194` and
  `:2250-2274`

This is C++ undefined behavior. Independently converting
`record_video`, `recording_draining`, and `stop_record` to atomics would still
allow impossible combinations because the fields jointly describe one
lifecycle transition.

`CameraEachSelect` is also mutable shared state containing plain booleans,
strings, counters, and pointers:

- definition: `src/video_capture.h:346-374`
- live GUI writes: `src/orange.cpp:5291-5316`, `:5358-5365`, `:5410`, and
  `:5481-5484`
- acquisition reads: `src/acquire_frames.cpp:1637-1647` and `:2004`
- YOLO read: `src/yolo_worker.cpp:1868`

Recommended action:

- replace the recording booleans with one synchronized lifecycle state machine;
- publish an immutable per-session camera-selection/configuration snapshot;
- route supported live changes through an explicit synchronized command
  channel or narrowly scoped atomic settings.

### 3. YOLO Failure Can Strand the Display Worker

Severity: `high`

Display treats `has_detections` as evidence that it must wait for CPU-side YOLO
postprocessing, then busy-spins until `detections_ready` becomes true:

- `src/opengldisplay.cpp:159-173`

YOLO publishes `detections_ready` only on its successful path:

- `src/yolo_worker.cpp:1760-1762`

The per-item exception handler records CUDA-event state but does not publish a
terminal detection state:

- `src/yolo_worker.cpp:2037-2048`

`cudaSetDevice` can also throw before the worker's inner `try` block:

- `src/yolo_worker.cpp:1190-1199`

The display wait has no stop predicate or timeout, so GUI teardown can block
joining the display worker. `has_detections` itself is a plain boolean that is
used both for "YOLO scheduled" and "YOLO found objects," creating a separate
race and semantic ambiguity.

Recommended action:

- replace the two flags with an atomic terminal state such as
  `not_scheduled`, `pending`, `complete`, or `failed`;
- publish a terminal state from an entry-scoped guard on every exit path;
- use a cancellation-aware wait instead of a busy loop.

### 4. Worker Lifetime and Shutdown Are Not RAII-Safe

Severity: `high`

`CThreadWorker::~CThreadWorker()` is empty:

- `src/threadworker.h:179-182`

`COffThreadMachine::~COffThreadMachine()` calls `StopThread()`, but by that
point virtual dispatch cannot reach `CThreadWorker::DoStopThread()` and its
condition-variable notifications:

- `src/offthreadmachine.cpp:19-28`
- `src/offthreadmachine.cpp:70-94`
- `src/threadworker.h:418-445`

A worker sleeping on the queue condition variable may therefore never wake if
an owner relies on destruction for cleanup. Several most-derived destructors
also free CUDA or other worker resources before the base stop:

- `src/opengldisplay.cpp:111-129`
- `src/yolo_worker.cpp:1057-1085`
- `src/encoder_preprocess_worker.cpp:353-404`

Normal GUI teardown explicitly stops many workers first, reducing the common
case risk, but the class-level lifetime guarantee is unsafe during exceptions,
partial construction, or alternate ownership paths.

There is also a producer/consumer ordering concern in
`ModernRecordingPipeline::request_stop()`: hardware consumers are stopped
before preprocess producers:

- `src/modern_recording_pipeline.cpp:277-296`

Recommended action:

- require stop/join in the most-derived owner before resource destruction;
- prefer a scoped or `std::jthread`-like worker owner;
- close ingress, drain preprocess while hardware workers remain live, then
  drain and stop hardware workers.

### 5. Deferred External-IPC Recovery Can Recycle an In-Use Source

Severity: `high`

In deferred-release mode, Orange registers a pending source before waiting for
the recorder ACK. If ACK reading fails, Orange removes the entry from the
pending map and releases it:

- `src/recording_ingress.cpp:757-784`

A missing ACK does not prove that the recorder rejected the descriptor. The
recorder may already have imported the CUDA allocation and begun consuming it.
Reset, session refresh, and destruction also release all pending entries
without an explicit recorder `RELEASE`:

- `src/recording_ingress.cpp:169-173`
- `src/recording_ingress.cpp:198-204`
- `src/recording_ingress.cpp:807-827`
- `src/recording_ingress.cpp:851-861`

The normal recorder path correctly emits `RELEASE` only after source
consumption is synchronized:

- `tools/external_recorder_ipc_probe.cpp:3423`

Recommended action:

- quarantine ambiguous entries until an explicit `RELEASE` arrives;
- otherwise release only after the recorder process is known dead and has been
  joined, making further CUDA access impossible;
- record quarantine depth and timeout cause in session health metadata.

### 6. External IPC Has More Than One Socket Reader

Severity: `high`

The external handoff worker polls protocol lines from its worker thread:

- `src/recording_ingress.cpp:207-240`

The main-thread drain query can also call `poll_protocol_lines()`:

- `src/recording_ingress.cpp:175-184`

Both paths operate on `socket_fd_` and `receive_buffer_`, and the receive buffer
is mutated without a socket-I/O ownership lock:

- `src/recording_ingress.cpp:416-455`

Concurrent reads can consume an ACK or `RELEASE` in the wrong caller and race
the string buffer.

Recommended action:

- make the worker thread the sole socket owner;
- have `IsDrained()` read only atomically published protocol state;
- send commands to the socket owner through a local command queue.

### 7. Queue Saturation Does Not Match `fail_on_drop`

Severity: `high`

`CThreadWorker::PutObjectToQueueIn()` blocks until capacity is available and
returns false only after stop is requested:

- `src/threadworker.h:253-271`

Recording ingress and acquisition interpret false as queue rejection/drop:

- `src/recording_ingress.cpp:1045-1203`
- `src/acquire_frames.cpp:2134-2174`

As a result, a full recording queue does not trigger the configured
`fail_on_drop` path. It blocks acquisition, risking SDK-ring starvation and
camera frame loss outside the queue that supposedly owns the overload policy.

Recommended action:

- separate `PutBlocking` and `TryPut` APIs;
- use a nonblocking fail-stop contract for lossless acquisition-to-recording
  admission;
- define preview/analytics behavior separately, such as decimation or
  drop-oldest;
- preserve ownership release on every rejection path.

### 8. Drain Detection Has a Queue-to-In-Flight Visibility Window

Severity: `medium`

The base worker removes an item and decrements queue depth before a derived
worker increments its local `in_flight` counter:

- dequeue: `src/threadworker.h:428-434`
- handoff increment: `src/recording_ingress.cpp:104-117`
- external IPC increment: `src/recording_ingress.cpp:263-268`
- preprocess increment: `src/encoder_preprocess_worker.cpp:858-882`

Another thread can briefly observe queue depth zero and in-flight zero while an
item is already executing.

Recommended action:

- maintain an active-item count in the base worker and update it as part of the
  dequeue transition; or
- use ordered end-of-stream tokens with explicit acknowledgements rather than
  sampled queue/in-flight state.

### 9. External Writer and GOP Queues Are Unbounded

Severity: `high`

An `FFmpegWriterQueueConfig` limit of zero means unlimited:

- `src/FFmpegWriter.h:23-26`

External merged, rolling-clip, and per-shard writers are constructed without
limits:

- `tools/external_recorder_ipc_probe.cpp:2189-2196`
- `tools/external_recorder_ipc_probe.cpp:2342`
- `tools/external_recorder_ipc_probe.cpp:3059-3066`

The external merged GOP coordinator also has no GOP-count or byte limit:

- `tools/external_recorder_ipc_probe.cpp:2049-2076`
- `tools/external_recorder_ipc_probe.cpp:2551`

A slow disk, failed writer, or missing frontier GOP can therefore grow memory
indefinitely, multiplied across base, clip, and shard outputs. The in-process
coordinator already demonstrates the stronger pattern with byte and in-flight
GOP limits:

- `src/shared_recording_output.cpp:317-396`

Recommended action:

- give every external packet and reorder queue explicit packet, byte, and GOP
  limits;
- make overflow a latched recorder failure;
- stop descriptor admission when the required recorder becomes unhealthy.

### 10. Recorder Worker Failure Is Observed but Not Enforced

Severity: `high`

The client parses `RECORDER_STATUS` but does not make the reported worker state
authoritative:

- `src/recording_ingress.cpp:478-503`

The recorder can continue accepting and acknowledging descriptors after a
worker failure, and supervisor polling primarily treats process/wait failures
as fatal rather than the published `worker_failed` state:

- `tools/external_recorder_ipc_probe.cpp:4360-4468`
- `src/external_recorder_supervisor.cpp:1548`

For required or `fail_on_drop` recording, worker failure should close
admission, propagate immediately to session control, and force the manifest to
`incomplete`.

### 11. Media Write Failures Can Still Produce a Completed Session

Severity: `high`

`av_interleaved_write_frame()` errors are printed but neither returned nor
latched:

- `src/FFmpegWriter.cpp:286-295`

Flush, trailer, and close return values are ignored:

- `src/FFmpegWriter.cpp:137-150`

`writer_thread_failed()` exists but is not consumed by in-process recording
owners:

- `src/FFmpegWriter.h:59-64`

Metadata CSV writes also do not consistently check stream health:

- `src/shared_recording_output.cpp:483-489`
- `src/encoder_hw_worker.cpp:458-463`

GUI session success is based on external-recorder lifecycle state rather than
in-process mux/metadata health:

- `src/gui/recording_finalizer.cpp:2032-2034`
- `src/gui/recording_finalizer.cpp:2086`

Headless single-clip completion has a similar start/drain-based decision:

- `src/orange_headless_client.cpp:8779-8802`

A disk-full, EIO, mux, trailer, or CSV failure can therefore leave truncated
artifacts while `recording_session.json` says `completed`.

Recommended action:

- latch every FFmpeg and metadata stream failure;
- expose writer health through recording workers and session orchestration;
- require healthy writer/trailer state, expected files, positive packet counts,
  and configured count/continuity invariants before publishing `completed`.

### 12. Canonical Metadata Writes Are Not Durably Atomic

Severity: `high`

The snapshot JSON helper writes a fixed `.tmp` file and renames it, but it does
not check close-time failure or synchronize the file and parent directory:

- `src/project.cpp:3796-3843`

A partial temp file can therefore replace a previously good canonical file
after ENOSPC or another delayed write failure. Session JSON and CSV writers
truncate their canonical targets directly:

- `src/session/recording_session.cpp:776-807`
- `src/session/recording_session.cpp:842-899`

Rolling JSON and CSV are written separately with no generation identifier or
commit marker:

- `src/session/recording_session.cpp:1650-1655`

GUI and headless finalization publish several related artifacts sequentially,
so interruption can leave a mutually inconsistent set:

- `src/gui/recording_finalizer.cpp:716-869`
- `src/orange_headless_client.cpp:8761-8852`

Recommended action:

- centralize artifact writes through a same-directory unique temp file;
- serialize, flush/check, close/check, `fsync` the file, rename, and `fsync` the
  parent directory;
- add a generation/session identifier to related artifacts;
- publish the parent manifest or latest pointer last as the transaction commit;
- add ENOSPC and interrupted-finalization fault-injection tests.

### 13. GUI Pose Snapshots Can Record False Runtime Provenance

Severity: `high`

GUI snapshot construction hardcodes enabled pose as `mode="noop"` and
`backend="noop"`:

- `src/gui/recording_snapshots.cpp:223-241`

The actual `PoseWorker` can resolve the same environment to
`mode="real"` and `backend="tensorrt"`:

- `src/pose_worker.cpp:567-601`

A real TensorRT GUI pose recording can consequently be archived as noop.

Recommended action:

- have snapshot generation consume the worker's already-resolved immutable
  runtime configuration rather than reconstructing it independently.

### 14. `timestamp_sys` Does Not Match Its Documented Meaning

Severity: `medium`

The contract defines `timestamp_sys` as realtime nanoseconds captured at frame
receive:

- `docs/output_artifacts_contract.md:1670-1683`

Acquisition captures monotonic receive time immediately after `GetFrame`, then
performs optional PTP register polling, and only afterward samples
`CLOCK_REALTIME`:

- `src/acquire_frames.cpp:1560-1583`
- PTP SDK register reads: `src/acquire_frames.cpp:1011-1014`

The realtime/camera timestamp pairing therefore includes a
`ptp_register_read_decimate`-dependent control-plane delay.

Recommended action:

- sample realtime and monotonic receive timestamps adjacent to the successful
  `GetFrame` return;
- retain `ptp_check_done_host_ns` as a separate diagnostic timestamp.

### 15. External Continuity Metadata Is Sometimes Synthesized

Severity: `medium`

External manifest construction can hardcode `recording_frame_id_gaps=0` or
derive first/last IDs from encoded counts rather than observed IDs:

- GUI rolling: `src/gui/recording_finalizer.cpp:567-579`
- GUI single clip: `src/gui/recording_finalizer.cpp:1431-1442`
- headless rolling: `src/orange_headless_client.cpp:5942-5952`

Actual recording IDs are available in external routing rows:

- `tools/external_recorder_ipc_probe.cpp:1549-1597`

Non-rolling finalization can also skip a missing expected GOP and regenerate
sequential output PTS, time-compressing a gap:

- `tools/external_recorder_ipc_probe.cpp:2416`
- `tools/external_recorder_ipc_probe.cpp:2489`

Recommended action:

- publish measured first, last, gap, duplicate, and regression counters from
  recorder-observed IDs;
- make missing or duplicated non-rolling frame/GOP identity an integrity
  failure rather than silently normalizing it.

### 16. Camera Frame-ID Gap Accounting Is Wrong Around Wrap

Severity: `medium`

The 16-bit frame-ID helper uses zero both as an initialization sentinel and a
wrapped predecessor, and its wrap formula is off by one:

- `src/video_capture.h:388-412`

It runs for every acquired frame:

- `src/acquire_frames.cpp:1584-1587`

At 100 fps, a 16-bit counter wraps in about eleven minutes, so this affects
ordinary long recordings.

Recommended action:

- track initialization separately from the ID value;
- compute modular distance explicitly;
- unit-test consecutive wrap, one-gap wrap, duplicates, regressions, and an SDK
  sequence that legitimately contains zero.

### 17. Camera-Buffer Return Failures Are Ignored

Severity: `medium`

Major `EVT_CameraQueueFrame()` paths discard the SDK return value, including
normal, pending, shutdown, and final-release paths:

- `src/acquire_frames.cpp:1499`
- `src/acquire_frames.cpp:1662`
- `src/acquire_frames.cpp:1845`
- `src/acquire_frames.cpp:2289`
- `src/acquire_frames.cpp:2486`
- `src/worker_entry_release.h:23`

A failed return can silently shrink the SDK receive ring while Orange proceeds
as if ownership was restored.

Recommended action:

- centralize camera-frame return handling;
- expose per-camera failure counters and the last SDK error;
- make repeated or recording-time return failure session-fatal.

### 18. External Recorder IPC Lacks Strict Identity and Framing

Severity: `high`

The diagnostic recorder unlinks the configured path, binds it, and changes the
socket to mode `0666` without checking `chmod`:

- `tools/external_recorder_ipc_probe.cpp:1356-1379`

The server has no peer-credential check. Its line reader performs blocking
one-byte reads with no maximum line size or timeout:

- `tools/external_recorder_ipc_probe.cpp:1383-1408`

The server parses `CLIENT_HELLO` but does not strictly match the expected
session, stream, camera, or role. It also accepts control or frame input without
first requiring a valid hello:

- `tools/external_recorder_ipc_probe.cpp:4476-4528`
- `tools/external_recorder_ipc_probe.cpp:4531-4545`

Recommended baseline:

- place sockets in an owner/group-only runtime directory;
- use `0600` or intentionally managed `0660` permissions;
- check `SO_PEERCRED`;
- bound line length and read time;
- require the handshake before any frame/control message;
- exactly match session, stream, camera, version, and role;
- validate dimensions, pixel format, byte size, monotonic IDs, and GOP
  invariants before CUDA import/copy.

### 19. GUI Recording Configuration Is Fail-Open

Severity: `medium`

GUI camera loading resets recording configuration to defaults, then ignores a
recording parser failure because no error output is requested:

- `src/project.cpp:2662-2676`

Unsupported schema IDs/versions warn but continue:

- `src/project.cpp:3245-3272`

A typo, unsupported configuration, or narrowed oversized value can therefore
silently change AQ, split-GOP, output, or routing behavior.

Recommended action:

- reject invalid active recording configuration at startup;
- return field-specific validation errors;
- range-check before narrowing integer values;
- archive the resolved configuration actually used.

### 20. Global Host Fast-Math Conflicts With Validation Code

Severity: `medium`

`ORANGE_USE_FAST_MATH` defaults on, and non-Debug C++ targets receive
`-Ofast -ffast-math` globally:

- `CMakeLists.txt:22`
- `CMakeLists.txt:994-1000`

Production host code relies on NaN/Inf sentinels and `std::isfinite` checks:

- `src/spatial_layout_schema.cpp:28-31`
- `src/projected_center_preflight.cpp:111-153`
- `src/pose_worker.cpp:168-173`

Fast-math permits the compiler to assume finite values and can invalidate these
guards.

Recommended action:

- keep strict floating-point semantics for configuration, geometry, metadata,
  and model-result validation;
- enable fast math only for measured numeric hot translation units or kernels
  whose contracts explicitly exclude NaN/Inf.

### 21. Session Orchestration Remains Duplicated

Severity: `medium`

The repository's architecture documentation accurately describes the current
transitional state:

- shared recording runtime: mostly unified;
- shared recording policy/validation: unified;
- shared GUI/headless session lifecycle/orchestration: not yet unified.

References:

- `docs/session_orchestration_architecture.md:24-44`
- GUI orchestration: `src/orange.cpp` and
  `src/session/recording_session.cpp`
- headless orchestration: `src/orange_headless_client.cpp`

This duplication is already producing behavior drift in completion gates,
continuity reporting, snapshot provenance, and validation depth.

Recommended action:

- continue toward one typed session orchestrator with GUI and headless adapters;
- centralize completion criteria, artifact commits, resolved runtime snapshots,
  and external-recorder health handling.

### 22. PTP Coordination Uses Racy, Unbounded Hand-Rolled Barriers

Severity: `high`

`PTPParams` stores all coordination state as plain integers and booleans:

- `src/camera.h:244-254`

The synchronization code mixes GCC atomic builtins with ordinary reads and
writes of the same objects:

- `src/video_capture.cpp:46-69`
- `src/acquire_frames.cpp:2437-2459`
- `src/orange_headless_client.cpp:4797-4801`

The start and stop waits have no deadline or cancellation predicate. A camera
that fails before reaching the barrier can therefore strand its peers or the
headless startup thread indefinitely. Mixing atomic and non-atomic access to
the same storage is also a C++ data race even when one access happens through a
compiler builtin.

Recommended action:

- replace the shared fields with a mutex/condition-variable barrier or
  consistently atomic generation-based state;
- include a stop predicate, deadline, and failed-camera identity;
- publish timeout/failure details into session metadata.

### 23. Generic Per-Item Exception Handling Can Leak Pooled Ownership

Severity: `high`

`CThreadWorker::ThreadRunning()` catches an exception from `WorkerFunction`,
latches the error, and continues, but has no generic rejected-item cleanup hook:

- `src/threadworker.h:460-481`

Workers are therefore safe from `std::terminate`, but not automatically safe
from retaining the dequeued object's ownership. Examples include display CUDA
work before its release:

- throwing work begins: `src/opengldisplay.cpp:150-187`
- release occurs later: `src/opengldisplay.cpp:244-250`

YOLO also performs `cudaSetDevice` before its internal guard/`try` and releases
the entry only at the end of the function:

- `src/yolo_worker.cpp:1190-1199`
- `src/yolo_worker.cpp:2050-2054`

Repeated exceptions can leak pool entries and eventually stall acquisition.

Recommended action:

- establish the ownership guard as the first operation in each worker;
- or give `CThreadWorker` a mandatory cleanup hook for a dequeued item that
  exits through an exception;
- test exception injection before and after CUDA submission.

### 24. The GUI Image-Writer Worker Is Incomplete and Unowned

Severity: `medium`

The GUI allocates and starts one `ImageWriterWorker`:

- `src/orange.cpp:4342-4343`

The final cleanup path has no corresponding stop or delete:

- `src/orange.cpp:6880-6928`

Acquisition constructs a save job containing only an event pointer:

- `src/acquire_frames.cpp:2004-2013`

The worker requires a CPU buffer, dimensions, and output path, so the submitted
job cannot succeed:

- `src/image_writer_worker.cpp:28-48`

The worker also uses scalar `delete` for a buffer described as array-allocated:

- `src/image_writer_worker.cpp:53-57`

Recommended action:

- give the worker explicit scoped ownership and stop/join cleanup;
- construct a complete immutable save job with clear buffer ownership;
- use a pooled or RAII buffer and correct array/custom-deleter semantics;
- reset or report `frame_save_state` after success or failure.

## Good Practices Already Present

### Frame Ownership and Pooling

- `src/worker_entry_ownership_core.h` uses acquire/release CAS loops,
  resurrection prevention, underflow diagnostics, and move-only RAII leases.
- `src/bounded_object_pool.h` provides bounded capacity, high-water, miss, and
  invalid-return telemetry.
- acquisition entries, CUDA events, encoder preprocess entries, and crop/pose
  frames are predominantly preallocated and pooled.

### Normal-Path Stream Synchronization

- the intended delayed-consumer abstraction correctly separates camera-source
  readiness from Orange-owned-frame readiness;
- source-release CUDA events protect normal encoder preprocess lifetimes;
- detached external IPC ACKs after the recorder owns a safe copy;
- direct-input external IPC normally emits `RELEASE` after source consumption.

### Recording and Validation

- in-process split-GOP output has ordering, byte, and in-flight-GOP limits;
- session contract construction is substantially centralized in
  `src/session/recording_session.{h,cpp}`;
- recording drain/finalization is explicitly modeled and has targeted tests;
- rolling validation checks clip/index/manifest/snapshot coherence and
  cross-clip continuity;
- external verification checks counters, routing, media sanity, storage, and
  handshake artifacts.

### Build and Concurrency Testing

- project C++ targets receive `-Wall -Wextra`;
- CI performs a release build and CTest run;
- CI has a targeted TSan job for queue and worker-entry ownership stress tests;
- `docs/thread_sanitizer_runbook.md` accurately documents the TSan coverage
  boundary.

The next valuable host-state tests are for `CameraControl`, display detection
completion, external socket ownership, drain state, and PTP cancellation.

## Recommended Remediation Sequence

### P0: Correctness and Lifetime

1. Use the owned delayed-consumer pointer in helper-GPU preprocessing and add a
   camera-buffer-reuse regression test.
2. Replace shared lifecycle booleans with a synchronized state machine and
   immutable session snapshots.
3. Publish terminal YOLO completion on every exit and make display waits
   cancellable.
4. Quarantine ambiguous deferred IPC frames and make one thread the sole socket
   owner.
5. Make worker stop/join ownership explicit before resource destruction.
6. Make per-item ownership exception-safe and replace PTP polling barriers with
   bounded, cancellable synchronization.

### P1: Failure Authority and Boundedness

1. Split blocking and nonblocking queue APIs and align each stream with an
   explicit overload contract.
2. Bound every external writer/reorder queue by packets, bytes, and GOPs.
3. Propagate recorder worker failure immediately to admission and session
   control.
4. Latch FFmpeg, trailer, metadata, and camera-return failures.
5. Make those health signals prerequisites for a `completed` manifest.
6. Implement durable, generation-aware artifact commits.
7. Harden external IPC credentials, identity, framing, and descriptor checks.

### P2: Metadata Truth and Architectural Convergence

1. Archive resolved pose/runtime configuration rather than reconstructing it.
2. Capture realtime receive timestamps adjacent to `GetFrame`.
3. Measure external continuity rather than synthesizing it.
4. Fix and test 16-bit camera frame-ID wrap accounting.
5. Fail closed on active recording configuration/schema errors.
6. Restrict fast math to explicitly safe, measured hot code.
7. Complete the shared GUI/headless session orchestrator.
8. Repair or retire the incomplete GUI image-writer path.

## Validation Expected After Remediation

Static fixes should be followed by targeted tests before broad hardware runs:

- helper-copy source-poison/reuse test;
- TSan tests for lifecycle/config snapshots and display completion states;
- external IPC ACK-loss, `RELEASE`-loss, peer-death, and concurrent-drain tests;
- queue saturation tests proving the configured overload outcome;
- disk-full/EIO and trailer-failure injection proving `incomplete` manifests;
- interrupted multi-artifact commit/recovery tests;
- frame-ID wrap, duplicate, gap, and regression tests;
- strict IPC handshake/identity/framing tests;
- then the established one-, two-, and four-camera PTP external-recorder
  validations, including decoded video sanity and steady-state latency checks.
