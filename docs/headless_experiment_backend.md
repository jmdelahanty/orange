# Headless Experiment Backend

Date: 2026-04-01
Scope: current headless recording implementation, how to use it for benchmark
experiments, and whether the current worker/fanout strategy is the right
architecture to build on.

See also:

- `docs/experiment_runner_plan.md`
- `docs/headless_cli_design.md`
- `docs/nvenc_benchmark_runsheet.md`
- `docs/nvenc_throughput_todo.md`
- `src/orange_headless_client.cpp`
- `src/acquire_frames.cpp`
- `src/modern_recording_pipeline.cpp`

## Current Status

The headless client now uses the same modern recording workers as the GUI
recording path:

- `ModernRecordingPipeline`
- `EncoderPreprocessWorker`
- `EncoderHwWorker`
- `acquire_frames(...)`

That is a meaningful improvement because it removes the old backend mismatch
between GUI recording and headless recording.

The current headless startup path is:

1. open cameras
2. allocate EVT frame buffers
3. write `recording_snapshot.json` and initialize the PTP summary
4. create one `CameraResources` pool per camera
5. create one `ModernRecordingPipeline` per camera
6. force a record-only camera selection:
   - `stream_on = false`
   - `record = true`
   - `yolo = false`
   - `crop_and_encode = false`
   - `send_frame_ipc = false`
7. launch one `acquire_frames(...)` thread per camera

That means headless experiments now run through the same preprocess and hardware
encode path as the main app, and they should produce the same recording-path
artifacts:

- `recording_snapshot.json`
- `Cam*_pipeline_perf.csv`
- encoded video + metadata sidecars

## What This Is Good For Right Now

The current headless implementation is a good base for:

- record-only throughput experiments,
- single-host benchmark automation,
- remote runs where the main goal is to exercise the same encode path as GUI,
- artifact-driven evaluation using the existing pipeline CSV and snapshot
  outputs.

For benchmarking, the intended operating mode is narrow:

- display off,
- YOLO off,
- record on,
- explicit camera serial selection instead of "first available camera",
- native recording output geometry unless the remote control contract is
  extended,
- one run per recording folder.

Current temporary selection syntax:

- `camera=all` or omit camera selection entirely to use every camera visible to
  that headless client.
- `camera=02010093` to run a single serial.
- `camera=02010093+02010094` or repeated `camera=...` tokens to run a subset.

Today this selection is parsed from the loose `encoder_basic_setup` string used
by the remote control contract. That is a stopgap until the structured
experiment spec / runner replaces it.

This is the right shape for throughput testing because it minimizes unrelated
consumers and keeps the benchmark focused on acquisition -> preprocess ->
encode.

For experiment artifacts, each run should also preserve GPU identity in a human
readable way. Numeric `gpu_id` alone is not enough when later comparing runs
across machines. The snapshot should make it possible to tell that, for
example, `gpu_id = 0` mapped to `NVIDIA RTX A6000` on a given host.

## Important Current Limitation

The previous stop/drain mismatch on the modern headless path has now been
closed: `acquire_frames(...)` once again participates in the PTP stop barrier
and signals `ptp_stop_reached` so the headless manager can drain cleanly.

So the current state is:

- compile-time parity is good,
- artifact parity is good enough for local and single-host automated runs,
- repeated remote start/stop automation is now plausible on the modern path,
  but still worth validating with a few controlled runs before treating it as a
  hardened distributed benchmark harness.

That means the main remaining gaps are no longer basic stop coordination. They
are experiment-runner control-plane work, explicit camera selection, and better
run manifest/reporting.

## Critical Assessment Of The Worker / Fanout Strategy

Short version:

- for the shipping app, the worker/fanout model is still a reasonable design,
- for a record-only experiment harness, it is broader than necessary,
- and the next abstraction step should narrow the control plane rather than add
  more logic to the fanout core.

### What Is Good About It

The current strategy has several real strengths.

- It preserves backend parity between GUI and headless. That is the most
  important architectural win for benchmarking.
- The fanout model lets one acquired frame feed display, recording, and YOLO
  without copying by default when the source can be shared safely.
- The record-only headless mode now naturally collapses to a simple case:
  `dispatch_count == 1`, recording only. That means the generic system still
  reaches its simplest runtime behavior for the benchmark case.
- When GPU-direct is available and there is only one consumer, the current
  acquisition logic can still use the direct-pointer fast path instead of the
  ring-copy path.
- The artifact contract is already tied to this path, so reusing it avoids a
  second benchmark-only backend.

So the high-level model does not need a wholesale rethink before experiments.

### What Is Weak About It

The current implementation also carries real costs.

- `acquire_frames(...)` is a general multi-consumer router, not a focused
  record-only path. That makes the experiment backend harder to reason about
  than it needs to be.
- The hot path still reserves and returns YOLO event resources even when YOLO is
  disabled. For record-only experiments that is unnecessary work and small but
  real measurement noise.
- Lifetime management is manual:
  - intrusive `ref_count`
  - recycle queues
  - separate event pools
  - GPU-direct requeue handling
  This is performant, but it is brittle and easy to get wrong.
- The lifecycle is still duplicated between GUI and headless. `ModernRecordingPipeline`
  extracted the per-camera recording worker pair, but there is still no true
  `RecordingSession` abstraction owning start / stop / drain / join for the
  whole run.
- `CameraControl` is still a shared mutable control block for all cameras on the
  process. That is workable for the app, but it is a weak control-plane API for
  experiment automation.
- The remote control contract still passes `encoder_basic_setup` as a loose
  string. That is good enough for compatibility, but it is not a solid basis for
  experiments because it cannot cleanly carry:
  - output geometry,
  - experiment ids,
  - run ids,
  - policy flags,
  - feature toggles,
  - future direct-input mode selection.

### Bottom Line

The worker/fanout architecture is still good enough to keep as the shared data
plane.

What should be rethought is not the existence of workers, but the abstraction
boundary above them.

The next step should be:

- keep the current worker graph as the shipping hot path,
- stop building more experiment logic directly into GUI/headless startup code,
- add a real `RecordingSession` control-plane abstraction,
- and add a narrower record-only fast path, or at minimum skip unused fanout
  bookkeeping when display / YOLO / image-save are disabled.

## Recommendation For Experiments

Use the current headless backend for experiments only under these assumptions:

- record-only mode,
- display and YOLO disabled,
- local or tightly supervised remote runs,
- stable artifact collection via `recording_snapshot.json` and
  `Cam*_pipeline_perf.csv`,
- benchmark evaluation using the existing run sheet and plotting tools.

Do not yet treat the headless path as the final experiment orchestration layer.

The correct near-term sequence is:

1. fix modern headless stop/drain coordination,
2. extract `RecordingSession`,
3. move single-host experiment automation onto that abstraction,
4. only then revisit distributed orchestration.

## Recommended Follow-Up Tasks

1. Port the legacy remote stop semantics onto the modern acquisition path so
   repeated remote start/stop runs are trustworthy.
2. Extract a real `RecordingSession` abstraction so GUI and headless stop
   duplicating lifecycle code.
3. Replace the loose `encoder_basic_setup` string with a structured run config.
4. Refactor per-frame resource reservation so mandatory resources are always
   acquired, but consumer-specific resources are only acquired when that
   consumer is enabled for the frame.
5. Add a record-only acquisition fast path, or at minimum skip YOLO-event
   handling when YOLO is disabled.
6. Build the experiment runner on top of that narrower session abstraction, not
   directly on GUI/headless code.
