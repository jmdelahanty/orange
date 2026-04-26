# External Split-GOP Recorder Design

Date: 2026-04-26

Scope: define the first production-oriented protocol and architecture for
moving split-GOP full-frame recording across the analytics process boundary.

## Bottom Line

The external recorder should become a recorder supervisor plus encoder shards.
The analytics process should only publish frame descriptors and wait for a
detach ACK. It should not call NVENC, harvest bitstreams, mux video, or manage
split-GOP encode scheduling.

Production still needs split-GOP / multi-GPU recording. The external
single-camera `60 fps` tests proved that process isolation removes the
same-process CUDA/NVENC runtime-lock tail, but they did not prove that one A16
encoder can carry `4512x4512 @ 100 fps`. The production design must preserve
the existing multi-GPU throughput strategy while moving encoder ownership out
of the analytics process.

## Non-Negotiable Contracts

- Detach ACK means the recorder owns a safe copy of the frame.
- Detach ACK does not mean encode, bitstream harvest, mux, or disk write is
  complete.
- The analytics process may recycle the ingress/source lease only after ACK.
- Full-frame NVENC and FFmpeg calls stay out of the analytics process.
- Production full-frame recording must not silently drop frames.
- Diagnostic frame selection is allowed only when the config and artifacts make
  it explicit.
- Recorder failures after ACK must make the recording unhealthy/final-failed,
  because the analytics process no longer owns the source frame.

## Process Architecture

Recommended V1:

```text
analytics process
  acquisition
  YOLO / crop / pose
  descriptor publisher
  detach ACK wait

external recorder supervisor process
  per-camera ingress sockets
  routing policy
  recorder-owned detach/copy pools
  encoder shard on GPU A
  encoder shard on GPU B
  optional additional encoder shards
  GOP/order coordinator
  mux/writer
  recorder health/artifacts
```

Use one supervisor process first. Inside it, each encoder shard owns its CUDA
context, NVENC session, registered buffers, encode queue, and bitstream harvest
loop.

One process per GPU shard may become useful later for stronger fault isolation,
but it adds IPC, shutdown, and health coordination complexity. The first
split-GOP implementation should avoid that unless same-supervisor contention
shows up in measurements.

## Wire Protocol

Use versioned messages. The current probe can evolve into this contract without
changing the core ownership model.

Message types:

- `HELLO`: protocol version and recorder capabilities.
- `SESSION_START`: run id, camera set, output policy, route policy, codec
  profile, and expected frame geometry.
- `FRAME`: one source-frame descriptor.
- `ACK`: per-frame detach result.
- `DRAIN`: stop accepting frames and finish all accepted work.
- `STOP`: stop accepting frames and shut down cleanly.
- `FINAL`: final recorder status and summary path.
- `ERROR`: fatal recorder or per-frame failure.
- `HEARTBEAT`: recorder liveness and queue pressure.

`FRAME` fields:

- `schema_version`
- `session_id`
- `run_id`
- `camera_serial`
- `stream_id`
- `recording_frame_id`
- `local_frame_id`
- `camera_frame_id`
- `gop_index`
- `frame_index_within_gop`
- `is_gop_boundary`
- `source_gpu_id`
- `width`
- `height`
- `pitch_bytes`
- `pixel_format`
- `payload_bytes`
- `cuda_ipc_mem_handle`
- `source_ready_event_handle` if added later
- `capture_timestamp_ns`
- `publish_timestamp_ns`
- `route_hint` if supplied by analytics config

`ACK` fields:

- `session_id`
- `camera_serial`
- `recording_frame_id`
- `status`
- `detach_copied`
- `recorder_slot_id`
- `assigned_gpu_id`
- `assigned_shard_id`
- `gop_index`
- `ack_start_ns`
- `ack_done_ns`
- `error_code`
- `error_message`

The ACK path must stay short: import or reuse IPC mapping, copy to a
recorder-owned slot, record slot metadata, send ACK. Encoding work is queued
after the copy is safe.

## Routing Policy

Required routing modes:

- `source_only`: encode on the same GPU as the frame source. Useful as a
  fallback and same-GPU diagnostic.
- `pure_offload`: encode all frames on a non-source GPU. Useful when the source
  GPU cannot tolerate any encode load, but not enough for full `100 fps` if one
  encoder cannot carry the stream.
