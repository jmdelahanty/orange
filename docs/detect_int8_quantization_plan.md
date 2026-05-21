# Detect INT8 Quantization Plan

Date: 2026-04-26

Scope: define what the model/engine build workflow should provide if we try
INT8 for the single-fish YOLO detect model, and define the accuracy and latency
gates before an INT8 engine can replace the current A16 FP16 engine.

## Bottom Line

The current A16-specific FP16 TensorRT engine is the known-good baseline. INT8
is the next model-runtime optimization to try, but it is an accuracy-sensitive
model export/build problem, not just a new `trtexec` flag.

The preferred production path is:

```text
training/export machine
  -> representative calibration/validation data
  -> PTQ or QAT export with explicit ONNX Q/DQ nodes
  -> A16 TensorRT INT8 engine build
  -> Orange headless + GUI validation
```

The fastest bounded experiment is:

```text
Orange/A16 machine
  -> collect representative frames or preprocessed tensors
  -> run a TensorRT INT8 calibrator
  -> build an implicit-quantization INT8 engine
  -> compare accuracy and latency against the A16 FP16 engine
```

Do not promote an INT8 engine based on latency alone. This detector is
intentionally top-1/single-fish, so small confidence or localization shifts can
turn into missed crops even if throughput improves.

## Current Baseline

Current best candidate:

```text
/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a16_gpu5_trt100_fp16_bo5_avg32.engine
```

Observed long-run two-camera result:

```text
2010095 steady detect p95: 3.950 ms
2010096 steady detect p95: 3.944 ms
```

That is the baseline INT8 must beat while preserving detection quality.

The Orange model interface is:

```text
input:
  images:   FP32, shape (1, 3, 640, 640)

outputs:
  num_dets: INT32, shape (1, 1)
  bboxes:   FP32, shape (1, 1, 4)
  scores:   FP32, shape (1, 1)
  labels:   INT32, shape (1, 1)

final layer:
  EfficientNMS_TRT
```

The current preprocess contract in
[optimized_yolo_preprocess.cu](/home/jeremy/orange-gop-split-a16/src/optimized_yolo_preprocess.cu:1)
is:

```text
source image:
  4512x4512 Mono8 in the common production path

network tensor:
  resize with letterbox to 640x640
  padding value 114 / 255
  mono replicated into B, G, R channels
  planar channel layout: BBB... GGG... RRR...
  FP32 values normalized to [0, 1]
```

The calibrator/export workflow must reproduce this tensor contract. Feeding
raw 20MP frames, JPEGs with a different resize path, RGB channel order, or a
different normalization range will produce calibration scales for the wrong
input distribution.

## Terms

Q/DQ means explicit quantization nodes in the model graph:

```text
FP32 tensor
  -> QuantizeLinear      (Q)
  -> quantized tensor
  -> DequantizeLinear    (DQ)
  -> FP32 tensor with known quantization scale
```

The ONNX operators are `QuantizeLinear` and `DequantizeLinear`. They tell
TensorRT where quantization is allowed and what scale/zero-point to use.
TensorRT can then fuse Q/DQ-marked regions into actual INT8 kernels.

PTQ means post-training quantization:

```text
trained FP32/FP16 model
  -> representative calibration data
  -> infer quantization scales
  -> export Q/DQ ONNX or build an INT8 engine
```

PTQ does not retrain the model. It is faster to try, but it can reduce
accuracy if the model is sensitive to activation clipping or lower precision.

QAT means quantization-aware training:

```text
train or fine-tune while simulating INT8 quantization
  -> model learns to tolerate rounding/clipping
  -> export Q/DQ ONNX
  -> build TensorRT INT8 engine
```

QAT is more work, but it is the higher-confidence production path when PTQ
causes false negatives, box jitter, or confidence shifts.

Implicit TensorRT calibration means the ONNX has no Q/DQ nodes. TensorRT runs a
calibration set through the floating-point graph, builds activation histograms,
chooses dynamic ranges, and opportunistically uses INT8 tactics. This path is
useful for a quick experiment, but NVIDIA's newer TensorRT guidance prefers
explicit Q/DQ quantization for durable model artifacts.

## Calibration Data Requirements

Calibration does not require labels. Accuracy validation does.

The dataset contract for Palette is documented in
[Detect TensorRT Calibration Dataset](/home/jeremy/orange-gop-split-a16/docs/detect_tensorrt_calibration_dataset.md).
That document should be treated as the handoff contract when collecting
Orange frames/tensors for INT8 engine creation.

For TensorRT INT8, "calibration frames" means representative examples used to
measure activation ranges inside the floating-point network so TensorRT can
choose INT8 scales. They are not training labels, and they are not necessarily
the final validation set. Bad calibration frames can make a fast INT8 engine
miss fish by clipping useful activations or wasting the limited INT8 range on
conditions that never occur in production.

Recommended calibration data shape:

- Use real Orange camera frames or exact preprocessed tensors.
- Cover every production camera family used by the engine. For the local
  four-camera A16 setup, include `2010093`, `2010094`, `2010095`, and
  `2010096`, not only the old two-camera pair.
