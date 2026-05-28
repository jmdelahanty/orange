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
  source lease held until the recorder's source-safe boundary
  no full-frame NVENC APIs

External recorder process
  CUDA IPC import
  recorder-owned detach copy or direct input
  ACK, plus RELEASE when source release is deferred
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

A longer 30-second single-camera placement comparison clarified the remaining
contention:

- Same-GPU external encode (`analytics GPU 5`, recorder/NVENC GPU `5`) kept the
  YOLO CPU launch path fast, but `capture_to_detect_done_ms p95` stayed around
  `4.59 ms`.
- Paired-GPU external encode (`analytics GPU 5`, recorder/NVENC GPU `6`) kept
  the same ACK/encode health and reduced `capture_to_detect_done_ms p95` to
  about `3.24 ms`.
- The improvement came from GPU completion timing (`infer_ms` / `sync_ms`), not
  from host queue or CPU preprocessing.

Production implication: external process isolation removes the same-process
CUDA/NVENC runtime-lock tail, while encode GPU placement controls remaining
hardware/fabric pressure. Full-rate `4512x4512 @ 100 fps` recording still
requires external split-GOP/multi-GPU routing; the paired-GPU result does not
mean one helper GPU can encode the whole stream at production rate.

## Non-Negotiable Boundary

The source-safe boundary is the critical contract.

```text
Before the source-safe boundary:
  Orange must keep the source frame lease alive.

After the source-safe boundary:
  Orange may recycle the source frame.
  External recorder has consumed or copied the source pixels.
```

In detached-copy mode, ACK is the source-safe boundary and means "the recorder
owns a safe copy", not "encoding finished". In direct-input deferred-release
mode, `ACK ... deferred_release` only means "the recorder accepted the frame";
Orange must keep the source frame leased until the recorder sends `RELEASE`.

`nvEncEncodePicture`, `nvEncLockBitstream`, muxing, and disk output must stay
behind the process boundary and after frame acceptance. If ACK waits for encode
completion, the design recreates the latency coupling we are trying to remove.

## Current Prototype

Implemented:

- `recording_sink_mode = "external_ipc"` in the analytics process.
- Unix-domain socket frame descriptors.
- CUDA IPC memory handle export/import.
- Recorder-owned device detach copy.
- ACK-gated source lease recycle for detached-copy mode.
- Deferred source release for direct-input experiments:
  - recorder sends `ACK ... deferred_release` after accepting work
  - Orange keeps the source frame leased
  - recorder sends `RELEASE` after source consumption
  - summaries report `source_releases_sent` and `source_release_failures`
- External-process optional NVENC encode with:
  - `--encode`
  - `--encode-max-fps`
  - `--encode-csv`
  - `--bitstream-out`
  - `--mp4-out`
  - `--mp4-keyframe`
  - `--summary-json`
- A single-camera smoke runner:
  - `scripts/run_external_recorder_smoke.sh`
  - writes `external_video_sanity.json`
  - fails on missing/empty MP4, decode failure, black frames, or flat frames
- A checked-in smoke spec:
  - `experiment_specs/2010096_headless_real_yolo_external_ipc_encode_smoke.json`
- Headless artifact counters:
  - `external_ipc_frames_acked_final`
  - `external_ipc_failures_final`
  - `external_ipc_ack_timeouts_final`
- Diagnostic session/routing metadata in descriptors and recorder artifacts:
  - session id
  - stream id
  - GOP index
  - frame index within GOP
  - assigned shard/GPU
  - routing policy
- Diagnostic external split-GOP routing in `external_recorder_ipc_probe`:
  - `--shard-gpu-ids`
  - one encode worker per shard GPU
  - whole-GOP modulo routing
  - per-shard encode CSV/MP4 artifacts
  - merged GOP-ordered base MP4 output
  - `external_gop_routing.csv`
- Full-rate one-camera and two-camera PTP smoke runners:
  - `scripts/run_external_recorder_smoke.sh`
  - `scripts/run_external_recorder_two_camera_ptp_smoke.sh`
- Recorder pre-listen prewarm, headless YOLO synthetic prewarm, and
  `fixed.ptp_register_read_decimate` support in the smoke harnesses.

Limitations:

