# Process-Isolated Recorder Plan

Date: 2026-04-25

Scope: design and implement a testable process boundary between
latency-critical analytics and full-frame recording so CUDA/NVENC encode and
bitstream harvest cannot block YOLO through same-process runtime locks.

## Bottom Line

The next high-signal architecture experiment is process isolation for
full-frame recording.

The first implementation should be a headless discriminator, not the final
production recorder. It should answer one question:

```text
Does YOLO return toward the no-full-frame fast path when NVENC encode/harvest
is active on the same physical GPU but in a separate process?
```

If yes, build the external recorder backend. If no, the dominant contention is
probably below the process boundary, and the next work should shift toward GPU
placement, graph islands, or deeper driver/hardware isolation.

## Current Evidence

Useful positive evidence:

- Disabling full-frame recording makes YOLO fast.
- YOLO `cpu_preprocess_ms p95` drops from about `8 ms` to about `0.25 ms`.
- YOLO `cudaLaunchKernel_v7000 p95` drops from about `8 ms` to about
  `0.24-0.25 ms`.
- The actual `mono_to_yolo_optimized` GPU kernel remains about `0.07 ms p95`.

Useful negative evidence:

- Event-driven worker queues improved some wakeups but did not eliminate the
  detect tail.
- YOLO stream priority / nonblocking stream settings did not solve the issue
  and may regress fairness.
- In-process split submit/harvest reduced encoder submit p95 from about
  `11.85 ms` to about `0.31 ms`, but YOLO p95 did not improve.
- `ORANGE_NVENC_HARVEST_DELAY_US=1000` created a real `1.105 ms p95` harvest
  delay before `nvEncLockBitstream`, but YOLO p95 still did not improve.

Current interpretation:

- Full-frame `nvEncLockBitstream` and related NVENC/CUDA driver activity remain
  the strongest observed source of detect-side host API stalls.
- The issue is not simply that the encoder worker called harvest in the wrong
  thread.
- Same-process CUDA/NVENC runtime or driver-lock coupling remains the main
  hypothesis to test.

## Design Principle

Separate process/runtime ownership from hardware placement.

Allowed:

- analytics and recording can share a physical GPU when necessary,
- analytics GPU can also host an external recorder NVENC session,
- single-session recording and multi-GPU split-GOP recording can use the same
  external-recorder interface.

Not allowed:

- analytics process should not call full-frame NVENC APIs,
- analytics process should not block on `nvEncLockBitstream`,
- analytics and recording should not share an NVENC session object,
- recording output completion should not be part of the analytics frame lease.

The desired boundary is:

```text
main orange process:
  acquisition
  YOLO / pose / crop analytics
  GUI or headless control
  frame publication and quick detach accounting
  no full-frame NVENC calls

external recorder process:
  frame import
  source-to-encoder copies
  NVENC sessions
  nvEncEncodePicture
  nvEncLockBitstream
  bitstream mux/output
```

## Why Same-GPU Still Matters

The first process-isolation test should run NVENC on the same physical GPU as
analytics because that is the hard discriminator:

```text
same GPU, same process:
  currently bad

same GPU, separate process:
  proves whether process/runtime isolation helps

separate GPU, separate process:
  proves less because hardware placement also changed
```

For the minimal same-GPU test, the external encoder load should be limited to a
rate one NVENC session can sustain. Use `60 fps`, not `100 fps`, for
`4512x4512` full-frame single-session encoding. The goal is not to prove
production throughput. The goal is to test whether a separate NVENC process
still stalls YOLO host launches.

Production `100 fps` full-frame recording still requires split-GOP / multi-GPU
recording. Process isolation does not remove that constraint.

## Phase 0: Headless Discriminator

Goal: prove or reject the process-boundary hypothesis with the smallest useful
experiment.

Prerequisite status:

- Real headless YOLO now has an audit-only first slice through
  `orange_client --yolo-engine` and `fixed.yolo_worker.mode = "real"`.
- Use `recording_sink_mode=preprocess_only` for the analytics process when the
  external encoder load is supplied by Process B.
- Checked-in Process A baseline spec:
  `experiment_specs/2010096_headless_real_yolo_preprocessonly_a16_gpu5.json`.