- `hybrid_split`: distribute GOPs across source and helper GPUs. This should be
  the default production direction.

Initial `hybrid_split` policy:

```text
assigned_shard = gop_index % shard_count
```

This is intentionally simple and inspectable. Weighted routing can come later
if measurements show one GPU or encoder is slower.

For the current host, the first camera-specific production experiment should
preserve A16 pairing/topology awareness. For camera `2010096`, the validated
diagnostic pairing was analytics GPU `5` with recorder GPU `6`. Production
split-GOP should still allow the analytics/source GPU to carry a controlled
share if that is required for `100 fps` throughput.

## Transfer Policy

V1 should detach raw camera frames into recorder-owned device slots. For Mono8
camera frames this is the right first choice because raw is one byte per pixel,
while NV12 would be larger and would also couple color conversion/preparation
to the source side.

The encode shard on the assigned GPU should prepare the encoder input format
from the recorder-owned raw slot. If the assigned encode GPU differs from the
source GPU, the recorder should record whether the transfer used peer access,
staged copy, or a fallback path.

Required transfer metadata:

- source GPU id
- assigned encode GPU id
- peer access available
- transfer mode
- detach copy time
- source-to-shard transfer time if separate from detach
- encode input prepare time

## Ordering And Mux

Encoder shards may finish out of order. The output side must not rely on
arrival order.

Recommended V1:

```text
encoder shard
  -> encoded packet with camera_serial, gop_index, frame_index, pts
  -> pending GOP map
  -> GOP order coordinator
  -> mux/writer
```

The GOP order coordinator releases only contiguous complete GOPs for a camera.
Each camera can be coordinated independently unless a later output format
requires cross-camera synchronization.

For the first implementation, prefer keeping one output file per camera. That
matches the current validation model and keeps cross-camera mux ordering out of
the first split-GOP experiment.

## Buffers And Backpressure

Every queue needs an explicit bound and an artifact counter.

Required bounds:

- max pending descriptors per camera
- max detach slots per shard
- max encode input slots per shard
- max in-flight GOPs per camera
- max pending encoded packets
- max writer queue bytes
- max ACK latency

Production semantics:

- If the recorder cannot ACK before the source lease deadline, fail the
  recording visibly.
- If the recorder ACKs but later cannot encode or write the frame, mark the
  recording unhealthy and report final failure.
- Do not silently skip full-rate production frames.

Diagnostic semantics:

- FPS caps and frame selection are allowed.
- Skipped frames must be counted as skipped, not dropped.
- The summary must record the selection policy.

## Failure Semantics

Failure before ACK:

- Analytics still owns the source lease.
- Recorder returns NACK or times out.
- Analytics marks an external recorder failure and can decide whether to stop
  recording.

Failure after ACK:

- Recorder owns the only safe recording copy.
- Recorder must mark the session unhealthy.
- The final summary must identify missing frame ids, failed GOPs, and any
  incomplete output files.

Shutdown:

- `DRAIN` means finish all ACKed frames and finalize outputs.
- `STOP` means stop accepting new descriptors and then follow drain semantics
  unless a fatal error prevents it.
- `FINAL` must include final counters and artifact paths.

## Artifacts

The external recorder must produce artifacts that can be joined with the
analytics run without terminal logs.

Required files:

- `recorder_session_summary.json`
- `recorder_frames.csv`
- `recorder_detach_timing.csv`
- `recorder_encode_timing.csv`
- `recorder_gop_routing.csv`
- `recorder_queue_depths.csv`
- `external_video_sanity.json`
- per-camera output video

Important summary fields:

- protocol version
- session id / run id
- camera serials
- route policy
- source GPU ids
- encode GPU ids
- codec, preset, tuning, GOP, rate control, bitrate
- frame counts received, ACKed, skipped, encoded, failed
- ACK timeout count
- detach p50/p95/p99
- transfer p50/p95/p99
- encode p50/p95/p99
- `nvEncEncodePicture` p50/p95/p99
- `nvEncLockBitstream` p50/p95/p99
- writer p50/p95/p99
- video sanity result
- final health state

## First Implementation Slice

The next coding slice should not attempt every production feature at once.

