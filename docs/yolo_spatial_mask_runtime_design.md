# YOLO Spatial Mask Runtime Design

Status: initial GUI/headless implementation complete; live four-camera
performance and wall-contact recall validation pending.

## Goal

Orange may optionally use the exact camera-native dish geometry selected for a
recording to:

1. replace neural-network input pixels outside a derived input circle with
   tensor zero; and
2. prevent decoded detections whose bounding-box centroid is outside the
   accepted detection region from reaching tracking, IPC, crop production, or
   pose inference.

The original camera buffer and full-frame recording remain unchanged. The
feature is optional and disabled by default.

Input masking alone is not a containment guarantee. A convolutional network can
still emit a prediction in a uniform region because of biases and receptive
field context. The post-decode centroid gate is therefore the load-bearing
containment policy; input masking removes invalid visual evidence and reduces
the opportunity for outside-region predictions.

## Geometry Roles

The runtime must retain three distinct meanings:

- `accepted_inner_rim_boundary`: immutable physical evidence for the water-side
  inner rim.
- `valid_detection_region`: the accepted, slightly outward centroid gate.
- `input_mask_region`: a runtime-derived circle based on the valid detection
  circle plus an explicit `input_context_outset_px`. This extra context avoids
  clipping a wall-adjacent fish whose visible body can extend beyond its valid
  centroid.

The runtime derivation never rewrites the source calibration artifact.

## One Inference Graph, Two Preprocess Specializations

"Fused preprocessing" and "CUDA graph" describe different layers of the
current implementation.

The existing preprocessing kernel performs source sampling, resize, letterbox,
normalization, mono-channel replication, and planar tensor writes. It is
submitted before the captured TensorRT graph. The captured graph contains the
TensorRT enqueue and device-to-host output copies only.

The accepted implementation shape is:

```text
camera source event
        |
        v
masked OR unmasked specialized preprocess kernel
        |
        v
the same TensorRT inference CUDA graph
        |
        v
CPU decode -> spatial centroid decision -> tracking/crop/IPC
```

The CUDA source shares one templated implementation and produces two compiled
specializations:

```cpp
mono_to_yolo_optimized<false>(...);  // existing behavior
mono_to_yolo_optimized<true>(..., circle_mask);  // circle test fused in
```

A uniform host-side decision selects the specialization once per frame from an
immutable per-camera policy. Mask-disabled execution therefore retains the
original pixel work without a per-pixel enabled branch. Mask-enabled execution
maps each output pixel to camera coordinates, applies the analytic squared
distance test, and skips the four bilinear source reads when outside the input
circle.

The TensorRT input address, engine, bindings, and inference graph are identical
in both modes. Enabling a mask does not destroy, recapture, or duplicate the
inference graph.

## Runtime Modes

The first contract supports:

- `off`: no mask evaluation and the original preprocessing path.
- `audit`: evaluate centroid decisions and log would-reject results without
  changing tensors or downstream detections.
- `gate_only`: enforce the centroid gate without modifying network input.
- `gate_and_input_mask`: use the masked preprocessing specialization and enforce
  the centroid gate.

If a non-`off` mode is explicitly requested, missing, stale, dimensionally
incompatible, or non-camera-native geometry is an arming error. Orange must not
silently claim masked operation while running unmasked.

The GUI exposes `YOLO dish spatial policy` beside model selection and resolves
that choice at recording arm. It initializes from the following environment
configuration, which also supplies the headless/benchmark interface:

```text
ORANGE_YOLO_SPATIAL_MASK_MODE=off|audit|gate_only|gate_and_input_mask
ORANGE_YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX=0
ORANGE_YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS=750
```

`off` is the default. A GUI change overrides the process-start environment for
subsequent recording arms without changing camera/model configuration. GUI and
headless execution both produce the same immutable policy contract.

## Lifecycle

At experiment/recording arm, Orange resolves the exact selected
daily-registration artifact for each active camera, verifies its identity and
native raster, derives the input circle, and submits an immutable policy to the
camera's YOLO worker. The worker applies a pending policy only at a frame
boundary and acknowledges its generation before recording begins.

The hot path performs no JSON parsing, filesystem access, homography work,
registration polling, or mask rasterization. Geometry remains fixed for the
run. A new registration requires a new arm operation.

## Audit Contract

Each YOLO event records:

- runtime mode and whether input masking and enforcement were active;
- artifact, registration, and checksum/fingerprint identity;
- raw, inside, outside, and downstream detection counts;
- rejected/would-reject boxes with centroid and signed boundary distance; and
- the exact input and centroid circles used.

The recording geometry contract carries the same runtime policy. Citrus H5 can
therefore preserve what Orange actually applied rather than only what geometry
was available.

## Performance Contract

Orange must not create a masked 4512x4512 source copy or add a second full
network-tensor pass. The analytic test is fused into the existing preprocess
kernel. Validation compares `off`, `gate_only`, and
`gate_and_input_mask` at four cameras and 100 fps using:

- `acquisition_to_detect_done_ms`;
- GPU preprocess duration;
- inference and completion timing;
- YOLO queue wait;
- camera/preprocess/recording drops; and
- input-mask and gate counters.

The current high-effort four-camera reference averages approximately 3.869 ms
steady detect p95. Initial acceptance requires no gaps or drops and no material
p95 regression; 0.1 ms is the provisional investigation threshold rather than
an assumed entitlement.

Positive-fish validation must separately measure wall-contact recall. No-fish
runs are sufficient only for performance and lifecycle validation.
