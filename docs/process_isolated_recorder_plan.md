# Process-Isolated Recorder Plan

Date: 2026-04-25

Scope: design and implement a testable process boundary between
latency-critical analytics and full-frame recording so CUDA/NVENC encode and
bitstream harvest cannot block YOLO through same-process runtime locks.

See also:

- `docs/external_recorder_implementation_plan.md`

## Bottom Line

The next high-signal architecture experiment is process isolation for
full-frame recording.

The first synthetic headless discriminator has already run. It supports, but
does not fully prove, the process-boundary hypothesis because the synthetic
encoder did not reproduce the long in-process Linux NVENC harvest waits seen in
the real GUI path.

The live detach prototype has now passed its first one-camera smoke. The next
implementation step should therefore be:

```text
1. attach real NVENC encode/harvest to the external process' recorder-owned
   buffer,
2. start at a sustainable one-camera same-GPU rate,
3. compare YOLO timing against the validated two-camera PTP baseline once the
   external encoder path is mechanically correct.
```

The key architecture question remains:

```text
Does YOLO return toward the no-full-frame fast path when NVENC encode/harvest
is active, but full-frame recording APIs and nvEncLockBitstream are outside the
analytics process?
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

New two-camera PTP evidence from 2026-04-25:

- Free-run two-camera headless artifacts were misleading on the local
  `100_cam4` setup because `2010095` encoded all-black frames at only about
  `24.6 Mbps` while `2010096` encoded real dish content around `151 Mbps`.
- The PTP headless spec
  `experiment_specs/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp.json`
  produced valid real content on both cameras:
  `2010095` about `150.8 Mbps`, `2010096` about `151.3 Mbps`.
- The GUI PTP/AQ-off run using local config folder
  `/home/jeremy/orange_data/config/local/100_cam4_ptp` also produced valid
  real content on both cameras:
  `2010095` about `150.64 Mbps`, `2010096` about `150.75 Mbps`.
- GUI PTP and headless PTP both landed around `11-12 ms`
  `capture_to_detect_done_ms p95`.
- YOLO queue wait stayed tiny in the GUI PTP run, about `0.014-0.015 ms p95`;
  the tail was mostly `cpu_pre_sync_ms`, about `6.6-7.7 ms p95`.

Interpretation:

```text
PTP is required for valid two-camera load comparisons on this host/config.
GUI is not adding a new large regression versus headless PTP.
The remaining tail is still host-side CUDA/NVENC submission/sync contention.
```

No-fish/zero-detection runs are acceptable for this process-isolation layer
because acquisition, YOLO CUDA/TensorRT submission, and full-frame split-GOP
recording still run. They do not validate crop ROI, pose, tracking, or
positive-detection end-to-end crop/pose latency.

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

Real-frame external load status:

- `tools/nvenc_stress_load.cpp` now supports `--pattern raw-file` so Process B
  can loop cached NV12 frames captured from `pre_encoder_reference_capture`.
- This is still not the final live detach path. It does not pass camera-owned
  GPU memory across a process boundary. It does, however, replace synthetic
  solid/noise input with real pre-encoder dish frames and can optionally write
  an elementary bitstream via `--bitstream-out`.
- Use this as the next low-risk discriminator before implementing CUDA IPC:
  if real-frame external load still keeps YOLO near the preprocess-only fast
  path, process isolation remains promising; if it regresses YOLO toward the
  in-process GUI tail, real content/output pressure is enough to contend below
  the process boundary.
- First same-GPU real-frame external run on 2026-04-25 used the existing
  `2010096` pre-encoder NV12 dump
  `2026_04_09_preenc_smoke_2010096_a6000_retry/.../Cam2010096_preenc_ref.bin`
  with `4512x4512`, pitch `4608`, 16 cached frames, `60 fps`, and elementary
  bitstream output.
- Result:
  - Process A remained healthy at about `100 fps`, no camera drops, and no
    frame-ID gaps.
  - YOLO `cpu_preprocess_ms p95 = 0.0149 ms` and
    `cpu_pre_sync_ms p95 = 0.0776 ms`, essentially unchanged from the
    preprocess-only baseline (`0.0165 ms` and `0.0798 ms`).
  - `acquisition_to_detect_done_ms p95` rose from `3.4894 ms` to `4.4122 ms`
    and `total_ms p95` rose from `3.4606 ms` to `4.3865 ms`.
  - The added latency was visible in GPU completion timing:
    `infer_ms p95` rose from `2.8012 ms` to `3.5419 ms`, and `sync_ms p95`
    rose from `3.3545 ms` to `4.3012 ms`.
  - The added latency was not visible in CPU launch/preprocess timing:
    `cpu_preprocess_ms p95` and `cpu_pre_sync_ms p95` were slightly lower than
    baseline.
  - External NVENC wrote about `150.4 Mbps`; `nvEncLockBitstream p95` stayed
    only `0.0029 ms`, with `encode_total_ms p95 = 0.1886 ms`.
- Interpretation: real-frame external NVENC on the same GPU still does not
  reproduce the in-process YOLO host-side stall. It does still add about
  `0.9 ms p95` of same-GPU hardware/fabric completion latency. This means
  process isolation should be treated as a way to remove same-process
  runtime-lock coupling, not as a way to make shared GPU/NVENC/copy resources
  free. Encode GPU placement and split-GOP routing remain first-class design
  choices. This result is still from cached raw-file input, not yet live CUDA
  IPC / split-GOP recorder handoff.

Live CUDA IPC detach status:

- Implemented experimental `recording_sink_mode = "external_ipc"` and
  `tools/external_recorder_ipc_probe.cpp`.
- The analytics process exports an owned CUDA source buffer handle over a Unix
  domain socket. The external probe imports the handle, copies the frame into a
  recorder-owned device buffer, sends `ACK <recording_frame_id>`, and only then
  the analytics process recycles the source lease.
- Socket path defaults to
  `/tmp/orange_external_recorder_<camera_serial>.sock`, with optional
  `ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_<serial>` or
  `ORANGE_EXTERNAL_RECORDER_SOCKET` overrides.
- First one-camera smoke on `2010096` / A16 GPU `5` used real headless YOLO and
  `recording_sink_mode = "external_ipc"`.
- Result:
  - probe ACKed `601` frames;
  - acquisition sustained about `99.85 fps`;
  - camera drops, frame-id gaps, get-frame errors, preprocess drops, and encode
    failures were all zero;
  - post-warmup YOLO `acquisition_to_worker_start_ms p95 = 0.0552 ms`;
  - post-warmup YOLO `cpu_preprocess_ms p95 = 0.0119 ms`;
  - post-warmup YOLO `acquisition_to_detect_done_ms p95 = 3.6366 ms`;
  - external D2D detach copy `copy_ms p95 = 0.039 ms` after frame 50.
- Interpretation: the source lease / ACK boundary is mechanically viable at
  one-camera `100 fps` and does not reintroduce the same-process YOLO CPU
  launch/preprocess tail. This prototype does not encode yet; the next slice is
  to run NVENC from the external process' recorder-owned buffer before ACKing
  encode completion independently of the analytics lease.

External-process NVENC first slice:

- `tools/external_recorder_ipc_probe.cpp` now has an optional external encode
  mode:
  - `--encode` enables a dedicated encoder thread;
  - `--encode-max-fps <fps>` caps encode cadence while still ACKing all
    descriptors;
  - `--bitstream-out <path>` writes a raw HEVC/H.264 elementary stream;
  - `--encode-csv <path>` writes per-encoded-frame timing.
- The listener thread keeps the detach boundary:
  - for selected encode frames, it copies the imported CUDA IPC source into a
    recorder-owned slot, sends the ACK, then queues that slot to the encoder
    thread;
  - skipped frames are ACKed without a detach copy because the external
    recorder has intentionally decided not to record them;
  - `nvEncEncodePicture`, `nvEncLockBitstream`, and bitstream writing happen
    after ACK on the external process' encoder thread.
- First same-GPU encode smoke on `2010096` / A16 GPU `5` used real headless
  YOLO, `recording_sink_mode = "external_ipc"`, and external HEVC encode capped
  at `60 fps`.
- Result:
  - analytics process received `601` frames at about `99.85 fps`;
  - post-warmup `runs.csv` reported `submitted_frames_final = 500`,
    `external_ipc_frames_acked_final = 501`, `external_ipc_failures_final = 0`,
    and `external_ipc_ack_timeouts_final = 0`;
  - external process encoded `360` frames, skipped `241` by the `60 fps` cap,
    and dropped `0` for queue pressure;
  - external detach `copy_ms p95 = 0.0346 ms` after frame 50;
  - external encode `encode_total_ms p95 = 0.1404 ms`,
    `lock_bitstream_ms p95 = 0.0073 ms`, and
    `bitstream_fetch_ms p95 = 0.0515 ms` after the first 20 encoded frames;
  - raw HEVC output size was about `113.9 MB` for the short smoke.
- YOLO result:
  - `acquisition_to_worker_start_ms p95 = 0.0488 ms`;
  - `yolo_queue_wait_ms p95 = 0.0173 ms`;
  - `cpu_preprocess_ms p95 = 0.0149 ms`;
  - `cpu_pre_sync_ms p95 = 0.0914 ms`;
  - `acquisition_to_detect_done_ms p95 = 4.5895 ms`;
  - `total_ms p95 = 4.5613 ms`.
- Interpretation: external-process NVENC did not bring back the bad
  same-process `8-10 ms` CPU launch/preprocess tail. The extra latency versus
  detach-only is same-GPU completion pressure (`infer_ms` / `sync_ms`), not CPU
  API orchestration. This supports continuing toward a real external recorder,
  while keeping GPU placement and split-GOP routing as first-class design
  variables.

30-second GPU placement comparison:

- Same-GPU run:
  `scripts/run_external_recorder_smoke.sh --duration 30 --warmup 2 --encode-fps 60 --output-dir /tmp`
- Paired-GPU run:
  `scripts/run_external_recorder_smoke.sh --duration 30 --warmup 2 --encode-fps 60 --recorder-gpu-id 6 --output-dir /tmp`
- Both runs used camera `2010096`, analytics/YOLO on A16 GPU `5`, real live
  frames, external HEVC `p1/ll`, and a `60 fps` external encode cap.
- Both runs received/ACKed `3203` descriptors, encoded `1922` frames, skipped
  `1281` by policy, dropped `0`, had `0` IPC failures/timeouts, had `0`
  camera gaps/get-frame errors, and passed external MP4 sanity.
- Same GPU `5 -> 5` post-warm p95:
  `capture_to_detect_done_ms = 4.591`, `total_ms = 4.560`,
  `infer_ms = 4.104`, `sync_ms = 4.463`,
  external `encode_total_ms = 0.112`, and
  `nvEncLockBitstream_ms = 0.0028`.
- Paired GPU `5 -> 6` post-warm p95:
  `capture_to_detect_done_ms = 3.245`, `total_ms = 3.222`,
  `infer_ms = 2.718`, `sync_ms = 3.130`,
  external `encode_total_ms = 0.126`, and
  `nvEncLockBitstream_ms = 0.0028`.
- Interpretation: process isolation kept the YOLO CPU launch path fast in both
  placements. Moving external NVENC off the analytics GPU reduced the remaining
  GPU completion pressure. This is a placement signal, not a claim that one
  helper GPU can encode full production rate; full `4512x4512 @ 100 fps`
  recording still requires external split-GOP / multi-GPU routing.

Current limitations of this slice:

- The probe now writes MP4, keyframe sidecar, per-frame CSVs, and a summary
  JSON, but it is still a diagnostic recorder rather than a production backend.
- The smoke encodes a capped subset (`60 fps`) while acquisition remains
  `100 fps`; it is a same-GPU contention discriminator, not production
  full-frame coverage.
- The repeatable runner is
  `scripts/run_external_recorder_smoke.sh`; it uses the default per-camera
  socket path so the existing `sudo -n /usr/local/bin/orange-local-benchmark`
  wrapper can run without extra env passthrough.
- The external process still does only single-session encode. Production
  `4512x4512 @ 100 fps` still requires external split-GOP / multi-GPU routing.

Candidate command shape:

```bash
./scripts/run_process_isolation_discriminator.sh \
  --gpu-id 5 \
  --nvenc-gpu-id 5 \
  --nvenc-fps 60 \
  --nvenc-duration 45 \
  --nvenc-pattern host-noise