1. Extend the current external recorder descriptor/summary with
   `session_id`, `stream_id`, `assigned_gpu_id`, `assigned_shard_id`,
   `gop_index`, and `routing_policy`.
2. Add a two-shard external recorder diagnostic mode for one camera.
3. Route complete GOPs by `gop_index % shard_count`.
4. Emit `recorder_gop_routing.csv`.
5. Keep the first run capped at `60 fps` until MP4 validity and ordering are
   proven.
6. Then run one-camera `100 fps` split-GOP using the same routing code.
7. Only after that, move to two-camera PTP.

Acceptance gates:

- One-camera `2010096`, analytics GPU `5`, shards on GPU `5` and GPU `6`.
- Valid MP4 with decoded-frame sanity.
- No ACK failures/timeouts.
- No encode failures.
- No missing frame ids in uncapped production-mode tests.
- YOLO `cpu_preprocess_ms` and `cpu_pre_sync_ms` stay near the fast external
  path.
- `capture_to_detect_done_ms` materially beats the current in-process
  `11-12 ms p95` two-camera PTP baseline once split-GOP is tested under
  production load.

### Metadata-Only Single-Shard Slice

Implemented on 2026-04-26:

- Analytics external IPC descriptors now include session id, stream id,
  `gop_index`, `frame_index_within_gop`, a route-hint GPU id, shard id `0`, and
  routing policy `single_shard`.
- `external_recorder_ipc_probe` accepts the extended descriptor while remaining
  compatible with the previous shorter `FRAME` line.
- The recorder stamps the actual assignment from its own CLI:
  `--gpu-id`, `--shard-id`, and `--routing-policy`.
- Detach CSV and encode CSV now carry session/GOP/shard metadata.
- The smoke runner writes `external_gop_routing.csv`.
- No data-path behavior changed yet: this is still one external recorder
  process, one encoder lane, and `single_shard` routing.

Next implementation step:

- Add a two-shard diagnostic mode that creates two encoder lanes and routes
  complete GOPs by `gop_index % shard_count`.

### Two-Shard Diagnostic Slice

Implemented on 2026-04-26:

- `external_recorder_ipc_probe` accepts `--shard-gpu-ids`, for example
  `--shard-gpu-ids 5,6`.
- The probe creates one external encode worker per listed GPU.
- Frames are routed by complete GOP:
  `assigned_shard_id = gop_index % shard_count`.
- Each shard owns its CUDA/NVENC path and writes separate diagnostic artifacts:
  per-shard encode CSV and per-shard MP4.
- `external_gop_routing.csv` records the selected shard/GPU for every frame.
- The smoke runner accepts `--shard-gpu-ids`.
- This slice intentionally kept output split by shard; merged per-camera output
  was added in the following slice.

Validation smoke:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_external_recorder_smoke.sh \
  --duration 3 \
  --warmup 1 \
  --encode-fps 60 \
  --output-dir /tmp \
  --shard-gpu-ids 5,6
```

Result from `/tmp/orange_external_recorder_2010096_20260425_221647`:

- `401` descriptors received and ACKed.
- `262` frames encoded, `139` skipped by the `60 fps` cap, `0` encode drops.
- Shard `0` on GPU `5`: `135` frames encoded.
- Shard `1` on GPU `6`: `127` frames encoded.
- GOP routing alternated correctly: GOP `0 -> shard 0`, GOP `1 -> shard 1`,
  GOP `2 -> shard 0`, GOP `3 -> shard 1`.
- Both per-shard MP4s were present and `ffprobe`-readable at `4512x4512`.

### Merged GOP Output Slice

Implemented on 2026-04-26:

- Multi-shard `external_recorder_ipc_probe` now creates a merged GOP output
  coordinator when `--shard-gpu-ids` and `--mp4-out` are both present.
- Per-shard encoder workers still own their CUDA/NVENC lanes and diagnostic
  MP4s, but returned packets are also submitted to the coordinator.
- The coordinator buffers encoded packets by GOP, releases GOPs in recording
  order, and writes the base per-camera MP4 path
  `Cam<serial>_external.mp4`.
- GOP completion is inferred both from the final frame in a GOP and from
  routing progress into the next GOP. This is required for capped diagnostics
  where the frame-rate limiter may skip the terminal frame of a GOP.
- The summary JSON now includes a `merged_output` section with packet counts,
  pending GOP/byte counts, writer queue stats, MP4 path, and failure state.
- `outputs.mp4` and `output_file_sizes.mp4_bytes` now refer to the merged base
  MP4 when merged output is enabled.
- Per-shard MP4s remain in `external_encode_shards` for diagnosis.
- The smoke runner no longer skips video sanity for multi-shard mode; it checks
  the merged base MP4.

Validation smoke:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_external_recorder_smoke.sh \
  --duration 3 \
  --warmup 1 \
  --encode-fps 60 \
  --output-dir /tmp \
  --shard-gpu-ids 5,6
```

