# NVENC Direct Input Plan

Date: 2026-03-31
Scope: summarize the current NV12/NVENC findings, document why the current
recording path still pays an extra copy cost, and outline the recommended
direct-registration implementation before coding.

## Short Conclusion

- Keeping `NV12` as the encoder-facing format is reasonable for the current
  8-bit 4:2:0 recording path.
- The bigger efficiency issue in the current code is not "using NV12" by
  itself. The bigger issue is that we prepare an NV12 frame in CUDA and then
  copy that frame again into NVENC's internal input buffer.
- The preferred next step is to let preprocess write directly into a ring of
  CUDA NV12 surfaces that are registered with NVENC, then encode from those
  registered surfaces without the extra device-to-device copy.

## NVIDIA-Side Findings

- NVENC accepts both YUV and RGB input formats. The NVIDIA Video Codec SDK
  programming guide explicitly describes NVENC as taking YUV/RGB input and
  recommends querying supported input formats instead of assuming a single
  universal format.
- `NV12` is still a good practical default for this pipeline because the
  current recordings are standard 8-bit 4:2:0 H.264/HEVC outputs.
- This does not mean "convert everything to NV12 as early as possible" is the
  best design rule for the codebase. The better rule is:
  - keep the pipeline in the most convenient format until the encoder boundary,
  - then minimize conversions and copies at that boundary.

Primary source:
- NVIDIA Video Codec SDK, NVENC Video Encoder API Programming Guide:
  https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html

## Camera-Native YUV Or RGB Output

If the camera can emit a color format directly, that may be better than the
current `RAW Bayer -> RGBA -> NV12` path, but only if it reduces total system
cost rather than just moving work from one stage to another.

The practical tradeoff is:

- `RAW Bayer`
  - lowest capture-side bandwidth,
  - local pipeline keeps full control over debayer and color handling,
  - but we pay the GPU debayer and color-conversion cost ourselves.
- camera-native `RGB`
  - can remove the local debayer stage,
  - but often increases transport and DMA bandwidth substantially,
  - so it is only attractive if capture bandwidth has plenty of headroom.
- camera-native `YUV`
  - is attractive when it is already close to the encoder target format,
  - but less attractive when it still needs significant local conversion.

Recommended policy:

- If the camera can output `NV12` or `YUV420` at the required resolution and
  frame rate with acceptable image quality, that is the most attractive
  recording-path input format.
- If the camera can output `RGB`, only prefer it when the transport budget is
  comfortably large enough to absorb the higher pixel bandwidth. In that case,
  it is worth benchmarking direct `ARGB`/`ABGR` input to NVENC against local
  `RGB/RGBA -> NV12` conversion.
- If the camera can only output `YUV422`, it may still reduce work relative to
  RGB, but it does not remove the need to convert into the encoder's preferred
  4:2:0 path for standard H.264/HEVC recording.
- If the camera's processed color output comes with baked-in ISP behavior that
  reduces image-control or reproducibility, `RAW Bayer` may still be the better
  source format even if it costs more GPU work locally.

Current bias for this codebase:

- keep `RAW Bayer` as the default source assumption unless the camera can emit
  `NV12`/`YUV420` natively at the required operating point,
- treat camera-native `RGB` as a benchmark candidate, not an automatic win,
- prefer formats closest to the encoder boundary rather than converting early
  without a measured reason.

## Frame Ownership: Smart Pointer vs Pooled Slot

For this pipeline, the important optimization target is not "use a smart
pointer". The important target is "avoid copies while keeping frame lifetime
correct across asynchronous workers".

What a smart pointer helps with:

- shared ownership semantics,
- automatic release when the last owner goes away,
- simpler local API shape in non-hot paths.

What a smart pointer does not help with:

- reducing GPU bandwidth by itself,
- changing whether camera-owned buffers can be safely shared across multiple
  asynchronous consumers,
