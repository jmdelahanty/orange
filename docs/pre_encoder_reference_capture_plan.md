# Pre-Encoder Reference Capture Plan

Date: 2026-04-08
Scope: bounded design for capturing the prepared recording-path frame
immediately before NVENC submission so codec settings can be compared against a
true pre-compression reference.

See also:

- `docs/codec_quality_evaluation_protocol.md`
- `docs/nvenc_throughput_todo.md`
- `docs/encoding_importance_map_todo.md`
- `docs/nvenc_direct_input_v1_plan.md`
- `docs/recording_metadata.md`
- `docs/detect_int8_quantization_plan.md`

## Goal

Add a benchmark-only `pre_encoder_reference_capture` feature to the modern
recording path so Orange can save short clips of the exact frame representation
seen by the encoder before compression.

Status update (2026-05-04): the implementation slice has landed. The code now
has a bounded headless/spec control path, `PreEncoderReferenceWriter`, an
`EncoderHwWorker` tap before encode submission, raw dump/index/metadata
artifacts, recording snapshot/analyzer fields, and disabled-by-default behavior.
This plan remains useful for contract intent and validation gates; see
`docs/pre_encoder_reference_capture_todo.md` for the current validation status.

This should support later offline codec bake-offs such as:

- current default encode settings
- `cqp`
- `lossless`
- future static dish-prior map-guided encoding
- future motion-aware or bbox-aware map-guided encoding

## Design Position

This is not a general-purpose "raw recording mode."

It should be treated as:

- a bounded experiment / benchmark feature
- opt-in only
- intended for small clips
- capturing prepared encoder input, not sensor-native camera raw

The reference question it answers is:

- "what did the codec remove from the prepared recording-path frame?"

Not:

- "what did the camera sensor produce before local preprocessing?"

## In Scope

- modern full-frame recording path only:
  - `EncoderPreprocessWorker -> EncoderHwWorker`
- capture after preprocess and before NVENC submission
- exact prepared recording-path representation for that path
- bounded capture by `max_frames` or `max_seconds`
- metadata sidecars sufficient for later offline replay and interpretation
- both current copy path and direct-input path using the same reference-capture
  abstraction

## Out Of Scope

- legacy `GPUVideoEncoder` path
- long-run uncompressed recording as a user-facing default mode
- sensor-native Bayer / Mono8 reference capture
- exact YOLO input tensor capture
- polished consumer-facing UI workflow in v1
- final offline replay/encode harness in the same slice
- portable interchange container work if a simpler raw dump is sufficient

## Core Decisions

### 1. Capture The Prepared Frame, Not Camera Raw

The feature should capture the prepared frame after resize / debayer /
colorspace conversion and before NVENC sees it.

Reason:

- this is the most direct reference for codec comparisons,
- it isolates codec loss from upstream preprocessing,
- it matches the codec-quality protocol already documented elsewhere.

### 2. Use One Tap Point Across Copy And Direct-Input Paths

The preferred tap point is in `EncoderHwWorker`, after waiting for
`preprocess_complete_event` and before:

- `CopyToDeviceFrame(...)` in the copy path
- `EncodeFrame(...)` in the direct-input path

Reason:

- both paths already converge there,
- it captures the same conceptual frame representation in both modes,
- it avoids adding a second capture-specific branch inside preprocess.

### 3. Keep V1 Benchmark-Only And Bounded

The first slice should require explicit enablement and one of:

- `max_frames`
- `max_seconds`

The capture should stop automatically once the bound is reached.

Reason:

- long uncompressed capture can become an I/O benchmark instead of a codec
  benchmark,
- bounded clips are enough for codec comparison,
- it reduces risk to normal recording behavior.

### 4. Preserve The Actual Prepared Surface Layout In V1

The first slice should prefer a dead-simple raw dump with metadata over a more
polished container.

Recommended v1 artifact set:

- `Cam<serial>_preenc_ref.bin`
- `Cam<serial>_preenc_ref_index.csv`
- `Cam<serial>_preenc_ref.json`

Recommended stored frame layout:

- one frame after another,
- each frame stored as `pitch * height * 3 / 2` bytes,
- `NV12` semantics for both color and mono paths,
- metadata records width, height, pitch, frame size, pixel format, and whether
  the source path was color or mono.

Reason:

- simplest write path,
- simplest replay contract,
- no misleading claim that the file is a standard playable video container.

### 5. V1 Should Reuse The Current Prepared NV12 Representation

The modern recording path already feeds `NV12` into NVENC.
The reference capture should preserve that same representation.

For v1:

- color is captured as prepared `NV12`
- mono is also captured as prepared `NV12` with neutral UV plane

Reason:

- avoids branching the feature by camera type,
- keeps the replay format uniform,
- matches the actual encoder input representation.

### 6. Do Not Make Normal Recording Depend On This Feature

When disabled, the path should behave exactly as it does today.

When enabled, the feature should add:

- extra D2H copy cost
- extra disk I/O

but should not require a second recording backend or a permanent architectural
split.

