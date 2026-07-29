#pragma once

#include "video_capture.h"

#include <cstddef>
#include <memory>
#include <string>

class COpenGLDisplay;
class CropAndEncodeWorker;
class CropPreviewWorker;
class CropProducerWorker;
class FrameIPCManager;
class PoseWorker;
class SpatialSnapshotWorker;
class YoloWorker;

namespace orange::gui {

// Move-only owner for the non-texture resources whose lifetime is scoped to
// one GUI camera stream. This includes the display worker object, but not the
// OpenGL/CUDA interop textures it writes; those remain owned and destroyed by
// the GUI context thread. Acquisition borrows pointers from this object, so
// callers must stop/join acquisition before destroying or moving a live
// runtime.
class PerCameraStreamRuntime {
public:
    PerCameraStreamRuntime() noexcept = default;
    ~PerCameraStreamRuntime() noexcept;

    PerCameraStreamRuntime(const PerCameraStreamRuntime&) = delete;
    PerCameraStreamRuntime& operator=(const PerCameraStreamRuntime&) = delete;
    PerCameraStreamRuntime(PerCameraStreamRuntime&& other) noexcept;
    PerCameraStreamRuntime& operator=(PerCameraStreamRuntime&& other) noexcept;

    void Bind(CameraEmergent* camera, CameraParams* params);
    bool bound() const noexcept { return camera_ != nullptr && params_ != nullptr; }
    bool empty() const noexcept;

    void InitializeCameraResources(
        std::size_t maximum_frame_bytes,
        bool enable_yolo_events,
        int acquire_work_entries);
    CameraResources& camera_resources() noexcept { return camera_resources_; }
    const CameraResources& camera_resources() const noexcept {
        return camera_resources_;
    }

    void SetFrameIpcManager(std::unique_ptr<FrameIPCManager> manager) noexcept;
    FrameIPCManager* frame_ipc_manager() const noexcept;
    void SetFrameIpcInitError(std::string error);
    const std::string& frame_ipc_init_error() const noexcept {
        return frame_ipc_init_error_;
    }

    void OpenCameraStream(const char* context);
    void AllocateFrameArray(int frame_count);
    void AllocateAndQueueFrameBuffer(int frame_index);
    int allocated_frame_buffer_count() const noexcept {
        return allocated_frame_buffer_count_;
    }
    bool stream_opened() const noexcept { return stream_opened_; }

    void SetDisplayWorker(std::unique_ptr<COpenGLDisplay> worker) noexcept;
    void SetCropProducerWorker(
        std::unique_ptr<CropProducerWorker> worker) noexcept;
    void SetCropEncodeWorker(
        std::unique_ptr<CropAndEncodeWorker> worker) noexcept;
    void SetCropPreviewWorker(
        std::unique_ptr<CropPreviewWorker> worker) noexcept;
    void SetSpatialSnapshotWorker(
        std::unique_ptr<SpatialSnapshotWorker> worker) noexcept;
    void SetPoseWorker(std::unique_ptr<PoseWorker> worker) noexcept;
    void SetYoloWorker(std::unique_ptr<YoloWorker> worker) noexcept;

    COpenGLDisplay* display_worker() const noexcept;
    CropProducerWorker* crop_producer_worker() const noexcept;
    CropAndEncodeWorker* crop_encode_worker() const noexcept;
    CropPreviewWorker* crop_preview_worker() const noexcept;
    SpatialSnapshotWorker* spatial_snapshot_worker() const noexcept;
    PoseWorker* pose_worker() const noexcept;
    YoloWorker* yolo_worker() const noexcept;

    // Normal teardown calls these phases explicitly to preserve the existing
    // cross-camera drain order. The destructor invokes all phases as a
    // fail-safe for startup cancellation and partial construction.
    void StopWorkers() noexcept;
    void FinalizeAndDestroyWorkers() noexcept;
    void ReleaseStreamAndIpc() noexcept;
    void ReleaseCameraResources() noexcept;
    void Reset() noexcept;

private:
    void MoveFrom(PerCameraStreamRuntime&& other) noexcept;
    void SelectCameraDeviceForCleanup() const noexcept;
    void ClearBorrowedFrameView() noexcept;

    CameraEmergent* camera_ = nullptr;
    CameraParams* params_ = nullptr;
    int gpu_id_ = -1;
    std::string camera_serial_;

    bool camera_resources_initialized_ = false;
    bool stream_opened_ = false;
    int allocated_frame_buffer_count_ = 0;
    std::unique_ptr<Emergent::CEmergentFrame[]> frame_array_;
    CameraResources camera_resources_;
    std::unique_ptr<FrameIPCManager> frame_ipc_manager_;
    std::string frame_ipc_init_error_;

    std::unique_ptr<COpenGLDisplay> display_worker_;
    std::unique_ptr<CropProducerWorker> crop_producer_worker_;
    std::unique_ptr<CropAndEncodeWorker> crop_encode_worker_;
    std::unique_ptr<CropPreviewWorker> crop_preview_worker_;
    std::unique_ptr<SpatialSnapshotWorker> spatial_snapshot_worker_;
    std::unique_ptr<PoseWorker> pose_worker_;
    std::unique_ptr<YoloWorker> yolo_worker_;
};

}  // namespace orange::gui