- `external_recorder_ipc_probe` is still a diagnostic tool, not a recorder
  backend.
- Output naming, session metadata, routing policy, and failure semantics are
  still diagnostic rather than production recorder policy.
- The descriptor has useful routing/session fields, but there is not yet a
  versioned production protocol with explicit `STOP` / `DRAIN` / `FINALIZE`,
  heartbeat, or health messages.
- External split-GOP/multi-GPU routing exists in the diagnostic probe, and GUI
  session supervision now starts, drains, finalizes, and summarizes external
  recorder processes for single-clip `external_ipc` runs. User-visible
  heartbeat/failure reporting remains follow-up work.
- No production GUI UI selection has been added yet; in-process recording
  remains the GUI fallback/default path unless `external_ipc` is selected by
  config or environment.

## Current MP4 Smoke Result

Command:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_external_recorder_smoke.sh --duration 3 --warmup 1 --encode-fps 60 --output-dir /tmp
```

Result from `2026_04_25_212327`:

- Analytics root:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_external_ipc_encode_smoke_2010096_20260425_212327`
- Recorder artifacts:
  `/tmp/orange_external_recorder_2010096_20260425_212327`
- External recorder received and ACKed `401` frames.
- It encoded `241` frames, skipped `160` by the `60 fps` cap, and dropped `0`.
- Detach copy `p95 = 0.033864 ms`.
- External encode total `p95 = 1.685832 ms`.
- `nvEncLockBitstream p95 = 0.003807 ms`.
- MP4 output:
  `/tmp/orange_external_recorder_2010096_20260425_212327/Cam2010096_external.mp4`
- `ffprobe` saw `duration = 4.017 s` and `size = 76,638,875 bytes`.
- A decoded grayscale sample check confirmed the MP4 was not black:
  mean luma about `220`, luma stddev about `79.5`, and
  `black_fraction_lt8 = 0.0` across sampled frames.

Interpretation:

- The MP4/mux path works mechanically.
- The current runner now gates on decoded-frame content sanity, not only MP4
  container validity.
- The external-process path still keeps the YOLO CPU launch path fast during a
  same-GPU live-camera smoke.
- This is not yet a production throughput test because it is one camera and
  external encode is capped at `60 fps`.

## 30-Second GPU Placement Result