- Include fish present, no-fish, fish near edges, fish partially occluded,
  bright/dim lighting, normal dish background, bubbles/debris, motion blur,
  and any exposure/gain settings used in production.
- Prefer hundreds to low thousands of frames/tensors. More is not always
  better if the set is redundant; coverage matters more than raw count.
- Keep a manifest with source run folder, camera serial, frame id, timestamp,
  preprocessing version, and whether fish is present.
- Keep calibration and validation split. It is fine for calibration frames to
  be unlabeled, but the held-out validation set needs labels or trusted FP16
  reference outputs.

Preferred calibration representation:

```text
FP32 tensors named images, shape 1x3x640x640
same letterbox/resize/padding/channel replication/normalization as Orange
```

Acceptable capture sources, in preference order:

1. A future YOLO calibration dump that writes the exact post-preprocess FP32
   `images` tensors. This is the cleanest because the TensorRT calibrator sees
   exactly what Orange feeds to `enqueue`.
2. Source-like full-resolution Mono8 frames plus a calibrator that reuses the
   exact Orange YOLO preprocessing code offline.
3. Current `pre_encoder_reference_capture` artifacts, but only with the
   caveats below.

### Using `pre_encoder_reference_capture`

Orange already has a bounded dump path for short raw frame clips:
`fixed.pre_encoder_reference_capture` in experiment specs, or
`--preenc-ref-max-frames` / `--preenc-ref-max-seconds` in local headless mode.

This is useful for INT8 calibration only if it is interpreted correctly:

- It captures the prepared encoder input, not sensor-native camera raw and not
  the already-preprocessed YOLO tensor.
- The v1 artifact is `NV12`: `Cam<serial>_preenc_ref.bin` plus
  `Cam<serial>_preenc_ref_index.csv` and `Cam<serial>_preenc_ref.json`.
- For the current Mono8 full-frame path, the luma plane can be treated as the
  source image for offline YOLO preprocessing when metadata confirms full
  resolution, no downsample, expected pitch/height, and normal camera settings.
- The UV plane is encoder-format bookkeeping for mono sources and should not be
  treated as extra model information.
- The offline calibrator must still run the same Orange YOLO preprocessing
  contract: resize/letterbox to `640x640`, padding `114/255`, mono replicated
  into planar B/G/R channels, FP32 normalized to `[0, 1]`.
- Do not feed the raw NV12 dump directly to TensorRT calibration.

Current implementation constraints:

- `pre_encoder_reference_capture` requires the real in-process recording path.
  It is not supported with `stream_only`, `preprocess_only`, or
  `external_ipc`.
- Keep captures short. The feature adds GPU-to-host copy and disk I/O, so it is
  a data-collection tool, not a normal throughput benchmark mode.
- For four-camera external IPC latency work, keep using the external IPC specs;
  use separate short in-process reference captures only to collect calibration
  examples.

Example experiment-spec fragment for collecting a small calibration sample:

```json
{
  "fixed": {
    "duration_s": 3,
    "warmup_s": 1,
    "display": false,
    "yolo": false,
    "stream_only": false,
    "recording_sink_mode": "real",
    "config_folder": "/home/jeremy/orange-gop-split-a16/config/validated_split_gop_hevc_100fps_gop25_fourcam_a16",
    "output_root": "/home/jeremy/orange_data/exp/unsorted",
    "pre_encoder_reference_capture": {
      "enabled": true,
      "max_frames": 120
    }
  }
}
```

After the run, use `runs.json` or `runs.csv` to find:

```text
pre_encoder_reference_raw_dump_path
pre_encoder_reference_index_path
pre_encoder_reference_metadata_path
```

Then build a calibration manifest with, at minimum:

```text
camera_serial
recording_frame_id
timestamp
raw_dump_path
byte_offset
byte_size
width
height
pitch
pixel_format
source_run
fish_present / no_fish / unknown
```

For a production-quality calibration set, sample across several short captures
rather than dumping one long homogeneous segment. A good first target is
roughly `100-300` frames per camera across multiple visual states, then a
separate held-out validation set with positive fish examples.

### Missing Better Capture Path

The repository does not currently have a first-class "YOLO calibration tensor
dump" that writes the exact `1x3x640x640` FP32 input tensors. If INT8 becomes
the next active model-runtime experiment, that is the cleanest small feature to
add:

- tap after `optimized_yolo_preprocess.cu` produces the model input
- sample every Nth accepted YOLO frame
- write tensor bytes or `.npy` plus manifest rows
- include camera serial, frame id, source image dimensions, preprocessing
  version/hash, engine/model id, and normalization metadata
- keep it bounded and disabled by default

Until that exists, use the pre-encoder reference dump only as a source-frame
capture mechanism and reproduce YOLO preprocessing offline.

Recommended validation data shape:

- Use a held-out set separate from calibration.
- Include positive fish scenes, no-fish scenes, and difficult edge cases.
- Include labels or a trusted FP16 reference output so we can compare box,
  score, and detection/no-detection behavior.
- Include enough positive fish examples to catch single-fish false negatives.