- expressing "return this frame to the camera SDK only after all consumers are
  finished and the relevant CUDA work has completed".

The current code already uses a custom shared-ownership model:

- `WORKER_ENTRY` carries the frame pointer and metadata,
- acquisition sets a dispatch reference count based on active consumers,
- each worker decrements that count when finished,
- the last consumer returns the slot to the pool or requeues the camera frame.

That means the current design is already solving the same core ownership
problem that a per-frame `shared_ptr` would solve.

Why pooled slots are a better fit here than per-frame `shared_ptr`:

- this is a very hot path with large frames and high frame rates,
- per-frame heap/control-block churn is the wrong direction,
- generic smart pointers do not naturally model camera-buffer requeue rules,
- generic smart pointers do not naturally model pooled CUDA memory reuse,
- explicit slot ownership maps better to fixed camera/encoder ring-buffer
  systems.

Recommended policy:

- keep pooled frame slots for hot-path frame transport,
- keep explicit event-based release and camera requeue behavior,
- prefer intrusive or explicit slot reference counting over general-purpose
  `shared_ptr` for these frame objects,
- if a cleaner API is desired later, wrap pooled slots in a lightweight
  `FrameHandle` abstraction rather than moving the hot path to heap-allocated
  smart-pointer ownership.

## Current Practice Assessment

Overall, the current pipeline follows the right performance-oriented design
principles for very large frames at high frame rates.

What is already following good practice:

- frame memory is preallocated and reused instead of heap-allocating per frame,
- frame ownership is shared across workers without automatically forcing a copy,
- the acquisition path already takes a direct-pointer fast path when there is a
  single consumer and the camera buffer is already on the correct GPU,
- copies are mainly used when the frame needs an independent lifetime across
  multiple asynchronous consumers,
- CUDA events are used to coordinate reuse and camera-buffer requeue behavior.

This means the current implementation is already aligned with the main
high-throughput goals:

- avoid unnecessary allocation churn,
- avoid unnecessary copies when single-consumer direct access is safe,
- make ownership and reuse explicit rather than implicit.

Where the current implementation is weaker:

- lifetime handling is manual and distributed across several workers,
- `WORKER_ENTRY` mixes transport payload, ownership state, camera SDK requeue
  state, and synchronization state,
- the current "more than one consumer means ring copy" rule is simple and safe
  but coarse,
- the design is efficient but somewhat brittle because correctness depends on
  multiple workers following the release protocol exactly.

Practical reading:

- the current system is using mostly good hot-path practices,
- the main risk is maintainability and correctness under future changes,
- the next abstraction improvement should be a clearer pooled frame-slot or
  `FrameHandle` model,
- the next throughput improvement should target avoidable copies such as the
  current NV12 staging-to-NVENC copy, not a migration to general-purpose smart
  pointer ownership.

## Current Runtime Path In This Repo

Active GUI recording currently uses:

`EncoderPreprocessWorker -> EncoderHwWorker -> FFmpegWriter`

For color cameras, the active path is:

1. Debayer Bayer RAW to RGBA.
2. Optionally resize RGBA.
3. Convert RGBA to NV12 in CUDA.
4. Hand the prepared NV12 frame to `EncoderHwWorker`.
5. Copy that prepared NV12 frame into NVENC's internal input buffer.
6. Call `EncodeFrame()`.
7. Push encoded packets to the writer thread.

For mono cameras, the path is:

1. Copy luma.
2. Fill UV with a neutral plane.
3. Hand the prepared NV12 frame to `EncoderHwWorker`.
4. Copy the prepared NV12 frame into NVENC's internal input buffer.
5. Encode.

The important point is that preprocess already produces encoder-ready NV12, but
the hardware worker still performs an additional copy into NVENC-owned memory.

## Current Code Evidence

- `EncoderPreprocessWorker` prepares NV12 in its own buffer pool:
  - color path: `RGBA -> NV12`
  - mono path: luma copy + neutral UV fill
