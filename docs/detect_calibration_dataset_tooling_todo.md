# Detect Calibration Dataset Tooling TODO

Date: 2026-05-21

Scope: deferred implementation checklist for turning Orange frame captures into
TensorRT-ready calibration and validation datasets for the single-fish detect
model.

Status: deferred. This document describes the next tooling slice. It does not
change Orange runtime behavior.

## Goal

Build a bounded dataset extraction path that lets Palette calibrate and validate
an INT8 TensorRT engine against the same input distribution Orange feeds to the
detect model.

The first useful slice is an offline helper that reads existing
`pre_encoder_reference_capture` artifacts, extracts the mono/luma source image,
runs Orange-compatible YOLO preprocessing, and writes tensors plus a manifest.

The cleaner later slice is a direct Orange YOLO input tensor dump that taps the
post-preprocess `images` tensor itself.

## Non-Goals

- Do not train or fine-tune the model in Orange.
- Do not build TensorRT engines in this helper.
- Do not make long-running raw capture a normal recording mode.
- Do not use decoded MP4 frames as the preferred calibration source.
- Do not feed `NV12` bytes directly to TensorRT calibration.
- Do not expose this through the GUI until the headless/offline workflow is
  validated.

## Phase 1. Offline Extraction Helper

Proposed tool:

```text
scripts/extract_detect_calibration_tensors.py
```

Required inputs:

- [ ] One or more Orange recording folders or explicit
      `Cam<serial>_preenc_ref.json` paths.
- [ ] Matching `Cam<serial>_preenc_ref.bin` raw dumps.
- [ ] Matching `Cam<serial>_preenc_ref_index.csv` indexes.
- [ ] Optional dataset-role mapping: `calibration`, `validation`, or `holdout`.
- [ ] Optional scene labels/tags such as `fish_present`, `no_fish`,
      `edge_case`, `low_contrast`, `motion_blur`, or `unknown`.

Required CLI controls:

- [ ] `--out-dir`.
- [ ] `--dataset-id`.
- [ ] `--role calibration|validation|holdout`.
- [ ] `--camera-serial <serial>` filter, repeatable.
- [ ] `--max-frames-per-camera`.
- [ ] `--sample-stride` or `--sample-every`.
- [ ] `--input-width` and `--input-height`, default `640x640`.
- [ ] `--pad-value`, default `114`.
- [ ] `--output-format`, at least `npy` or raw little-endian `float32`.
- [ ] `--dry-run` to validate inputs and print planned counts.

## Phase 2. Pre-Encoder Artifact Validation

Before extracting tensors, the helper should validate every source artifact:

- [ ] Metadata `capture_mode` is `pre_encoder_reference`.
- [ ] Pixel format is expected for current mono captures, currently prepared
      `NV12`.
- [ ] `width`, `height`, `pitch`, and `frame_size` are present and positive.
- [ ] `pitch >= width`.
- [ ] `frame_size == pitch * height * 3 / 2` for NV12.
- [ ] Raw dump file size is compatible with the index and frame size.
- [ ] Every index row has valid `byte_offset` and `byte_size`.
- [ ] Index rows are monotonic by `reference_frame_index`.
- [ ] `recording_frame_id`, camera timestamp, and system timestamp are
      preserved in the output manifest.
- [ ] Reject or clearly mark captures where `resize_enabled` or output geometry
      means the luma plane is not source-like full-frame mono input.

## Phase 3. Luma Extraction

For current mono-camera `NV12` pre-encoder captures:

- [ ] Treat the Y plane as source-like mono/luma input.
- [ ] Ignore the UV plane.
- [ ] Read each frame from `byte_offset` using `byte_size`.
- [ ] Interpret the Y plane as `height` rows with `pitch` bytes per row.
- [ ] Use only the first `width` bytes of each row as active image pixels.
- [ ] Preserve source dimensions and pitch in the output manifest.
- [ ] Add a small optional preview image writer for human spot checks.

## Phase 4. Orange-Compatible YOLO Preprocessing

The offline preprocessing path must reproduce the current Orange detection
tensor contract:

```text
source:
  uint8 mono/luma image

preprocess:
  letterbox resize to 640x640 by default
  bilinear interpolation
  pad value 114
  FP32 conversion
  divide by 255.0
  replicate mono value into B, G, R planes
  planar NCHW layout

output tensor:
  name: images
  shape: 1x3x640x640
  dtype: float32
  value range: [0, 1]
```

Implementation checklist:

- [ ] Put the preprocessing code in a small, testable module rather than burying
      it in one script.
- [ ] Record the preprocessing contract version in the manifest.
- [ ] Record input size, pad value, interpolation, channel order, layout,
      normalization, and dtype in the manifest.
- [ ] Preserve deterministic output for the same source frame and settings.
- [ ] Make channel replication explicit; do not convert mono to unrelated RGB.
- [ ] Do not apply mean/std normalization unless Orange runtime changes.
- [ ] Do not use OpenCV defaults without confirming they match Orange letterbox
      geometry and bilinear sampling closely enough.

