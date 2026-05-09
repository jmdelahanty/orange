# Headless Experiment Backend

Date: 2026-04-01
Scope: current headless recording implementation, how to use it for benchmark
experiments, and whether the current worker/fanout strategy is the right
architecture to build on.

See also:

- `docs/experiment_runner_plan.md`
- `docs/headless_cli_design.md`
- `docs/headless_codec_quality_test_handoff.md`
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
2. create the run folder and initialize per-run artifacts when a run folder is
   provided:
   - `recording_snapshot.json`
   - `ptp_sync_summary.json`
   - GPU monitoring metadata
3. allocate EVT frame buffers
4. create one `CameraResources` pool per camera
5. create one `ModernRecordingPipeline` per camera when recording is enabled
6. force a record-only camera selection when recording is enabled:
   - `stream_on = false`
   - `record = true`
   - `yolo = false`
   - `crop_and_encode = false`
   - `send_frame_ipc = false` by default, or `true` when explicit headless
     frame IPC is enabled
7. launch one `acquire_frames(...)` thread per camera

That means headless experiments now run through the same preprocess and hardware
encode path as the main app, and they should produce the same recording-path
artifacts:

- `recording_snapshot.json`
- `Cam*_pipeline_perf.csv`
- encoded video + metadata sidecars

Current invocation modes:

- local single-run CLI:
  - `orange_client --mode local --record-folder /abs/path/to/run --camera 02010093 --codec hevc --preset p1 --tuning ll`
- local discovery:
  - `orange_client --mode local --list-cameras`
- remote/network-controlled mode:
  - `orange_client --mode remote`

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

Local CLI note:

- local single-run mode now accepts direct flags instead of requiring
  `encoder_basic_setup`
- local mode now also accepts `--experiment-spec <path>` for single-host matrix
  runs
- the first experiment-spec implementation is intentionally constrained to:
  - `display=false`
  - `yolo=false`
  - explicit local runs only, not remote orchestration

Experiment specs now support two useful fixed-mode toggles:

- `fixed.sync_mode = "free_run" | "ptp_gate"`
- `fixed.stream_only = true | false`
- `fixed.acquisition_buffer_mode = "auto" | "force_ring_copy"`
- `fixed.recording_sink_mode = "real" | "preprocess_only" | "immediate_recycle" | "threaded_handoff_only" | "external_ipc"`
- `fixed.helper_noop_source_read = true | false`
- `fixed.helper_copy_bytes = -1 | 0 | <positive byte count>`
- `fixed.helper_copy_delay_ns = 0 | <positive nanoseconds>`
- `fixed.frame_ipc.enabled = true | false`
- `fixed.frame_ipc.mode = "producer_only" | "verify_drain"`
- `fixed.pose_worker.mode = "off" | "noop" | "real"`
- `fixed.recording_control.record_for_seconds = 0 | <positive seconds>`
- `fixed.recording_control.clip_seconds = 0 | <positive seconds>`
- `fixed.external_recorder_contract.mode = "off" | "diagnostic_ipc_v1"`

`fixed.stream_only = true` keeps the experiment runner in acquisition-only mode
for that run:

- no `ModernRecordingPipeline` is created
- no video output is expected
- the run folder is still created
- the run still writes:
  - `recording_snapshot.json`
  - `ptp_sync_summary.json`
  - `Cam*_pipeline_perf.csv`
  - `nvidia_smi_dmon.csv`
- stream-only runs do not update the shared `latest_recording.json` pointers
- `runs.csv` evaluates `acq_fps_mean` / `acq_fps_p95` instead of requiring an
  encoder snapshot

That gives us a documented “stream-only experiment spec” mode instead of having
to drop down to the ad hoc direct CLI.

`fixed.recording_control` is an experimental recording-duration control for
headless runs. It lets the stream/run continue after the recorder has been
stopped and drained:

```json
"fixed": {
  "duration_s": 20,
  "recording_control": {
    "record_for_seconds": 6,
    "clip_seconds": 0
  }
}
```

Current behavior:

- recording is armed at the normal headless recording start time
- for `fixed.recording_control`, the duration clock starts when the first
  `recording_frame_id` is observed, so a PTP gate startup countdown does not
  consume requested recording time before frames arrive
