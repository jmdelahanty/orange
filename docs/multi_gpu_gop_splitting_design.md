# Multi-GPU GOP-Splitting Design

Date: 2026-04-12
Updated: 2026-04-14
Scope: document what was learned about NVIDIA's `AppEncMultiInstance` style
GOP splitting, explain what it means for the current Orange recording pipeline,
describe a practical first design for this repo, list current blockers and
risks, and note future GPU-output / GPUDirect directions that are explicitly
not priority for v1.

## Short Conclusion

- A GOP-splitting prototype is feasible in Orange, but it is not a small local
  change inside `EncoderHwWorker::EncodeFrame(...)`.
- Orange already understands multi-GPU placement at the camera / worker level.
  The missing pieces are:
  - a GOP scheduler,
  - ordered merge / mux semantics,
  - bounded compressed-output buffering,
  - and a cross-GPU transport policy.
- The safest first version should:
  - keep the current host-bitstream + CPU mux path,
  - use bounded host RAM buffering per GOP,
  - start on the current copy path rather than direct-input mode,
  - and treat GPU-side bitstream output / GPUDirect output as later work.

## Related Docs

- `docs/nvenc_direct_input_plan.md`
- `docs/nvenc_throughput_todo.md`
- `docs/nvenc_benchmark_runsheet.md`
- `docs/headless_split_gop_smoketest.md`
- `docs/threading_model_overview.md`
- `docs/recording_segment_rollover_todo.md`

## Primary External References

- NVIDIA NVENC Video Encoder API Programming Guide:
  https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html
- NVIDIA GPUDirect Storage Overview Guide:
  https://docs.nvidia.com/gpudirect-storage/overview-guide/contents.html

## Definitions

- `GOP splitting`:
  assign whole GOPs from one logical stream to different encoder sessions.
- `Ada SFE`:
  NVENC split-frame encoding, where one frame is partitioned into horizontal
  strips and encoded in parallel by multiple NVENC engines on one GPU.
- `AppEncMultiInstance` style scheduling:
  multiple independent encode sessions, GOP-level assignment, compressed output
  accumulated in RAM and concatenated / released in stream order later.
- `source GPU`:
  the CUDA device currently receiving or preprocessing the camera stream.
- `encode GPU`:
  the CUDA device that runs the NVENC session for a given GOP.

The important distinction is:

- Ada SFE is per-frame parallelism inside the encoder hardware.
- GOP splitting is coarse-grained scheduling across independent encode
  sessions.

## Topology Glossary

When reading `nvidia-smi topo -m`, the practical meanings for this project are:

- `NV#`:
  NVLink connectivity. This is a dedicated GPU-to-GPU interconnect and is
  generally the best case for cross-GPU transfer bandwidth and latency.
- `PIX`:
  PCIe connectivity traversing at most one PCIe bridge. This is still PCIe, not
  NVLink, but it is usually one of the better PCIe topologies for cross-GPU
  traffic.
- `PHB`:
  the path traverses a PCIe host bridge, typically the CPU root complex. This
  is generally worse than `PIX`.
- `SYS`:
  the path traverses PCIe and the system interconnect between host bridges /
  NUMA domains. This is generally the least attractive common topology for
  heavy cross-GPU frame traffic.

Practical ranking for Orange:

- `NV#` best
- `PIX` good
- `PHB` / `NODE` middling
- `SYS` worst among the common cases we expect to see here

Important caution:

- `PIX` does not mean "free" and does not mean "NVLink".
- It only means the PCIe topology is relatively favorable compared with a worse
  path like `SYS`.

## What We Learned

### 1. NVIDIA's suggestion is plausible, but it is not the same as Ada SFE

Based on the NVIDIA guidance already summarized in
`docs/nvenc_direct_input_plan.md`, the suggested sample behavior is:

- one CUDA device with multiple encoder sessions,
- whole GOPs assigned round-robin across those sessions,
- input consumed frame-by-frame,
- compressed output accumulated in RAM,
- final output written only after concatenation in stream order.

That means the design pressure in Orange is mostly around scheduling, merge,
buffering, and failure handling rather than around the low-level NVENC API call
shape itself.

### 2. Orange already has multi-GPU plumbing

Relevant current behavior:

- camera / worker placement is explicitly keyed by `gpu_id`,
- acquisition / preprocess / encode workers all bind a concrete CUDA device,
- headless already supports per-camera GPU overrides.

Refs:

- `src/orange_headless_client.cpp`
- `src/camera.cpp`
- `src/encoder_preprocess_worker.cpp`
- `src/encoder_hw_worker.cpp`

So "multi-GPU support" in the broad sense already exists. The hard part is
single-stream splitting across those GPUs.

### 3. The current recording path is frame-oriented, not GOP-oriented

Today:

- `ENCODER_WORKER_ENTRY` carries one prepared frame plus metadata,
- one `EncoderHwWorker` owns one `NvEncoderCuda`,
- packets are pushed to the writer immediately after each frame encode.

Refs:

- `src/encoder_pipeline.h`
- `src/encoder_hw_worker.cpp`

There is no current abstraction for:

- `gop_index`,
- expected last frame of a GOP,
- per-GOP completion,
- per-GOP RAM buffers,
- or ordered release of completed GOPs.

### 4. The current mux path assumes packet arrival order is the timeline

`FFmpegWriter::push_packet(...)` currently allocates an `AVPacket`, copies the
packet bytes, and stamps `pts/dts` from an internal sequential counter. The
`nPts` argument is not used for actual timestamping.

Refs:

- `src/FFmpegWriter.cpp`

This is acceptable when one encoder session emits packets in display order, but
it becomes a design blocker once multiple sessions can finish GOPs out of order.

### 5. The current writer queue is unbounded

`SafeQueue` is a plain `std::queue` with no byte or item budget, and
`FFmpegWriter` uses it directly for `AVPacket*`.

Refs:

- `src/thread.h`
- `src/FFmpegWriter.h`

That means compressed-output backlog can already grow without a hard limit.
Adding GOP-level buffering without explicit budgets would increase this risk.

### 6. The active output path is already host-bitstream based

