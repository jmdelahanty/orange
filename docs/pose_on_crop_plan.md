# Pose-on-Crop Integration Plan

## Summary
Add a pose estimation worker that runs a TensorRT engine on cropped detections, draws pose overlays, and publishes pose results via IPC. Target: keep up with 60 FPS recordings.

## Current Assumptions
- Pose should run on every frame that produces a crop (i.e., when YOLO fires and produces detections).
- Output should be drawn on the overlay and also published over IPC.
- Use single-frame payloads (no batching) for now.
- Goal is to keep up with 60 FPS recordings.

Note: YOLO does **not** necessarily run every frame (decimation is possible), so "pose on every frame with a crop" may mean less than 60 FPS if YOLO is decimated.

## TODO
### 1) Define pose input contract
- Confirm TRT engine input size/layout/type (NCHW/NHWC, FP16/INT8, RGB/BGR/NV12, normalization).
- Define crop selection (best detection only vs all detections per frame).
- Decide how to handle frames with no detections (skip pose vs reuse last crop).

### 2) Pipeline hook + data flow
- Identify the exact tap point for GPU crop data (prefer zero-copy GPU buffer).
- Decide whether pose consumes the crop used for encode or a separate crop path.
- Ensure pose waits on crop completion (CUDA events/streams).

### 3) Worker design
- Add a `PoseWorker` with queueing/backpressure and single-frame payloads.
- Integrate with `WORKER_ENTRY` lifecycle (`ref_count`, recycle queue).
- Define drop policy if queue grows (to preserve 60 FPS).

### 4) Overlay
- Add pose skeleton rendering in display path.
- Map pose coordinates from crop space to full-frame display coordinates.

### 5) IPC
- Extend Frame IPC payload to include pose keypoints + scores.
- Publish per-frame pose data keyed by `recording_frame_id` and timestamps.

### 6) Config / UI
- Per-camera enable toggle and engine path.
- Optional thresholds for pose confidence.
- Safe failure behavior when engine missing or load fails.

### 7) Performance verification
- Add timing logs + queue depth monitoring.
- Verify sustained throughput at 60 FPS under load.

## Open Questions
1) TRT engine input: size, layout, data type, normalization, batch size?
2) Detection selection: pose on all detections or only top confidence?
3) Output schema: what keypoints + coordinate system are produced?
4) IPC payload: ok with per-frame list of keypoints + scores?
5) If YOLO is decimated, should pose run at YOLO rate or be forced to full rate?