Result from `/tmp/orange_external_recorder_2010096_20260425_222953`:

- `401` descriptors received and ACKed.
- `259` frames encoded, `142` skipped by the `60 fps` cap, `0` encode drops.
- Merged output wrote `259` packets, released `17` GOPs, and ended with
  `0` pending GOPs / `0` pending bytes.
- Merged MP4 size was `82236594` bytes.
- Video sanity passed on the merged base MP4:
  `frames=259`, `mean_luma=174.802`, `max_black_fraction_lt8=0.000827`.
- Shard `0` on GPU `5`: `135` frames encoded.
- Shard `1` on GPU `6`: `124` frames encoded.
- GOP routing still alternated correctly by modulo.

Full-rate validation smoke:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_external_recorder_smoke.sh \
  --duration 3 \
  --warmup 1 \
  --encode-fps 100 \
  --encode-max-fps 0 \
  --queue-depth 32 \
  --output-dir /tmp \
  --shard-gpu-ids 5,6
```

Result from `/tmp/orange_external_recorder_2010096_20260425_223406`:

- `401` descriptors received and ACKed.
- `401` frames encoded, `0` skipped, `0` encode drops.
- Merged output wrote `401` packets, released `17` GOPs, and ended with
  `0` pending GOPs / `0` pending bytes.
- Merged MP4 size was `77519555` bytes.
- Video sanity passed on the merged base MP4:
  `frames=401`, `mean_luma=220.444`, `max_black_fraction_lt8=0.000000`.
- Shard `0` on GPU `5`: `201` frames encoded.
- Shard `1` on GPU `6`: `200` frames encoded.
- YOLO stayed on the fast external-recorder path at p95:
  `capture_to_detect_done_ms = 4.758445`,
  `cpu_preprocess_ms = 0.015669`, and
  `cpu_pre_sync_ms = 0.098204`.

Operational finding:

- Queue depth `8` was too small for full-rate GOP modulo routing: a prior
  `100 fps` run with `--queue-depth 8` encoded `388` frames and dropped `3`.
- Queue depth `32` absorbed the burst. The reason is that whole-GOP routing
  gives each shard about half-rate average load, but the active shard receives
  a full GOP burst at the source frame rate. The queue must cover at least one
  GOP plus scheduling jitter.
- `--encode-max-fps 0` disables the diagnostic frame-rate cap while keeping
  `--encode-fps 100` as the nominal MP4 frame rate.

### Two-Camera PTP External Recorder Smoke

Implemented on 2026-04-26:

- Added `scripts/run_external_recorder_two_camera_ptp_smoke.sh`.
- The runner starts one `external_recorder_ipc_probe` process per camera and
  then launches one headless `ptp_gate` benchmark using
  `recording_sink_mode = external_ipc`.
- Default local topology:
  `2010095 -> analytics GPU 5, recorder shards 5,6`.
- Default local topology:
  `2010096 -> analytics GPU 7, recorder shards 7,8`.
- The runner uses the local `100_cam4_ptp` config folder, uncapped external
  encode (`--encode-max-fps 0`), nominal MP4 `100 fps`, and queue depth `32`.
- The runner writes one merged MP4 and per-shard diagnostic MP4s per camera,
  plus one summary JSON per camera.
- Video sanity runs on both merged camera MP4s.

Validation smoke:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_external_recorder_two_camera_ptp_smoke.sh \
  --duration 6 \
  --warmup 1
```

Result from `/tmp/orange_external_recorder_ptp_20260425_224354`:

- Benchmark artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_20260425_224354`.
- Benchmark summary: `1` completed run, `1` pass, `0` failures.
- PTP was healthy: both cameras reported `401` frames, `0` dropped frames,
  `0` frame-id gaps, and sub-microsecond PTP offsets.
- `2010095`: `401` descriptors received, `401` ACKed, `401` encoded,
  `0` skipped, `0` encode drops.
- `2010096`: `401` descriptors received, `401` ACKed, `401` encoded,
  `0` skipped, `0` encode drops.
- `2010095` merged MP4 sanity passed:
  `frames=401`, `mean_luma=220.433`, `max_black_fraction_lt8=0.039601`.
- `2010096` merged MP4 sanity passed:
  `frames=401`, `mean_luma=177.659`, `max_black_fraction_lt8=0.134326`.
- `2010095` shard split: GPU `5` encoded `201` frames, GPU `6` encoded `200`.
- `2010096` shard split: GPU `7` encoded `201` frames, GPU `8` encoded `200`.
- YOLO stayed materially better than the old in-process `11-12 ms` PTP
  baseline:
  `2010095 capture_to_detect_done_ms p95 = 6.697993`,
  `2010096 capture_to_detect_done_ms p95 = 5.953366`.
- YOLO preprocessing stayed short at p95:
  `2010095 cpu_preprocess_ms p95 = 0.416902`,
  `2010096 cpu_preprocess_ms p95 = 0.017142`.
- There were still rare large max outliers around startup/shutdown:
  `capture_to_detect_done_ms max` about `54 ms` on both cameras. Treat this as
  a tail to investigate before calling the architecture production-ready.

Startup/steady-state interpretation:

- The largest YOLO tails are concentrated at startup, not shutdown or random
  steady-state points.
- Frame `1` on both cameras spent about `49 ms` in `cpu_pre_sync_ms`:
  `2010095 cpu_pre_sync_ms = 49.704845`,
  `2010096 cpu_pre_sync_ms = 49.138623`.
- Frames `2-6` then waited behind that first slow YOLO frame, showing high
  `acquisition_to_worker_start_ms` while their own preprocess/inference work
  was normal.
- After frame `50`, steady-state `capture_to_detect_done_ms` was much tighter:
  `2010095 p95 = 6.650664`, `max = 6.985833`;
  `2010096 p95 = 5.882332`, `max = 6.342927`.
- After frame `50`, steady-state worker-start latency was also bounded:
  `2010095 acquisition_to_worker_start_ms p95 = 2.108304`,
  `max = 2.261402`;
  `2010096 p95 = 1.562080`, `max = 1.936482`.

Recorder first-use interpretation:

- The external recorder has a separate first-use spike when each camera first
  routes a GOP to its secondary shard GPU.
- Frame `26` is the first frame of GOP `1` and the first frame assigned to
  shard `1`.
- For `2010095`, frame `26` routed from source GPU `5` to shard GPU `6` and
  measured `copy_ms = 180.136509`.
- For `2010096`, frame `26` routed from source GPU `7` to shard GPU `8` and
  measured `copy_ms = 182.922434`.
- Later frames on those shard GPUs returned to normal enough that queue depth
  `32` absorbed the spike with `0` encode drops.
- This looks like lazy CUDA/IPC/peer-copy path setup for secondary recorder
  shards, not a steady-state throughput limit.

Pre-listen recorder prewarm:

- Implemented on 2026-04-25 in `external_recorder_ipc_probe`.
- New recorder flags:
  `--prewarm-slots`, `--prewarm-bytes`, and `--prewarm-peer-copy`.
- The smoke runners default to `--prewarm-slots 4` and derive
  `--prewarm-bytes` from the camera config for Mono8 frames.
- When `--prewarm-bytes` is available, recorder-owned CUDA detach slots are
  allocated before the Unix socket is created. The runner therefore waits for
  this work before launching the camera benchmark.
- The first real IPC handle can still be used for a 1-byte source-to-shard
  peer-copy warmup via `--prewarm-peer-copy`.
- Important implementation detail: prewarm must allocate the slots that will be
  popped first from the free-slot stack. Allocating low-index slots while the
  worker pops from the back leaves the first real frames on the lazy allocation
  path.

Prewarm diagnostic result:

- Run:
  `/tmp/orange_external_recorder_ptp_20260425_231526`.
- Analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_20260425_231526`.
- Command shape:
  `scripts/run_external_recorder_two_camera_ptp_smoke.sh --duration 6 --warmup 1 --skip-video-sanity`.
