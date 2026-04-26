# External Recorder Implementation Plan

Date: 2026-04-26

Scope: turn the successful external IPC/NVENC prototype into a production
recording architecture that keeps full-frame encode and bitstream harvest out
of the analytics process.

## Bottom Line

The prototype result is strong enough to continue.

The architecture target is:

```text
Orange analytics process
  acquisition
  YOLO / crop / pose
  fast frame descriptor publication
  source lease held only until detach ACK
  no full-frame NVENC APIs

External recorder process
  CUDA IPC import
  recorder-owned detach copy
  detach ACK
  NVENC preprocess / encode / harvest
  mux / output / metadata
```

The first external encode smoke showed the important behavior:

- External NVENC did not reintroduce the old `8-10 ms` YOLO CPU-side
  launch/preprocess tail.
- YOLO `cpu_preprocess_ms p95` stayed about `0.0149 ms`.
- YOLO `cpu_pre_sync_ms p95` stayed about `0.0914 ms`.
- `capture_to_detect_done_ms p95` rose to about `4.59 ms`, but the increase
  landed in same-GPU completion pressure, not CPU API orchestration.

That means process isolation is solving the right class of problem. It does
not make shared GPU/NVENC/memory resources free, so GPU placement and split-GOP
routing remain first-class design choices.

## Non-Negotiable Boundary

Detach ACK is the critical contract.

```text
Before ACK:
  Orange must keep the source frame lease alive.

After ACK:
  Orange may recycle the source frame.
  External recorder owns a copy and may encode/harvest/write independently.
```

ACK must mean "the recorder owns a safe copy", not "encoding finished".

`nvEncEncodePicture`, `nvEncLockBitstream`, muxing, and disk output must stay
behind the process boundary and after the detach ACK. If ACK waits for encode
completion, the design recreates the latency coupling we are trying to remove.

## Current Prototype

Implemented:

- `recording_sink_mode = "external_ipc"` in the analytics process.
- Unix-domain socket frame descriptors.
- CUDA IPC memory handle export/import.
- Recorder-owned device detach copy.
- ACK-gated source lease recycle.
- External-process optional NVENC encode with:
  - `--encode`
  - `--encode-max-fps`
  - `--encode-csv`
  - `--bitstream-out`
- Headless artifact counters:
  - `external_ipc_frames_acked_final`
  - `external_ipc_failures_final`
  - `external_ipc_ack_timeouts_final`

Limitations:

- `external_recorder_ipc_probe` is still a diagnostic tool, not a recorder
  backend.
- Output is a raw elementary stream, not MP4.
- The first encode smoke encoded a capped subset at `60 fps`.
- No recorder-side session metadata or robust shutdown protocol exists yet.
- No split-GOP/multi-GPU routing exists outside the analytics process yet.

## Stage 1: Production-Like Single-Camera External Recorder

Goal: turn the probe into a repeatable single-camera external recording test
with valid video output.

Implementation tasks:

1. Keep `external_recorder_ipc_probe` as the low-level diagnostic binary.
2. Add a new production-ish binary or mode, tentatively `external_recorder`.
3. Preserve the same descriptor, detach copy, and ACK contract.
4. Add MP4 mux output instead of raw `.hevc`.
5. Reuse existing writer/output code where clean, but do not pull the full
   in-process worker stack into the recorder if it obscures the isolation
   boundary.
6. Write recorder-side artifacts:
   - detach timing CSV
   - encode timing CSV
   - output summary JSON
   - dropped/skipped/encoded frame counts
   - codec/preset/GOP/bitrate config
7. Add clean shutdown:
   - stop accepting descriptors
   - drain encoder queue
   - flush NVENC
   - finalize MP4
   - report final counters

Acceptance gate:

- Single camera `2010096`.
- Same GPU first, A16 GPU `5`.
- External encode capped at `60 fps`.
- Real headless YOLO enabled.
- `recording_sink_mode = "external_ipc"`.
- Valid MP4 produced and decoded-frame sanity check passes.
- `external_ipc_failures_final = 0`.
- `external_ipc_ack_timeouts_final = 0`.
- External encode queue drops are zero.
- YOLO CPU-side p95 stays near the fast path:
  - `cpu_preprocess_ms p95 < 0.2 ms`
  - `cpu_pre_sync_ms p95 < 0.5 ms`

## Stage 2: Repeatable Runner

Goal: make the architecture test a single command so results are reproducible.

Add a script that:

1. Starts the external recorder.
2. Waits for the Unix socket.
3. Runs `orange_client` through the existing `sudo -n` benchmark wrapper.
4. Stops/drains the recorder.
5. Collects:
   - Orange recording folder
   - `Cam*_pipeline_perf.csv`
   - `Cam*_yolo_perf.csv`
   - recorder detach CSV
   - recorder encode CSV
   - recorder output video
   - recorder summary JSON