## Phase 5. Output Artifacts

Each extraction run should write:

```text
dataset_manifest.json
dataset_manifest.csv or jsonl
tensors/
previews/                 # optional
extraction_summary.json
```

Manifest rows should include:

- [ ] Dataset id and role.
- [ ] Orange source recording folder.
- [ ] Camera serial.
- [ ] Source artifact paths.
- [ ] Source artifact checksums.
- [ ] Reference frame index.
- [ ] Recording frame id.
- [ ] Camera timestamp and system timestamp.
- [ ] Source byte offset and byte size.
- [ ] Source width, height, pitch, pixel format, and frame size.
- [ ] Output tensor path.
- [ ] Output tensor checksum.
- [ ] Tensor name, shape, dtype, layout, and byte order.
- [ ] Preprocessing settings and contract version.
- [ ] Scene tags and fish-present state: `true`, `false`, or `unknown`.
- [ ] Split assignment: `calibration`, `validation`, or `holdout`.

## Phase 6. Dataset Quality Controls

- [ ] Report per-camera counts.
- [ ] Report role/split counts.
- [ ] Report scene-tag counts.
- [ ] Warn if any camera has too few frames.
- [ ] Warn if calibration and validation splits share the same source frame.
- [ ] Warn if all frames are from one short homogeneous run.
- [ ] Compute tensor min, max, mean, and standard deviation.
- [ ] Check B/G/R planes are equal for mono captures.
- [ ] Check padding regions are exactly `114 / 255.0` within float tolerance.
- [ ] Optionally produce contact-sheet previews for quick human inspection.

## Phase 7. Parity Tests

The helper should have a small test set that protects the Orange preprocessing
contract:

- [ ] Synthetic all-zero frame produces zero image content and `114/255`
      padding where applicable.
- [ ] Synthetic all-255 frame produces one-valued image content.
- [ ] Known checkerboard or ramp image preserves expected resize/letterbox
      geometry.
- [ ] Pitch greater than width is handled correctly.
- [ ] Invalid frame sizes and offsets fail closed.
- [ ] Output tensor shape and dtype match `1x3x640x640 float32`.
- [ ] A captured real frame can be converted and previewed.

Preferred later parity check:

- [ ] Compare offline helper output against Orange CUDA preprocessing output on
      a fixed fixture frame, with an explicit tolerance and documented expected
      differences if CPU/OpenCV preprocessing is used.

## Phase 8. Direct YOLO Tensor Dump

The direct dump is the preferred long-term capture path once INT8 work becomes
active.

Implementation checklist:

- [ ] Add a bounded, opt-in config such as `yolo_input_tensor_capture`.
- [ ] Keep it headless/spec-only for the first slice.
- [ ] Require exactly one bound: max tensors or max seconds.
- [ ] Sample accepted YOLO frames after `optimized_yolo_preprocess.cu` has
      written the TensorRT input buffer.
- [ ] Capture the exact `images` tensor bytes: `1x3x640x640 float32`.
- [ ] Copy asynchronously to host without unbounded hot-path stalls.
- [ ] Preserve source camera serial, recording frame id, camera timestamp,
      engine path, input dimensions, and preprocessing metadata.
- [ ] Disable by default.
- [ ] Fail clearly if the engine input dtype or shape is not supported by the
      dump contract.
- [ ] Keep the capture budget small enough that normal acquisition and
      recording behavior are not reclassified as benchmark results.

Open design question:

- [ ] Decide whether the tensor dump should live in `YoloWorker`, in the
      `YOLOv8` wrapper, or in a small capture helper called after preprocess and
      before `enqueueV3`.

## Phase 9. Acceptance Criteria

The deferred tooling slice is complete when:

- [ ] The offline helper converts a real four-camera pre-encoder capture into
      calibration tensors and manifests.
- [ ] The helper rejects malformed or incompatible pre-encoder artifacts.
- [ ] Palette can consume the manifest and tensors without guessing Orange
      preprocessing semantics.
- [ ] Calibration and validation splits are explicit and non-overlapping.
- [ ] A small test suite protects luma extraction, pitch handling, letterbox,
      normalization, and tensor layout.
- [ ] The docs identify whether a dataset came from pre-encoder luma extraction
      or exact post-YOLO-preprocess tensor dumping.
- [ ] No default Orange recording, GUI, or external IPC behavior changes.

## Related Docs

- [Detect TensorRT Calibration Dataset](/home/jeremy/orange-gop-split-a16/docs/detect_tensorrt_calibration_dataset.md)
- [Detect INT8 Quantization Plan](/home/jeremy/orange-gop-split-a16/docs/detect_int8_quantization_plan.md)
- [Palette / Orange Tensor Input Contract](/home/jeremy/orange-gop-split-a16/docs/palette_orange_tensor_input_contract.md)
- [Pre-Encoder Reference Capture TODO](/home/jeremy/orange-gop-split-a16/docs/pre_encoder_reference_capture_todo.md)