Commands:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_external_recorder_smoke.sh --duration 30 --warmup 2 --encode-fps 60 --output-dir /tmp
scripts/run_external_recorder_smoke.sh --duration 30 --warmup 2 --encode-fps 60 --recorder-gpu-id 6 --output-dir /tmp
```

Artifacts:

- Same GPU `5 -> 5`:
  `/tmp/orange_external_recorder_2010096_20260425_213750`
- Paired GPU `5 -> 6`:
  `/tmp/orange_external_recorder_2010096_20260425_213850`

Both runs:

- Camera `2010096`, analytics/YOLO on A16 GPU `5`.
- Real live camera frames, external HEVC `p1/ll`, AQ/temporal AQ/lookahead off.
- External encode capped at `60 fps`.
- `3203` descriptors received and ACKed.
- `1922` frames encoded, `1281` skipped by frame-selection policy, `0` encode
  drops.
- `0` external IPC failures/timeouts.
- `0` camera frame-id gaps and `0` get-frame errors.
- External MP4 sanity passed with `black_fraction_lt8 = 0.0`.

Post-warm p95 comparison:

| Test | Analytics GPU | Recorder/NVENC GPU | `capture_to_detect_done_ms` | `total_ms` | `infer_ms` | `sync_ms` | `encode_total_ms` | `nvEncLockBitstream_ms` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Same GPU | 5 | 5 | `4.591` | `4.560` | `4.104` | `4.463` | `0.112` | `0.0028` |
| Paired GPU | 5 | 6 | `3.245` | `3.222` | `2.718` | `3.130` | `0.126` | `0.0028` |

Interpretation:

- External process isolation prevented the old same-process `8-10 ms` YOLO
  host-side stall in both runs.
- Moving NVENC to GPU `6` materially reduced the remaining YOLO completion
  pressure on GPU `5`.
- `nvEncLockBitstream` remained tiny in both runs, so the old issue was not
  simply "NVENC takes a long time"; it was where NVENC/CUDA driver work lived
  relative to the analytics process and GPU.
- For production, route full-frame encode work through an external recorder
  process and minimize the analytics GPU's encode share, but keep split-GOP
  routing because one A16 encoder cannot carry the whole `100 fps 20MP` stream.

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

Stage 1, Stage 2, and the first Stage 5 split-GOP diagnostics now exist as a
diagnostic slice:

```text
external recorder with valid MP4 output
single-command runner
single-camera same-GPU 60 fps validation
single-camera paired-GPU 60 fps placement comparison
one-camera full-rate two-shard split-GOP validation
two-camera PTP full-rate external split-GOP validation
headless supervised recorder lifecycle validation
```

The highest-signal remaining work is no longer headless split-GOP routing or
headless process lifecycle validation; it is GUI/session production hardening:

1. Validate the GUI/session external IPC path with
   `ORANGE_PTP_REGISTER_READ_DECIMATE=100`
   and the high-effort A16 detect engine candidate.
2. Add user-visible recorder heartbeat/failure reporting beyond the current
   finalization summary.
3. Turn the diagnostic descriptor/routing contract into a versioned production
   recorder protocol.

Current contract-hardening status:

- `fixed.external_recorder_contract.mode = "diagnostic_ipc_v1"` now records
  the expected external recorder artifact paths in experiment specs.
- `external_recorder_supervisor_plan` now performs a metadata-only dry run of
  the contract, expands each stream into an `external_recorder_ipc_probe` argv,
  and validates sink mode, selected stream coverage, artifact paths, and shard
  routing before any camera/GUI work is required.
- `src/external_recorder_contract_utils.*` now owns shared contract extraction,
  per-camera materialization, supervisor-plan artifact generation, and the
  shared external-recorder `recording_session.json` artifact shapes. The helper
  also annotates contracts with `recording_control` and current `rollover`
  intent so GUI/session and headless supervised paths describe the same backend
  contract. The GUI external recorder path uses this helper, and it is linked
  into `orange_client` for the supervised headless path.
- The supervised headless path now also uses that helper for the provisional
  `external_recorder_session.json` and `external_recorder_supervisor_plan.json`
  writes, and its parser uses the same wrapper-object extraction rule as the
  GUI contract loader.
- The same helper now also owns the shared artifact shapes for
  `external_recorder_supervisor_runtime.json`,
  `external_recorder_verifier_handoff.json`, and
  `external_recorder_finalization.json`. Headless still owns process lifecycle
  and verifier execution, but the durable JSON contracts no longer live as
  headless-local literals.
- `src/external_recorder_lifecycle.*` now owns the shared supervised lifecycle
  wrapper for plan build, initial artifact writes, recorder process
  start/stop, per-camera socket/session env-var handoff, runtime artifact
  writing, and verifier-handoff writing. Headless calls this wrapper, and the
  GUI now uses it for the first external IPC recording slice.
- GUI/session external IPC now starts supervised recorder processes on record
  start, refreshes the IPC socket/session handoff after stream startup,
  preserves recording drain state until the IPC handoff queues are empty, stops
  the supervised recorders at finalization, and writes an
  `orange_gui_external_ipc` single-clip `recording_session.json` from external
  recorder summaries. Two-camera GUI hardware validation passed on 2026-05-21;
  see the latest GUI/session validation note below.
- A single-camera supervised headless smoke on 2026-05-07 validated that path:
  analytics root
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_external_ipc_supervised_encode_smoke_20260507_215347`,
  recorder root
  `/tmp/orange_external_recorder_supervised_2010096_20260507_215347`.
  The recorder exited cleanly, wrote `external_recorder_session.json`,
  `external_recorder_supervisor_plan.json`,
  `external_recorder_supervisor_runtime.json`,
  `external_recorder_verifier_handoff.json`, and
  `external_recorder_finalization.json`, and passed video sanity on the
  external MP4. That run also confirmed the need for invoking-user filesystem
  ownership when supervised artifacts live under `/tmp`; recorder artifact
  writes and MP4-open failure handling now cover that case.
