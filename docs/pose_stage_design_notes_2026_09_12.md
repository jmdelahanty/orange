# Pose Stage Design Notes: SLEAP, A Head Crop, One Graph, And The Camera Buffer

Follow-up to `docs/pose_second_stage_review_2026_09_04.md` (sections A to F).
Four questions from 2026-09-12: what SLEAP achieves with TensorRT and how
that compares with this rig; how to design a second, smaller crop for the
components on the fish's head; whether one CUDA graph for both stages would
work; and whether an arbitrary pose engine of the right shape (a SLEAP model
in particular) could be dropped into the pose worker. Plus one question about
the lifecycle: why the camera buffer is held for 2.35 ms.

Numbers from this rig are the measured ones in section F of the review (two
cameras, dies 1 and 7, GA107 with 10 SMs each, 4512x4512 mono at 100 fps,
pose real on every frame from a synthetic centre box). Numbers from SLEAP are
what the papers and the sleap-nn release notes report, on their hardware.

## What SLEAP reports

| Source | Setup | Number |
|---|---|---|
| SLEAP, Nature Methods 2022 | TensorFlow, top-down (centroid + centered instance), 1024x1024 frames, 13 landmarks, two animals, batch 1 | 3.2 ms per frame (312 fps); under 3.5 ms end to end |
| Same | large batches | 762 fps flies, 358 fps mice |
| Same | hardware | Titan RTX class; "comparable" on an RTX 2080 Ti |
| SLEAP preprint, bioRxiv 2020 | Titan RTX, batch 1 | 39 to 89 fps depending on model; 430 fps only at batch 128 |
| sleap-nn 0.1.0 release notes, 2026 | TensorRT FP16 on an RTX A6000, batch 8 | single instance 11,039 fps; top-down 525 fps; bottom-up 524 fps (3.5x to 5.6x over PyTorch) |
| sleap-nn export guide | export contract | fixed image size at export, batch up to 8, FP32 / FP16 / TF32; top-down exports as two engines with separate `centroid_scale`, `instance_scale`, `crop_size` |
| DeepLabCut-Live, eLife 2021 | GPU, dynamic cropping | 10 to 15 ms |
| SqueakPose Studio, 2026 | YOLOv11n-pose, TensorRT, Jetson Orin Nano | 15 to 20 ms FP16, 7 to 10 ms INT8 |

Three things to take from the table. SLEAP's throughput headlines are
batch numbers; its latency number is 3.2 ms at batch 1 on a 72-SM GPU with
no camera in the path. The sleap-nn TensorRT figure for the two-stage
pipeline is about 1.9 ms per frame at batch 8 on an 84-SM A6000, which is
throughput, not latency. And SLEAP's pipeline has the same shape as ours: a
cheap first stage on a downsampled frame, a fixed-size crop centred on the
animal, then a small instance network on the crop, decoded by peak finding
on heatmaps.

## Where this rig stands

| Stage, per frame | Measured (2026-09-12, mean / p95) | Notes |
|---|---|---|
| Acquisition to detect done | 2.275 / 2.326 ms | preprocess 0.076, graph 1.974, host slack around the graph |
| Detect done to crop thread start | 0.167 / 0.403 ms | thread hop 1 |
| Crop thread to crop enqueued | 0.096 / 0.112 ms | CPU work, includes the pose enqueue |
| Crop enqueued to pose thread start | 0.064 / 0.183 ms | thread hop 2 |
| Pose start to pose done | 0.957 / 1.279 ms | crop-ready wait, resize, yolov8n-pose graph at 256, output copy, sync |
| Capture to pose done | 3.559 / 3.974 ms | |