- `EncoderHwWorker` creates `NvEncoderCuda(..., NV_ENC_BUFFER_FORMAT_NV12)`.
- `EncoderHwWorker` then calls `NvEncoderCuda::CopyToDeviceFrame(...)` before
  `EncodeFrame()`.
- `orange.cpp` currently creates a temporary encoder instance just to discover
  the internal NVENC pitch for those staging buffers.

This means the active implementation is built around:

- preprocess-owned NV12 staging surfaces,
- NVENC-owned input surfaces,
- one extra copy between them.

## Wrapper Findings

The local NVENC wrapper already has most of the primitives needed for direct
registration.

What it already supports:

- registering external CUDA device pointers via `RegisterInputResources(...)`,
- mapping registered resources on encode,
- unmapping them after encoded packet retrieval.

What it does not expose cleanly to the application today:

- an external-input mode where the app supplies the encoder input ring instead
  of `NvEncoderCuda` allocating its own internal CUDA frames.

Right now `CreateEncoder()` always leads to internal input buffer allocation,
and the app only sees the wrapper through the "get next internal frame, copy
into it, then encode" model.

## Recommended Direction

Use direct registration for the CUDA-prepared NV12 buffers.

The target shape is:

1. Allocate a small ring of CUDA NV12 surfaces in app code.
2. Register that ring with NVENC once at encoder startup.
3. Have preprocess write directly into one registered surface per frame.
4. Submit the corresponding registered surface to encode.
5. Recycle the surface only after NVENC has finished with it.

This keeps the current high-level worker split while removing the explicit
`CopyToDeviceFrame(...)` step.

## Proposed Implementation Shape

## 1. Add External-Input Support To The Local Wrapper

Introduce a local extension to the bundled `NvEncoderCuda` wrapper that allows
the application to use externally managed CUDA input buffers.

The cleanest shape is one of:

- a small subclass exposing `RegisterExternalInputBuffers(...)`, or
- a wrapper flag such as `use_external_input_buffers`, plus a public method to
  register those buffers after encoder initialization.

Requirements:

- skip internal CUDA input allocation when external mode is enabled,
- still allocate normal bitstream output buffers,
- preserve the wrapper's existing map/unmap behavior.

## 2. Turn The Preprocess Pool Into The Encoder Input Ring

Today `EncoderPreprocessWorker` owns a large pool of generic prepared NV12
buffers. In the direct-input design, that pool should become the registered
encoder input ring.

Implications:

- pool entries need stable slot identity,
- pool size should match NVENC's true buffer count instead of an arbitrary
  `120`,
- the slot identity should travel with the work item into `EncoderHwWorker`.

`ENCODER_WORKER_ENTRY` should grow from "pointer + timestamps + event" into
something closer to:

- slot index,
- surface pointer,
- pitch,
- timestamps,
- preprocess completion event.

## 3. Allocate External Surfaces With Real Pitch

The current code asks a temporary encoder for pitch and then allocates staging
buffers around that number. That works for the copy path, but it is not the
best ownership model for direct input.

Recommended change:

- allocate the external NV12 ring with `cudaMallocPitch` or `cuMemAllocPitch`,
- treat that returned pitch as the authoritative pitch for both preprocess and
  registration,
- register those exact pitched surfaces with NVENC.

This would remove the temporary "create encoder just to read pitch" helper
logic.

## 4. Keep Lifetime Rules Explicit

This is the most important correctness constraint.

Once a registered surface has been submitted to NVENC, it cannot be reused
until the wrapper has advanced far enough to unmap that input resource.

The wrapper currently unmaps old mapped inputs after encoded output retrieval.
That means:

- preprocess must not immediately reclaim a slot after `EncodeFrame()`,
- the hardware worker must return the slot to the free pool only after the
  corresponding registered resource is no longer in use,
