# Deferred: Detection On A Big GPU (A6000 Prototype, L40S / RTX 6000 Ada Target)

Date: 2026-09-05. Status: **deferred**. Decision on 2026-09-05: keep the
four-camera pipeline on the A16s as it stands (see
`detect_latency_review_2026_09_03.md` for where that stands: 2.20 ms mean,
2.29 p95, 2.32 p99 acquisition-to-detect with full-frame recording, crops
and crop video on) and revisit this when hardware or a latency need
justifies it. Everything below is measured on the rig's RTX A6000 on
2026-09-04 or is a scaled estimate marked as such.

## What was measured

Same detect ONNX (omnifin0 cedar-shadow v007) built for the A6000 with
`trtexec --fp16`; cameras acquired by GPUDirect straight into GPU 0;
engine-only true direct read; no recorder (the A6000 cannot host one, see
below). Specs: `onecam_engine_only_direct_read_omnifin_a16`,
`onecam_engine_only_direct_read_omnifin_a6000`,
`threecam_engine_only_direct_read_omnifin_a6000`. Engine:
`orange_data/detect/omnifin0_cedar_shadow_v007_detect_20260206-235656_25f3fbcb_a6000_gpu0_trt100_fp16.engine`.

| | A16 die (GA107, 10 SMs) | RTX A6000 (GA102, 84 SMs) |
|---|---|---|
| trtexec GPU compute, median, fp16 with EfficientNMS in the graph | 2.06 ms | 0.68 ms |
| one camera: graph mean / p95 | 2.061 / 2.067 | 0.708 / 0.737 |
| one camera: acquisition to detect mean / p95 / p99 | 2.197 / 2.211 / 2.226 | 0.886 / 0.937 / 1.014 |
| three synchronized cameras on the one GPU: graph mean / p95 | 2.00 / 2.03 each, on its own die | 1.25 / 1.57 |
| three synchronized cameras: acquisition to detect mean / p95 / p99 | 2.20 / 2.21 / 2.23 | 1.43 / 1.78 / 2.40 |

Readings: 8x the SMs and 3.8x the bandwidth buy 3x on this batch-one
640-input graph, which is launch-bound; the CUDA graph already hides most
launch cost. Three batch-one graphs landing together (PTP synchronizes the
cameras to microseconds) overlap only partly on 84 SMs, so the mean gain
drops to 1.5x and the p99 advantage is gone at three cameras; at four it
would be worse. GPUDirect RDMA into the A6000 works (three cameras, 100
fps each, zero gaps).

## Why the A6000 in this rig cannot take the job alone

- No peer access with any A16 die (`nvidia-smi topo -p2p r`: NS against
  all of them; the A16 dies have it with each other across both cards).
  CUDA IPC handles from the analytics process cannot be opened on an A16
  from A6000 memory, so no A16 recorder can read frames the A6000 holds
  (the external crop recorder placed on the A6000 died on its first
  frame with `cudaIpcOpenMemHandle: peer access is not supported`).
- One NVENC engine, which cannot encode even one camera's 20 MP at 100
  fps.

Detection on the A6000 with recording on the A16s therefore means two
host hops per frame (20 MB device-to-host, 20 MB host-to-device, about
1.7 ms each at PCIe 4 rates, off the detect path), 8 GB/s in and 8 GB/s
out of the A6000's link for four cameras, and the copies contending with
the graph on the A6000 the way copies did on the A16 dies. Feasible on
paper; the registered-source recorder would still work because the
host-to-device copy can land in the pool buffer. Not worth building
unless the batched inference below is built too.

## The two designs, if revisited

### A. Batched inference on one big GPU (A6000 prototype)

Because PTP delivers all four frames within microseconds, run one
batch-four inference per period instead of four batch-one graphs. Batch
four on this graph should cost about 1.0 to 1.2 ms for all four (estimate
from the three-camera overlap measurement), a floor near 1.3 ms per camera
against 2.2 today. Work: a batch-N engine build; a scheduler that gathers
the four frames (bounded wait, degrade to a smaller batch if a camera is
late); per-camera output demux back into each camera's YOLO worker path;
and, for recording, the host bounce above or a big card with its own
encoders. The A6000 is enough to prototype the inference half with three
cameras and no recorder.

### B. Two L40S or RTX 6000 Ada cards, two cameras each (preferred)

Each card owns its two cameras end to end: RDMA in, detection, recording
through the card's own NVENC engines from the same memory (the
registered-source path we already run on the A16 detect die), crops and
pose in the idle window on the same card. Nothing crosses a PCIe link
after the frame arrives, so the card switch, the other-die shard, the
crop placement rules and the host bounce all disappear.

- Encode fits: three eighth-generation NVENC engines per card; a GA107
  engine sustains a 20 MP HEVC stream at about 100 fps at p1 and the Ada
  engine is roughly 2x that, so about 600 fps of 20 MP per card against
  the 200 fps two cameras need. Split-GOP becomes optional headroom.
- Latency, estimated from the A6000 measurement: graph 0.4 to 0.5 ms on
  142 Ada SMs at higher clocks; two synchronized cameras per card either
  overlap partly or take one batch-two inference of about 0.6 ms; frame
  floor roughly 0.8 to 1.0 ms with recording on.
- Engineering remaining: a recording profile placing both cameras'
  encoders on the same GPU (the strategy config already expresses
  single-GPU placement); the optional batch-two engine. The copy after
  detection, the registered source, the crop interleave and the verifier
  checks are placement-independent and carry over unchanged.
- Fit: the L40S is a passive 350 W datacenter card that needs chassis
  airflow; the RTX 6000 Ada is the same silicon with active cooling and
  the same three NVENC engines, the right fit for the pancake0
  workstation. GPUDirect RDMA is supported on both.
- Verification plan when a card is present: the engine-only specs on it
  (one camera, then two synchronized), then a two-camera registered spec
  with the recorder on the same GPU, then the crop interleave spec with
  the crop recorder on the same GPU, then the 30-minute endurance. About
  half a day of rig time; every estimate above becomes a measurement.

## What stays on the A16s meanwhile

The tree's defaults (event sync, deferred PTP latch, detect-priority
gate, registered source with copy fallback, copy after detection, GOP-parity
crop interleaving, encoder prewarm at hello, the cap and crop-count
verifier checks) and the roadmap in the review document: four-camera runs
when 2010096 is back, a real animal, the GUI smoke, and the engine (INT8
or input size) as the only A16-side lever with more than 0.1 ms in it.
