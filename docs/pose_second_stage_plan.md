# Plan: Second-Stage Pose TensorRT on Cropped ROI

Date: 2026-01-27
Author: Codex (outline)

## Goal
Add a second-stage pose model (TensorRT) that consumes a high-resolution GPU
crop and produces real-time pose results without breaking current display or
recording throughput.

## Goal Update (2026-04-22)

The intended path is pose inference on the high-resolution GPU crop before any
crop-video encoding. The encoded crop video is an optional recording artifact,
not the input to pose.

Implications:

- Crop generation should become a reusable producer stage.
- `CropAndEncodeWorker` should not own the only crop-generation path.
- Pose should consume a GPU crop payload before NVENC/NV12 conversion.
- Crop recording should consume the same crop payload, or a derived resized copy,
  after pose-compatible crop production.
- If the pose model wants a larger crop than the current default `256x256`,
  the pipeline should generate that larger crop first and downsample/convert
  only for crop-video encoding when needed.

Latency goal update (2026-04-23):

- Pose should happen as close as possible to the detection result that selected
  the ROI.
- The desired ordering is:
  - run detection on a reduced-resolution view of the frame,
  - resolve the ROI immediately,
  - produce the high-resolution crop once,
  - launch pose from that crop before lower-priority consumers such as preview
    or crop-video encode add avoidable delay.
- This means the long-term pose path should be treated as the primary low-
  latency continuation of detection, not merely as a background sidecar
  consumer of the crop pipeline.
- The current noop `PoseWorker` validates ownership and queueing, but it does
  not yet prove the final low-latency ordering.

High-frame-rate note (2026-04-23):

- A future `~2.8 MP Mono8 @ 500 fps` workflow is not automatically more
  bandwidth-heavy than the current `4512x4512 Mono8 @ 100 fps` workflow.
- Roughly:
  - current `20 MP @ 100 fps` is about `2.04 GB/s` per camera,
  - future `2.8 MP @ 500 fps` is about `1.4 GB/s` per camera.
- The harder part at `500 fps` is the frame budget, not just the byte budget:
  - `100 fps` leaves about `10 ms/frame`,
  - `500 fps` leaves about `2 ms/frame`.
- Therefore the pose architecture should optimize first for low per-frame host
  overhead, explicit ownership boundaries, and bounded best-effort fanout,
  rather than assuming raw bandwidth will be the dominant constraint.

## Current Assumptions (2026-02-13)
- Pose runs on every frame that produces a crop (i.e., when YOLO fires and produces detections).
- Output should be drawn on the overlay and published over IPC.
- Use single-frame payloads (no batching) for now.
- Target is to keep up with 60 FPS recordings.
- Note: YOLO does not necessarily run every frame (decimation is possible), so pose rate may be less than 60 FPS if YOLO is decimated.

## Constraints / Existing Architecture
- `CropAndEncodeWorker` already generates a configurable square crop on GPU.
- `CThreadWorker` stages rely on per-frame CUDA events for readiness.
- Most stages are asynchronous and do not pass items to output queues.
- `CropAndEncodeWorker` currently uses a single `d_cropped_rgba_` buffer.
- Detection is intentionally run on a downsampled representation of the full
  frame because `4512x4512` full-resolution detect is too slow to be practical.
- Therefore the pose path is inherently hybrid:
  - detect on reduced-resolution full-frame input,
  - pose on a high-resolution crop extracted from the original source frame.

## Proposed Architecture
Add a new worker stage:
- `PoseWorker` (new): consumes `PoseEntry` items containing a cropped GPU buffer + a readiness event.
- A small pool of `PoseEntry` buffers to avoid overwriting data in flight.
- Events to synchronize crop -> pose without CPU blocking.

High-level flow:
```
Acquisition -> YOLO -> CropProducer -> PoseWorker -> (IPC/UI/log)
                                |--> preview (PBO)
                                |--> crop encode (NVENC)
```

The crop producer owns ROI selection, source-to-crop geometry, GPU crop
production, buffer pooling, and readiness events. Preview, crop encoding, and
pose inference are consumers. A slow consumer should drop its own work rather
than backpressure acquisition or full-frame recording.