- the safe recycle point should be tied to the wrapper's output-delay progress,
  not just to local stream completion.

This is the main reason the change is mostly a buffer-lifecycle refactor rather
than a codec refactor.

## 5. Keep A Fallback Path During Bring-Up

The direct-input mode should be guarded during rollout.

Recommended:

- keep the current copy path available behind a runtime flag or compile-time
  switch while validating the new path,
- compare correctness and throughput between:
  - current `prepare NV12 -> copy -> encode`,
  - direct registered NV12 input.

## What This Change Does Not Require

- It does not require changing the file writer.
- It does not require changing bitrate, GOP, or packet handling.
- It does not require changing the mono/color control flow structure.
- It does not require switching away from `NV12` immediately.

## Risks And Validation Needs

## Primary Risk: Slot Reuse Too Early

If a registered input surface is returned to preprocess before NVENC has fully
released it, the next frame can overwrite in-flight encoder input.

This is the biggest risk and should drive the design.

## Secondary Risk: Pitch / Registration Mismatch

If preprocess, registration, and encode submission disagree about pitch or
surface geometry, the resulting output can be corrupted or subtly wrong.

## Color / Range Risk

The current local RGBA-to-NV12 kernel uses fixed BT.601-style coefficients. If
recorded output should instead be BT.709 or use different range assumptions,
that remains a separate color pipeline question. Direct registration does not
change that by itself.

## Operational Validation Needed

- long recording runs without slot corruption,
- no regressions in mono mode,
- no regressions in downsample mode,
- correct drain/stop behavior,
- lower GPU copy overhead or improved throughput under the same workload.

## Throughput Limit Hypothesis

It is plausible that the observed encode ceilings are at least partly real
single-session NVENC hardware limits, not only software inefficiency.

This section summarizes the current reasoning for two observed behaviors:

- full-resolution recording appearing to plateau around `60 fps`,
- `2.8 MP @ 400 fps` failing to keep up on anything slower than `p1 ll`.

## Official NVENC Performance Guidance

NVIDIA's NVENC Application Note publishes indicative encode performance in
frames per second for different architectures, codecs, presets, and tuning
modes.

Important constraints from NVIDIA:

- published performance is per NVENC engine,
- multiple NVENC engines increase aggregate multi-session throughput,
- a single encoding session does not automatically exceed per-engine limits,
- on Ada, a single session can only go beyond a single NVENC engine when Split
  Frame Encoding is explicitly used for supported codecs.

Primary source:
- NVIDIA NVENC Application Note:
  https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html

The published tables are measured at:

- `1920x1080`,
- `YUV 4:2:0`,
- `8-bit`,
- highest video clocks for the listed reference GPUs.

That means they are not exact predictions for this project, but they are useful
for rough pixel-throughput budgeting.

## Local Hardware Context

Known GPUs in the target environment:

- `RTX A6000`
  - official NVIDIA workstation page identifies it as an Ampere GPU.
- `A16`
  - official NVIDIA product material identifies it as an Ampere-based quad-GPU
    board.
  - official A16 datasheet lists `4 NVENC / 8 NVDEC` per board.

Relevant implication:

- `RTX A6000` should be reasoned about using Ampere-era NVENC numbers.
- `A16` also uses Ampere-era NVENC capability assumptions, but aggregate board
  throughput is distributed across multiple physical GPUs on the board rather
  than making one single encode session faster by itself.

Official sources:

- RTX A6000 product page:
  https://www.nvidia.com/en-us/products/workstations/rtx-a6000/
- A16 product page / datasheet:
  https://www.nvidia.com/en-sg/data-center/products/a16-gpu/
  https://images.nvidia.com/content/Solutions/data-center/vgpu-a16-datasheet.pdf

## Rough Pixel-Rate Budgeting

Useful approximation:

- `pixel_rate ~= width * height * fps`

For the cases discussed:

- `4512 x 4512 @ 60 fps`
  - `20,358,144 pixels/frame`
  - about `1.22 Gpixels/s`
- `2.8 MP @ 400 fps`
  - about `1.12 Gpixels/s`

Using the official Ampere per-engine NVENC tables as directional guidance:

- HEVC `P1 CBR LL`: `943 fps @ 1080p`
  - about `1.95 Gpixels/s`
- HEVC `P3 CBR LL`: `467 fps @ 1080p`
  - about `0.97 Gpixels/s`
- H.264 `P1 CBR LL`: `868 fps @ 1080p`
  - about `1.80 Gpixels/s`
- H.264 `P3 CBR LL`: `613 fps @ 1080p`
  - about `1.27 Gpixels/s`

Inference from those numbers:

- `2.8 MP @ 400 fps` is already above the official Ampere single-engine
  `HEVC P3 LL` budget and close enough to `H.264 P3 LL` that any local
  overhead can push it over the edge.
- `4512 x 4512 @ 60 fps` is also above the official Ampere single-engine
  `HEVC P3 LL` budget, but still within rough `P1` territory.
- That matches the observed pattern that `p1 ll` can survive while `p3` falls
  behind.

This is not proof by itself, but it is strong evidence that the observed
behavior is consistent with real single-session NVENC throughput limits.

## Why Pool Exhaustion Happens

When input FPS exceeds sustained encode FPS, the preprocess buffer pool is
expected to drain over time.

Current code facts:

- preprocess owns a fixed prepared-frame pool,
- the pool size is currently `120`,
- frames are only returned after the hardware encoder worker finishes with them.

So if:

- input rate = `400 fps`,
- sustained encode rate = `300 fps`,

then the deficit is `100 fps`, and a `120`-slot pool can be exhausted in about
`1.2 seconds`.

That means encoder-pool exhaustion is consistent with a steady-state throughput
deficit and does not automatically imply a leak.

## Repo-Specific Overheads That Lower The Practical Ceiling

Even if NVENC is near the physical limit, the current implementation can make
the effective ceiling lower than the raw NVENC hardware tables suggest.

Known contributors:

- extra device-to-device copy from prepared NV12 into NVENC's internal input
  frame,
- preprocess CUDA work before encode,
- default VBR quality path still enabling AQ and Temporal AQ,
- queueing and output-delay effects in the wrapper,
- possible GPU video-clock differences from NVIDIA's reference measurements.

This means the true observed ceiling in this repo can be lower than the
published silicon-only directional numbers, even when the dominant bottleneck is
still fundamentally NVENC throughput.

## Current Reading

Current best explanation:

- the encode limits being observed are likely a mix of:
  - real single-session NVENC throughput limits,
  - preset-driven quality/performance tradeoffs,
  - avoidable local overhead such as the extra NV12 copy.

Most likely conclusions:

- `4512x4512 @ 60 fps` may genuinely be near the single-session limit on an
  Ampere GPU depending on codec and exact clocks,
- `2.8 MP @ 400 fps` falling behind at `p3 ll` is fully consistent with
  official Ampere HEVC guidance,
- removing the extra copy and testing the cheapest RC path are still worthwhile,
  but may not fully remove the limit,
- aggregate throughput can scale by spreading sessions across available GPUs,
  but a single encode session will still behave like a single-engine workload
  unless special features such as Split Frame Encoding are available and used.

## NVIDIA Guidance Received

External guidance from NVIDIA support indicated the following direction:

- newer Ada GPUs such as `L40S` and `L4` are better fits for the hardest
  single-stream encode cases,
- `L40S` provides `3x NVENC`,
- `L4` provides `2x NVENC`,
- both support Split Frame Encoding (SFE),
- NVIDIA has also developed a sample approach that can split GOP work across
  multiple GPUs, which could be applied to `A16`, but at the cost of added
  latency.

This guidance is consistent with the official NVIDIA documentation.