So 3.56 ms end to end on a 10-SM die from a 20 MP frame, against SLEAP's
3.2 ms on a 72-SM GPU from a 1 MP frame. Measured afterwards on this rig's
own big GPU (one camera into the RTX A6000, review section F): 1.93 ms mean
/ 2.08 ms p95 capture to pose done, with the pose graph at 0.43 ms by trtexec
but the pose stage still 0.82 ms p50, because the worker's fixed per-frame
overhead dominates once the graph is fast. Same band, a seventh of the
silicon, and a much heavier first stage: our detector is a YOLO at 640 with
NMS in the graph (2.0 ms), where SLEAP's centroid stage is a small UNet on a
downsampled frame. The pose stage plus its two hops is about 1.3 ms, and
about 0.5 ms of that is host slack rather than GPU work.

## A second, smaller crop for the head

**Superseded the same day.** The detector's boxes already enclose only the
head and the swim bladder (the training labels came from background
subtraction with erosion, which trimmed the tail), and the fish are held in
about 3 mm of water under a top-down camera that sees the whole space, so
scale is constant. The head crop is therefore a smaller fixed crop centred
on the box centroid, and the pose model retrained on that crop is the head
model. No coarse pose, no keypoint-driven second ROI, no rotation stage.
The crop rule and the training path are in
`docs/analytics_pipeline_configuration_design_2026_09_12.md`. The coarse-to-fine design
below is kept as the answer for a rig where the box is not already the
head.

The 256 crop is already at 1:1 sensor resolution, and its three keypoints
(eyes and bladder) are head landmarks, so the coarse stage already locates
and orients the head. The design that follows is coarse-to-fine:

1. Detector gives the fish box (existing).
2. Coarse pose on the 256 crop gives the eyes and bladder (existing model).
3. A head crop at 1:1 resolution, centred between the eyes and rotated to
   the eye-to-bladder axis, feeds a fine model trained only on the head
   components.

The rotation is one warp kernel (bilinear sample of a rotated square,
identical in cost to the resize kernel) and it removes orientation from
what the fine model has to learn, which is the main reason a head-only
model can be small. Choose the head crop so that the smallest component
you care about is at least four to six pixels wide in the model input at
1:1; the fish's length in sensor pixels sets that number and is not
recorded here.

Two alternatives and why not first:

- **A head class in the detector.** Free at inference (same graph), but at
  the detector's 7x downsample a head of about 80 px is about 11 px in the
  640 input, at the edge of what yolo11n resolves. Worth a training
  experiment, not a design dependency.
