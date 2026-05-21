# Palette / Orange Tensor Input Contract

Status: current Orange audit as of 2026-05-16.

Purpose: make the runtime pixel contract explicit for Palette crop caches that
feed downstream keypoint, pose, detection, segmentation, or future TensorRT
deployment work. This document is about TensorRT inference input. Do not infer
this contract from Orange's recording encoder path.

## Executive Summary

For monochrome cameras, Orange TensorRT inference starts from a single-channel
`uint8` mono/luma image. Orange then performs preprocessing outside TensorRT and
writes a 3-channel planar FP32 NCHW tensor into the TensorRT input buffer.

For mono input, the three model channels are replicated luma:

```text
B = mono / 255
G = mono / 255
R = mono / 255
```

Because all three channels are equal for mono sources, this is numerically
equivalent to `R = G = B = mono / 255`, despite the current kernel storing the
planes in B, G, R order.

The best Palette crop-cache contract for Orange mono recordings is therefore:

```text
pynvvc_luma_v1: [N, H, W] uint8 decoded luma crops
```

Then apply Orange-compatible model preprocessing at consumption time. Do not
make normalized CHW tensors the canonical cache unless the cache is explicitly
versioned to a single model input size, layout, normalization, and engine
contract.

## Recording Path Is Separate

Orange's mono recording path prepares NV12 for NVENC by copying the mono image
into the Y plane and filling UV with neutral chroma. That is an encoder input
format, not the TensorRT input format.

TensorRT inference does not consume NV12 Y/UV. It consumes the original mono
camera buffer or an owned mono ROI crop, then Orange preprocessing converts that
mono buffer into a 3-channel FP32 planar tensor.

Palette decoding an Orange MP4 through PyNvVideoCodec and reading the decoded
NV12 Y plane is representation-compatible with the mono inference source. The
decoded luma is reconstructed video luma, not necessarily byte-identical to the
original camera frame after lossy encoding, but the channel contract matches.

## Detection TensorRT Contract

Current deployed detection uses the shared `YOLOv8` TensorRT wrapper.

Observed current engine interface:

```text
input:
  images: FP32, shape [1, 3, 640, 640]

outputs:
  num_dets: INT32, shape [1, 1]
  bboxes:   FP32, shape [1, 1, 4]
  scores:   FP32, shape [1, 1]
  labels:   INT32, shape [1, 1]
```

The C++ wrapper discovers input dimensions from the TensorRT engine and uses
`dims.d[2]` and `dims.d[3]` as input height and width. The current checked
engine interface is documented as FP32 `1x3x640x640`.

Detection preprocessing:

- source for mono cameras: raw `uint8` mono camera frame
- resize: letterbox to engine input size
- interpolation: bilinear in the mono path
- padding value: `114`
- conversion: `uint8` to FP32
- normalization: divide by `255.0`
- layout: planar 3-channel tensor, NCHW
- channel order in memory: B plane, then G plane, then R plane
- mean/std subtraction: none
- preprocessing location: outside the TensorRT engine

For mono cameras, `YoloWorker` passes the raw mono GPU buffer directly to
`YOLOv8::preprocess_gpu(..., false)`. Color cameras have a separate Bayer/RGBA
path, but that is not the mono recording contract.

Important implementation note: `YOLOv8` allocates the TensorRT input buffer
using the engine-reported dtype size, but preprocessing writes to that buffer as
`float*`. The deployed detection engine must therefore have FP32 input.

## Pose / Keypoint TensorRT Contract

Orange has a separate real pose/keypoint TensorRT backend:
`TensorRtPoseBackend` in `src/pose_worker.cpp`.

The real pose backend enforces:

```text
input dtype: FP32
input shape: [1, 3, H, W] NCHW
current real pose engine: [1, 3, 256, 256]

output dtype: FP32
output shape: [1, 5 + 3K, N]
```

The current real pose smoke spec records `input_dtype = "fp32"` and
`input_layout = "nchw"`. Some noop/config metadata defaults still mention
`fp16`, and the headless parser accepts `fp16|fp32|uint8`, but the actual real
TensorRT pose backend rejects non-FP32 input tensors.

Pose source representation:

- `CropProducer` creates `d_crop_mono`, an owned single-channel `uint8` ROI crop.
- The ROI is copied from the mono source frame with `cudaMemcpy2DAsync` or the
  optional `mono_roi_copy_kernel`.
- `PoseWorker` waits for `crop_ready_event`.
- `TensorRtPoseBackend::infer()` preprocesses `d_crop_mono` with the same
  `launch_optimized_yolo_preprocess(..., false)` path used for mono detection.

Pose preprocessing is therefore the same mono-to-3-channel FP32 path:

- letterbox resize to engine input size
- pad value `114`
- divide by `255.0`
- planar NCHW
- B, G, R planes, all equal for mono input
- no mean/std subtraction
- outside TensorRT