- Launch the baseline through the sudo wrapper with `--yolo-perf-log` and
  `--yolo-perf-sample 1` so Process A always emits `Cam*_yolo_perf.csv` without
  relying on arbitrary sudo environment passthrough.

Process A baseline result:

- Validated on 2026-04-25 with:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_preprocessonly_a16_gpu5_run2`.
- The analytics-only process ran camera `2010096` on A16 GPU `5` with real
  TensorRT YOLO and `recording_sink_mode=preprocess_only`.
- Acquisition held `~100 fps`; no camera drops, no frame-id gaps, and no
  get-frame errors were reported.
- Steady-state after frame 200:
  `acquisition_to_worker_start_ms p95 = 0.0388 ms`,
  `yolo_queue_wait_ms p95 = 0.0181 ms`,
  `acquisition_to_detect_done_ms p95 = 3.4894 ms`,
  `cpu_preprocess_ms p95 = 0.0165 ms`,
  `total_ms p95 = 3.4606 ms`.
- This is the no-full-frame, no-GUI Process A reference. If external same-GPU
  NVENC load regresses these numbers toward the GUI in-process behavior, the
  process boundary alone is insufficient or the remaining contention is
  hardware/fabric-level. If these numbers stay close, same-process
  CUDA/NVENC/runtime coupling remains the strongest explanation.

Process A:

```text
headless orange analytics
  real camera acquisition if available
  YOLO enabled
  full-frame in-process recording disabled
  crop/pose optional but preferably disabled for first discriminator
  Nsight attached to this process
```

Process B:

```text
external NVENC load
  separate OS process
  same physical GPU as Process A for first test
  4512x4512-equivalent input
  60 fps
  same codec/preset/tuning as close as practical
  calls nvEncEncodePicture and nvEncLockBitstream continuously
```

The external load can start as synthetic. It does not need real camera frames
for the first discriminator as long as it exercises the same CUDA/NVENC encode
and bitstream harvest APIs at comparable resolution and cadence.

Phase 0 first discriminator status:

- Implemented `tools/nvenc_stress_load.cpp` as a separate-process synthetic
  NVENC stressor and `scripts/run_process_isolation_discriminator.sh` to run it
  beside headless real-YOLO analytics.
- The first same-GPU tests used camera `2010096` analytics on A16 GPU `5` and
  external NVENC stress on the same GPU at `4512x4512 @ 60 fps`.
- Synthetic solid-frame NVENC was too compressible/light:
  `nvEncLockBitstream p95 = 0.0030 ms`, `encode_total p95 = 0.1406 ms`.
- Host-noise NVENC increased output volume but still did not recreate the
  same-process GUI/NVENC blocking pattern:
  `nvEncLockBitstream p95 = 0.0031 ms`, `encode_total p95 = 0.1780 ms`.
- Headless YOLO baseline `acquisition_to_detect_done_ms p95 = 3.4894 ms`.
- Same-GPU external solid NVENC:
  `acquisition_to_detect_done_ms p95 = 4.4383 ms`,
  `cpu_preprocess_ms p95 = 0.0137 ms`.
- Same-GPU external host-noise NVENC:
  `acquisition_to_detect_done_ms p95 = 4.4115 ms`,
  `cpu_preprocess_ms p95 = 0.0154 ms`.

Interpretation:

- A separate NVENC process on the same GPU adds real shared-GPU work and moves
  YOLO p95 up by about `0.9 ms`, mostly in GPU completion/sync time.
- It does not reproduce the same-process multi-ms YOLO CPU launch/preprocess
  stalls where `cpu_preprocess_ms` was around `8 ms`.
- This supports, but does not fully prove, the process-boundary hypothesis:
  same-process CUDA/NVENC runtime contention is likely a major component, while
  shared GPU/NVENC/copy load remains a secondary cost.
- The next discriminator should make the external process block harder in
  `nvEncLockBitstream` or move real camera-frame detach/copy into the external
  recorder. The current synthetic process does not yet reproduce the long
  Linux NVENC harvest waits seen in the GUI path.

Candidate command shape:

```bash
./scripts/run_process_isolation_discriminator.sh \
  --gpu-id 5 \
  --nvenc-gpu-id 5 \
  --nvenc-fps 60 \
  --nvenc-duration 45 \
  --nvenc-pattern host-noise