- after `record_for_seconds`, the runner requests the same explicit recording
  drain used by the GUI stop-recording path
- acquisition continues until the overall `duration_s + warmup_s` run deadline
- the recording folder gets `recording_session.json` with the requested
  duration, actual start/stop/drain timing, per-camera artifact paths, and a
  single `clip_0000` entry
- pass/fail for this mode checks the video exists, video duration is close to
  `record_for_seconds`, acquisition stayed healthy, and error/drop counters
  remain within policy
- `scripts/verify_timed_recording.py <experiment_root>` verifies the current
  timed-recording contract from `recording_session.json`, per-camera clip
  artifacts, `runs.json`, and `ffprobe`

`clip_seconds > 0` enables the headless-only rolling-clip path. The current
implementation uses seamless GOP-boundary writer switching:

- clip folders are written under `clips/clip_000000`,
  `clips/clip_000001`, etc.
- each clip writes `clip_manifest.json`, `Cam<serial>.mp4`,
  `Cam<serial>_meta.csv`, and `Cam<serial>_keyframe.json`
- the parent folder writes `recording_session.json` with
  `mode = "rolling_clips"`
- `recording_frame_id` remains continuous across clip metadata
- MP4 timestamps are clip-local and start at zero for each clip
- the next clip writer is preopened before the switch
- the active writer switches at a GOP first-frame boundary
- each new clip starts with an IDR/SPS/PPS picture and keyframe frame `0`
- the manifest records `rollover.implementation =
  "headless_gop_boundary_writer_switch"`, `seamless_writer_switch = true`,
  `records_during_rollover = true`, and `next_writer_preopened = true`
- GOP-boundary alignment can move individual clip durations by up to one GOP;
  use the total ffprobe duration and continuous `recording_frame_id` coverage
  for the recording-duration check

The shared session and clip manifest contract is documented in
`docs/recording_session_manifest_contract.md`. The manifest builder and
rolling-clip validation live in `src/session/recording_session.*` so later
GUI/session and external-recorder paths can share the same contract.

Validated smoke:

- spec:
  `experiment_specs/2010096_headless_timed_recording_control_smoke_a16_gpu5.json`
- artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_timed_recording_control_smoke_a16_gpu5`
- stream requested `20 s`, recording requested `6 s`
- `recording_session.json` reported `status = completed`,
  `stop_reason = record_for_seconds_elapsed`, `drain_completed = true`, and
  `actual_recording_duration_s = 6.005`
- `ffprobe` reported `Cam2010096.mp4 duration = 6.000 s`
- `summary.json` reported `pass_runs = 1`, `fail_runs = 0`
- camera result reported `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, and `0` encode failures

Validated rolling smoke:

- spec:
  `experiment_specs/2010096_headless_rolling_clip_smoke_a16_gpu5.json`
- latest artifact:
  `/tmp/orange_seamless_rolling_bt1/2010096_headless_seamless_rolling_clip_smoke_bt1`
- stream requested `24 s`, recording requested `18 s`, clip interval `6 s`
- `recording_session.json` reported `mode = "rolling_clips"`,
  `status = completed`, `stop_reason = record_for_seconds_elapsed`, and
  `drain_completed = true`
- ffprobe reported three clips totaling `18.000 s`
- all clip keyframe sidecars start with keyframe frame `0`; rollover forces
  the first NVENC picture in each clip to IDR with SPS/PPS
- `summary.json` reported `pass_runs = 1`, `fail_runs = 0`
- camera result reported `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, and `0` encode failures
- `scripts/verify_timed_recording.py` passed with total ffprobe duration
  `18.000 s` for a requested `18.000 s`

Longer seamless rolling validation:

- artifact:
  `/tmp/orange_seamless_rolling_long_bt2/2010096_headless_seamless_rolling_clip_long_bt2`
- stream requested `42 s`, recording requested `36 s`, clip interval `6 s`
- six clip folders, continuous frames `1-3600`, total ffprobe duration
  `36.000 s`
- `summary.json` reported `pass_runs = 1`, `fail_runs = 0`
- camera result reported `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, and `0` encode failures
- `scripts/verify_timed_recording.py` passed

Two-camera PTP seamless rolling validation:

- spec:
  `experiment_specs/2010095_2010096_headless_ptp_rolling_clip_smoke_a16.json`