### 7. INT8 Calibration Use Is Indirect

The reference-capture dump can help collect representative frames for a YOLO
INT8 calibration experiment, but it is not itself a TensorRT calibration tensor
dump.

For INT8 work:

- use it as a bounded source-frame capture mechanism,
- read the full-resolution luma plane from the captured `NV12` frames when the
  metadata confirms the expected full-frame Mono8 path,
- ignore the neutral UV plane for mono cameras,
- run the exact Orange YOLO preprocessing offline to produce
  `1x3x640x640` FP32 `images` tensors,
- keep a manifest linking each calibration tensor back to camera serial,
  recording frame id, timestamp, and byte offset in the raw dump.

Do not feed `Cam<serial>_preenc_ref.bin` directly to TensorRT calibration.
The dump is encoder-ready `NV12`, while the detector expects planar FP32
B/G/R channels after resize/letterbox/normalization. The cleaner future feature
for INT8 would be a separate bounded YOLO calibration tensor dump after
`optimized_yolo_preprocess.cu`; this reference-capture feature should remain
focused on prepared encoder input.

## Proposed Control Shape

The control plane should expose a narrow reference-capture config block rather
than a generic "raw mode."

Suggested fields:

```json
{
  "enabled": true,
  "max_frames": 600,
  "max_seconds": 10,
  "output_dir": "/path/to/run",
  "capture_mode": "pre_encoder_reference"
}
```

Rules:

- exactly one of `max_frames` or `max_seconds` should be required in v1
- if both are supplied later, the earliest stopping condition wins
- if disabled or omitted, no additional capture occurs
- the current headless implementation supports this only with
  `recording_sink_mode = "real"`; it is not supported with `stream_only`,
  `preprocess_only`, or `external_ipc`

V1 recommendation:

- prefer headless / benchmark entry points first
- delay broad GUI exposure until semantics are proven

## Artifact Shape

### Static Metadata

`Cam<serial>_preenc_ref.json` should include at minimum:

- camera serial
- recording output width and height
- pixel format: `nv12`
- pitch in bytes
- frame size in bytes
- path type:
  - `copy`
  - `direct_input`
- source path flavor:
  - `color`
  - `mono`
- resize enabled
- codec settings snapshot for the run
- capture start / stop criteria

### Per-Frame Index

`Cam<serial>_preenc_ref_index.csv` should include at minimum:

- sequential reference frame index
- recording frame id
- timestamp
- timestamp_sys
- byte offset in the raw dump
- byte size

### Raw Frame Dump

`Cam<serial>_preenc_ref.bin` should contain the raw prepared frames in order.

V1 should optimize for:

- correctness
- easy replay
- obvious metadata linkage

not for immediate playability in common media tools.

## Runtime Shape

Desired v1 flow:

1. Recording starts with reference capture enabled.
2. Preprocess produces the prepared frame as usual.
3. `EncoderHwWorker` waits for preprocess completion.
4. If the reference-capture budget is not exhausted:
   - copy the prepared frame from GPU to host staging
   - append it to the raw dump
   - append frame metadata to the index
5. Continue with normal encode submission.
6. Once the configured frame/time budget is reached:
   - stop further reference capture
   - keep normal recording behavior unchanged
7. On shutdown:
   - flush and close reference artifacts cleanly
   - emit final metadata summary

## Files Likely To Change

- `src/encoder_hw_worker.h`
- `src/encoder_hw_worker.cpp`
- `src/encoder_pipeline.h`
- `src/modern_recording_pipeline.cpp`
- `src/modern_recording_pipeline.h`
- one or more new helper files for reference-capture writing / metadata
- `docs/recording_metadata.md`

Possible later additions:

- headless benchmark config plumbing
- offline replay tooling

## Risks

### Throughput Distortion

Reference capture adds:

- GPU-to-host copy cost
- host memory traffic
- disk I/O

So it can distort live throughput if used too long.

That is acceptable only because the feature is intentionally short and
benchmark-oriented.

### Ambiguous Ground Truth

If the feature is described as "raw recording," users may compare the wrong
layers and draw incorrect conclusions.

The docs and naming should consistently say:

- `pre_encoder_reference_capture`

not:

- raw recording
- raw encoding

### Shutdown / Partial Artifact Issues

Bounded capture must still close artifacts cleanly when:

- recording stops early
- the process drains
- the capture budget expires before recording ends

## Validation Targets

- captured clip length respects `max_frames` or `max_seconds`
- metadata matches the prepared frame layout actually written
- color and mono paths both produce valid reference artifacts
- direct-input and copy paths both capture the same conceptual tap point
- normal recording remains unchanged when feature is disabled
- reference-capture mode is usable for later offline codec bake-off work

## Recommended First Implementation Order

1. Add a narrow config object and disabled-by-default control path.
2. Add a simple raw-dump + sidecar writer.
3. Tap the prepared frame in `EncoderHwWorker` before encode submission.
4. Support bounded capture and clean shutdown.
5. Validate short clips on one camera first.
6. Only then expand tooling around offline replay or GUI exposure.