## What That Means For Current Hardware

Current local hardware:

- `RTX A6000`
- `A16` boards

These are Ampere-generation parts, not Ada-generation parts.

Implication:

- they can provide good aggregate encode throughput,
- but they do not solve the hardest single-session throughput problem the same
  way an Ada GPU with multi-NVENC Split Frame Encoding can.

In practical terms:

- `RTX A6000` is still a strong single-stream candidate within normal
  single-session limits,
- `A16` is better thought of as a high aggregate-capacity platform because it
  contains multiple physical GPUs on one board,
- neither should be assumed to provide the same single-session acceleration path
  that Ada SFE provides.

## What Ada Changes

Official NVIDIA SDK docs state that multi-NVENC Split Frame Encoding is
available for `HEVC` and `AV1`.

Important details:

- SFE partitions a single input frame into horizontal strips,
- those strips are encoded simultaneously by multiple NVENC engines,
- this improves single-session encoding speed,
- this comes with some quality degradation,
- this does not increase aggregate throughput when multiple sessions already
  fully utilize the NVENC engines,
- implicit SFE requires:
  - 2 or more NVENCs on the GPU,
  - frame height >= `2112` for HEVC or >= `2048` for AV1,
  - compatible preset/tuning combinations.

Official docs also state:

- `L4` exposes `2 NVENC`,
- `L40S` exposes `3 NVENC`.

That makes the NVIDIA guidance highly relevant for the current workload:

- `4512x4512` frames satisfy the frame-height condition for SFE,
- low-latency `P1-P4` presets are compatible with implicit SFE for HEVC/AV1,
- therefore a single hardest stream that is near the single-engine limit on
  Ampere could gain meaningful headroom on `L4` or `L40S`.

Primary sources:

- Split Frame Encoding in the NVENC Video Encoder API Programming Guide:
  https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html#multi-nvenc-split-frame-encoding-in-hevc-and-av1
- L4 product specifications:
  https://www.nvidia.com/en-eu/data-center/l4/
- L40S product specifications:
  https://www.nvidia.com/en-eu/data-center/l40s/

## Why GOP Splitting Across GPUs Is Different

The NVIDIA suggestion about splitting GOP work across multiple GPUs should be
treated as a different class of solution from Ada SFE.

Ada SFE:

- accelerates one encode session within the encoder hardware itself,
- is intended to raise single-session encode speed,
- is available only for HEVC/AV1 and only on supported multi-NVENC GPUs.

Multi-GPU GOP splitting:

- spreads work across separate GPUs over time or GOP boundaries,
- can increase usable throughput on platforms like `A16`,
- requires more buffering, orchestration, and output handling,
- is likely to increase end-to-end latency,
- is therefore a poor fit when low latency is a hard requirement.

Practical reading:

- if the main goal is maximum single-stream speed with the least extra
  orchestration, Ada SFE is the cleaner path,
- if the main goal is aggregate throughput and some additional latency is
  acceptable, multi-GPU GOP splitting can be a viable architecture on `A16`.

## Should We Try Multi-GPU GOP Splitting On A16

Answer: yes, but only after the lower-risk single-GPU work is measured and
exhausted.

Why it is plausible in this codebase:

- the current encoder config already uses a GOP structure that is friendly to
  segment boundaries,
- default GOP is effectively `1 second` unless overridden,
- B-frames are disabled,
- IDR period matches GOP length,
- SPS/PPS is repeated at GOP boundaries.

That means GOP-aligned partitioning and later stitching is much easier than it
would be with long inter-GOP dependencies or frame reordering.

Why it should not be the first implementation target:

- it adds orchestration complexity,
- it adds buffering and therefore latency,
- it does not remove the current local overheads,
- it is harder to validate than fixing the current single-GPU path.

Latency concern:

- with the current default GOP policy, GOP splitting implies buffering roughly
  one GOP at a time,