- artifact:
  `/tmp/orange_two_camera_ptp_rolling_bt2/2010095_2010096_headless_ptp_seamless_rolling_bt2`
- stream requested `24 s` plus `2 s` warmup, recording requested `18 s`, clip
  interval `6 s`, `ptp_register_read_decimate = 100`
- `recording_session.json` anchored `recording.started_at_elapsed_s` at first
  recorded frame after the PTP gate countdown, then stopped at
  `18.006 s` of recording-clock time
- both cameras wrote three clip folders with continuous frames `1-1801`, total
  ffprobe duration `18.010 s`, and `0` per-clip metadata gaps
- `summary.json` reported `pass_runs = 1`, `fail_runs = 0`
- camera result reported `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, `0` encode failures, and video-content status `pass`
- steady YOLO `acquisition_to_detect_done_ms` p95 after frame 50 was
  `3.655 ms` for `2010095` and `3.927 ms` for `2010096`
- `scripts/verify_timed_recording.py` passed for both cameras

`fixed.frame_ipc` is an explicit testability knob for the same shared-memory
frame IPC path used by the GUI:

- queue names are always serial-based: `/shm_cam_<camera_serial>`
- `producer_only` creates Orange `FrameIPCManager` writers and expects an
  external consumer, such as Citrus, to drain the queues
- `verify_drain` also starts a built-in headless reader per selected camera and
  writes `frame_ipc_summary.json`
- `verify_drain` is single-consumer test mode; do not run it while Citrus is
  expected to consume the same queues
- `unlink_existing_queues=true` removes stale `/dev/shm/shm_cam_<serial>`
  objects before creating writers

Example:

```json
"fixed": {
  "frame_ipc": {
    "enabled": true,
    "mode": "verify_drain",
    "unlink_existing_queues": true,
    "require_base_frames": true,
    "allow_push_failures": false
  }
}
```

Validated smoke:

- Date: 2026-04-21
- Spec template:
  `experiment_specs/2010096_frame_ipc_verify_stream_only_a16_gpu5.json`
- Successful retry spec:
  `/tmp/2010096_frame_ipc_verify_stream_only_a16_gpu5_retry.json`
- Command:

  ```bash
  sudo -n /usr/local/bin/orange-local-benchmark \
    --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
    /tmp/2010096_frame_ipc_verify_stream_only_a16_gpu5_retry.json
```

`fixed.recording_sink_mode = "external_ipc"` is the first process-isolated
recorder detach prototype:

- acquisition/YOLO stay in the headless analytics process,
- the recording path exports an owned CUDA source buffer over a Unix socket,
- an external recorder probe imports the CUDA IPC memory handle and copies the
  frame into recorder-owned device memory,
- the analytics process keeps the source lease until the probe sends a detach
  ACK,
- the diagnostic recorder can then encode detached frames and write MP4/CSV
  artifacts outside the analytics process.

The external IPC path writes these pipeline CSV columns and corresponding
`runs.json` / `runs.csv` fields:

- `external_ipc_frames_acked`
- `external_ipc_failures`
- `external_ipc_ack_timeouts`

For `external_ipc` experiment specs, nonzero failures/timeouts or fewer ACKed
frames than submitted frames fail the run even though the mode is otherwise
metrics-only.

`fixed.external_recorder_contract` is the spec-level contract for the external
recorder artifacts produced outside the analytics process. Orange validates the
contract shape and records the expected per-camera summary/video/routing paths
in `runs.json` / `runs.csv`; `scripts/verify_external_recorder_session.py`
validates those artifacts after the recorder process finalizes. The schema is
documented in `docs/external_recorder_contract.md`.

The external IPC contract now also carries the session `recording_control`
intent and a `rollover` object. Timed single-video external IPC runs can use
`record_for_seconds > 0` with `clip_seconds = 0`. External IPC rolling clips
are intentionally rejected for now: `fixed.recording_sink_mode = "external_ipc"`
with `fixed.recording_control.clip_seconds > 0` fails before camera start with
`external recorder rolling clips are not implemented yet; use in-process
recording for rolling clips or external_ipc with clip_seconds=0`. This keeps
the headless supervised path from pretending to produce seamless external
rollover before the recorder owns GOP-boundary writer switching.

For control-plane checks that should not touch cameras, TensorRT, sockets, or
NVENC, use the dry-run supervisor-plan CLI:

```bash
./targets/release/external_recorder_supervisor_plan \
  --spec experiment_specs/2010095_2010096_external_recorder_supervisor_plan_ptp.json \
  --check
