# A16 TensorRT Detect Engine Rebuild

Date: 2026-04-26

Scope: document the first A16-specific TensorRT rebuild of the current
single-fish detect model, the measured latency impact, and the recommended
validation path before changing the default engine.

## Bottom Line

Rebuilding the existing ONNX detect model directly on an A16 GPU produced a
material detect-path improvement without changing Orange's hot-path ownership,
external recorder, PTP, YOLO worker, or CUDA graph code.

Standalone TensorRT on A16 GPU `5`:

```text
existing default engine:
  throughput:       345.342 qps
  latency p95:      3.69727 ms
  GPU compute p95:  2.90808 ms

A16-built candidate engine:
  throughput:       433.117 qps
  latency p95:      3.11029 ms
  GPU compute p95:  2.31738 ms

A16 high-effort candidate engine:
  throughput:       449.411 qps
  latency p95:      3.03638 ms
  GPU compute p95:  2.24268 ms
```

Short two-camera headless PTP external-recorder smoke with the A16-built
candidate:

```text
Cam2010095:
  frames:                  101/101
  steady detect p95:       4.029197 ms
  steady YOLO total p95:   3.965077 ms
  drops/gaps/errors:       0

Cam2010096:
  frames:                  101/101
  steady detect p95:       4.024558 ms
  steady YOLO total p95:   3.990675 ms
  drops/gaps/errors:       0
```

Short two-camera headless PTP external-recorder smoke with the high-effort
`bo5_avg32` candidate:

```text
Cam2010095:
  frames:                  101/101
  steady detect p95:       3.878305 ms
  steady YOLO total p95:   3.850672 ms
  drops/gaps/errors:       0

Cam2010096:
  frames:                  101/101
  steady detect p95:       3.888202 ms
  steady YOLO total p95:   3.854450 ms
  drops/gaps/errors:       0
```

Long two-camera headless PTP external-recorder validation with the high-effort
`bo5_avg32` candidate:

```text
Cam2010095:
  frames:                  2803/2803
  steady detect p95:       3.950019 ms
  steady YOLO total p95:   3.900415 ms
  worker-start p95:        0.049393 ms
  detach-copy p95:         0.178866 ms
  drops/gaps/errors:       0

Cam2010096:
  frames:                  2803/2803
  steady detect p95:       3.943577 ms
  steady YOLO total p95:   3.905365 ms
  worker-start p95:        0.048911 ms
  detach-copy p95:         0.164749 ms
  drops/gaps/errors:       0
```

Compared with the previous longer external-recorder + decimated-PTP baseline
at about `4.580 ms` / `4.585 ms` steady detect p95, the long validation shows
about a `0.63-0.64 ms` p95 win for the high-effort candidate while preserving
`100 fps` acquisition and external split-GOP recording health.

## Artifacts

Source ONNX copied from the model-generation workstation:

```text
/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb.onnx
```

Existing default engine:

```text
/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_fp16.engine
sha256: 980feb852f43aa794f480097c9497a0837a9c8b519cbebbb64f84adbcffa5709
size:   7.9 MiB
```

A16-built candidate engine:

```text
/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a16_gpu5_trt100_fp16.engine
sha256: a9100f421ca77ab898faba847d915c5f5ea9386fb75a97a9585be935d737581e
size:   8.2 MiB
```

High-effort A16 candidate engine:

```text
/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a16_gpu5_trt100_fp16_bo5_avg32.engine
sha256: 88a37effb634540ba987d1ebb39e952037c1082cbba0a7e79c7c82de2d03847b
size:   8.6 MiB
```

Short smoke output:

```text
/tmp/orange_external_recorder_ptp_20260426_015757
/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_a16_engine_external_ipc_20260426_015757
```

High-effort short smoke output:

```text
/tmp/orange_external_recorder_ptp_20260426_021347
/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_a16_bo5_avg32_external_ipc_20260426_021347
```

High-effort longer validation output:

```text
/tmp/orange_external_recorder_ptp_20260426_021831
/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_a16_bo5_avg32_external_ipc_20260426_021831
```

## Build Command

The candidate was built with TensorRT `10.0.1` using A16 CUDA device `5`:

```bash
/usr/local/TensorRT-10.0.1.6/targets/x86_64-linux-gnu/bin/trtexec \
  --device=5 \
  --onnx=/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb.onnx \
  --saveEngine=/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a16_gpu5_trt100_fp16.engine \
  --fp16 \
  --profilingVerbosity=detailed
```

Build observations:

- TensorRT selected `NVIDIA A16`, CUDA device ID `5`.
- The A16 has compute capability `8.6`, `10` SMs, and a `128-bit` memory bus
  on this host.
- ONNX parse succeeded.
- ONNX IR version was `0.0.6`.
- ONNX opset was `11`.
- Producer was `pytorch 2.6.0`.
- `EfficientNMS_TRT` was found and imported as a plugin.
- Engine generation took `140.926 s`.
- TensorRT built with `FP32+FP16`; input/output bindings remain FP32/INT32.

## Model Interface

The rebuilt engine preserves the interface Orange expects:

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

Important interpretation:

- This is intentionally a single-best-fish model.
- The engine returns at most one detection per frame.
- CPU postprocess remains tiny because NMS is inside TensorRT and only one
  result is copied/converted.
- The app does not need a CPU NMS path for this engine.

## Why This Helped

TensorRT `.engine` files are compiled execution plans, not portable model
descriptions. They encode tactic choices for a specific TensorRT version,
plugin environment, precision policy, and target GPU characteristics.

The previous engine did run on A16, but TensorRT printed the expected warning
when benchmarking it:

```text
Using an engine plan file across different models of devices is not recommended
and is likely to affect performance or even cause errors.
```

The A16-built candidate lets TensorRT select tactics directly for the A16
profile. That lowered standalone GPU compute p95 from about `2.91 ms` to about
`2.32 ms`. In the current hot path, where host launch/PTP/recording overhead
has already been reduced, that model-execution reduction shows through as
lower detect p95.

## TensorRT Tactics

A TensorRT tactic is the concrete GPU implementation TensorRT chooses for an
operation or fused group of operations.

The ONNX model describes abstract graph operations such as:

```text
Conv
SiLU
Resize
MatMul
Softmax
EfficientNMS_TRT
```

The engine records much more specific choices:

```text
which convolution algorithm
which Tensor Core kernel
which tile size
which memory layout
which fusion boundary
which workspace requirement
which launch structure
```

Those concrete choices are tactics. During engine build, TensorRT benchmarks
candidate tactics on the selected GPU and keeps the plan it expects to be
fastest under the selected precision, layout, and workspace constraints.

This is why a plan built on one GPU can run but still be suboptimal on another
GPU. An RTX A6000 and an A16 are both Ampere-class, but an A16 GPU slice on
this host has only `10` SMs and a `128-bit` memory bus. A tactic that is good
for a larger GPU can be the wrong balance of tiling, memory traffic, and
occupancy for A16.

Two relevant builder knobs:

- `--builderOptimizationLevel=<n>` controls how aggressively TensorRT searches
  the tactic space. Higher values can find faster plans but take longer to
  build.
- `--avgTiming=<n>` controls how many timing measurements are averaged when
  comparing tactics. Higher values can reduce noisy choices but also increase
  build time.

The first A16 rebuild already proved tactic selection matters for this model.
The next bounded experiment is a higher-effort A16 build with
`--builderOptimizationLevel=5` and `--avgTiming=32`.

That experiment also helped, but more modestly than the first A16 rebuild. The
high-effort build command was:

```bash
/usr/local/TensorRT-10.0.1.6/targets/x86_64-linux-gnu/bin/trtexec \
  --device=5 \
  --onnx=/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb.onnx \
  --saveEngine=/home/jeremy/orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a16_gpu5_trt100_fp16_bo5_avg32.engine \
  --fp16 \
  --builderOptimizationLevel=5 \
  --avgTiming=32 \
  --profilingVerbosity=detailed
```

Build and standalone benchmark result:

```text
build time:         460.859 s
engine size:        8.516 MiB
throughput:         449.411 qps
latency p95:        3.03638 ms
GPU compute p95:    2.24268 ms
```

Compared with the first A16-built candidate, this reduced standalone GPU
compute p95 by another `0.07470 ms` and improved throughput by about `3.8%`.
That is a real but smaller win. The extra build time is a one-time cost, so the
candidate is worth app-level validation, but the expected app-level p95
improvement may be close to measurement noise on a short run.

The short app-level smoke did show an additional win:

```text
first A16 candidate:
  Cam2010095 steady detect p95: 4.029197 ms
  Cam2010096 steady detect p95: 4.024558 ms

high-effort A16 candidate:
  Cam2010095 steady detect p95: 3.878305 ms
  Cam2010096 steady detect p95: 3.888202 ms
```

That is another `0.14-0.15 ms` p95 reduction on the same short smoke shape.
The following `30 s` validation confirmed the improvement was not just a
short-run artifact:

```text
high-effort A16 candidate, 30 s:
  Cam2010095 frames:            2803 received / 2803 acked / 2803 encoded
  Cam2010095 steady detect p95: 3.950019 ms
  Cam2010095 steady total p95:  3.900415 ms

  Cam2010096 frames:            2803 received / 2803 acked / 2803 encoded
  Cam2010096 steady detect p95: 3.943577 ms
  Cam2010096 steady total p95:  3.905365 ms
```

The long run had `0` frame-id gaps, `0` get-frame errors, `0` skips, `0`
drops, no recorder failures, and `pending_gops = 0` for both cameras.

## Smoke Command

The smoke used a temporary spec with only the YOLO engine path changed to the
A16 candidate:

```bash
cd /home/jeremy/orange-gop-split-a16

scripts/run_external_recorder_two_camera_ptp_smoke.sh \
  --spec /tmp/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_a16_engine.json \
  --duration 3 \
  --warmup 1 \
  --ptp-register-read-decimate 100 \
  --skip-video-sanity
```

The generated spec preserved:

- two-camera PTP gate sync,
- external split-GOP recorder,
- `ORANGE_YOLO_DETACH_INPUT=1`,
- `ORANGE_YOLO_READY_EVENT_FASTPATH=1`,
- `ORANGE_ANALYTICS_EARLY_OWNED_FRAME=1`,
- YOLO `decimate=1`,
- YOLO `prewarm_iterations=3`.

The high-effort smoke used the same command shape with a temp spec pointing at
the `bo5_avg32` engine:

```bash
cd /home/jeremy/orange-gop-split-a16

scripts/run_external_recorder_two_camera_ptp_smoke.sh \
  --spec /tmp/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_a16_bo5_avg32.json \
  --duration 3 \
  --warmup 1 \
  --ptp-register-read-decimate 100 \
  --skip-video-sanity
```

The longer validation used the same temp spec for `30 s`:

```bash
cd /home/jeremy/orange-gop-split-a16

scripts/run_external_recorder_two_camera_ptp_smoke.sh \
  --spec /tmp/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_a16_bo5_avg32.json \
  --duration 30 \
  --warmup 1 \
  --ptp-register-read-decimate 100 \
  --skip-video-sanity
```

## Caveats

- The first smoke was short: `101` frames per camera. The later `30 s` run
  covered `2803` frames per camera and confirmed the latency signal, but it is
  still not a long soak.
- The candidate was built on A16 GPU `5` and then used on both analytics GPUs
  `5` and `7`. They are the same GPU class, but a longer two-camera validation
  and GUI validation should still verify both deployment paths.
- Standalone `trtexec` includes its own host input-transfer path with random
  inputs. Orange uses GPUDirect camera frames plus the custom CUDA preprocess
  path. Compare app-level YOLO CSVs before making product decisions.
- Do not overwrite the default app config or default engine path until a
  GUI/session validation passes.

## Recommended Next Steps

1. Run a GUI/session validation through
   `./scripts/run_gui_aq_off_validation.sh`. The launcher sets
   `ORANGE_PTP_REGISTER_READ_DECIMATE=100` and uses
   `ORANGE_DEFAULT_DETECT_ENGINE` to select the high-effort A16 candidate for
   that run without changing the persistent app config.
2. If it passes, update the default app model config to the high-effort
   A16-built engine.
3. Keep the old engine available for rollback until a positive-detection
   crop/pose smoke validates downstream behavior with a fish or equivalent
   target.
4. Consider a longer soak after the GUI path selects the new engine by default.
5. Treat INT8 as the next model-runtime experiment after the GUI gate. See
   [detect_int8_quantization_plan.md](/home/jeremy/orange-gop-split-a16/docs/detect_int8_quantization_plan.md)
   for the calibration, Q/DQ, PTQ/QAT, and validation requirements.