The active path today is:

1. NVENC produces a compressed packet.
2. The local wrapper exposes host-visible packet bytes.
3. `EncoderHwWorker` receives those bytes in `encoder_.vPacket`.
4. `FFmpegWriter::push_packet(...)` copies them again into `AVPacket`.
5. FFmpeg muxes and writes MP4 on the CPU side.

This is already documented in `docs/nvenc_direct_input_plan.md`.

Implication:

- a first GOP-splitting prototype can stay on the same host-bitstream path,
- there is no need to solve GPU-resident muxing first.

### 7. The repo does not currently show a general peer-copy path

The one obvious cross-GPU example today is the display path, and it falls back
to a host bounce buffer rather than explicit CUDA peer access.

Ref:

- `src/opengldisplay.cpp`

So a GOP-splitting prototype should not assume that peer access / GPU-to-GPU
copy infrastructure already exists elsewhere in the app.

### 8. Observed topology on `pancake0` makes A16-only GOP splitting much more plausible than mixed `A6000 + A16`

Observed on `2026-04-12`:

- `GPU0` is `NVIDIA RTX A6000`
- `GPU1..GPU8` are `NVIDIA A16`
- `nvidia-smi topo -m` shows:
  - `GPU1..GPU4` are mutually `PIX`
  - `GPU5..GPU8` are mutually `PIX`
  - traffic between those two A16 groups is `SYS`
  - there is no `NV#` connectivity shown
- the local CUDA peer-access helper reports:
  - `A6000 <-> A16`: `can_access_peer=no`
  - `A16 <-> A16`: `can_access_peer=yes`, `enable_peer_access=yes`

Practical reading:

- the best first GOP-splitting experiments should stay entirely within one A16
  `PIX` group,
- crossing from one A16 `PIX` group to the other may still work, but should be
  treated as a more expensive path,
- mixed `A6000 + A16` GOP splitting is a poor first target because it lacks
  CUDA peer access on this host,
- there is still no evidence here that cross-GPU transfer is "free"; only that
  direct peer access is possible across the A16 set.

Recommended initial A16 test pairs on this host:

- `GPU1 + GPU2`
- `GPU1 + GPU3`
- `GPU5 + GPU6`
- `GPU5 + GPU7`

Recommended paths to avoid in v1:

- any `GPU0 + A16` pairing
- any cross-group `GPU1..GPU4 <-> GPU5..GPU8` pairing until the in-group
  `PIX` pairs have been measured

## Clarifying GOP-Oriented Scheduling

The most common confusion is to think "GOP-oriented" means all raw frames in a
GOP must be present before encode starts. That is not required.

Example: `GOP = 60`, `FPS = 60`, `2 sessions`

```text
frames:
  0........................59 | 60......................119 | 120...
            GOP 0                          GOP 1

assignment:
  GOP 0 -> session A
  GOP 1 -> session B
  GOP 2 -> session A
```

Actual behavior:

- session A can start encoding as soon as frame `0` arrives,
- session B can start encoding as soon as frame `60` arrives,
- no one needs all `60` raw frames resident before encoding begins.

The hard latency component is instead:

- the next independently assignable work unit begins only at the next GOP
  boundary,
- and completed GOPs may need to wait in RAM until all prior GOPs are ready to
  be released to the muxer in order.

So "buffering a GOP" mostly means:

- buffering compressed output packets until release is safe,
- not buffering all raw input frames before encode starts.

## Current Orange Constraints And Blockers

## Blocker 1: One encoder worker currently means one encoder session on one GPU

`EncoderHwWorker` constructs one `NvEncoderCuda`, configures it once, and then
uses it for the lifetime of the worker.

Ref:

- `src/encoder_hw_worker.cpp`

To support GOP splitting, Orange needs a new orchestration layer above the
current worker model rather than trying to overload one worker with hidden
multi-session behavior.

## Blocker 2: No GOP identity in the hot-path handoff

`ENCODER_WORKER_ENTRY` currently contains:

- prepared frame pointer,
- slot id,
- pitch,
- `recording_frame_id`,
- timestamps,
- completion event.

Ref:

- `src/encoder_pipeline.h`

It does not contain:

- `gop_index`,
- `frame_index_within_gop`,
- `last_frame_in_gop`,
- session assignment,
- expected flush generation.

Those need to be added explicitly, or a GOP-owned object must be introduced
above the frame entries.

## Blocker 3: Mux ordering is tied to arrival order

Today the encode worker calls:

- `writer_.video->push_packet(packet.data(), size, entry->recording_frame_id)`

but `FFmpegWriter` then ignores the supplied frame id and assigns PTS from an
internal counter.

Refs:

- `src/encoder_hw_worker.cpp`
- `src/FFmpegWriter.cpp`

This means:

- if GOP `N+1` finishes before GOP `N`, Orange cannot safely release it to the
  current writer path,
- and even if release order is forced, the writer API is still too implicit for
  future reordering or discontinuity handling.

## Blocker 4: No bounded RAM budget for compressed backlog

Current state:

- worker input queues are bounded by `CThreadWorker::maxQueueSize`,
- `FFmpegWriter`'s packet queue is not bounded,
- there is no per-stream buffered-byte budget for compressed output.

Refs:

- `src/threadworker.h`
- `src/thread.h`

If GOP splitting is added without explicit budgets, long stalls in muxing or
disk I/O could cause unbounded RAM growth.

## Blocker 5: Direct-input surfaces are single-device resources today

The current direct-input path allocates a ring of encoder-facing CUDA surfaces
inside `EncoderPreprocessWorker` on that worker's current device.

Refs:

- `src/encoder_preprocess_worker.h`
- `src/encoder_preprocess_worker.cpp`

That is a good design for one preprocess worker feeding one local NVENC
session. It is not a good first foundation for cross-GPU GOP splitting, because
the surfaces, slot ids, and retirement rules are all local to one device.

Implication:

- direct-input mode should not be the first implementation target for GOP
  splitting,
- the first prototype should use the current copy path, which is easier to
  reason about across device boundaries.

## Blocker 6: Cross-GPU transfer policy is not decided