```

That tool expands `fixed.external_recorder_contract` into per-stream recorder
argv arrays and fails fast on invalid sink mode, missing selected streams,
invalid shard routing, or missing artifact paths.

For the opt-in supervised path, set
`fixed.external_recorder_contract.supervise_processes = true`. In that mode
`orange_client` starts one recorder process per contract stream, waits for each
Unix socket before starting camera acquisition, exports per-camera socket and
session env vars for the handoff worker, waits for recorder finalization after
analytics shutdown, and writes supervisor session/plan/runtime JSON under the
external recorder artifact root. It also writes
`external_recorder_verifier_handoff.json`, which records the verifier command to
run after `runs.json` and any required video-sanity artifact exist.

In the supervised path, the headless client now runs that finalization itself.
It writes a provisional `runs.json`, runs `scripts/external_video_sanity.py` for
each required stream MP4, then runs
`scripts/verify_external_recorder_session.py`. The results are recorded in
`external_recorder_finalization.json`; any failed decode sanity or verifier
check fails the run with the concrete reason in `runs.json`.

The first supervised smoke spec is:

```text
experiment_specs/2010096_headless_real_yolo_external_ipc_supervised_encode_smoke.json
```

The two-camera PTP supervised smoke spec is:

```text
experiment_specs/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_supervised.json
```

Validated supervised smokes:

- One-camera artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_external_ipc_supervised_encode_smoke`
  with external recorder root
  `/tmp/orange_external_recorder_supervised_2010096`. The run passed
  `external_video_sanity.py` and `verify_external_recorder_session.py`;
  recorder summary reported `800` received/ACKed frames, `480` encoded frames
  at the diagnostic `60 fps` cap, and `0` encode drops.
- Two-camera PTP artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_supervised`
  with external recorder root `/tmp/orange_external_recorder_supervised_ptp`.
  The run passed per-camera video sanity and session verification; each camera
  received/ACKed/encoded `400` external frames with `0` drops, two shards, and
  `gop_modulo` routing.
- In the two-camera PTP run, steady-state post-frame-50
  `acquisition_to_detect_done_ms p95` was `4.594 ms` for `2010095` and
  `4.645 ms` for `2010096`; YOLO queue wait p95 stayed at or below
  `0.018 ms`.

- Artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_frame_ipc_verify_stream_only_a16_gpu5_retry/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25/frame_ipc_summary.json`
- Result:
  - queue: `/shm_cam_2010096`
  - `frames_sent = 901`
  - `reader_messages_popped = 901`
  - `reader_frame_id_gaps = 0`
  - `ipc_push_failures = 0`
  - run row `pass`

Headless YOLO audit-log test mode:

- `fixed.yolo_event_log.mode = "synthetic"` generates deterministic
  `Cam<serial>_yolo_events.jsonl` rows without creating a real TensorRT YOLO
  worker.
- In `runs.json` / `runs.csv`, `yolo=false` still means no real TensorRT YOLO
  worker ran. Synthetic audit coverage is reported separately through
  `yolo_event_log_mode`, `yolo_event_log_status`, and the
  `yolo_event_log_*` counters.
- The experiment summary validates file presence, parseability, event sequence,
  detection cadence, zero-detection rows, and joins to `Cam<serial>_meta.csv`.
- Synthetic rows are audit-only for now; they do not call
  `FrameIPCManager::updateFrameWithDetections(...)`.
- See [headless_synthetic_yolo_event_log_plan.md](./headless_synthetic_yolo_event_log_plan.md).
- See [headless_real_yolo_worker_plan.md](./headless_real_yolo_worker_plan.md)
  for the future opt-in TensorRT YOLO worker path.

Headless pose worker schema:

- `fixed.pose_worker.mode = "off"` is the default and does not request any
  pose artifacts.
- `fixed.pose_worker.mode = "noop"` is the first schema/validation mode. It
  expects `Cam<serial>_pose_events.jsonl` rows with
  `pose.backend = "noop"`, `pose.mode = "noop"`, `pose.status = "no_result"`,
  `pose.instance_count = 0`, and an empty `poses` array.
