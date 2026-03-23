# Recording Downsample TODO

Date: 2026-02-25
Scope: add an opt-in recording downsample mode so users can write smaller video
files at runtime without affecting capture timing or stream behavior.

## Decision: Output Modes

- Support two recording resize modes:
  - `factor`: power-of-two downsample only (`1`, `2`, `4`, `8`, optionally `16`)
  - `exact_size`: explicit output width/height for known downstream needs such
    as `1024x1024`
- Keep `factor=1` as default (no resize).
- Do not generalize this into unconstrained arbitrary-shape resizing yet.
  The immediate goal is to support exact known output sizes without committing
  to crop/pad/stretch policies for all aspect-ratio combinations.
- The factor mode remains a pipeline safety/consistency rule, not a strict CUDA
  memory allocator requirement.
- Hard requirement is that encoded dimensions are even for NV12/chroma 4:2:0.
  The current RGB->NV12 kernel also assumes 2x2 chroma blocks, so odd width or
  odd height is unsafe.
- Multiples of `4` or `8` are not a hard correctness requirement here. They may
  still be a reasonable performance/alignment policy later, but correctness in
  the current pipeline is driven by even width and even height.
- Logical output dimensions and encoder memory pitch are separate concerns:
  - output width/height must satisfy the NV12 geometry rule above,
  - NVENC pitch/alignment is provided by the encoder input buffer and should be
    handled separately by the preprocess path.

## Current State

- There is an existing UI control for `downsample streaming` that only affects
  display path scaling.
- Recording pipeline currently encodes at camera native resolution.
- Existing notes for recording downsample are conceptual and need concrete
  implementation tasks.

## Audit Update (2026-03-16)

- Re-checked current code: display downsample exists, recording downsample does not.
- `CameraEachSelect.downsample` still feeds the display path only.
- Encoder initialization still uses native camera dimensions in both:
  - `EncoderHwWorker` (GUI/full-frame path),
  - `GPUVideoEncoder` (headless/legacy path).
- No `record_downsample` config, runtime control, or recording metadata field exists yet.
- Re-confirmed the implementation detail behind the even-dimension rule:
  - the current RGB->NV12 path writes UV for the top-left pixel of each 2x2
    chroma block, so odd width/height is unsafe without kernel changes.
- For the current 4512x4512 camera:
  - `2x` -> `2256x2256` is valid,
  - `4x` -> `1128x1128` is valid,
  - exact-size `1024x1024` is also valid and keeps source/output aspect ratio aligned,
  - `3x` would happen to stay even for this sensor, but arbitrary factors remain
    out of scope because the runtime policy is still power-of-two-only.
- Product decision update:
  - exact-size resize is allowed for known targets like `1024x1024`,
  - non-matching aspect ratios should remain out of scope for now unless we add
    explicit crop/pad/stretch policy and metadata.

## TODO Plan

## Phase 1: User Controls and Config

- [x] Replace the single `record_downsample` concept with explicit recording
  resize config:
  - `record_output_mode = factor | exact_size`
  - `record_downsample_factor`
  - `record_output_width`
  - `record_output_height`
- [x] Keep recording resize settings separate from `downsample streaming`.
- [x] Allowed choices in UI:
  - factor mode: `1x`, `2x`, `4x`, `8x` (optional `16x` behind validation),
  - exact-size mode: width/height integer inputs.
- [x] Define config source and precedence:
  - global encoder config default,
  - optional per-camera override,
  - per-camera overrides global.
- [x] Keep feature opt-in: default remains full-resolution recording
  (`record_output_mode=factor`, `factor=1`).
- Status note:
  - Phase 1 UI/config landed.
  - Active GUI full-frame recording now consumes these settings.
  - Headless/legacy encoder parity is still pending.

## Phase 2: Validation and Dimension Policy

- [x] Validate factor mode:
  - factor is power-of-two and `>= 1`,
  - target dimensions are derived from source dimensions and factor.
- [x] Validate exact-size mode:
  - width/height explicitly provided,
  - width/height are even,
  - width/height meet minimum size threshold,
  - width/height preserve source aspect ratio for now.
- [x] Enforce even output width and height before NV12 conversion.
- [x] Treat "must be multiple of 4" as optional optimization policy, not as a
  hard validation rule.
- [x] Keep width/height validation separate from encoder pitch handling:
  - dimensions are validated from requested geometry,
  - pitch comes from the resolved NVENC input frame layout.
- [x] Enforce minimum output dimension threshold (for example >= 64) to avoid
  unusable recordings.
- [x] For now, reject exact-size requests that imply aspect-ratio change rather
  than silently stretching or cropping.
- [x] On invalid settings, auto-fallback to native `1x` with explicit warning.

## Phase 3: Preprocess Pipeline Changes

- [x] In `EncoderPreprocessWorker`, add recording resize stage before NV12
  conversion:
  - allocate per-camera resize intermediate buffers,
  - resize on GPU (NPP),
  - convert resized image to NV12.
- [x] Keep mono path correct:
  - resize luma source,
  - generate/fill UV plane consistently for NV12 output.
- [x] Ensure buffer pool sizing and event reuse reflect resolved output
  dimensions, not native camera dimensions.

## Phase 4: Encoder Initialization and Metadata

- [x] In `EncoderHwWorker`, initialize NVENC using resolved output
  `encodeWidth/encodeHeight`.
- [x] Update metadata tags and encoder snapshot fields to include:
  - `output_mode`,
  - `downsample_factor` when factor mode is used,
  - `requested_output_size` when exact-size mode is used,
  - resolved output resolution.
- [x] Include resize info in `recording_snapshot.json` encoder metadata so
  downstream consumers know recorded geometry and policy.
- [ ] Mirror behavior in headless path (`GPUVideoEncoder`) for parity.

## Phase 5: Runtime Behavior and Lifecycle

- [x] Lock recording resize settings while streaming is active:
  - active GUI path creates recording workers at stream startup,
  - factor/exact-size controls are disabled while subscribed,
  - changing resize settings requires stopping streaming so workers can be
    recreated with the new geometry.
- [ ] Keep startup/stop/drain behavior unchanged relative to current encoder
  lifecycle.
- [ ] Verify no additional backpressure in acquisition/YOLO/pose paths.

## Phase 6: Validation

- [ ] Functional test:
  - factor mode with `1x` unchanged behavior,
  - factor mode with `2x/4x/8x` output dimensions and playback validity,
  - exact-size mode with `1024x1024` output dimensions and playback validity.
- [ ] Metadata test:
  - encoder snapshot reports correct output mode, factor/size inputs, and
    resolved dimensions.
- [ ] Performance test:
  - verify FPS/drop behavior remains within acceptable range under long runs.
- [ ] Edge-case tests:
  - invalid factors,
  - invalid exact sizes,
  - aspect-ratio mismatch rejection,
  - small source resolutions near min threshold,
  - mono and color camera paths.