## Why Split Crop Before Pose

The current GUI crop path is useful as a validated transitional implementation,
but it should not be extended directly into pose. In the current combined path,
one worker owns crop selection, preview copy, crop-video encode submission,
metadata writes, synchronization, and source-frame release. The measured crop
worker tail latency can exceed one `100 fps` frame period even when there are no
drops. That tail latency is a warning about ownership coupling, not necessarily
about crop-kernel cost.

Pose should be added after crop production is separated from crop consumers:

```text
YOLO result
  -> CropProducer
      -> bounded CropFrame buffer
      -> crop_ready_event
      -> release original source frame
  -> consumers:
      -> preview
      -> crop video encoder
      -> pose TensorRT
```

This is the important source-frame lifetime boundary. The original camera frame
should only be held until the ROI has been copied into a crop-owned GPU buffer.
After that, preview, encoding, and pose can run independently. If a downstream
consumer is slow, it should drop or skip crop work rather than delaying original
camera-buffer reuse.

Implementation checklist before pose:

- Current first split slice: `CropAndEncodeWorker` snapshots frame/detection
      metadata, copies detected ROIs into a bounded crop-owned Mono8 GPU buffer,
      records a crop-ready event on a dedicated producer CUDA stream, and
      releases the source `WORKER_ENTRY` after that source-to-crop copy
      completes. Preview and crop video encoding now consume this internal crop
      payload from the consumer stream, but they are still inside the same
      worker.
- [x] Define the first internal `CropFrame` payload with frame, timestamp,
      geometry, detection, GPU pointer, and CUDA event fields needed by preview
      and crop recording.
- [x] Add a bounded crop buffer/event pool so crop payloads are not overwritten
      while internal consumers are still using them.
- [x] Extract `CropProducer` from the current crop-video worker.
- [x] Make crop-video encoding consume the internal `CropFrame`.
- [ ] Make GUI crop preview an independent `CropFrame` consumer with
      drop/rate-limit policy. It currently consumes the internal crop payload
      inside the same worker.
- [x] Add consumer lease/ref-count handling so a crop buffer returns to the pool
      only after all accepted consumers release it.
- [x] Add producer and per-consumer aggregate counters.
- [x] Add a noop `PoseWorker` consumer that waits on `crop_ready_event`,
      exercises the shared crop queue/lease path, and releases its crop lease
      without running TensorRT yet.
- [x] Split first-pass perf logging into producer timing and consumer timing
      with `crop_pool_wait_ms`, `crop_producer_cpu_ms`,
      `crop_preview_cpu_ms`, and `encode_submit_cpu_ms`.
- [ ] Revalidate GUI `100 fps` YOLO + full-frame record + crop record before
      adding pose.

Current noop pose slice (2026-04-23):

- `PoseWorker` is now a real bounded worker-stage with its own CUDA stream.
- `CropProducer` can offer a `CropFrame` lease to that worker without creating
  a second crop copy.
- `CropAndEncodeWorker` retains one consumer lease for preview / crop-video
  encode and releases it independently from the noop pose worker.
- Shutdown ordering keeps the noop pose worker alive until crop production is
  fully drained so queued crop leases can return safely.
- This validates the ownership model for future pose TensorRT work, but it does
  not yet implement model loading, tensor conversion, IPC output, or GUI pose
  overlays.

Current latency interpretation (2026-04-23):

- The new `Cam<serial>_pose_perf.csv` aggregate summaries show that the noop
  pose queue is not the main problem:
  - `queue_high_water` stayed at `1`,
  - `queue_full_drops` stayed at `0`,
  - `crop_ready_to_pose_start` is tiny relative to the rest of the path.
- The current tail is still upstream, mainly in `detect_to_crop_ready`, which
  includes queue residence in the current combined crop worker plus the staged
  detach / crop-producer host-side handoff.
- Therefore the next architectural optimization target is not "make noop pose
  faster"; it is to shorten and isolate the `detect -> crop_ready` path.

