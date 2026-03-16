# Recording Downsample TODO

Date: 2026-02-25
Scope: add an opt-in recording downsample mode so users can write smaller video
files at runtime without affecting capture timing or stream behavior.

## Decision: Allowed Factors

- Use power-of-two factors only: `1`, `2`, `4`, `8` (and optionally `16` only
  when camera resolution remains practical).
- Keep `1` as default (no downsample).
- This is mainly a pipeline safety/consistency rule, not a strict CUDA memory
  allocator requirement.
- Hard requirement is that encoded dimensions are even for NV12/chroma 4:2:0.
  The current RGB->NV12 kernel also assumes 2x2 chroma blocks, so odd width or
  odd height is unsafe.

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

## TODO Plan

## Phase 1: User Controls and Config

- [ ] Add explicit `record_downsample` control in recording settings (separate
  from `downsample streaming`).
- [ ] Allowed choices in UI: `1x`, `2x`, `4x`, `8x` (optional `16x` behind
  validation).
- [ ] Define config source and precedence:
  - global encoder config default,
  - optional per-camera override,
  - per-camera overrides global.
- [ ] Keep feature opt-in: default remains full-resolution recording (`1x`).

## Phase 2: Validation and Dimension Policy

- [ ] Validate `record_downsample` as power-of-two and `>= 1`.
- [ ] Compute target dimensions from source camera dimensions and factor.
- [ ] Enforce even output width and height before NV12 conversion.
- [ ] Enforce minimum output dimension threshold (for example >= 64) to avoid
  unusable recordings.
- [ ] On invalid settings, block start with clear UI error or auto-fallback to
  `1x` with explicit warning (choose one policy and keep consistent).

## Phase 3: Preprocess Pipeline Changes

- [ ] In `EncoderPreprocessWorker`, add downsample stage before NV12 conversion:
  - allocate per-camera downsample intermediate buffers,
  - resize on GPU (NPP),
  - convert resized image to NV12.
- [ ] Keep mono path correct:
  - downsample luma source,
  - generate/fill UV plane consistently for NV12 output.
- [ ] Ensure buffer pool sizing and event reuse reflect downsampled dimensions.

## Phase 4: Encoder Initialization and Metadata

- [ ] In `EncoderHwWorker`, initialize NVENC using downsampled
  `encodeWidth/encodeHeight`.
- [ ] Update metadata tags and encoder snapshot fields to include:
  - `downsample_factor`,
  - resolved output resolution.
- [ ] Include downsample info in `recording_snapshot.json` encoder metadata so
  downstream consumers know recorded geometry.
- [ ] Mirror behavior in headless path (`GPUVideoEncoder`) for parity.

## Phase 5: Runtime Behavior and Lifecycle

- [ ] Lock `record_downsample` for active recording session:
  - changing value during recording should apply to next recording segment only.
- [ ] Keep startup/stop/drain behavior unchanged relative to current encoder
  lifecycle.
- [ ] Verify no additional backpressure in acquisition/YOLO/pose paths.

## Phase 6: Validation

- [ ] Functional test:
  - `record_downsample=1` unchanged behavior,
  - `record_downsample=2/4/8` output dimensions and playback validity.
- [ ] Metadata test:
  - encoder snapshot reports correct `downsample_factor` and dimensions.
- [ ] Performance test:
  - verify FPS/drop behavior remains within acceptable range under long runs.
- [ ] Edge-case tests:
  - invalid factors,
  - small source resolutions near min threshold,
  - mono and color camera paths.
