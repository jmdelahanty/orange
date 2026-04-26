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