## Data Structures
### New: CropFrame / PoseEntry (pool item)
- `unsigned char* d_crop` in the crop producer's canonical GPU layout
- Optional derived tensor buffer for pose if model layout differs
- `cudaEvent_t* crop_ready_event`
- `uint64_t frame_id / recording_frame_id`
- `uint64_t timestamp / timestamp_sys`
- camera id/serial
- source frame dimensions
- crop rectangle in source-frame coordinates
- detection rectangle/confidence
- transform from crop-local coordinates back to full-frame coordinates

### New: PoseResults (output)
- Minimal output struct (pose keypoints + score)
- Stored in IPC, shared queue, or directly into a results ring buffer

## Worker Integration
### CropAndEncodeWorker changes
- Maintain a pool of crop buffers/events (size ~4-8 to start).
- When a detection exists:
  - Acquire a free crop frame from the pool.
  - Write the high-resolution crop into the crop frame using a crop stream.
  - Record `crop_ready_event` on that stream.
  - Offer the crop frame to pose, preview, and crop encode consumers according
    to enabled config.
- If no crop frame is available, drop crop/pose for that frame and count the
  drop. Do not block full-frame recording.

### PoseWorker
- Owns TRT engine and a CUDA stream.
- On `WorkerFunction`:
  - `cudaStreamWaitEvent` on `crop_ready_event`.
  - Convert or bind the crop buffer into the model input layout if needed.
  - Run TRT inference on the crop tensor.
  - Write results to output (IPC/UI/log).
  - Return buffer and event to pool.

## Event Model
- `crop_ready_event` recorded in crop worker stream.
- Pose worker waits on the event in its own stream.
- No CPU sync in hot path.

## Configuration
- Add a toggle in `CameraEachSelect` (e.g., `pose` flag).
- Optional decimation: `ORANGE_POSE_DECIMATE` (like YOLO).
- TRT engine path configurable per camera.

## Performance Notes
- Keep pose model small enough for the required rate; do not assume the default
  `256x256` crop if the scientific need is a higher-resolution crop.
- Run only when detection exists.
- Keep queue sizes bounded and drop pose when overloaded.
- Avoid extra GPU->CPU copies; only copy results.
- Minimize latency from `detect_done -> crop_ready -> pose_start`. This matters
  more than preview or crop-video latency if pose is intended for reactive use.
- Because detect runs on a downsampled frame while pose needs a high-resolution
  crop from the original source, the key optimization target is the handoff
  between those two stages, not the elimination of downsampled detect itself.

## Fast Path vs Sidecar Work

- The intended low-latency path is:
  - `detect -> crop produce -> pose`
- Crop-video encoding is important, but it is not latency-critical in the same
  way. It should behave as a downstream sidecar consumer of the produced crop,
  not as a stage that pose must wait behind.
- This means "decouple crop from encode" really means:
  - crop production is realtime-critical,
  - pose is realtime-critical,
  - crop-video encode is throughput-critical and should be allowed to lag
    behind without stalling pose.
- This change alone does not remove the staged detach cost. It removes preview /
  encode work from the critical path after crop production so that pose waits on
  as little as possible.

## Why GPUDirect Detach Looks Slow

- A normal same-GPU VRAM-to-VRAM copy of a `4512x4512 Mono8` frame (`~20 MB`)
  should be well under `1 ms` on modern hardware.
- The measured `~6 ms` staged-detach tail is therefore not raw device-memory
  bandwidth. It is mostly the cost of safely touching the GPUDirect-backed
  source buffer from CUDA:
  - external buffer ownership / fencing,
  - driver/runtime synchronization before the detach can be submitted,
  - then the actual copy.
- This matches the measurements:
  - once data is in ordinary `cudaMalloc` device memory, ROI crop submission is
    cheap,
  - the long tail follows "touch the GPUDirect source", not "copy bytes on the
    GPU".
- An earlier full-resolution stable copy can still help latency, but mostly by
  moving and overlapping the detach, not by making the underlying GPUDirect
  detach magically cheap.
- Best case, if the camera/SDK can deliver directly into a stable buffer we own,
  later copies behave like normal GPU memory operations. Until then, the
  software plan should assume:
  - GPUDirect buffer = ingress lease,
  - owned device buffer = analytics workspace.