## Segmentation Contract

No deployed TensorRT segmentation inference path was found in the current Orange
code. Orange has spatial/dish mask artifacts and docs that mention segmentation
quality, but there is no segmentation engine loader, input-buffer allocation,
preprocess path, or `enqueueV3` path analogous to detection or pose.

Future segmentation deployment should either reuse this mono-luma cache plus a
clearly versioned preprocess step, or define a new explicit model-specific
contract.

## Source References

Detection TensorRT engine loading and binding:

- `src/yolov8_det.cpp:54` reads the `.engine` file.
- `src/yolov8_det.cpp:68` creates the TensorRT runtime.
- `src/yolov8_det.cpp:74` deserializes the CUDA engine.
- `src/yolov8_det.cpp:86` iterates TensorRT IO tensors.
- `src/yolov8_det.cpp:100` reads input profile shape.
- `src/yolov8_det.cpp:118` sets `inp_h_int` and `inp_w_int` from NCHW dims.
- `src/yolov8_det.cpp:181` allocates input device buffers.
- `src/yolov8_det.cpp:302` binds tensor addresses.
- `src/yolov8_det.cpp:355` calls `enqueueV3`.

Detection source and preprocessing:

- `src/yolo_worker.cpp:948` constructs `YOLOv8` from the selected engine path.
- `src/yolo_worker.cpp:1437` chooses color vs mono preprocessing.
- `src/yolo_worker.cpp:1443` passes mono camera memory to `preprocess_gpu`.
- `src/yolov8_det.cpp:218` implements `YOLOv8::preprocess_gpu`.
- `src/yolov8_det.cpp:224` launches preprocessing into the TensorRT input buffer.
- `src/optimized_yolo_preprocess.cu:16` defines the mono preprocessing kernel.
- `src/optimized_yolo_preprocess.cu:34` sets pad value `114`.
- `src/optimized_yolo_preprocess.cu:67` divides by `255.0`.
- `src/optimized_yolo_preprocess.cu:74` writes replicated mono into B/G/R planes.

Pose TensorRT backend:

- `src/pose_worker.cpp:197` defines `TensorRtPoseBackend`.
- `src/pose_worker.cpp:213` reads the pose engine file.
- `src/pose_worker.cpp:214` creates the TensorRT runtime.
- `src/pose_worker.cpp:218` deserializes the CUDA engine.
- `src/pose_worker.cpp:222` creates the execution context.
- `src/pose_worker.cpp:360` reads input/output dtypes and enforces FP32.
- `src/pose_worker.cpp:381` enforces input shape `1x3xHxW`.
- `src/pose_worker.cpp:416` allocates input/output buffers.
- `src/pose_worker.cpp:425` binds tensor addresses.
- `src/pose_worker.cpp:307` preprocesses `d_crop_mono`.
- `src/pose_worker.cpp:318` calls `enqueueV3`.

Pose crop source:

- `src/crop_producer.h:40` defines `CropFrame::d_crop_mono`.
- `src/crop_producer.cpp:112` allocates mono crop buffers.
- `src/crop_producer.cpp:392` sizes each crop as `crop_width * crop_height`.
- `src/crop_producer.cpp:556` copies the mono ROI into `d_crop_mono`.
- `src/kernel.cu:168` defines the optional mono ROI copy kernel.
- `src/pose_worker.cpp:714` waits for the crop-ready CUDA event.

Current documented model interfaces:

- `docs/a16_tensorrt_detect_engine_rebuild.md:173` documents detection input
  as FP32 `1x3x640x640`.
- `docs/headless_experiment_backend.md:513` documents the first supported real
  pose engine as FP32/NCHW `1x3x256x256`.
- `experiment_specs/2010096_headless_real_yolo_pose_real_synthetic_center_box_pool32_a16_gpu5.json:28`
  records the current real pose smoke config.

## Palette Recommendation

Use this as the canonical mono crop cache:

```text
name: pynvvc_luma_v1
shape: [N, H, W]
dtype: uint8
source: decoded NV12 Y/luma plane from Orange MP4
semantics: mono/luma crop before model preprocessing
```

Consumers that want Orange-equivalent TensorRT input should transform each crop
with the model-specific Orange preprocess:

```text
uint8 luma crop
  -> letterbox resize to model HxW with pad 114
  -> FP32
  -> divide by 255
  -> replicate into 3 planar channels
  -> NCHW [1, 3, H, W]
```

Do not use replicated RGB `[N, H, W, 3] uint8` as the canonical cache unless a
specific downstream consumer cannot do replication cheaply. It is larger and
does not better represent Orange's runtime source. Do not use normalized CHW as
the canonical cache unless it is explicitly tied to one engine/model version.