- at `60 fps`, that is about `1 second` of GOP latency,
- at `400 fps`, that is also about `1 second` of GOP latency but with a much
  larger per-GOP frame count.

That makes it a poor first choice when low-latency availability of encoded
output matters.

Recommended policy:

- for low-latency recording:
  - prioritize direct registered NV12 input,
  - cheaper RC modes,
  - codec/preset benchmarking,
  - best single-GPU placement (`RTX A6000` first).
- for maximum archival or batch-throughput recording where extra latency is
  acceptable:
  - multi-GPU GOP splitting on `A16` is worth trying if single-GPU cleanup
    still cannot sustain the required operating point.

If pursued later, one design adjustment should be considered first:

- shorten the GOP from the current default before evaluating multi-GPU GOP
  splitting, so the added buffering penalty is less severe.

## Benchmarking Plan

The goal of benchmarking is to separate:

- true NVENC single-session throughput limits,
- preset / codec / rate-control cost,
- local preprocess cost,
- local copy-path cost.

The shortest useful plan is not a giant matrix. It is a staged narrowing plan.

## Metrics To Capture For Every Run

For each benchmark run, capture at minimum:

- configured resolution,
- configured input FPS,
- actual sustained encode FPS,
- sustained preprocess FPS if available,
- sustained acquisition FPS,
- dropped-frame count,
- preprocess free-buffer count trend,
- encode failure count,
- codec,
- preset,
- tuning,
- rate-control mode,
- GPU id,
- whether the run used the current copy path or direct registered input.

If possible, also capture:

- GPU video clocks,
- GPU encoder utilization,
- GPU SM utilization,
- average encode-stage time per frame,
- average preprocess-stage time per frame.

## Benchmark Guardrails

- Run one encode session at a time first.
- Pin each run to one known GPU.
- Do not mix display, YOLO, and recording unless the purpose of the test is to
  measure multi-consumer contention.
- Warm up each run before reading steady-state numbers.
- Run long enough to observe whether the preprocess pool drains over time rather
  than relying on a short burst.

Recommended baseline duration:

- `30` seconds for quick triage,
- `2-5` minutes for confirmation.

## Phase 1: Establish The Encode Ceiling With The Current Path

Use the current shipping path first so results are comparable.

Primary test cases:

1. `4512x4512 @ 60 fps`
2. `2.8 MP @ 400 fps`

For each case, test:

- codec: `h264`, `hevc`
- preset/tuning:
  - `p1 ll`
  - `p3 ll`
- rate control:
  - cheapest supported low-latency mode,
  - current default mode used in practice

Interpretation:

- if `h264 p3 ll` works but `hevc p3 ll` does not, the bottleneck is likely
  mostly real NVENC encode complexity rather than preprocess alone,
- if both fail at nearly the same point, local overhead is more suspicious,
- if `p1 ll` survives and `p3 ll` falls behind, that strongly supports a real
  preset-driven NVENC throughput limit.

## Phase 2: Remove Non-Encoder Contenders

Repeat the same runs with:

- display disabled,
- YOLO disabled,
- any nonessential worker fan-out disabled.

Interpretation:

- if throughput improves materially, the system is not purely encoder-limited,
- if throughput barely changes, the encoder path is the dominant bottleneck.

## Phase 3: Separate Preprocess From Encode

Add or enable per-stage timing for:

- preprocess worker,
- hardware encoder worker.

Goal:

- determine whether preprocess time or encode time dominates wall-clock
  throughput for the failing runs.

Interpretation:

- if preprocess time is already near the frame budget, NVENC is not the only
  problem,
- if preprocess is comfortably under budget but encode falls behind, the limit
  is more likely true NVENC throughput,
- if preprocess is under budget but encode only barely over budget, the extra
  copy path becomes a high-priority candidate.

## Phase 4: Quantify The Copy Path Cost

After the direct registered-input path exists, rerun a reduced matrix:

- `hevc p1 ll`
- `hevc p3 ll`
- `h264 p1 ll`
- `h264 p3 ll`

for:

- current `prepare NV12 -> copy -> encode`,
- direct `prepare NV12 -> registered input -> encode`.

Interpretation:

- if direct input buys little, the current limit is mostly hard NVENC capacity,
- if direct input buys a noticeable margin, the old copy path was materially
  reducing the usable ceiling,
- if direct input changes pool-drain behavior without changing encode FPS much,
  the pipeline may still benefit from better latency and lower burst pressure.

## Phase 5: Validate Rate-Control Cost

The current code path can still enable AQ and Temporal AQ in non-CQP paths.
That means "low latency" is not necessarily the absolute cheapest encode path.

Benchmark at least:

- current default recording RC path,
- CQP path,
- any lowest-overhead low-latency RC configuration the code supports.

Interpretation:

- if CQP or a stripped-down LL path materially increases throughput, the limit
  is not only preset complexity,
- if differences are small, preset/codec/hardware capacity dominate more than
  RC policy.

## Phase 6: Cross-GPU Placement

Since the environment contains an `RTX A6000` and `A16` GPUs, test the same
single-session workload on each target GPU independently.

Goals:

- determine whether the `RTX A6000` gives meaningfully better single-session
  headroom,
- determine whether the `A16` is better used as aggregate multi-stream capacity
  rather than for a single hardest stream.

Interpretation:

- if single-session results are similar, then the main win from the `A16`
  environment may come from distributing streams across GPUs,
- if one GPU class is clearly better for the hardest stream, reserve that GPU
  for the highest-resolution or highest-FPS session.

## Minimum High-Value Matrix

If time is limited, run only these first:

1. `4512x4512 @ 60`, `hevc p1 ll`, current path
2. `4512x4512 @ 60`, `hevc p3 ll`, current path
3. `2.8 MP @ 400`, `hevc p1 ll`, current path
4. `2.8 MP @ 400`, `hevc p3 ll`, current path
5. `2.8 MP @ 400`, `h264 p3 ll`, current path
6. Repeat the failing cases with direct registered input once available

This set should quickly answer:

- whether the behavior is mostly HEVC-specific,
- whether `p3` is simply beyond the practical single-session budget,
- whether the extra copy is meaningfully reducing headroom.

## Expected Good Outcome

At the end of this plan, the desired state is:

- we know whether the current limit is mostly hardware or mostly software,
- we know how much headroom direct registered input actually buys,
- we know whether `RTX A6000` or `A16` should carry the hardest streams,
- we know which preset/codec combinations are realistic for each operating
  point before doing larger implementation work.

## Decisions Worth Making Before Coding

1. Wrapper API shape:
   - extend the bundled wrapper locally, or
   - add a thin app-local adapter around it.

2. External ring ownership:
   - keep ownership in `EncoderPreprocessWorker`,
   - or move ownership to `EncoderHwWorker` and lease slots to preprocess.

3. Pool sizing rule:
   - use exactly `encoder_buffer_count`,
   - or use `encoder_buffer_count + small_margin`.

4. Rollout strategy:
   - direct input only for NV12 first,
   - or test RGB input to NVENC separately afterward.

5. Bring-up safety:
   - keep copy-path fallback enabled until direct input passes long-run testing.

## Recommendation

Recommended order:

1. Extend the local NVENC wrapper to support externally registered CUDA input
   buffers.
2. Change the preprocess buffer pool into a true registered NV12 input ring.
3. Remove the explicit `CopyToDeviceFrame(...)` step from the hardware worker.
4. Add minimal slot-lifecycle telemetry so buffer reuse is observable.
5. Keep the old copy path available until direct registration is proven stable.

This is the highest-signal next step if the goal is to reduce avoidable encode
pipeline overhead without rewriting the whole recording architecture.