## CUDA Graph Capture (Pose TRT)
- Prefer CUDA Graph launch for steady-state pose inference to reduce host enqueue
  overhead and internal TRT contention.
- A future low-latency graph strategy may need two graph families rather than
  one monolithic graph:
  - `detect_only`: reduced-resolution detect path for frames with no pose work
    accepted,
  - `detect_plus_pose`: reduced-resolution detect followed immediately by
    high-resolution crop preparation and pose launch for accepted ROI frames.
- This is attractive because the detect stage and the pose stage do not consume
  the same image representation:
  - detect uses a downsampled full-frame view,
  - pose uses a crop derived from the original-resolution source.
- A single monolithic graph for every camera path is unlikely to fit the
  conditional nature of the workflow. A pair of steady-state graphs, or graph
  islands around the accepted detect->crop->pose path, is more plausible.
- Proposed behavior:
  - default graph-capture enabled (with runtime opt-out flag),
  - warmup once, capture once per pose worker/model instance, then launch graph
    per frame.
- If detect+pose graphing is explored later, the first timing comparison should
  be:
  - `detect_done -> pose_done` with ordinary enqueue,
  - versus `detect_done -> pose_done` with the combined graph path.
- Required fallback:
  - if capture fails or unsupported path is detected, continue with normal TRT
    enqueue path (no pipeline interruption).
- Recapture triggers:
  - model/engine swap,
  - input tensor shape/layout change.
- Instrumentation:
  - capture success/failure counters,
  - graph launch vs enqueue timing,
  - end-to-end pose latency and drop impact.

## Risks / Unknowns
- Crop buffer lifetime (must be pooled to avoid overwrite).
- TRT engine latency may compete with YOLO on GPU.
- Requires a clean path for pose results into UI/IPC.

## Implementation Steps (high level)
1. Define `CropFrame`/`PoseEntry` + pool (similar to encoder preprocess pool).
2. Extract a crop producer from the current crop/encode path.
3. Add `PoseWorker` class (similar to YOLO but simpler TRT-only).
4. Wire crop producer outputs to preview, crop encode, and pose consumers.
5. Wire `PoseWorker` creation in `orange.cpp` when enabled.
6. Add output path (IPC or UI overlay).
7. Add configs + decimation + logging.

## Concrete implementation plan
### New files
1. `src/pose_worker.h` and `src/pose_worker.cpp`
2. Optional: `src/pose_types.h` (if you want to keep structs out of `video_capture.h`)

### Existing files to touch
1. `src/video_capture.h`
   - Add per-camera toggles and model path fields (e.g. `pose`, `pose_model`).
2. Crop producer files (new or extracted from `src/crop_and_encode_worker.*`)
   - Add a bounded crop buffer/event pool.
   - Add consumer registration for preview, crop encode, and pose.
   - Enqueue pose work from produced `CropFrame` payloads when detections exist.
3. `src/crop_and_encode_worker.h` / `src/crop_and_encode_worker.cpp`
   - Convert to a crop-video consumer, or keep as transitional code until the
     crop producer split is complete.
4. `src/orange.cpp`
   - Create and start `PoseWorker` per camera when enabled.
   - Wire to the crop producer, not directly to crop-video encoding.
   - Add UI toggles + model selection.
5. `CMakeLists.txt`
   - Probably no changes (glob on `src/*.cpp`), but verify on orange_client if needed.

## Skeletons (code outline)
### pose_types.h (optional)
```cpp
#pragma once
#include <cuda_runtime.h>
#include <vector>

struct CropFrame {
    unsigned char* d_crop = nullptr;
    int crop_width = 0;
    int crop_height = 0;
    int source_width = 0;
    int source_height = 0;
    float crop_x = 0.0f;
    float crop_y = 0.0f;
    float detection_x = 0.0f;
    float detection_y = 0.0f;
    float detection_w = 0.0f;
    float detection_h = 0.0f;
    float detection_confidence = 0.0f;
    cudaEvent_t* crop_ready_event = nullptr;
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
    int camera_id = 0;
};

struct PoseResult {
    uint64_t frame_id = 0;
    std::vector<float> keypoints; // x,y,score...
};
```

