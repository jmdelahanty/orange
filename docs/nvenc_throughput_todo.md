# NVENC Throughput TODO

Date: 2026-03-31
Scope: implementation and validation plan for raising practical recording
throughput, distinguishing low-risk single-GPU improvements from higher-risk
multi-GPU options.

See also:

- `docs/nvenc_direct_input_plan.md`
- `docs/nvenc_benchmark_runsheet.md`
- `docs/codec_quality_evaluation_protocol.md`
- `docs/pre_encoder_reference_capture_plan.md`
- `docs/pre_encoder_reference_capture_todo.md`
- `docs/experiment_runner_plan.md`
- `docs/headless_experiment_backend.md`
- `docs/headless_cli_design.md`
- `docs/color_recording_pipeline.md`
- `docs/encoding_alternatives.md`

## Goal

Reach the highest sustainable full-resolution and high-FPS recording throughput
possible while preserving correctness, understanding where the real bottleneck
is, and avoiding premature complexity.

## Priority Order

Implementation priority should be:

1. Measure the current path cleanly.
2. Improve the current single-GPU path.
3. Re-measure and decide whether the remaining limit is mostly hard NVENC
   capacity.
4. Only then evaluate multi-GPU or newer-GPU architecture changes.

## Immediate Next 3 Tasks

1. [x] Add first-pass benchmark telemetry to the current pipeline.
   - current code now emits once-per-second pipeline summaries with:
     - preprocess FPS,
     - encode FPS,
     - display / YOLO / preprocess / encode queue depths,
     - available preprocess buffers/events,
     - preprocess drops / waits,
     - encode failures / slow-frame count.
   - still needs runtime validation on target hardware.

2. [ ] Run the benchmark matrix on:
   - `RTX A6000`,
   - one GPU on an `A16`.
   Goal:
   - confirm whether the current limit is mostly hardware,
   - identify whether `h264`, `hevc`, `p1`, or `p3` is the real boundary.
   Use:
   - `docs/nvenc_benchmark_runsheet.md`

3. [ ] Implement direct registered NV12 input behind a fallback switch.
   Goal:
   - remove the extra staging-to-NVENC copy,
   - quantify how much headroom that copy currently costs.

## Phase 1: Benchmarking And Observability

- [ ] Add or expose stable per-stage metrics for:
  - acquisition FPS,
  - preprocess FPS,
  - encode FPS,
  - preprocess stage time,
  - encode stage time,
  - dropped frames,
  - encode failures,
  - free preprocess-buffer count trend,
  - queue depth trend.
- [ ] Log the exact encode config used for each run:
  - codec,
  - preset,
  - tuning,
  - RC mode,
  - GOP length,
  - output resolution,
  - target FPS,
  - GPU id.
- [ ] Run the benchmark matrix documented in
  `docs/nvenc_benchmark_runsheet.md`.
- [ ] Compare single-session results on:
  - `RTX A6000`,
  - one GPU on an `A16` board.
- [ ] Confirm whether the failing cases are:
  - HEVC-specific,
  - preset-specific,
  - or still failing similarly even on cheaper settings.

## Phase 2: Current Path Cleanup

- [ ] If headless is going to be used for automated benchmarks, validate and fix
  modern headless stop/drain semantics first.
  Goal:
  - make repeated remote start/stop runs trustworthy,
  - avoid benchmarking a path that still has legacy-control-plane mismatch.
- [ ] Add an explicit "throughput-first" encode configuration option for tests:
  - lowest-overhead RC path supported by current code,
  - minimal extra quality features,
  - still valid for production-style output.
- [ ] Benchmark `h264` vs `hevc` under the same operating points.
- [ ] Benchmark current default RC path vs CQP vs cheapest LL path.
- [ ] Benchmark `AQ/TemporalAQ on` vs `AQ/TemporalAQ off` on representative
  large-frame recordings with mostly static background.
  Goal:
  - determine whether generic perceptual AQ is helping or misallocating bits for
    this scene class,
  - determine whether AQ cost is meaningful for throughput.
- [ ] Verify whether display / YOLO / multi-consumer fan-out materially changes
  encode throughput for the target runs.
- [ ] Measure whether the generic fan-out bookkeeping adds measurable overhead
  even in record-only benchmark mode.
  Goal:
  - determine whether record-only experiments need a narrower acquisition path,
  - especially around unused YOLO-event and multi-consumer bookkeeping.
- [ ] Refactor acquisition resource reservation so it matches enabled consumers.
  Goal:
  - allocate mandatory per-frame resources unconditionally,
  - allocate consumer-specific resources only when that consumer is active,
  - stop reserving YOLO resources on frames where YOLO is disabled,
  - improve both benchmark cleanliness and general main-path clarity.