For a single stream split across GPUs, Orange needs an answer to:

- do we preprocess on the source GPU and transfer prepared NV12 to the remote
  encode GPU,
- or transfer a less-processed representation and preprocess remotely,
- or maintain duplicate preprocess capability on multiple GPUs.

There is no existing project-wide answer yet, and the best choice is a tradeoff
between:

- transfer bandwidth,
- duplicate preprocess cost,
- implementation complexity,
- and correctness risk.

## Proposed V1 Design

## Design Goals

- Raise practical throughput for one logical stream by alternating whole GOPs
  across two encoder sessions.
- Preserve output correctness:
  - packet order,
  - frame-id continuity,
  - timestamp continuity,
  - metadata continuity.
- Keep the current MP4 writer stack for v1.
- Bound RAM growth explicitly.
- Keep the first version debuggable.

## Non-Goals For V1

- direct-input external CUDA surfaces across multiple encode GPUs,
- GPU-resident MP4 muxing,
- zero-copy end-to-end output,
- aggressive optimization of every copy before correctness is proven.

## Recommended First Topology

For one camera stream:

- one `source GPU` continues to receive the camera frames,
- two encoder sessions exist:
  - session A on encode GPU A,
  - session B on encode GPU B,
- whole GOPs are assigned round-robin:
  - `gop_index % 2 == 0` -> A,
  - `gop_index % 2 == 1` -> B.

Recommended first implementation choices:

- keep display, YOLO, crop preview, and future pose logically anchored to the
  source GPU,
- treat recording as a side branch off the source-GPU frame,
- use the current host-bitstream writer path,
- start with one explicit transfer policy and measure it before adding
  alternatives.

Two transfer policies are worth considering first:

- `local preprocess + remote encode`:
  preprocess on the source GPU, transfer prepared `NV12` for remote-owned GOPs,
  then encode remotely.
- `remote preprocess + remote encode`:
  transfer the raw camera frame for remote-owned GOPs, preprocess on the remote
  GPU, then encode there.

Current practical bias:

- if recording stays full-resolution or near full-resolution and the source
  format is `Mono8` or raw Bayer, `remote preprocess + remote encode` is
  likely the better bandwidth candidate,
- if recording is heavily downsampled before encode, `local preprocess + remote
  encode` may become the smaller transfer.

Both should be treated as benchmarkable modes rather than assumed truths.

## Vision GPU vs Encode GPU Separation

Orange already has several stages that naturally want one stable per-camera
processing GPU:

- acquisition owns the `WORKER_ENTRY` and its source device,
- YOLO runs on `associated_camera_params_->gpu_id`,
- crop preview / crop recording live off the same frame identity,
- planned second-stage pose is downstream of crop generation.

Practical recommendation:

- keep one stable `source GPU` per camera for:
  - acquisition,
  - display-facing work,
  - YOLO,
  - crop generation,
  - future pose,
- make GOP splitting a recording-only branch rather than a change in camera or
  vision GPU ownership.

Why:

- display, YOLO, and pose should not need to know which GPU encoded the current
  GOP,
- camera GPUDirect ownership is currently tied to one `gpu_id`,
- changing the "camera GPU" dynamically by GOP would create unnecessary
  cross-cutting complexity.

In other words:

- `camera / vision GPU` should stay stable,
- `encode GPU` may vary by GOP.

## Source GPU Encoder Policy

The source GPU may also have a usable NVENC. That should be treated as a policy
decision, not as something to leave idle by default.

There are three meaningful policies:

### Policy A: Local Only

- source GPU does acquisition, vision, preprocess, and encode for all GOPs.

Use when:

- cross-GPU transfer cost dominates,
- or one GPU is already sufficient.

### Policy B: Hybrid Split

- source GPU encodes some GOPs locally,
- helper GPU encodes the others.

Use when:

- the source GPU has a usable encoder,
- transferring every recorded frame is unnecessarily expensive,
- and the source GPU can still tolerate some recording work.

This is the most likely sweet spot for current hardware because:

- it uses both encoders,
- it cuts cross-GPU traffic roughly in proportion to the offloaded GOP share,
- and it does not assume the source GPU encoder should sit idle.

### Policy C: Pure Offload

- source GPU does acquisition and vision only,
- helper GPU encodes all recording GOPs.

Use when:

- protecting the source GPU from recording-side work is more important than the
  transfer cost,
- or measurements show local preprocess / local encode causes unacceptable
  contention with YOLO / pose / display.

Current recommendation:

- do not assume `Policy C` by default,
- benchmark `Policy B` first,
- keep `Policy C` available as a comparison point if vision-side contention is
  suspected.

## Raw-vs-Prepared Transfer Decision Rule

For remote-owned GOPs, the transfer representation should be chosen by size and
by where the desired compute should live.

For the current source formats:

- raw `Mono8` or Bayer: about `1 byte/pixel`
- prepared `NV12`: `1.5 bytes/pixel`

Implication:

- at full resolution, moving raw frames is about `33%` less transfer than
  moving prepared `NV12`,
- for mono specifically, moving raw `Mono8` avoids paying inter-GPU bandwidth
  for a synthetic UV plane that can be generated remotely,
- for Bayer, moving raw may also be attractive if the remote GPU has enough
  preprocess headroom.

But once heavy resize is introduced:

- the prepared output may become much smaller than the raw input,
- in that case local preprocess before transfer can become the better choice.

Recommended rule:

- full-res or lightly-downsampled mono / Bayer recording:
  prefer benchmarking raw-frame transfer first,
- heavily-downsampled recording:
  benchmark transfer after resize / encoder-boundary preparation.

This might not be the final highest-throughput architecture, but it is the most
defensible first implementation.

## New Concepts To Add

### `GopRoutingInfo`

Per frame:

- `gop_index`
- `frame_index_within_gop`
- `is_last_frame_in_gop`
- `assigned_session_id`
- `assigned_gpu_id`

### `EncodedPacketRecord`

Per encoded packet:

- packet bytes,
- originating `recording_frame_id`,
- originating timestamps,
- whether the packet is a keyframe / IDR if known.