- `fixed.external_recorder_contract.supervise_processes = true` is the first
  opt-in headless lifecycle slice. `orange_client` starts recorder processes,
  waits for sockets, exports per-camera socket/session env vars, waits for
  recorder shutdown after analytics exits, and writes supervisor
  session/plan/runtime plus verifier-handoff artifacts.
- Contract-only plan specs exist for one-camera and two-camera PTP checks:
  `experiment_specs/2010096_external_recorder_supervisor_plan_smoke.json` and
  `experiment_specs/2010095_2010096_external_recorder_supervisor_plan_ptp.json`.
- A real supervised one-camera smoke spec exists at
  `experiment_specs/2010096_headless_real_yolo_external_ipc_supervised_encode_smoke.json`.
- A real supervised two-camera PTP smoke spec exists at
  `experiment_specs/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_supervised.json`.
- `external_recorder_ipc_probe` summaries now carry
  `schema_id = "orange.external_recorder.summary"`.
- `scripts/verify_external_recorder_session.py` validates analytics IPC
  counters, recorder summaries, shard routing, merged MP4 finalization, GOP
  routing CSVs, and video sanity.
- In supervised headless mode, `orange_client` now writes provisional
  manifests, runs per-stream external MP4 video sanity, runs the session
  verifier, and records the result in `external_recorder_finalization.json`.
- External IPC rolling clips are implemented for supervised headless diagnostic
  recorder runs. `fixed.recording_control.clip_seconds > 0` is passed to the
  recorder, which rotates GOP-boundary clip writers and reports
  `rollover.implementation =
  "external_recorder_gop_boundary_writer_rotation"`. The verifier now checks
  rolling clip count, clip MP4/metadata/keyframe sidecars, and continuous
  `recording_frame_id` coverage across clips.
- For supervised headless rolling runs, `orange_client` now rewrites the
  analytics `recording_session.json` from the external summaries after recorder
  finalization. The shared manifest reports `mode = "rolling_clips"`,
  `producer = "orange_headless_external_ipc"`, and `recording_backend.mode =
  "external_ipc"`; the verifier requires its per-camera clip artifacts to match
  the external summaries.
- The smoke runners generate `external_recorder_session.json`, inject the same
  contract into the temporary experiment spec, and run the verifier
  automatically.

Supervised hardware smoke result:

- One-camera supervised artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_external_ipc_supervised_encode_smoke_20260507_215347`;
  recorder artifact:
  `/tmp/orange_external_recorder_supervised_2010096_20260507_215347`. The run
  passed with `800` frames received/ACKed, `480` encoded at the diagnostic
  `60 fps` cap, `0` encode drops, and `video_sanity=pass`.
- Two-camera PTP supervised artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_supervised_20260507_222657`;
  recorder artifact:
  `/tmp/orange_external_recorder_supervised_ptp_20260507_222657`. The run
  passed with `400/400` frames received/ACKed/encoded for each camera, `0`
  encode drops, `0` camera frame-id gaps, `0` GetFrame errors, merged MP4
  output enabled, and `video_sanity=pass` for both cameras.
- Two-camera steady-state post-frame-50 `acquisition_to_detect_done_ms p95` was
  `4.488 ms` for `2010095` and `4.598 ms` for `2010096`; YOLO queue wait p95
  was `0.018 ms` and `0.020 ms`, respectively.
