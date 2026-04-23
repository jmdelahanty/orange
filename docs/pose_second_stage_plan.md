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
      metadata and defers source `WORKER_ENTRY` release with CUDA source-safe
      events. This reduces the original frame lifetime coupling, but preview and
      crop video encoding are still inside the same worker.
- [ ] Define `CropFrame` as the shared crop payload and include all frame,
      timestamp, geometry, detection, GPU pointer, and CUDA event fields needed
      by preview, crop recording, and pose.
- [ ] Add a bounded crop buffer/event pool so crop payloads are not overwritten
      while consumers are still using them.
- [ ] Extract `CropProducer` from the current crop-video worker.
- [ ] Make crop-video encoding a `CropFrame` consumer.
- [ ] Make GUI crop preview a `CropFrame` consumer with drop/rate-limit policy.
- [ ] Add consumer lease/ref-count handling so a crop buffer returns to the pool
      only after all accepted consumers release it.
- [ ] Add producer and per-consumer drop counters.
- [ ] Split perf logging into producer timing and consumer timing.
- [ ] Revalidate GUI `100 fps` YOLO + full-frame record + crop record before
      adding pose.

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

## CUDA Graph Capture (Pose TRT)
- Prefer CUDA Graph launch for steady-state pose inference to reduce host enqueue
  overhead and internal TRT contention.
- Proposed behavior:
  - default graph-capture enabled (with runtime opt-out flag),
  - warmup once, capture once per pose worker/model instance, then launch graph
    per frame.
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
