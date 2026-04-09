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

- [ ] Support only the modern full-frame recording path.
- [ ] Capture after preprocess and before NVENC submission.
- [ ] Keep the feature opt-in and disabled by default.
- [ ] Require a bounded capture budget such as `max_frames` or `max_seconds`.
- [ ] Do not treat this as a general long-run raw-recording mode.
- [ ] Preserve normal recording behavior when the feature is disabled.

## Phase 1. Control Plane

- [ ] Define a narrow `pre_encoder_reference_capture` config shape.
- [ ] Pick v1 enablement path:
  - [ ] benchmark/headless first
  - [ ] GUI exposure later or not at all in v1
- [ ] Enforce that a bound is provided.
- [ ] Define stopping behavior once the bound is reached.

## Phase 2. Artifact Contract

- [ ] Define the v1 artifact filenames:
  - [ ] raw frame dump
  - [ ] per-frame index
  - [ ] static metadata
- [ ] Define the static metadata schema.
- [ ] Define the per-frame CSV/index fields.
- [ ] Record enough metadata to replay the clip later:
  - [ ] width
  - [ ] height
  - [ ] pitch
  - [ ] pixel format
  - [ ] frame size
  - [ ] color vs mono path
  - [ ] resize state
  - [ ] timestamps / frame ids
- [ ] Decide whether metadata should be documented in
  `docs/recording_metadata.md` in this slice or the next.

## Phase 3. Writer Plumbing

- [ ] Add a dedicated reference-capture writer/helper instead of overloading
  `FFmpegWriter`.
- [ ] Implement raw frame append to a simple binary dump.
- [ ] Implement per-frame index writes.
- [ ] Implement final metadata sidecar emission.
- [ ] Ensure artifacts close cleanly on normal stop and early stop.

## Phase 4. Tap Point Integration

- [ ] Add a unified tap point in `EncoderHwWorker`.
- [ ] Place the tap after `preprocess_complete_event` and before encode
  submission.
- [ ] Use the same tap point for:
  - [ ] current copy path
  - [ ] direct-input path
- [ ] Capture the prepared frame representation actually seen by the encoder.

## Phase 5. Frame Format

- [ ] Standardize v1 on prepared `NV12` reference frames.
- [ ] Store frame bytes as `pitch * height * 3 / 2`.
- [ ] Preserve the current mono path as prepared `NV12` with neutral UV.
- [ ] Validate that color and mono metadata make the stored representation
  interpretable later.

## Phase 6. Budgeting And Performance Safety

- [ ] Stop capture automatically when the configured budget is exhausted.
- [ ] Keep normal encode submission running after capture stops.
- [ ] Add minimal telemetry for:
  - [ ] captured frame count
  - [ ] capture bytes written
  - [ ] capture stopped because budget was reached
- [ ] Avoid presenting this feature as a throughput benchmark for long runs.

## Phase 7. Validation

- [ ] Short color-camera clip works and artifacts are readable.
- [ ] Short mono-camera clip works and artifacts are readable.
- [ ] Recorded frame count matches the configured bound.
- [ ] Byte offsets and frame sizes in the index are correct.
- [ ] Timestamps and frame ids match the captured sequence.
- [ ] Feature behaves correctly with:
  - [ ] current copy path
  - [ ] direct-input path
- [ ] Normal recording is unchanged when feature is disabled.

## Phase 8. Follow-On Work

- [ ] Add offline replay / encode tooling for captured reference clips.
- [ ] Add a small analysis helper for decoding or previewing the raw dump.
- [ ] Add explicit benchmark workflow docs once the first implementation lands.
- [ ] Decide later whether simultaneous encoded-output + reference-capture runs
  are sufficient, or whether a reference-only capture mode is also worth adding.

## Definition Of Done

- [ ] Orange can capture a short pre-encoder reference clip in the modern path.
- [ ] The clip is bounded and opt-in.
- [ ] The clip represents the prepared frame before compression, not sensor raw.
- [ ] Both the raw dump and metadata sidecars are sufficient for later offline
  codec comparison.
- [ ] The feature is not framed as a general raw-recording mode.
