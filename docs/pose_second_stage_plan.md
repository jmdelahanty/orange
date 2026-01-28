# Plan: Second-Stage Pose TensorRT on Cropped ROI

Date: 2026-01-27
Author: Codex (outline)

## Goal
Add a second-stage pose model (TensorRT) that consumes the 256x256 crop and produces real-time pose results without breaking current display/recording throughput.

## Constraints / Existing Architecture
- `CropAndEncodeWorker` already generates a 256x256 crop on GPU.
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
Acquisition -> YOLO -> CropWorker -> PoseWorker -> (IPC/UI/log)
                            |--> preview (PBO)
                            |--> encode (NVENC)
```

## Data Structures
### New: PoseEntry (pool item)
- `unsigned char* d_crop_rgba` (or NV12 if pose model expects it)
- `cudaEvent_t* crop_ready_event`
- `uint64_t frame_id / recording_frame_id`
- `uint64_t timestamp / timestamp_sys`
- Optional: camera id, bbox for traceability

### New: PoseResults (output)
- Minimal output struct (pose keypoints + score)
- Stored in IPC, shared queue, or directly into a results ring buffer

## Worker Integration
### CropAndEncodeWorker changes
- Maintain a pool of `PoseEntry` buffers (size ~4-8 to start).
- When a detection exists:
  - Acquire a free `PoseEntry` from the pool.
  - Write the crop into `PoseEntry::d_crop_rgba` using `m_display_stream`.
  - Record `crop_ready_event` on that stream.
  - Enqueue to `PoseWorker`.
- If no `PoseEntry` is available, drop pose for that frame (do not block crop/encode).

### PoseWorker
- Owns TRT engine and a CUDA stream.
- On `WorkerFunction`:
  - `cudaStreamWaitEvent` on `crop_ready_event`.
  - Run TRT inference on `d_crop_rgba`.
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
- Keep pose model small (256x256, FP16/INT8).
- Run only when detection exists.
- Keep queue sizes bounded and drop pose when overloaded.
- Avoid extra GPU->CPU copies; only copy results.

## Risks / Unknowns
- Crop buffer lifetime (must be pooled to avoid overwrite).
- TRT engine latency may compete with YOLO on GPU.
- Requires a clean path for pose results into UI/IPC.

## Implementation Steps (high level)
1. Define `PoseEntry` + pool (similar to encoder preprocess pool).
2. Add `PoseWorker` class (similar to YOLO but simpler TRT-only).
3. Extend `CropAndEncodeWorker` to enqueue pose work.
4. Wire `PoseWorker` creation in `orange.cpp` when enabled.
5. Add output path (IPC or UI overlay).
6. Add configs + decimation + logging.

## Concrete implementation plan
### New files
1. `src/pose_worker.h` and `src/pose_worker.cpp`
2. Optional: `src/pose_types.h` (if you want to keep structs out of `video_capture.h`)

### Existing files to touch
1. `src/video_capture.h`
   - Add per-camera toggles and model path fields (e.g. `pose`, `pose_model`).
2. `src/crop_and_encode_worker.h` / `src/crop_and_encode_worker.cpp`
   - Add a small pool of pose buffers + events.
   - Add `SetPoseWorker(...)`.
   - Enqueue pose work when detections exist.
3. `src/orange.cpp`
   - Create and start `PoseWorker` per camera when enabled.
   - Wire to `CropAndEncodeWorker`.
   - Add UI toggles + model selection.
4. `CMakeLists.txt`
   - Probably no changes (glob on `src/*.cpp`), but verify on orange_client if needed.

## Skeletons (code outline)
### pose_types.h (optional)
```cpp
#pragma once
#include <cuda_runtime.h>
#include <vector>

struct PoseEntry {
    unsigned char* d_crop_rgba = nullptr; // 256x256x4
    cudaEvent_t* crop_ready_event = nullptr;
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
    // Optional: bbox or camera id
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

class PoseWorker : public CThreadWorker<PoseEntry> {
public:
    PoseWorker(const char* name, SafeQueue<PoseEntry*>& free_pose_entries);
    ~PoseWorker() override;

protected:
    bool WorkerFunction(PoseEntry* entry) override;

private:
    cudaStream_t m_stream = nullptr;
    // TensorRT engine wrapper (similar to yolov8_det)
    // PoseTrtEngine engine_;
    SafeQueue<PoseEntry*>& free_pose_entries_;
};
```

### pose_worker.cpp (core logic sketch)
```cpp
bool PoseWorker::WorkerFunction(PoseEntry* entry) {
    if (!entry) return false;

    // Wait for crop to be ready
    if (entry->crop_ready_event) {
        ck(cudaStreamWaitEvent(m_stream, *entry->crop_ready_event, 0));
    }

    // Run TRT inference on entry->d_crop_rgba
    // engine_.infer(entry->d_crop_rgba, ...);

    // Publish results (IPC/UI/log)

    // Return buffer/event to pool
    free_pose_entries_.push(entry);
    return false;
}
```

### crop_and_encode_worker.h (additions)
```cpp
class PoseWorker; // forward

class CropAndEncodeWorker : public CThreadWorker<WORKER_ENTRY> {
public:
    void SetPoseWorker(PoseWorker* pose_worker);
    SafeQueue<PoseEntry*>& GetPosePool() { return free_pose_entries_; }
private:
    PoseWorker* pose_worker_ = nullptr;
    SafeQueue<PoseEntry*> free_pose_entries_;
    std::vector<PoseEntry> pose_entry_pool_;
    std::vector<cudaEvent_t> pose_event_pool_;
};
```

### crop_and_encode_worker.cpp (pose enqueue sketch)
```cpp
if (pose_worker_ && has_detection) {
    PoseEntry* pose_entry = nullptr;
    if (free_pose_entries_.pop(pose_entry)) {
        // Write crop into pose_entry->d_crop_rgba
        gpu_crop_and_resize_rgba(entry->d_image, pose_entry->d_crop_rgba, ...);
        ck(cudaEventRecord(*pose_entry->crop_ready_event, m_display_stream));

        pose_entry->frame_id = entry->frame_id;
        pose_entry->recording_frame_id = entry->recording_frame_id;
        pose_entry->timestamp = entry->timestamp;
        pose_entry->timestamp_sys = entry->timestamp_sys;

        pose_worker_->PutObjectToQueueIn(pose_entry);
    }
}
```

### orange.cpp wiring sketch
```cpp
// Create
if (cameras_select[i].pose) {
    pose_workers[i] = new PoseWorker(name.c_str(), cropAndEncodeWorkers[i]->GetPosePool());
    cropAndEncodeWorkers[i]->SetPoseWorker(pose_workers[i]);
}

// Start/stop alongside other workers.
```

## Notes on output path
- Easiest: publish pose results to IPC (similar to YOLO detections).
- UI overlay: store last pose per camera and render in ImGui / overlay shader.

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