### `PendingGop`

Per in-flight GOP:

- `gop_index`
- `session_id`
- `gpu_id`
- first / last expected frame id,
- count of frames submitted,
- count of frames encoded / drained,
- vector of encoded packet records,
- `total_bytes`,
- `complete`,
- error / aborted state.

### `OrderedMuxCoordinator`

Responsibilities:

- know the next `gop_index` expected for output,
- hold completed later GOPs until earlier GOPs are ready,
- release whole GOPs to the writer in correct order,
- enforce byte and age budgets,
- propagate overflow / failure states.

## Proposed Data Flow

1. Acquisition continues on the source GPU, and preprocess happens either on
   the source GPU or the assigned encode GPU depending on the chosen transfer
   policy.
2. Each frame is assigned:
   - `gop_index = recording_frame_id / resolved_gop_length`
   - `frame_index_within_gop = recording_frame_id % resolved_gop_length`
3. The scheduler picks the owning session / GPU for that GOP.
4. If the owning session is local:
   - preprocess locally if needed,
   - encode on the local GPU.
5. If the owning session is remote:
   - transfer either the raw frame or a prepared representation to the remote
     GPU according to the selected transport policy,
   - preprocess remotely if needed,
   - then encode there.
6. Each session emits compressed packets frame-by-frame.
7. Those packets are appended to the owning `PendingGop` in host RAM.
8. When a GOP is fully encoded and drained:
   - mark it complete,
   - attempt ordered release through `OrderedMuxCoordinator`.
9. Only completed GOPs whose predecessors are already flushed may be pushed to
   the muxer.

## Muxing Rule For V1

V1 should continue to use CPU-side FFmpeg muxing.

However, one cleanup should happen early:

- change the writer API so explicit frame index / timestamp information can be
  preserved instead of relying on arrival order.

Even if GOPs are always released in perfect order, making timestamps explicit is
the safer long-term contract.

## Buffering And Backpressure Design

### What Must Be Buffered

For v1:

- compressed packets per in-flight GOP,
- not whole raw GOPs by default.

### Why Exact Final GOP Size Does Not Need To Be Known Up Front

Compressed packet sizes are naturally variable.

So the correct allocation model is:

- append packet-by-packet,
- track cumulative bytes,
- enforce a hard budget,
- never rely on exact precomputed final GOP size.

### Recommended Initial Budgets

Initial prototype values should be explicit and conservative:

- test GOP length:
  - prefer `15` or `30` before `60`
- max in-flight GOPs per stream:
  - `3`
- max buffered compressed bytes per stream:
  - start with `128 MB`
- max writer queue packets / bytes:
  - add explicit bounds; do not leave `FFmpegWriter` unbounded

Rationale:

- current capped-bitrate paths are well below this under normal conditions,
- but the budget is still large enough for temporary jitter or short stalls,
- and small enough to fail predictably instead of growing forever.

### Overflow Policy

Prototype policy should be strict:

- on compressed-buffer budget overflow:
  - mark the stream unhealthy,
  - stop accepting new GOP work,
  - flush what is safe,
  - fail the recording explicitly.

Do not silently keep allocating.

## Latency Model

The hard latency cost is:

- the next independently assignable chunk begins at the next GOP boundary,
- plus any wait caused by ordered release of completed GOPs.

Example:

- `GOP = 60`
- `FPS = 60`
- GOP boundaries every `1.0 s`

That means:

- session B cannot start GOP 1 until frame 60 exists,
- if GOP 1 finishes before GOP 0, it still waits in RAM,
- so practical output latency often approaches one GOP duration in the simple
  ordered-release design.

This is why shorter GOPs should be tested first for any low-latency-sensitive
use case.

## Concrete Bandwidth Budget Example

To keep the transfer risk intuitive, compare one prepared-frame transfer
against one compressed-output stream for a representative large recording mode.

Example operating point:

- resolution: `4512 x 4512`
- pixel format at the encoder boundary: `NV12`
- frame rate: `60 fps`
- compressed target bitrate: `150 Mbps`

Prepared NV12 frame size:

- pixels per frame:
  - `4512 * 4512 = 20,358,144`
- bytes per frame for `NV12`:
  - `20,358,144 * 1.5 = 30,537,216 bytes`
  - about `30.5 MB`
  - about `29.1 MiB`

Prepared NV12 throughput at `60 fps`:

- `30,537,216 * 60 = 1,832,232,960 bytes/s`
- about `1.83 GB/s`
- about `1.71 GiB/s`

If cross-GPU transfer falls back to a host bounce path, the traffic becomes two
legs:

- device -> host,
- host -> device.

So the rough transfer load becomes:

- about `3.66 GB/s` of aggregate transfer traffic per remote-owned stream.

Now compare that with compressed output:

- `150 Mbps = 18.75 MB/s`

Practical reading:

- cross-GPU movement of prepared `NV12` frames is around two orders of
  magnitude larger than moving compressed packets,
- so PCIe / transfer topology is much more likely to matter on the prepared
  input side than on the final mux side.

This does not prove GOP splitting will fail on `A16`, but it does explain why
topology and peer-access measurement matter before assuming "same board" means
"no bandwidth concern".

## How To Check Peer Access From The Terminal

Three useful checks on a real target machine:

1. Topology view:

```bash
nvidia-smi topo -m
```

This shows how the GPUs are connected from the driver's perspective.

2. CUDA peer-access capability matrix:

```bash
nvcc -std=c++17 -O2 scripts/check_cuda_peer_access.cu -o /tmp/check_cuda_peer_access
/tmp/check_cuda_peer_access
```

That helper prints:

- all visible CUDA devices,
- whether `cudaDeviceCanAccessPeer()` reports peer access for each pair,
- and whether enabling peer access succeeds.

3. Real P2P bandwidth / latency, if CUDA samples are installed:

```bash
p2pBandwidthLatencyTest
```

The first two checks tell you whether peer access is even possible. The third
check tells you whether it is actually fast enough to matter.

Current note for `pancake0`:

- `p2pBandwidthLatencyTest` was not found on the path,
- CUDA demo binaries were present under `/usr/local/cuda/extras/demo_suite`,
  but that install did not include the P2P sample,
- so the local helper in `scripts/check_cuda_peer_access.cu` is the currently
  available capability check unless CUDA samples are installed later.

## Current Blockers And Resolution Plan

## Blocker A: implicit arrival-order timestamping

Resolution:

- add explicit packet timeline metadata to the writer path,
- or at minimum introduce a mux coordinator that guarantees strict ordered
  release before packets reach `FFmpegWriter`.

Recommended:

- do both.

## Blocker B: unbounded compressed-packet queue

Resolution:

- add writer queue bounds,
- add per-stream buffered-byte budgets,
- expose metrics:
  - buffered GOP count,
  - buffered compressed bytes,
  - oldest pending GOP age,
  - writer queue depth / bytes.

## Blocker C: no GOP-level state object

Resolution:

- add `GopRoutingInfo` and `PendingGop`,
- keep frame transport frame-oriented, but add a GOP coordinator above it.

## Blocker D: unknown cross-GPU transfer strategy

Resolution:

- prototype the simplest policy first:
  - preprocess on source GPU,
  - transfer prepared NV12 for remote GOPs,
  - host-bounce fallback if no peer path is available.
- measure whether transfer overhead erases the encode gain.

If it does, revisit:

- remote preprocess,
- raw-frame transfer,
- or staying single-GPU.

## Blocker E: direct-input complexity

Resolution:

- explicitly defer direct-input integration for GOP-splitting v1,
- revisit only after the correctness and throughput value of GOP splitting is
  proven on the copy path.

## Suggested Phased Implementation Plan

## Phase 0: Preconditions

- [ ] Confirm the target use case can tolerate added GOP-scale output latency.
- [ ] Pick one initial test operating point and one shorter GOP:
  - for example `GOP 15` or `GOP 30`.
- [ ] Add this design doc to the NVENC planning cross-links.

## Phase 1: Writer / Buffering Safety

- [ ] Add bounded queueing to `FFmpegWriter` or wrap it with a bounded writer
  coordinator.
- [ ] Add buffered-byte accounting and hard budgets.
- [ ] Add explicit metrics for compressed backlog and mux lag.
- [ ] Plumb explicit frame index / timestamp information through the writer API.

## Phase 2: GOP Abstraction

- [ ] Add GOP identity to the frame handoff path.
- [ ] Add `PendingGop` and `OrderedMuxCoordinator`.
- [ ] Keep one encoder session first and validate:
  - no ordering regressions,
  - no timestamp regressions,
  - clean flush / stop behavior.

This phase proves the new mux / buffering contract before cross-GPU complexity
is added.

## Phase 3: Multi-Session Correctness Bring-Up

- [ ] Add two encoder sessions but keep all work on one device first if needed
  for correctness testing.
- [ ] Alternate GOP ownership round-robin.
- [ ] Verify:
  - output order,
  - frame-id continuity,
  - keyframe sidecar correctness,
  - metadata continuity,
  - stop / flush correctness.

This phase is about orchestration correctness, not throughput.

## Phase 4: Cross-GPU Bring-Up

- [ ] Add remote-GPU staging slots for prepared NV12.
- [ ] Implement transfer path:
  - preferred local GPU copy if supported,
  - host-bounce fallback otherwise.
- [ ] Add metrics:
  - local vs remote GOP count,
  - cross-GPU transfer bytes,
  - transfer time,
  - encode time by session,
  - mux lag.

## Phase 5: Benchmark And Decision

- [ ] Compare against:
  - baseline single-GPU current copy path,
  - baseline direct-input single-GPU path,
  - GOP-splitting v1.
- [ ] Measure:
  - sustained FPS,
  - end-to-end output latency,
  - RAM usage,
  - failure rate,
  - transfer overhead.
- [ ] Decide whether GOP splitting is worth productionization or should remain
  experimental.

## Implementation Status As Of 2026-04-14

This section records the current state of the experiment branch
`exp/gop-split-a16`. The items below describe code that has been implemented in
that branch; they are not all merged into the main branch yet.

Implemented in the experiment branch:

- explicit CFR sample-index PTS plumbing so mux timestamps follow
  `recording_frame_id` rather than packet arrival order
- bounded writer-queue scaffolding and explicit split-GOP byte / count budgets
- GOP identity on the encode handoff:
  - `gop_index`
  - `frame_index_within_gop`
  - `is_last_frame_in_gop`
- ordered GOP release buffering so completed later GOPs wait until earlier GOPs
  are ready
- separation of `source GPU` ownership from `encode GPU` ownership in the
  recording path
- cross-GPU raw-frame staging support in the preprocess path when the encode
  target lives on another GPU
- per-camera `recording` config parsing / saving in camera JSON
- env overrides for split-GOP bring-up and debugging
- `RecordingIngress` as the acquisition-side seam for recording routing
- GOP-aware routing policy in `RecordingIngress`
- helper preprocess / helper hardware encoder instantiation for
  `hybrid_split`
- `SharedRecordingOutput` so primary and helper encoders feed one MP4 / metadata
  output path
- headless local mode (`orange_client --mode local`) using the same
  `ModernRecordingPipeline` path as the GUI recording pipeline

Current limitations:

- one-camera live split-GOP recording has now been validated headlessly on
  `pancake0` using `GPU5 + GPU6`, `hevc`, and the reduced pool overrides
  described below
- `pure_offload` is not fully wired as a real helper-target mode yet; helper
  targets are currently instantiated only for `hybrid_split`
- split-GOP is not a dedicated headless CLI flag yet; first runs should use
  camera JSON config or env overrides
- acquisition-facing stats are still primary-worker-centric; helper routing
  counters exist inside `RecordingIngress`, but they are not yet written into
  `recording_snapshot.json`
- direct-input is still intentionally out of scope for this path
- explicit validation is still missing for:
  - metadata continuity checks beyond file creation
  - keyframe sidecar correctness review
  - multi-camera and longer-duration artifact correctness

## First Live Bring-Up Findings As Of 2026-04-15