- Both cameras received/ACKed/encoded `401` frames, with `0` skips,
  `0` encode drops, and no worker failures.
- Pre-listen prewarm moved the large secondary-shard allocation/copy setup out
  of the measured detach path:
  `2010095 detach_copy_steady_p95 = 0.164589 ms`,
  `detach_copy_steady_max = 0.910026 ms`;
  `2010096 detach_copy_steady_p95 = 0.163637 ms`,
  `detach_copy_steady_max = 0.261190 ms`.
- YOLO service time did not materially change:
  `2010095 yolo_total_steady_p95 = 4.616950 ms`;
  `2010096 yolo_total_steady_p95 = 4.600458 ms`.
- Broad detect latency remains mostly worker-dispatch plus YOLO service:
  `2010095 detect_steady_p95 = 6.430631 ms`;
  `2010096 detect_steady_p95 = 6.520981 ms`.
- Remaining all-frame max tails are now mostly YOLO/runtime startup:
  `capture_to_detect_done_ms max` was about `54 ms` on both cameras, while
  recorder detach-copy steady-state max stayed below `1 ms`.

Headless YOLO synthetic prewarm:

- Implemented on 2026-04-25.
- `HeadlessYoloWorkerConfig` now supports `prewarm_iterations`.
- `YoloWorker::Warmup()` runs synthetic full-resolution preprocessing,
  TensorRT/CUDA-graph inference, stream synchronization, and postprocess before
  the camera acquisition threads start.
- The external-recorder smoke runners default to
  `--yolo-prewarm-iterations 3` and inject that into
  `fixed.yolo_worker.prewarm_iterations` in the generated experiment spec.

YOLO prewarm diagnostic result:

- Run:
  `/tmp/orange_external_recorder_ptp_20260425_233134`.
- Analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_20260425_233134`.
- Command shape:
  `scripts/run_external_recorder_two_camera_ptp_smoke.sh --duration 6 --warmup 1 --skip-video-sanity`.
- Both cameras received/ACKed/encoded `401` frames, with `0` skips,
  `0` encode drops, and no worker failures.
- The old first-live-frame YOLO startup tail was removed:
  prior frame `1` `cpu_pre_sync_ms` was about `49 ms` on both cameras; after
  synthetic prewarm, frame `1` was `4.005092 ms` for `2010095` and
  `0.130886 ms` for `2010096`.
- All-frame `capture_to_detect_done_ms max` dropped from about `54 ms` to
  single digits:
  `2010095 max = 8.271955 ms`;
  `2010096 max = 7.108273 ms`.
- Steady-state YOLO service stayed in the same target range:
  `2010095 yolo_total_steady_p95 = 4.587434 ms`;
  `2010096 yolo_total_steady_p95 = 4.573097 ms`.
- Broad detect steady-state p95 remains the next latency target:
  `2010095 detect_steady_p95 = 5.980286 ms`;
  `2010096 detect_steady_p95 = 6.726206 ms`.
- The remaining gap is mostly worker dispatch / acquisition-to-worker-start,
  not recorder detach or first-use YOLO runtime setup.

Next implementation step:

- Instrument and reduce steady-state acquisition-to-worker-start latency.
- Run a longer two-camera PTP external-recorder validation to determine whether
  late-run YOLO dispatch tails remain.
- Add GUI/session supervision for external recorder startup, heartbeat, drain,
  and finalization.
- Keep queue depth at least `gop_length + margin`; use `32` for `gop=25`.

## Open Questions

- Whether same-supervisor multi-shard is enough, or one process per shard is
  needed.
- Whether CUDA event IPC is needed for source-ready timing, or the current
  source readiness contract is sufficient.
- Whether GOP routing should be fixed modulo, weighted, or topology-driven.
- Whether the writer needs a new timestamp model before the first split-GOP
  prototype.
- How GUI lifecycle should supervise recorder startup, heartbeat, drain, and
  finalization.