- `fixed.pose_worker.mode = "real"` reserves the future TensorRT keypoint
  path. In this mode, `engine_path` is required by the schema, but the runtime
  inference path is not implemented yet.
- Headless pose currently requires `fixed.yolo_worker.mode = "real"` because
  `roi_source = "yolo_top_detection"` is the only supported ROI source.
- `stream_only = true` is rejected when pose is enabled because pose validation
  is recording-artifact based.
- This slice is intentionally validation-first: if `mode = "noop"` is enabled
  but the headless runtime does not produce `Cam<serial>_pose_events.jsonl`,
  `runs.json` / `runs.csv` mark the camera row as failed with
  `pose event log validation failed`.

Example:

```json
"fixed": {
  "yolo_worker": {
    "mode": "real",
    "engine_path": "/home/jeremy/orange_data/detect/model.engine",
    "decimate": 1,
    "publish_live_ipc": false,
    "timeout_ms": 500,
    "fail_on_init_error": true
  },
  "pose_worker": {
    "mode": "noop",
    "skeleton_id": "fish_v1",
    "input_width": 256,
    "input_height": 256,
    "input_layout": "nchw",
    "input_dtype": "fp16",
    "normalization": "model_default",
    "roi_source": "yolo_top_detection",
    "queue_depth": 32,
    "timeout_ms": 500,
    "prewarm_iterations": 0,
    "fail_on_init_error": true,
    "write_events_jsonl": true
  }
}
```

`fixed.recording_sink_mode` is a separate experimental diagnostic knob for
recording-enabled runs:

- `real`
  - normal recording pipeline
- `preprocess_only`
  - real preprocess workers and normal split-GOP helper routing
  - no hardware encoder or shared-output work
  - useful for separating preprocess from encode/output
- `immediate_recycle`
  - recording branch stays logically enabled, but frames are released
    immediately instead of entering preprocess / encode
- `threaded_handoff_only`
  - frames still cross a recording-side worker boundary, but no real
    preprocess / encode / output work is done

These sink modes are intended only for diagnosing the `100 fps` multi-camera
PTP recording failure mode. They intentionally produce no video output and
`runs.csv` evaluates them using acquisition metrics instead of encoder output.

`fixed.acquisition_buffer_mode` is a lower-level experimental acquisition-path
override:

- `auto`
  - current runtime default
  - direct camera GPU buffers are passed through when safe, otherwise the frame
    is copied into Orange-owned ring-buffer memory
- `force_ring_copy`
  - always copy GPUDirect camera buffers into Orange-owned ring-buffer memory
    before downstream recording work

This knob exists for diagnosing ownership / requeue / buffer-lifetime issues in
the acquisition-to-recording transition. It is intentionally headless-only and
does not belong in persisted camera config yet.

`fixed.helper_noop_source_read` and `fixed.helper_copy_bytes` are even narrower
split-GOP helper diagnostics:

- `helper_noop_source_read = true`
  - helper-routed preprocess workers release the source without reading or
    copying from it
  - primary-routed frames still use the normal preprocess path
- `helper_copy_bytes = -1`
  - default full-frame helper peer copy
- `helper_copy_bytes = 0`
  - route to the helper and run helper preprocess, but skip the source-to-helper
    peer copy payload
- `helper_copy_bytes > 0`
  - copy only that many bytes from the acquisition GPU into helper-GPU staging
    before helper preprocess
- `helper_copy_delay_ns > 0`
  - sleep in the helper preprocess worker before starting the cross-GPU helper
    peer copy
  - intended only for testing whether the helper copy is colliding with a
    sensitive acquisition receive/requeue window

These settings intentionally corrupt helper-side image content when the copy is
not the full frame. They are diagnostic-only controls for measuring whether
cross-GPU copy payload and timing perturb acquisition cadence.

Checked-in example:

- `experiment_specs/2010096_split_gop_hevc_100fps_stream_only_a16_gpu5.json`

Validated artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010096_split_gop_hevc_100fps_stream_only_a16_gpu5`

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
are mostly remote control-plane work and deeper session cleanup, not the basic
single-host runner.

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