```

Required outputs:

- analytics recording folder,
- analytics `Cam*_yolo_perf.csv`,
- recorder load timing CSV,
- terminal output listing analytics and recorder artifact paths.

Optional outputs:

- Nsight SQLite for Process A, when the discriminator is run under Nsight.
- combined summary JSON, if added to the wrapper later.

Primary success metrics:

- YOLO `cudaLaunchKernel_v7000 p95`,
- YOLO `pthread_rwlock_rdlock p95`,
- YOLO `cpu_preprocess_ms p95`,
- `capture_to_detect_done p95`,
- camera drops,
- YOLO queue depth.

Interpretation:

```text
YOLO returns near no-full-frame fast path:
  process/runtime CUDA/NVENC lock coupling is a major cause.
  Build the external recorder backend.

YOLO remains near current bad path:
  contention is probably below the process boundary or hardware/fabric-level.
  Do not spend heavily on recorder IPC yet.

YOLO partially improves:
  both same-process locks and hardware placement matter.
  Continue with external recorder, but keep GPU placement as a first-class
  scheduling problem.
```

## Phase 1: Minimal External Recorder

Goal: replace synthetic NVENC load with real frames while keeping the system
single-camera and single-session.

Initial scope:

- one camera,
- one external recorder process,
- one NVENC session,
- same GPU first,
- `60 fps` for `4512x4512` full-frame,
- headless only.

Main process responsibilities:

- acquire frames,
- run YOLO,
- publish a frame descriptor to recorder,
- keep source lease until recorder detach ack,
- release source lease after detach ack, not after encode completion,
- fail or degrade recording if recorder cannot detach within policy.

Recorder process responsibilities:

- import frame descriptor,
- wait for source readiness,
- copy into recorder-owned CUDA/NVENC input surface,
- send detach ack,
- submit frame to NVENC,
- harvest bitstream,
- write output.

Frame descriptor draft:

```text
camera_serial
recording_frame_id
timestamp_camera
timestamp_system
source_gpu_id
width
height
pitch
pixel_format
cuda_ipc_memory_handle or exported ring-slot handle
cuda_ipc_event_handle or readiness fence
recording_profile_id
route_policy
```

IPC control plane:

- use a Unix domain socket or local gRPC-like protocol for control and
  descriptors,
- keep messages explicit and versioned,
- include heartbeat and shutdown semantics,
- write per-run recorder config into `recording_snapshot.json`.

Data plane:

- prefer CUDA IPC handles for GPU-resident source frames,
- use interprocess CUDA events if source readiness can be exported safely,
- if GPUDirect acquisition buffers cannot be exported safely, add an
  analytics-side exportable detach ring as a fallback,
- recorder must copy to recorder-owned surfaces before acking detach.

Important: this fallback copy is still much cheaper architecturally than
same-process full-frame NVENC harvest. It may add memory bandwidth pressure,
but it should not put `nvEncLockBitstream` in the analytics process.

## Phase 2: External Split-GOP Recorder

Goal: preserve production `100 fps` full-frame throughput by moving existing
split-GOP behavior across the process boundary.

Architecture:

```text
main orange process
  -> recorder supervisor
       -> encoder shard for GPU A
       -> encoder shard for GPU B
       -> optional additional shards
       -> output/order coordinator
```

Responsibilities:

- supervisor receives frame descriptors,
- routing policy chooses primary/helper encode GPU,
- each encoder shard owns one or more NVENC sessions,
- shards copy/import frames into local encode surfaces,
- shards submit and harvest independently,
- output coordinator preserves GOP order and writer policy.

Supported modes behind one interface:

```text
single_session:
  one recorder process or shard
  one NVENC session
  lower FPS or lower resolution when single NVENC capacity requires it

split_gop:
  recorder supervisor plus multiple encoder shards
  existing GOP routing semantics
  production path for 4512x4512 @ 100 fps