The first real headless bring-up on `pancake0` produced two important results.

What worked:

- the structured experiment-spec workflow is now real:
  - `fixed.recording` was accepted by the experiment runner
  - the override was applied to `CameraParams` before pipeline startup
  - the requested split-GOP block was preserved in `run_config.json`
  - the runtime recording strategy override was captured in
    `recording_snapshot.json`
- the updated sudo wrapper successfully launched the experiment-branch binary
  using:
  - `--orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client`
- the first `hevc` split-GOP run on camera `2010096` with `GPU1 + GPU2` proved
  that the helper path is real:
  - both hardware encoder workers started
  - the helper preprocess / helper encoder path on `GPU2` came up
  - acquisition held at roughly `60 fps`
  - each encoder worker ran at roughly `30 fps`
  - camera dropped-frame count stayed at `0`

What failed:

- `h264` at `4512x4512` failed even in plain `single_session` with
  `nvEncInitializeEncoder(... ) returned error 8`
- the same camera and topology succeeded far enough under `hevc` to show that
  the `h264` failure was not a split-GOP-specific regression
- the first `hevc` split-GOP run then failed in the GOP coordinator with:
  - `split_gop pending GOP count exceeded configured limit`

Current interpretation:

- for camera `2010096`, `h264` is the wrong first codec at full `4512x4512`
  resolution; use `hevc` for future full-resolution bring-up on this camera
- the multi-GPU helper path is now validated at a basic startup / dispatch
  level
- the next blocker is in pending-GOP accounting / release behavior, not in:
  - camera open
  - helper worker instantiation
  - wrapper / spec plumbing
  - A16 peer routing at startup

## Immediate Next Bring-Up Plan

The next step should be a headless-first smoke test, not more architectural
refactoring.

Recommended first run:

- one camera only
- one A16 `PIX` pair only, for example `GPU1 + GPU2`
- keep the camera / vision source GPU fixed on `GPU1`
- use:
  - `mode = split_gop`
  - `placement = multi_gpu`
  - `encoder_gpu_ids = [1, 2]`
  - `source_encoder_policy = hybrid_split`
  - `transfer_mode = raw`
  - `strict = true`
- for full-resolution `4512x4512` camera `2010096`, prefer:
  - `codec = hevc`
- use a short run first:
  - stream-only sanity check
  - then a short recording, for example `10-15 s`
  - with a modest GOP like `30` or `60`

Why headless first:

- the local headless client already builds `ModernRecordingPipeline`
- it removes display / YOLO / crop / pose from the experiment
- it gives a cleaner first signal on routing, encode, mux, and drain behavior

Artifacts to inspect after the first run:

- `Cam<serial>.mp4`
- per-camera metadata CSV
- `recording_snapshot.json`
- console logs for drain / overflow / helper-routing failures

What the first run must prove:

- the recording completes without crash or drain timeout
- helper-owned GOPs really dispatch to the helper path
- output ordering and CFR timestamping remain correct
- no writer-queue overflow or pending-GOP budget overflow occurs
- one combined MP4 is produced rather than split per-encoder files

Current state after the first live run:

- helper-owned GOP dispatch now has live startup evidence
- the immediate failing condition is pending-GOP budget overflow
- so the next code step should be:
  - instrument and fix the pending-GOP release / accounting path before
    widening the experiment

## Root Cause And Fix As Of 2026-04-15

The original split-GOP backlog failures turned out to be coordination bugs in
the shared output path, not a raw encode-throughput limit.

Observed failure pattern:

- healthy acquisition at roughly `60 fps`
- helper encoder startup was successful
- first backlog overflow appeared near frame `241` with `gop=60`
- overflow logs showed `next_gop_to_flush` moving forward while old GOP keys
  still reappeared in `pending_gops_`

The instrumentation added during bring-up showed two distinct problems:

- packet sample identity was being inferred from session-local NVENC output
  timing rather than from the submitted global recording-frame order
- GOP completion was being marked when the last input frame of a GOP was
  submitted, not when all packets for that GOP had actually been emitted by
  NVENC

Together, those bugs let the shared-output coordinator flush a GOP too early.
Late packets for that GOP would then recreate old GOP keys behind the flush
frontier, which made the backlog counter grow until it tripped the
`max_inflight_gops` guard.

The experiment-branch fix in commit `8b8a1e9` does three things:

- preserves real split-GOP overflow / peak metrics in
  `recording_snapshot.json`
- matches emitted packets back to the global submitted sample order inside each
  encoder worker
- marks a GOP complete only after emitted-packet counts catch up with submitted
  frame counts for that GOP

## Successful Validation As Of 2026-04-15

The split-GOP path is now validated for one headless camera run on
`pancake0`.

Validated setup:

- camera: `2010096`
- codec: `hevc`
- source / helper pair: `GPU5 + GPU6`
- policy:
  - `mode = split_gop`
  - `placement = multi_gpu`
  - `source_encoder_policy = hybrid_split`
  - `transfer_mode = raw`
- wrapper overrides:
  - `--acquire-work-entries-max 64`
  - `--encoder-entry-pool-size 32`

Successful confirmation run:

- experiment id: `2010096_split_gop_smoke_a16_pair_5_6_hevc_rerun10`
- artifacts:
  [run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_60](</home/jeremy/orange_data/exp/unsorted/2010096_split_gop_smoke_a16_pair_5_6_hevc_rerun10/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_60>)

Observed result:

- `842` frames received
- `0` camera drops
- `0` preprocess drops
- `0` encode failures
- no split-GOP backlog overflow
- clean shutdown without the earlier shared-output close error

Key final snapshot signals from the successful run:

- `current_backlog_gops = 0`
- `overflow_detected = false`
- `overflow_events = 0`
- `peak_backlog_gops = 2`

## Throughput Expansion As Of 2026-04-15

After the first `gop=60` correctness run passed, the next question was whether
the same `GPU5 + GPU6` split-GOP path could sustain higher frame rates at full
`4512x4512` resolution.

### Stable `80 fps` Result

Validated run:

- experiment id:
  `2010096_split_gop_smoke_a16_pair_5_6_hevc_80fps_rerun7`