- [ ] Add a short pre-encoder reference-capture mode for codec comparisons.
  Goal:
  - compare candidate codec settings against the frame representation actually
    seen by the encoder,
  - separate codec loss from upstream debayer / resize / colorspace choices,
  - avoid using long uncompressed capture as a default recording mode.
  Use:
  - `docs/pre_encoder_reference_capture_plan.md`
  - `docs/pre_encoder_reference_capture_todo.md`

## Phase 3: Direct Registered NV12 Input

- [ ] Extend the local NVENC wrapper to support external CUDA input buffers
  instead of always allocating its own internal input pool.
- [ ] Register a ring of externally managed CUDA NV12 surfaces with NVENC.
- [ ] Size that registered ring from NVENC's true encoder buffer count rather
  than the current arbitrary preprocess pool depth.
- [ ] Change `EncoderPreprocessWorker` so its prepared-frame pool becomes the
  registered encoder-input ring.
- [ ] Carry slot identity and pitch through the preprocess -> hardware worker
  handoff.
- [ ] Keep raw-frame refcounting separate from encoder-slot availability;
  implement explicit slot lifecycle and retirement instead of extending
  `WORKER_ENTRY::ref_count`.
- [ ] Remove the explicit `CopyToDeviceFrame(...)` step from the hardware
  encode path when direct-input mode is enabled.
- [ ] Keep the current copy path available behind a fallback switch during
  bring-up.
- [ ] Add slot-lifecycle telemetry so surface reuse is observable and easy to
  debug.
- [ ] Re-run the failing benchmark cases with:
  - current copy path,
  - direct registered input path.

## Phase 4: Post-Cleanup Decision Point

- [ ] Decide whether the remaining throughput limit is mostly:
  - hard single-session NVENC capacity,
  - local preprocess cost,
  - remaining pipeline overhead,
  - or GPU placement.
- [ ] If the direct-input path materially helps, stabilize it before trying
  more invasive architecture changes.
- [ ] If the direct-input path barely changes results, treat the remaining
  ceiling as mostly true NVENC capacity and move to architecture decisions.

## Phase 5: GPU Placement Strategy

- [ ] Reserve the best single-session GPU for the hardest stream:
  - likely `RTX A6000` first, unless measurements say otherwise.
- [ ] Use `A16` primarily as aggregate multi-stream capacity unless single-GPU
  measurements show it is equally good for the hardest stream.
- [ ] Document the recommended stream-to-GPU placement policy after benchmarks.

## Phase 6: Optional RGB-Input Experiment

- [ ] Evaluate direct `ARGB`/`ABGR` input to NVENC as an alternative to local
  NV12 conversion.
- [ ] Validate channel ordering and color correctness.
- [ ] Compare:
  - preprocess cost,
  - encode FPS,
  - output correctness,
  - file size / quality behavior.
- [ ] Only keep this path if it produces a clear throughput benefit without
  unacceptable color or maintenance cost.

## Phase 7: Multi-GPU GOP Splitting On A16

This phase is intentionally later.

- [ ] Decide whether added latency is acceptable for the intended use case.
- [ ] If latency is acceptable, prototype GOP-aligned work splitting across
  multiple GPUs on `A16`.
- [ ] Before testing, choose a shorter GOP than the current 1-second default so
  added buffering cost is less severe.
- [ ] Define the segment ownership and stitching model:
  - GOP assignment,
  - timestamp continuity,
  - metadata continuity,
  - output concatenation/mux behavior.
- [ ] Measure:
  - end-to-end latency increase,
  - sustained throughput increase,
  - complexity/stability cost.
- [ ] Compare against the simpler alternative of distributing separate camera
  streams across separate GPUs.

## Phase 8: Future Ada Path

- [ ] If single-session headroom remains insufficient and the use case justifies
  hardware changes, evaluate Ada GPUs with Split Frame Encoding support:
  - `L4`,
  - `L40S`.
- [ ] Compare projected benefit of Ada SFE against:
  - current `RTX A6000`,
  - `A16` with or without GOP splitting.
- [ ] Prefer Ada SFE over multi-GPU GOP splitting when:
  - the problem is one hardest stream,
  - low latency still matters,
  - and hardware change is feasible.

## Definition Of Done

- [ ] The team can state with evidence whether the throughput ceiling is mostly
  hardware or software.
- [ ] The single-GPU path has been cleaned up and benchmarked with direct input.
- [ ] The team has evidence for whether generic `AQ/TemporalAQ` helps, hurts, or
  is neutral for the target scene class and downstream tasks.
- [ ] The recommended production operating points are documented for:
  - full-resolution recording,
  - high-FPS lower-resolution recording,
  - each target GPU class.
- [ ] A clear decision is recorded on whether to:
  - stop at single-GPU cleanup,
  - pursue multi-GPU GOP splitting,
  - or pursue newer Ada hardware for the hardest stream.