```

The main process should not care whether the recorder chooses single-session or
split-GOP internally. It should publish frames and receive detach/failure
signals through one backend interface.

## Phase 3: GUI Integration

Only after headless proves the process boundary:

- add GUI switch for `in_process`, `external`, and `disabled` full-frame
  recording backends,
- run one GUI smoke with external single-session at sustainable FPS,
- run one GUI smoke with external split-GOP at production `100 fps`,
- make external backend the default only if it preserves throughput and reduces
  YOLO p95 under real GUI load.

GUI smoke should confirm:

- no camera drops,
- no recorder fallback,
- split-GOP output files complete,
- crop/pose artifacts remain valid,
- YOLO p95 stays near the headless isolated result.

## Failure Policy

Recording failure must not silently corrupt analytics timing.

Recommended policy knobs:

```text
strict_recording=true:
  recorder detach timeout or queue overflow fails the recording session.

strict_recording=false:
  recorder may drop recording frames according to explicit policy, but must log
  drops and preserve analytics.
```

Main process guardrails:

- never wait indefinitely for recorder detach,
- never hold acquisition leases until encode completion,
- keep bounded descriptor queues,
- count detach timeouts,
- count recorder queue-full events,
- record recorder process crashes in snapshot/summary,
- stop publishing if recorder heartbeat fails.

Recorder guardrails:

- bounded input queue,
- bounded in-flight surface pool,
- explicit backpressure state,
- per-shard NVENC timing,
- watchdog for stalled harvest,
- orderly drain on stop,
- output integrity check at finalization.

## Instrumentation

Analytics process:

- `cpu_preprocess_ms`,
- `cudaLaunchKernel_v7000`,
- `pthread_rwlock_rdlock`,
- `capture_to_detect_done_ms`,
- YOLO queue depth,
- frame publish-to-detach-ack latency,
- detach timeout count,
- recorder queue-full count.

Recorder process:

- descriptor queue depth,
- source readiness wait,
- source import/copy time,
- detach ack latency,
- NVENC input pool wait,
- `nvEncEncodePicture`,
- `nvEncLockBitstream`,
- bitstream copy/unlock,
- writer queue wait/write,
- per-shard in-flight count.

Cross-process:

- frame id,
- camera serial,
- source GPU,
- encode GPU,
- route type,
- published time,
- detach ack time,
- encode submit time,
- bitstream ready time,
- writer commit time.

## Open Questions

- Are the current GPUDirect receive buffers safely exportable with CUDA IPC, or
  do we need an analytics-side exportable detach ring?
- Can source readiness events be exported as CUDA IPC events in the current
  acquisition path, or do we need a recorder-side stream wait/copy protocol?
- What is the cheapest descriptor transport that still supports robust
  crash/heartbeat semantics?
- Does same-GPU separate-process NVENC improve YOLO enough to justify the IPC
  complexity?
- How much additional memory bandwidth does recorder-owned detach copying add
  at `100 fps` split-GOP scale?
- Should encoder shards be one process per GPU, one process per camera, or one
  supervisor process with one CUDA context per encode GPU?

## Implementation Checklist

Phase 0:

- [x] Add or reuse a headless analytics mode with full-frame recording disabled
      and YOLO enabled.
- [x] Add an external NVENC load/stress process at `4512x4512 @ 60 fps`.
- [x] Add `scripts/run_process_isolation_discriminator.sh`.
- [ ] Attach Nsight to the analytics process first.
- [x] Summarize YOLO timing p95 and recorder NVENC p95 from CSV artifacts.
- [x] Run same-GPU external NVENC load.
- [ ] Run separate-GPU external NVENC load only after same-GPU result is known.

Phase 1:

- [ ] Define versioned recorder descriptor protocol.
- [ ] Prototype CUDA IPC memory/event import for one GPU.
- [ ] Add detach ack path and source-lease timeout policy.
- [ ] Encode one camera at sustainable single-session FPS.
- [ ] Write recorder timing artifact.
- [ ] Verify analytics p95 against Phase 0.

Phase 2:

- [ ] Move split-GOP routing into recorder supervisor.
- [ ] Add encoder shard lifecycle and per-shard timing.
- [ ] Preserve existing strict GOP/order/output policy.
- [ ] Reproduce production `100 fps` two-camera recording.
- [ ] Compare YOLO p95 to no-full-frame, same-process, and Phase 1 results.

Phase 3:

- [ ] Add GUI backend selection.
- [ ] Run GUI smoke with external recorder.
- [ ] Decide whether external recording becomes default.