```

Real-frame command shape:

```bash
./scripts/run_process_isolation_discriminator.sh \
  --gpu-id 5 \
  --nvenc-gpu-id 5 \
  --nvenc-fps 60 \
  --nvenc-duration 45 \
  --nvenc-pattern raw-file \
  --nvenc-raw-file /path/to/Cam2010096_preenc_ref.bin \
  --nvenc-raw-pitch 4608 \
  --nvenc-raw-frame-bytes 31186944 \
  --nvenc-raw-cache-frames 60 \
  --nvenc-bitstream-out /tmp/orange_external_real_frame.hevc
```

Use the `pitch` and `frame_size` values from the matching
`Cam<serial>_preenc_ref.json`; do not assume pitch equals width.

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

Before Phase 1 is treated as meaningful, add or run a decoded-frame validity
check. The earlier free-run headless run showed that a run can pass throughput
while one camera encodes black frames. The minimum sanity gate should decode at
least one representative frame per output video and record/fail on extremely
low luminance entropy or near-zero spatial standard deviation.

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

The target validation shape for Phase 2 is the now-validated two-camera PTP
load, not free-run:

- config folder equivalent to `100_cam4_ptp`,
- schema-4 AQ off and temporal AQ off,
- `sync_mode = "ptp_gate"`,
- full-frame videos present and valid,
- both cameras near real-content `150 Mbps`,
- no camera frame-ID gaps or encode failures,
- YOLO `cpu_pre_sync_ms` and `capture_to_detect_done_ms p95` compared against
  the GUI/headless PTP in-process baseline.

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
  complexity? Answer from the one-camera capped smoke: yes for host/runtime
  lock isolation, but same-GPU hardware completion pressure remains.
- How much additional memory bandwidth does recorder-owned detach copying add
  at `100 fps` split-GOP scale?
- Should encoder shards be one process per GPU, one process per camera, or one
  supervisor process with one CUDA context per encode GPU?

## Implementation Checklist

Status update (2026-05-04): Phase 0 and Phase 1 diagnostics succeeded. The
diagnostic external recorder now has metadata, MP4 output, multi-shard
split-GOP routing, GOP-ordered merged output, prewarm, and two-camera PTP smoke
coverage. The remaining work is to convert that diagnostic harness into a
production/session-managed recorder and GUI backend.

Phase 0:

- [x] Add or reuse a headless analytics mode with full-frame recording disabled
      and YOLO enabled.
- [x] Add an external NVENC load/stress process at `4512x4512 @ 60 fps`.
- [x] Add `scripts/run_process_isolation_discriminator.sh`.
- [ ] Attach Nsight to the analytics process first.
- [x] Summarize YOLO timing p95 and recorder NVENC p95 from CSV artifacts.
- [x] Run same-GPU external NVENC load.
- [x] Run separate-GPU external NVENC load after same-GPU result is known.
- [x] Validate two-camera headless PTP real-YOLO plus real split-GOP baseline.
- [x] Validate two-camera GUI PTP/AQ-off real split-GOP baseline.
- [x] Add decoded-frame entropy / black-frame sanity check to benchmark
      validation.
- [x] Run same-GPU external real-frame NVENC load from cached
      `pre_encoder_reference_capture` frames.

Phase 1:

- [ ] Define versioned production recorder descriptor protocol.
      Diagnostic descriptors already carry session/routing metadata, but the
      protocol still lacks explicit versioning, health, heartbeat,
      stop/drain/finalize, and production failure semantics.
- [x] Prototype CUDA IPC memory import for one GPU.
- [x] Add detach ack path and source-lease timeout policy.
- [x] Encode one camera at sustainable single-session FPS.
- [x] Write recorder timing artifact.
- [x] Verify analytics p95 against Phase 0 for same-GPU and paired-GPU capped
      single-session runs.

Phase 2:

- [x] Add diagnostic split-GOP routing to the external recorder probe.
- [x] Add diagnostic encoder shard lifecycle and per-shard timing.
- [x] Preserve GOP/order output in the diagnostic merged MP4 coordinator.
- [x] Reproduce full-rate `100 fps` two-camera PTP recording in the headless
      external-recorder diagnostic harness.
- [x] Compare YOLO p95 to no-full-frame, same-process, and Phase 1 results.
- [ ] Move diagnostic routing into production/session recorder supervision.
- [ ] Add production failure handling around shard startup, heartbeat, drain,
      finalization, and artifact publication.

Phase 3:

- [ ] Add GUI backend selection.
- [ ] Run GUI smoke with external recorder.
- [ ] Decide whether external recording becomes default.