- Four-camera PTP supervised artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010093_2010094_2010095_2010096_headless_real_yolo_aq_off_100fps_ptp_external_ipc_supervised_20260520_195813`;
  recorder metadata artifact:
  `/tmp/orange_external_recorder_supervised_fourcam_ptp_20260520_195813`;
  recorder MP4/summary outputs:
  `/tmp/orange_external_recorder_supervised_fourcam_ptp`. The run passed with
  `400/400` frames received/ACKed/encoded for each of
  `2010093/2010094/2010095/2010096`, `0` skips, `0` drops, `0` camera
  frame-id gaps, `0` GetFrame errors, merged MP4 output enabled, and
  `video_sanity=pass` for all four cameras.
- Four-camera steady-state post-frame-50 `acquisition_to_detect_done_ms p95`
  was `4.595 ms` for `2010093`, `4.601 ms` for `2010094`, `4.631 ms` for
  `2010095`, and `4.618 ms` for `2010096`; YOLO queue wait p95 stayed between
  `0.022 ms` and `0.028 ms`. This is within about `0.03-0.07 ms` of the good
  two-camera external IPC p95 average and about `60%` lower than the old
  two-camera in-process headless/GUI baselines at roughly `11-12 ms`.
- The four-camera run used real YOLO at `decimate = 1`, but produced `400`
  zero-detection event rows per camera and `0` positive detection rows. Treat
  it as validation of four-camera acquisition, full-rate real inference
  plumbing, and external split-GOP recording throughput, not detection quality.
- Four-camera A16 high-effort FP16 TensorRT artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010093_2010094_2010095_2010096_headless_real_yolo_aq_off_100fps_ptp_external_ipc_supervised_a16_bo5_avg32_20260520_202903`;
  recorder artifact:
  `/tmp/orange_external_recorder_supervised_fourcam_ptp_a16_bo5_avg32_20260520_202903`.
  This reran the same supervised external IPC shape with
  `/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a16_gpu5_trt100_fp16_bo5_avg32.engine`.
  The run passed with `400/400` frames received/ACKed/encoded for all four
  cameras, `0` skips, `0` drops, `0` frame-id gaps, `0` GetFrame errors, and
  `video_sanity=pass` for all merged MP4s.
- Four-camera high-effort steady-state post-frame-50
  `acquisition_to_detect_done_ms p95` was `3.888 ms` for `2010093`,
  `3.813 ms` for `2010094`, `3.931 ms` for `2010095`, and `3.844 ms` for
  `2010096`, averaging `3.869 ms`. Compared with the same-day four-camera
  default-engine average of `4.611 ms`, this is a `0.742 ms` or about `16.1%`
  p95 improvement. `infer_ms p95` dropped from about `4.10 ms` to
  `3.19-3.28 ms`; YOLO queue wait p95 remained `0.020-0.024 ms`.
- Operational note: the repeatable four-camera supervised spec currently uses
  static per-stream recorder output paths under
  `/tmp/orange_external_recorder_supervised_fourcam_ptp`. Use a stamped copy or
  clean/stamp those paths before preserving multiple comparison runs.
- One-camera supervised external IPC rolling artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_external_ipc_rolling_smoke_a16_gpu5_6`;
  recorder artifact:
  `/tmp/orange_external_recorder_rolling_2010096`.
  The run used `record_for_seconds = 6`, `clip_seconds = 2`, `encode_fps =
  100`, `encode_max_fps = 0`, and two GOP shards on GPUs `5,6`. The recorder
  received/ACKed/encoded `602` frames, had `0` encode drops, wrote four rolling
  clips covering `1-200`, `201-400`, `401-600`, and `601-602`, passed merged
  MP4 video sanity, and passed `scripts/verify_external_recorder_session.py`.
- Two-camera PTP supervised external IPC rolling artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_rolling_bridge_20260509_ptp_rolling_bridge_28478`;
  recorder artifact:
  `/tmp/orange_external_recorder_ptp_rolling_20260509_ptp_rolling_bridge_28478`.
  Both cameras received/ACKed/encoded `601` frames with `0` encode drops, wrote
  four rolling clips covering `1-200`, `201-400`, `401-600`, and `601`, passed
  merged MP4 video sanity, and passed verifier checks against the shared
  analytics `recording_session.json`.
- The two-camera PTP run used `fixed.ptp_register_read_decimate = 100`, sampled
  `9` PTP register reads per camera, and the cadence probe showed embedded
  timestamp skew from `-18 ns` to `+22 ns`.
- Operational note: this run found the host PTP stack stopped, auto-started
  `ptp4l`/`phc2sys`, and left them running on exit. Stop with
  `scripts/ptp_stack.sh stop` when no more PTP validation is needed.

Latest GUI/session external IPC validation:

- Four-camera GUI autorun artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_00_34_27`
- command shape:
  `ORANGE_GUI_AUTORUN=1 ORANGE_GUI_AUTORUN_RECORD_SECONDS=10 ORANGE_GUI_RECORDING_SINK_MODE=external_ipc ORANGE_CROP_RECORDING_SINK_MODE=external_ipc ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU=1 ORANGE_PTP_REGISTER_READ_DECIMATE=100 ./scripts/run_gui_aq_off_validation.sh`
- config: `100_cam4_ptp_fourcam`, four cameras
  `2010093/2010094/2010095/2010096`, `sync_mode = "ptp_gate"`,
  `ptp.enabled = true`, and `ptp.mode = "TwoStep"`
- the validation launcher auto-forwarded `ORANGE_CROP_FRAME_POOL_SIZE=128`
  for external crop IPC, derived from the default crop external encode queue
  depth of `64`
- full-frame external IPC: all four cameras wrote valid `4512x4512` MP4s with
  `1016` frames each, `0` external IPC failures, `0` ACK timeouts,
  `0` camera frame-ID gaps, `0` GetFrame errors, and `0` encode failures
- external crop IPC: all four crop streams received/encoded `1016/1016`
  frames with `0` drops; crop metadata rows, crop perf rows, keyframe
  `total_frames`, crop MP4 frame counts, and YOLO rows all matched
- crop fanout matched detection rows for every camera, and
  `producer_crop_frame_pool_misses_total = 0`
- YOLO queue p95 stayed under `1 ms` for all cameras (`0.393/0.397/0.481/0.623
  ms`), and steady detect p95 was `4.962/5.151/5.613/6.116 ms`
- `scripts/validate_gui_ptp_recording.py --latest-complete` with the external
  crop backend and per-camera recorder GPU checks passed with `0` warnings

Earlier two-camera GUI validation:

- GUI artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_21_12_39_24`
- command shape:
  `ORANGE_GUI_RECORDING_SINK_MODE=external_ipc ORANGE_PTP_REGISTER_READ_DECIMATE=100 ./scripts/run_gui_aq_off_validation.sh`
- config: `100_cam4_ptp`, `sync_mode = "ptp_gate"`, `ptp.enabled = true`,
  `ptp.mode = "TwoStep"`, A16 `640x640` TensorRT detect engine
- the GUI wrote `producer = "orange_gui_external_ipc"` and
  `recording_backend.mode = "external_ipc"` in `recording_session.json`
- both cameras recorded `1645` submitted/ACKed/encoded frames with
  `0` external IPC failures, `0` ACK timeouts, `0` camera frame-ID gaps,
  `0` GetFrame errors, and `0` encode failures
- merged external MP4s were `4512x4512`, `100 fps`, `1645` frames, about
  `151.3 Mbps`, and decoded video sanity passed
- YOLO steady detect p95 was `4.314 ms` for `2010095` and `4.227 ms` for
  `2010096`; YOLO queue p95 was `0.019/0.017 ms`
- `scripts/verify_external_recorder_session.py` passed against the external
  recorder artifact root
- `scripts/validate_gui_ptp_recording.py --latest-complete` now passes after
  the validator was updated to use `recording_session.json` external video
  paths instead of requiring root-level `Cam*.mp4` files

Latest source-lifetime hardening:

- The direct-input deferred-release path was added after a visual jitter test
  showed behavior consistent with source-buffer reuse before recorder-side
  source consumption had completed.
- In this mode, the recorder ACKs accepted work with `deferred_release`, then
  sends `RELEASE` only after the source frame is safe to recycle.
- Latest one-camera hardware smoke:
  `/tmp/orange_external_recorder_2010096_20260522_194514`.
- The run received, ACKed, encoded, and source-released `401/401` frames with
  `0` release failures, passed decoded video sanity, and measured
  `detach_copy_p95_ms = 0.002735`.
- YOLO post-frame-50 `acquisition_to_detect_done_ms p95` stayed in the good
  external-recorder range at about `4.383 ms`.

Next production slice: add GUI-visible recorder health/heartbeat and failure
reporting around the supervised lifecycle. The first GUI PTP-stack guard now
exists in the validation launcher/wrapper path: `ORANGE_GUI_PTP_STACK_MODE`
maps to `orange-gui-validation --ptp-stack-mode off|require|auto`, and autorun
PTP-gated validation defaults to `auto` so `ptp4l`/`phc2sys` are started and
rechecked before Orange opens cameras.

Detailed Stage 5 protocol and routing design:

```text
docs/external_split_gop_recorder_design.md
```