- **A temporal prior.** Centre the head crop on the previous frame's head,
  validated against the current box, and skip the coarse pose most frames
  (DeepLabCut-Live's dynamic cropping). Saves about 0.9 ms per frame but is
  stateful and needs a recovery path when the head is lost. Later, once the
  coarse-to-fine path is measured.

A caveat that changes the arithmetic: the detect review found the yolo11n
graph time on this die nearly flat with input size. The die is layer-count
bound, not pixel bound. A 128 head crop into a YOLO-pose model will not run
four times faster than 256; expect 0.9 ms to become perhaps 0.5 to 0.6 ms.
The milliseconds come from fewer layers, and the head-crop payoff is
resolution and accuracy. A SLEAP-style heatmap UNet is the natural shallow
model for a single known instance in a fixed crop.

## One CUDA graph for both stages

Yes, and it is the change to make first, because it attacks the part of the
budget that is not model compute.

**Shape.** One CUDA graph on the YOLO stream: preprocess, the detect engine,
a device-side crop-and-resize kernel that reads the top box straight from
the NMS output buffer, the pose engine, and the device-to-host copies of
both outputs. Every address is fixed, so stream capture is straightforward
(the detect engine already runs as a graph; a graph launch inside capture
becomes a child node). The crop reads the camera buffer, which is held until
2.35 ms regardless, so the hold does not change. The head stage is two more
nodes, the warp kernel and the fine engine, with no new thread.

**What it buys, from the measured numbers.**

- Both thread hops go away: 0.17 and 0.06 ms mean, 0.40 and 0.18 ms p95.
- Most of the 0.13 ms host penalty pose adds to detect goes away, since no
  extra threads wake per frame.
- The section D ordering becomes structural: frame N's pose is on the same
  stream as frame N's detect and cannot overlap frame N+1's graph.
- Expected capture-to-pose-done about 3.0 to 3.1 ms mean and 3.2 ms p95,
  against 3.56 and 3.97 today, with the pose graph unchanged at 0.8 ms.

**What it costs.**

- Pose runs on every frame, detection or not. At 0.8 ms inside a 7.5 ms
  idle window that is affordable; the CPU discards the result. CUDA 12.4
  conditional graph nodes can skip it if wanted.
- The detect-done signal must stay at the detect stage: an event record
  node between the two engines keeps the recorder handoff and the lever 2d
  pool copy at 2.15 ms. The pool copy then overlaps the pose graph and
  will cost pose something like the 0.26 ms it cost detect. Pose is not on
  the recorder's path, so that trade is fine.
- Crop video and preview still need the pooled `CropFrame`, so the crop
  producer path stays for them; the fused crop node can also write a
  staging buffer they copy from, or be repointed per launch with a
  node-parameter update.
- Gate for the prototype: `infer_ms` unchanged, capture-to-pose-done p95
  under 3.3 ms, on the two-camera synthetic spec.

## Arbitrary engines of the right shape, and a SLEAP model in particular

**What the backend fixes today** (`TensorRtPoseBackend` in
`src/pose_worker.cpp`):

| Aspect | Enforced today | Needed for a SLEAP centered-instance engine |
|---|---|---|
| Input | FP32, NCHW, 1x3xHxW; mono replicated into three planes, divided by 255, letterboxed by the shared preprocess kernel | grayscale 1x1xHxW, float in [0, 1]; no letterbox when crop size equals input size |
| Output | FP32, 1x(5+3K)xN with (C-5) divisible by 3; decoder takes channel 4 as the score, keypoints at 5+3k, picks the best candidate, undoes the letterbox | confidence maps 1xKx(H/s)x(W/s); decoder is per-channel peak finding with the output stride and optional sub-pixel refinement |
| Keypoint names | three built-in labels, otherwise `keypoint_i`; `skeleton_id` / `skeleton_path` are metadata only | read from the skeleton file |
| Config | `input_width/height/layout/dtype`, `normalization: model_default` only | add `input_channels`, `output_format` (`yolo_pose` or `heatmap`), `output_stride`, and let `skeleton_path` supply labels |

**How much work.** The runtime side is modest, on the order of a few
hundred lines: a decoder interface with two implementations (the existing
YOLO-pose one, and a heatmap peak finder, which on a KxHxW map of a few
tens of KB is a CPU loop or a trivial kernel), a one-channel and FP16
option in the preprocess kernel, the validation to match, and the perf and
event-log plumbing, which already carries per-frame keypoints and does not
care where they came from. The fixed-shape contract is what makes it
tractable: engines must be built for the GA107 dies with batch 1, which is
the existing detect-engine workflow (`docs/a16_tensorrt_detect_engine_rebuild.md`:
export ONNX, build with trtexec on the rig).

**Where the real work is.** Not the runtime. It is (1) training a SLEAP
model on this data with a crop size and anchor part that match what the
crop producer cuts, and (2) preprocessing parity: the exported engine must
see exactly what sleap-nn fed it in training (grayscale float in [0, 1],
same crop size, same input scale, no letterbox), which is a test with a
saved crop and a reference prediction, not a code problem. sleap-nn's
export produces fixed-size engines with batch up to 8; build ours at batch
1 from its ONNX. The hardest piece is the head-crop model's training data,
and that is the same whether the model is a SLEAP UNet or a YOLO-pose.

**Verdict.** Not too much work. The decoder abstraction is worth doing on
its own, because it makes the pose backend independent of the training
framework and lets the head model be whichever architecture measures best.
Sequence it after the fused graph, since the graph's crop node is where the
one-channel input lands anyway.

## Why the camera buffer is held for 2.35 ms

It is not because stages need it. Only one stage reads the camera buffer:
YOLO preprocess, for 0.08 ms, done at about 0.14 ms. Every other consumer
(the recorder's NVENC, the crop producer, display, snapshot) reads the pool
copy. The buffer goes back to the SDK when two things have happened:
preprocess has finished reading it (input-ready event, 0.14 ms) and the
20 MB pool copy has finished reading it (ready event, 2.35 ms). The copy is
what sets the hold.

The copy is late by design, and that is lever 2d. Issued at t = 0 it ran
alongside preprocess and slowed it from 0.08 to 0.33 ms; issued after
preprocess it ran alongside the graph and slowed the graph from 2.00 to
2.26 ms. Issued after the completion event it costs detect nothing, and the
buffer is returned at 2.35 ms instead of 0.36. The hold is the price of
keeping 0.21 ms off acquisition-to-detect, and the ring of 100 buffers
makes that price zero: a 2.35 ms hold at a 10 ms period is a quarter of
one buffer in flight. Even a 10 ms hold is one buffer. The ring would
matter above a few thousand frames per second, which is not a
configuration this sensor has.

What would shorten it, and why not to:

- Copy at t = 0: hold 0.36 ms, detect +0.21 ms. That is the trade lever 2d
  reversed.
- No copy at all, consumers read the camera buffer directly: the hold
  becomes the recorder's, 7 to 10 ms of NVENC per frame plus its deferred
  release of up to 32 to 48 frames, so 30 to 50 ring buffers out at once.
  Still within 100, but it also needs the SDK's buffer to be NV12-shaped
  and NVENC-registered, which is the lever 2c question the detect review
  left open, and it gives up the copy that isolates the recorder from
  acquisition.

The fused graph does not change this: if its crop node reads the camera
buffer, the "YOLO finished reading" condition moves from 0.14 to about
2.2 ms, and the copy at 2.35 ms still dominates. The only reason to think
about the hold at all is bandwidth, not buffer count: the copy moves 40 MB
through the die's memory per frame (20 in, 20 out) at 0.27 ms of copy
engine time, and that is what any future "read the ROI before the copy"
trick would be saving.

## Throughput levers, ranked

1. **The fused graph, detect through pose.** About 0.5 ms mean and 0.8 ms
   p95 off capture-to-pose, and pose stops costing detect anything. No
   model change.
2. **Fewer layers in the pose model, not fewer pixels.** A shallow head
   model or a heatmap UNet at 128 could bring 0.9 ms to 0.3 to 0.5 ms.
   Needs training; the decoder abstraction makes the architecture choice
   free at runtime.
3. **The detector at 2.0 ms.** INT8 and a smaller input remain the levers
   from the detect review. A SLEAP-style centroid network on a downsampled
   frame is far cheaper than a YOLO at 640 if the arena allows a
   single-class, single-animal detector, but that is a different detector
   programme.
4. **Batch 1 is correct.** SLEAP's headline throughput is batch 8 to 128.
   With one camera per die there is nothing to batch and latency is what
   the loop needs.
5. **Event-based completion on the pose thread** instead of a blocking
   stream sync, if the fused graph is not taken. Moot once the YOLO thread
   owns the whole graph.

Sources: SLEAP, Nature Methods 2022
(https://www.nature.com/articles/s41592-022-01426-1); SLEAP preprint,
bioRxiv 2020 (https://www.biorxiv.org/content/10.1101/2020.08.31.276246v1.full);
sleap-nn releases (https://github.com/talmolab/sleap-nn/releases); sleap-nn
export guide (https://nn.sleap.ai/dev/guides/export/); DeepLabCut-Live,
eLife (https://elifesciences.org/articles/61909); SqueakPose Studio
(https://pmc.ncbi.nlm.nih.gov/articles/PMC13336768/).