## Builder Deliverables

For a future model-engine builder, provide these artifacts together:

```text
model.onnx
model_fp16_a16.engine
model_int8_a16.engine
calibration.cache              # if implicit TensorRT calibration is used
calibration_manifest.json
validation_report.json
engine_build_commands.txt
engine_metadata.json
```

If using explicit quantization, also provide:

```text
model_qdq.onnx
quantization_method.txt        # PTQ or QAT, tool/version, settings
```

`engine_metadata.json` should include:

- TensorRT version.
- CUDA driver/runtime version.
- GPU model and CUDA device ID used to build.
- ONNX opset and producer.
- Input/output tensor names, shapes, and dtypes.
- Whether `EfficientNMS_TRT` is present.
- Precision mode: FP16, implicit INT8, explicit Q/DQ INT8, or mixed.
- Builder flags, including optimization level and timing settings.
- SHA256 for ONNX, cache, and engine artifacts.

## Quick Experiment Path

The quick path is useful to learn whether INT8 has enough latency upside to
justify a deeper model-export workflow.

1. Export a calibration tensor set from Orange or reproduce the exact Orange
   preprocess offline.
2. Implement a small TensorRT calibrator that feeds `images` batches as FP32
   `1x3x640x640` tensors.
3. Build an A16 INT8 engine using the same TensorRT version as the FP16 engine.
4. Save the calibration cache and engine metadata.
5. Run standalone `trtexec` latency against the FP16 A16 baseline.
6. Run an offline accuracy comparison against held-out validation frames.
7. Only then run the two-camera headless PTP external-recorder validation.

Important:

- `trtexec --int8` alone is not a production calibration workflow.
- `trtexec --calib=<file>` reads an existing calibration cache; the useful
  work is generating that cache from representative Orange tensors.
- Calibration input should still be FP32 even when the engine uses INT8
  internally.

## Preferred Production Path

The cleaner production path is to move quantization into the model workflow:

1. On the training/export machine, run PTQ with representative Orange tensors.
2. Export `model_qdq.onnx` containing `QuantizeLinear` and `DequantizeLinear`.
3. Verify that the exported graph preserves the same single-fish output
   contract and `EfficientNMS_TRT` compatibility.
4. Build the TensorRT engine on an A16 GPU.
5. Compare against the current FP16 A16 engine.
6. If PTQ accuracy is not acceptable, fine-tune with QAT and export a QAT Q/DQ
   ONNX instead.

Expected decision rule:

```text
PTQ Q/DQ is preferred if:
  latency improves materially
  false negatives do not increase
  box drift stays within crop safety margin
  score distribution remains compatible with current thresholds

QAT is preferred if:
  PTQ is fast but detection quality regresses
```

## Validation Gates

An INT8 engine should pass these gates before becoming the default:

1. Interface compatibility:

```text
input/output names match Orange expectations
input shape remains 1x3x640x640 unless code/config changes explicitly support otherwise
outputs remain top-1 num_dets/bboxes/scores/labels
EfficientNMS_TRT loads successfully
```

2. Standalone TensorRT latency:

```text
compare against FP16 bo5_avg32 engine on A16 GPU 5
record throughput, latency p50/p95/p99, GPU compute p50/p95/p99
```

3. Offline accuracy:

```text
compare INT8 vs FP16 on held-out frames
track detection/no-detection agreement
track false negatives on fish-present frames
track false positives on no-fish frames
track bbox center/size drift
track score deltas and threshold crossings
```

4. Headless app latency:

```text
two-camera PTP external-recorder run
ORANGE_PTP_REGISTER_READ_DECIMATE=100 equivalent
full split-GOP recording enabled
detect p95 must beat FP16 baseline materially
no drops, frame gaps, recorder failures, or pending GOP backlog
```

5. GUI/session validation:

```text
same camera/session shape as production
verify startup, engine load, display, recording, and artifacts
```

6. Positive-detection crop/pose validation:

```text
run with fish or equivalent positive target
verify ROI/crop is not degraded by INT8 box drift
verify downstream pose/crop artifacts still make sense
```

## Expected Outcomes

Possible good result:

```text
INT8 GPU compute p95 drops enough to move app detect p95 below current ~3.94 ms
accuracy remains equivalent to FP16 for top-1 fish detection
```

Possible neutral result:

```text
some layers remain FP16 because TensorRT chooses mixed precision
EfficientNMS and output-adjacent layers stay FP32
app-level p95 improves only modestly
```

Possible bad result:

```text
confidence shifts reduce recall
box drift threatens crop ROI
TensorRT plugin/layer constraints limit INT8 coverage
host/app overhead dominates enough that INT8 does not matter
```

The likely first experiment is still worth doing because the current host-side
tail has been reduced enough that model execution time is visible again.

## References

Official NVIDIA TensorRT references:

- TensorRT working with quantized types:
  <https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/work-quantized-types.html>
- TensorRT Python INT8 calibrator API:
  <https://docs.nvidia.com/deeplearning/tensorrt/10.16.0/_static/python-api/infer/Int8/EntropyCalibrator.html>