### pose_worker.h
```cpp
#pragma once
#include "threadworker.h"
#include "thread.h" // SafeQueue
#include "pose_types.h"
#include <atomic>

class PoseWorker : public CThreadWorker<CropFrame> {
public:
    PoseWorker(const char* name, SafeQueue<CropFrame*>& free_crop_frames);
    ~PoseWorker() override;

protected:
    bool WorkerFunction(CropFrame* entry) override;

private:
    cudaStream_t m_stream = nullptr;
    // TensorRT engine wrapper (similar to yolov8_det)
    // PoseTrtEngine engine_;
    SafeQueue<CropFrame*>& free_crop_frames_;
};
```

### pose_worker.cpp (core logic sketch)
```cpp
bool PoseWorker::WorkerFunction(CropFrame* entry) {
    if (!entry) return false;

    // Wait for crop to be ready
    if (entry->crop_ready_event) {
        ck(cudaStreamWaitEvent(m_stream, *entry->crop_ready_event, 0));
    }

    // Convert/bind entry->d_crop into the pose model input layout.
    // Run TRT inference on entry->d_crop or a derived tensor.
    // engine_.infer(entry->d_crop, ...);

    // Publish results (IPC/UI/log)

    // Return buffer/event to pool
    free_crop_frames_.push(entry);
    return false;
}
```

### crop producer sketch
```cpp
class PoseWorker;
class CropEncodeWorker;

class CropProducer : public CThreadWorker<WORKER_ENTRY> {
public:
    void SetPoseWorker(PoseWorker* pose_worker);
    void SetCropEncodeWorker(CropEncodeWorker* crop_encode_worker);
    SafeQueue<CropFrame*>& GetCropPool() { return free_crop_frames_; }
private:
    PoseWorker* pose_worker_ = nullptr;
    CropEncodeWorker* crop_encode_worker_ = nullptr;
    SafeQueue<CropFrame*> free_crop_frames_;
    std::vector<CropFrame> crop_frame_pool_;
    std::vector<cudaEvent_t> crop_event_pool_;
};
```

### crop producer enqueue sketch
```cpp
if (pose_worker_ && has_detection) {
    CropFrame* crop = nullptr;
    if (free_crop_frames_.pop(crop)) {
        // Write high-resolution crop into crop->d_crop.
        gpu_crop_and_resize_rgba(entry->d_image, crop->d_crop, ...);
        ck(cudaEventRecord(*crop->crop_ready_event, crop_stream_));

        crop->frame_id = entry->frame_id;
        crop->recording_frame_id = entry->recording_frame_id;
        crop->timestamp = entry->timestamp;
        crop->timestamp_sys = entry->timestamp_sys;

        pose_worker_->PutObjectToQueueIn(crop);
    }
}
```

### orange.cpp wiring sketch
```cpp
// Create
if (cameras_select[i].pose) {
    pose_workers[i] = new PoseWorker(name.c_str(), cropProducers[i]->GetCropPool());
    cropProducers[i]->SetPoseWorker(pose_workers[i]);
}

// Start/stop alongside other workers.
```

## Notes on output path
- Easiest: publish pose results to IPC (similar to YOLO detections).
- UI overlay: store last pose per camera and render in ImGui / overlay shader.

## Open Questions
1. TRT engine input: size, layout, data type, normalization, batch size?
2. Detection selection: pose on all detections or only top confidence?
3. Output schema: what keypoints + coordinate system are produced?
4. IPC payload: ok with per-frame list of keypoints + scores?
5. If YOLO is decimated, should pose run at YOLO rate or be forced to full rate?

## Testing / validation
1. Run with pose disabled (should behave exactly as today).
2. Enable pose and verify:
   - No memory leaks (pose pool returns).
   - FPS stays within target.
   - Pose output matches expected frames.
3. Stress test with decimation, larger queues, and multi-camera load.

## Success Criteria
- Pose inference runs without lowering main recording FPS below target.
- Display + crop preview remain smooth.
- Pose results are delivered with bounded latency.
