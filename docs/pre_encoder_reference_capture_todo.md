# Pre-Encoder Reference Capture TODO

Date: 2026-04-08
Scope: implementation checklist for the first `pre_encoder_reference_capture`
slice in the modern recording path.

See also:

- `docs/pre_encoder_reference_capture_plan.md`
- `docs/codec_quality_evaluation_protocol.md`
- `docs/nvenc_throughput_todo.md`
- `docs/encoding_importance_map_todo.md`

## Rules For This Slice

Status update (2026-05-04): the implementation slice has landed in the modern
recording path. `PreEncoderReferenceWriter` writes the raw dump, index, and
metadata sidecar; `EncoderHwWorker` taps the prepared NV12 frame before encode
submission; headless/spec config parsing and analyzer fields exist. Runtime
validation items remain open where no concrete validation artifact is recorded
below.

- [x] Support only the modern full-frame recording path.
- [x] Capture after preprocess and before NVENC submission.
- [x] Keep the feature opt-in and disabled by default.
- [x] Require a bounded capture budget such as `max_frames` or `max_seconds`.
- [x] Do not treat this as a general long-run raw-recording mode.
- [x] Preserve normal recording behavior when the feature is disabled.

## Phase 1. Control Plane

- [x] Define a narrow `pre_encoder_reference_capture` config shape.
- [x] Pick v1 enablement path: benchmark/headless first.
- [ ] Decide later whether GUI exposure belongs in this feature.
- [x] Enforce that a bound is provided.
- [x] Define stopping behavior once the bound is reached.

## Phase 2. Artifact Contract

- [x] Define the v1 artifact filenames:
  - [x] raw frame dump
  - [x] per-frame index
  - [x] static metadata
- [x] Define the static metadata schema.
- [x] Define the per-frame CSV/index fields.
- [x] Record enough metadata to replay the clip later:
  - [x] width
  - [x] height
  - [x] pitch
  - [x] pixel format
  - [x] frame size
  - [x] color vs mono path
  - [x] resize state
  - [x] timestamps / frame ids
- [x] Decide whether metadata should be documented in
  `docs/recording_metadata.md` in this slice or the next.

## Phase 3. Writer Plumbing

- [x] Add a dedicated reference-capture writer/helper instead of overloading
  `FFmpegWriter`.
- [x] Implement raw frame append to a simple binary dump.
- [x] Implement per-frame index writes.
- [x] Implement final metadata sidecar emission.
- [x] Ensure artifacts close cleanly on normal stop and early stop.

## Phase 4. Tap Point Integration

- [x] Add a unified tap point in `EncoderHwWorker`.
- [x] Place the tap after `preprocess_complete_event` and before encode
  submission.
- [x] Use the same tap point for:
  - [x] current copy path
  - [x] direct-input path
- [x] Capture the prepared frame representation actually seen by the encoder.

## Phase 5. Frame Format

- [x] Standardize v1 on prepared `NV12` reference frames.
- [x] Store frame bytes as `pitch * height * 3 / 2`.
- [x] Preserve the current mono path as prepared `NV12` with neutral UV.
- [x] Validate that color and mono metadata make the stored representation
  interpretable later.

## Phase 6. Budgeting And Performance Safety

- [x] Stop capture automatically when the configured budget is exhausted.
- [x] Keep normal encode submission running after capture stops.
- [x] Add minimal telemetry for:
  - [x] captured frame count
  - [x] capture bytes written
  - [x] capture stopped because budget was reached
- [x] Avoid presenting this feature as a throughput benchmark for long runs.

## Phase 7. Validation

- [ ] Short color-camera clip works and artifacts are readable.
- [ ] Short mono-camera clip works and artifacts are readable.
- [ ] Recorded frame count matches the configured bound.
- [ ] Byte offsets and frame sizes in the index are correct.
- [ ] Timestamps and frame ids match the captured sequence.
- [ ] Feature behaves correctly with:
  - [ ] current copy path
  - [ ] direct-input path
- [x] Normal recording is unchanged when feature is disabled.

## Phase 8. Follow-On Work

- [ ] Add offline replay / encode tooling for captured reference clips.
- [ ] Add a small analysis helper for decoding or previewing the raw dump.
- [ ] Add explicit benchmark workflow docs once the first implementation lands.
- [ ] Decide later whether simultaneous encoded-output + reference-capture runs
  are sufficient, or whether a reference-only capture mode is also worth adding.

## Definition Of Done

- [x] Orange has an implemented path to capture a short pre-encoder reference
      clip in the modern path.
- [x] The clip is bounded and opt-in.
- [x] The clip represents the prepared frame before compression, not sensor raw.
- [ ] Both the raw dump and metadata sidecars are validated as sufficient for
      later offline codec comparison.
- [x] The feature is not framed as a general raw-recording mode.