- artifacts:
  [run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_80](</home/jeremy/orange_data/exp/unsorted/2010096_split_gop_smoke_a16_pair_5_6_hevc_80fps_rerun7/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_80>)

Observed result:

- `1122` frames received
- calculated acquisition rate `80.000465 fps`
- `0` camera drops
- `0` preprocess drops
- `0` encode failures
- `runs.csv enc_fps_mean = 74.7599`
- end-of-run steady-state split was effectively even:
  - `enc_fps_primary ~= 40 fps`
  - `enc_fps_helpers ~= 40 fps`

Interpretation:

- `80 fps` is a stable operating point on this A16 pair for camera `2010096`
  with:
  - `hevc`
  - `hybrid_split`
  - `raw` transfer
  - reduced acquisition / preprocess pools
- the lower `runs.csv enc_fps_mean` is mostly startup drag; the per-second
  pipeline CSV shows steady-state operation at about `80 fps`

### Failed `100 fps` Attempt With `gop=100`

Validated run:

- experiment id:
  `2010096_split_gop_smoke_a16_pair_5_6_hevc_100fps_rerun1`
- artifacts:
  [run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_100](</home/jeremy/orange_data/exp/unsorted/2010096_split_gop_smoke_a16_pair_5_6_hevc_100fps_rerun1/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_100>)

Observed result:

- acquisition initially reached about `98.39 fps`
- first pipeline sample showed:
  - aggregate `enc_fps ~= 54.68`
  - `enc_fps_primary ~= 54.68`
  - `enc_fps_helpers = 0.0`
- the helper lane did not contribute in the first sample because the whole
  first `100`-frame GOP still belonged to the primary lane
- the run then hit the intermittent camera transport failure:
  - repeated `EVT_CameraGetFrame Error, 11`
  - then `Socket operation failed`

Interpretation:

- this run did not prove that `100 fps` is impossible
- it did show that `gop=100` front-loads too much work onto the source /
  primary lane before the helper lane can contribute
- for `100 fps`, a shorter GOP is the more appropriate next step

### Successful `100 fps` Result With `gop=50`

Validated run:

- experiment id:
  `2010096_split_gop_smoke_a16_pair_5_6_hevc_100fps_gop50_rerun1`
- artifacts:
  [run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_50](</home/jeremy/orange_data/exp/unsorted/2010096_split_gop_smoke_a16_pair_5_6_hevc_100fps_gop50_rerun1/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_50>)

Observed result:

- `1402` frames received over `14.02 s`
- calculated acquisition rate `99.935234 fps`
- `0` camera drops
- `0` preprocess drops
- `0` encode failures
- `runs.csv enc_fps_mean = 100.438`
- `runs.csv enc_fps_primary_mean = 50.2256`
- `runs.csv enc_fps_helpers_mean = 50.2127`
- `pending_gop_buffer.current_backlog_gops = 0`
- `pending_gop_buffer.peak_backlog_gops = 2`
- `pending_gop_buffer.overflow_events = 0`

Interpretation:

- `100 fps` is viable on `GPU5 + GPU6` for this camera when the GOP is short
  enough for the helper lane to join early
- reducing GOP from `100` to `50` was the key change; with `gop=50`, helper
  contribution was visible by the second pipeline sample and the run settled
  into a roughly `50/50` split across the two encode lanes
- the shared-output backlog coordinator remained healthy throughout the
  successful run

### Successful `100 fps` Result With `gop=25`

Validated short run:

- experiment id:
  `2010096_split_gop_smoke_a16_pair_5_6_hevc_100fps_gop25_rerun1`
- artifacts:
  [run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25](</home/jeremy/orange_data/exp/unsorted/2010096_split_gop_smoke_a16_pair_5_6_hevc_100fps_gop25_rerun1/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25>)

Observed result:

- `1402` frames received over `14.02 s`
- calculated acquisition rate `99.935120 fps`
- `0` camera drops
- `0` preprocess drops
- `0` encode failures
- `runs.csv enc_fps_mean = 100.571`
- `runs.csv enc_fps_primary_mean = 50.1549`
- `runs.csv enc_fps_helpers_mean = 50.4161`
- `pending_gop_buffer.current_backlog_gops = 0`
- `pending_gop_buffer.peak_backlog_gops = 2`
- `pending_gop_buffer.overflow_events = 0`

Interpretation:

- `gop=25` preserved the same clean `100 fps` behavior as `gop=50`
- startup/helper engagement was slightly better than `gop=50`
- the short-run output size was effectively unchanged from `gop=50`
- for random-seek-heavy visualization, `gop=25` is the better tested setting

### Successful `100 fps gop=25` Soak

Validated soak run:

- experiment id:
  `2010096_split_gop_smoke_a16_pair_5_6_hevc_100fps_gop25_soak60_rerun1`
- artifacts:
  [run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25](</home/jeremy/orange_data/exp/unsorted/2010096_split_gop_smoke_a16_pair_5_6_hevc_100fps_gop25_soak60_rerun1/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25>)

Observed result:

- `6206` frames received over `62.06 s`
- calculated acquisition rate `99.985451 fps`
- `0` camera drops
- `0` preprocess drops
- `0` encode failures
- `runs.csv enc_fps_mean = 100.017`
- `runs.csv enc_fps_primary_mean = 49.9697`
- `runs.csv enc_fps_helpers_mean = 50.0473`
- `pending_gop_buffer.current_backlog_gops = 0`
- `pending_gop_buffer.peak_backlog_gops = 2`
- `pending_gop_buffer.overflow_events = 0`
- `nvidia_smi_dmon.status = completed`

Interpretation:

- `100 fps`, `hevc`, `gop=25`, `GPU5 + GPU6`, `hybrid_split`, and `raw`
  transfer is now validated beyond a short smoke test
- the source/helper split remained effectively `50/50` over a full-minute soak
- `gop=25` is the current best validated baseline for seek-friendly
  visualization on this host

### Snapshot Topology Metadata As Of 2026-04-15

The run snapshot now persists split-GOP topology metadata in two sections:

- `encoders.<serial>.recording_strategy.split_gop.topology.static`
- `encoders.<serial>.recording_strategy.split_gop.topology.runtime`

`topology.static` contains stable host / pair facts:

- source / primary / helper GPU ids
- per-GPU runtime info such as `pci_bus_id`
- `copy_paths[*].topology_class` from `nvidia-smi topo -m`
- `copy_paths[*].peer_access_capability`

`topology.runtime` contains run-local evidence:

- `source_to_helper_copy_samples_total`
- `copy_paths[*].peer_access_observation`

For the validated `GPU5 -> GPU6` runs, the snapshot now records:

- `topology.static.copy_paths[0].topology_class = PIX`
- `topology.static.copy_paths[0].peer_access_capability.can_access_peer = true`
- `topology.runtime.copy_paths[0].peer_access_observation.enabled = true`

Important nuance:

- runtime peer-access enablement may be inferred from successful helper-copy
  samples rather than directly persisted from the CUDA driver path
- when that happens, the snapshot marks it explicitly with:
  - `enable_attempted_inferred`
  - `enabled_inferred`
  - `observation_source = successful_source_to_helper_copy_samples`

Host-specific caution:

- `GPU3` is a poor experiment target on `pancake0` because it participates in
  the desktop stack (`Xorg` / `gnome-shell`), which blocks `gpu-reset` and
  makes stale-memory cleanup harder
- `GPU5 + GPU6` is currently the best clean `PIX` pair for repeatable A16
  bring-up on this host

If that run passes after the coordinator fix, the next comparison should be:

- baseline `single_session`
- `split_gop + multi_gpu + hybrid_split + raw`
- later, `split_gop + multi_gpu + hybrid_split + prepared_nv12`

Only after that should the experiment expand to:

- `pure_offload`
- more than one helper GPU
- direct-input interaction
- broader 4-camera scheduling policy

## Immediate Code Follow-Ups After First Bring-Up

Assuming the first headless split-GOP run works, the next code tasks should be:

- propagate helper-routing counters into `recording_snapshot.json` so the first
  runtime evidence is preserved in artifacts
- aggregate helper and primary queue / FPS / failure stats more explicitly in
  acquisition-side reporting
- validate helper-worker stop / flush ordering under repeated short runs
- add a dedicated headless CLI surface for recording strategy selection if the
  env-based bring-up proves useful
- decide whether the first transfer benchmark should stay `raw`-only or add
  `prepared_nv12` immediately after the first success

This keeps the next stage focused on measurement and artifact quality rather
than more speculative refactoring.

## Risks

## Risk 1: RAM growth and process instability

If muxing, disk I/O, or one encoder session stalls, completed GOPs can pile up.
This is the most immediate systems risk and must be controlled with explicit
budgets.

## Risk 2: Incorrect ordering or timestamps

Because the current writer contract is arrival-order based, it is easy to
produce valid-looking but semantically wrong output if packet release order is
not handled carefully.

## Risk 3: Transfer overhead erases any NVENC gain

If the remote-GPU transfer cost is too high, the added encode parallelism may
not improve total throughput meaningfully.

## Risk 4: Added latency is unacceptable

Even if throughput improves, GOP-boundary chunking and ordered release may make
the feature a poor fit for low-latency recording needs.

## Risk 5: Failure handling becomes more complex

Partial GOP completion, one-session failure, and cross-GPU teardown introduce
new stop / drain edge cases that do not exist in the current single-session
path.

## Risk 6: Direct-input interaction becomes a trap

Trying to solve direct-input, GOP scheduling, cross-GPU transport, and muxing
all at once would make the feature much harder to debug. This is why direct
input should stay out of v1.

## Future GPU Output, GPU Muxing, And GPUDirect

This is explicitly not priority for the first GOP-splitting implementation, but
it is worth documenting now.

## What The Repo Already Has

The repo already contains NVIDIA's video-memory output wrapper:

- `src/NvEncoder/NvEncoderOutputInVidMemCuda.h`
- `src/NvEncoder/NvEncoderOutputInVidMemCuda.cpp`

That means Orange already has a local starting point for "keep compressed
bitstream on GPU longer" if we choose to explore it later.

## Future Goal A: GPU Bitstream Output + CPU Mux

Possible later design:

1. Use `NvEncoderOutputInVidMemCuda` so encoded bitstream lands in GPU memory.
2. Add a dedicated transfer stage to move compressed packets into pinned host
   buffers.
3. Keep the existing CPU-side FFmpeg mux / file-write path.

This can still be useful because it:

- moves the device-to-host copy out of the encode worker hot path,
- makes output buffering explicit,
- and may pair well with later asynchronous transfer tuning.

But it does not remove CPU muxing.

## Future Goal B: End-To-End GPU-Aware Output

A much larger future project would be:

- GPU-resident bitstream,
- GPU-aware downstream sink,
- container / storage path that does not immediately force host ownership.

That could mean one of:

- a GPU-aware network sender,
- a GPU-aware downstream CUDA consumer,
- or a GPUDirect Storage style path for storage I/O.

Important caution:

- this is a separate project from GOP splitting,
- MP4 muxing in the current app is CPU-side today,
- and there is no evidence yet that output-side CPU work is the primary
  bottleneck for the target workloads.

So the recommendation is:

- do not couple GPU-output or GPUDirect ambitions to GOP-splitting v1,
- only pursue them if later profiling shows packet transfer or CPU mux is a
  real limiter.

## Recommendation

Recommended project order:

1. Finish single-GPU direct-input measurement and stabilization.
2. Make the writer / buffering path safe and explicit.
3. Add GOP abstraction and ordered muxing on the existing host-bitstream path.
4. Prove multi-session correctness before cross-GPU throughput work.
5. Only then evaluate whether GOP splitting is worth carrying forward on the
   target hardware.

If the main requirement is maximum single-stream speed with minimal additional
latency, newer Ada hardware with SFE is still the cleaner architectural path.
If the main requirement is "get more throughput out of current hardware even at
the cost of more buffering and orchestration", GOP splitting on Orange is a
reasonable experimental feature.