6. Prints a compact summary:
   - ACK failures/timeouts
   - external encode drops
   - output bitrate
   - YOLO `cpu_preprocess_ms p95`
   - YOLO `cpu_pre_sync_ms p95`
   - YOLO `capture_to_detect_done_ms p95`

This runner should become the default way to compare:

```text
preprocess_only baseline
external_ipc detach-only
external_ipc external encode
in-process full-frame recording
```

Acceptance gate:

- The same command can be rerun without manually cleaning stale sockets.
- Failed recorder startup fails the experiment clearly.
- Missing ACKs, recorder drops, or invalid video fail the summary.

## Stage 3: Protocol And Configuration Hardening

Goal: stop relying on ad hoc CLI flags once the single-camera path works.

Protocol additions:

- protocol version
- run/session id
- camera serial
- recording profile id
- source dimensions and pitch
- output dimensions and pitch
- pixel format
- source GPU id
- requested encode GPU id
- frame timestamp fields
- clean `STOP` / `DRAIN` / `FINALIZE` messages
- recorder health/heartbeat

Config additions:

- recorder mode: `single_session` or `split_gop`
- socket path policy
- encode GPU policy
- max detach ACK latency
- queue depths
- target FPS or frame selection policy
- codec/preset/tuning/GOP/rate control
- output directory and filename policy

Recorder state should be written into `recording_snapshot.json` or a linked
recorder summary artifact so the analytics result and recorder result can be
joined without terminal logs.

Acceptance gate:

- The experiment artifacts are self-describing.
- A run can be interpreted without knowing which terminal command launched the
  recorder.

## Stage 4: Two-Camera Single-Session Diagnostics

Goal: test process isolation under two-camera acquisition while intentionally
not claiming production `100 fps` full-frame recording yet.

Use this stage to answer:

- Does the external recorder stay healthy while two camera acquisition and YOLO
  run?
- Does one camera starve the other at the detach socket/queue layer?
- Does same-GPU versus different-GPU placement change YOLO p95 as expected?

Expected constraints:

- One NVENC session still cannot sustain both `4512x4512 @ 100 fps` streams.
- Use frame selection/capped FPS for this stage.
- Treat this as a scheduling and isolation test, not the production throughput
  test.

Acceptance gate:

- Both cameras sustain acquisition with zero frame-id gaps.
- ACK failures/timeouts are zero.
- Recorder frame selection is explicit and visible in artifacts.
- YOLO CPU-side p95 does not return to the old same-process tail.

## Stage 5: External Split-GOP Recorder

Goal: preserve the required `100 fps 20MP` full-frame throughput by moving the
existing split-GOP idea across the process boundary.

Architecture:

```text
Orange analytics process
  -> recorder supervisor
       -> encoder shard on GPU A
       -> encoder shard on GPU B
       -> optional additional shards
       -> output/order coordinator
```

Responsibilities:

- Orange publishes frame descriptors and waits only for detach ACK.
- Recorder supervisor owns route policy.
- Encoder shards own CUDA contexts, NVENC sessions, and bitstream harvest.
- Output coordinator preserves GOP/order semantics and finalizes video files.
- Failures are reported back as recorder health, not as analytics thread
  blocking work.

Important design point:

Production recording cannot rely on one encoder for `4512x4512 @ 100 fps`.
External process isolation removes same-process runtime-lock coupling, but it
does not remove NVENC throughput limits. Split-GOP/multi-GPU routing remains
required.

Acceptance gate:

- Two-camera PTP config equivalent to `100_cam4_ptp`.
- Full-frame videos present and valid for both cameras.
- Both cameras near `100 fps`.
- Camera frame-id gaps are zero.
- External ACK failures/timeouts are zero.
- External encode failures are zero.
- YOLO p95 materially beats the current in-process GUI/headless PTP baseline
  around `11-12 ms`.

## Stage 6: GUI Integration

Goal: expose the backend without weakening the validated headless path.

Implementation tasks:

- Add backend selection:
  - `in_process`
  - `external`
  - `disabled`
- Start/monitor external recorder from the recording session layer.
- Surface recorder health in GUI status.
- Fail visibly on recorder startup, ACK timeout, or output finalization errors.
- Keep `in_process` available as fallback during rollout.

Acceptance gate:

- GUI two-camera PTP external split-GOP run is healthy.
- No camera drops.
- No recorder drops.
- Valid videos.
- Crop/pose artifacts still valid when a detectable subject is present.
- YOLO latency remains close to the headless external result.

## Near-Term Next Slice

The highest-signal next coding task is Stage 1 plus Stage 2:

```text
external recorder with valid MP4 output
single-command runner
single-camera same-GPU 60 fps validation
```

This turns the successful low-level prototype into a repeatable architecture
test. Only after that should we invest heavily in external split-GOP routing.
