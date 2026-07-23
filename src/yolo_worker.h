// src/yolo_worker.h
#ifndef YOLO_WORKER_H
#define YOLO_WORKER_H

#include "threadworker.h"
#include "yolov8_det.h"
#include "image_processing.h" // For FrameGPU, Debayer
#include "camera.h"           // For CameraParams
#include "video_capture.h"    // For CameraEachSelect, WORKER_ENTRY
#include "network_base.h"     // For EnetContext, ENetPeer
#include "shaman.h"           // For shaman::SharedBoxQueue
#include "velocity_tracker.h" // For VelocityTracker
#include "yolo_spatial_mask.h"
#include <chrono>
#include <vector>
#include <chrono>
#include <cuda.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include "common.hpp"         // For pose::Object

class COpenGLDisplay;
class CropProducerWorker;
namespace yolo_perf {
class YoloPerfLogger;
}
namespace yolo_event_log { class YoloEventLogger; }

class YoloWorker : public CThreadWorker<WORKER_ENTRY>
{
public:
    YoloWorker(const char* name,
               CameraParams* cam_params,
               CameraEachSelect* cam_select,
               CameraControl* camera_control,
               SafeQueue<WORKER_ENTRY*>& recycle_queue);
    ~YoloWorker() override;

    void SetENetTarget(EnetContext* host_ctx, ENetPeer* target_peer);
    void SetDisplayWorker(COpenGLDisplay* display_worker);
    void SetCropProducerWorker(CropProducerWorker* crop_worker);
    void Warmup(int iterations);
    // Policies are requested from the control thread and applied only at a
    // worker frame boundary. A nonzero generation is acknowledged by
    // WaitForSpatialMaskPolicy before recording is allowed to begin.
    bool RequestSpatialMaskPolicy(
        const orange::analytics_mask::Policy& policy,
        std::uint64_t* generation_out,
        std::string* error_out = nullptr);
    bool WaitForSpatialMaskPolicy(
        std::uint64_t generation,
        std::chrono::milliseconds timeout,
        std::string* error_out = nullptr);
    void DumpNextFrame() { m_dump_next_frame.store(true);}
    std::vector<TrackedObject> getTrackedObjects() const {
        return velocity_tracker_.getTrackedObjects();
    }
    float getSpeedCmPerSec(int track_id) const {
        return velocity_tracker_.getSpeedCmPerSec(track_id);
    }

    CameraParams* GetCameraParams() const { return associated_camera_params_; }

    struct YoloDetectionOutput {
        unsigned long long frame_id;
        unsigned long long timestamp;
        uint64_t timestamp_sys;
        std::vector<pose::Object> detections;
    };

    double get_fps() const {
        return current_fps_.load(std::memory_order_relaxed);
    }

private:
    bool WorkerFunction(WORKER_ENTRY* f) override;
    void OnFlushTick() override {}  // no flush-time housekeeping
    void WorkerReset() override;
    void OnQueueInEnqueued(WORKER_ENTRY* entry, int queue_depth_after_enqueue) override;
    void OnQueueInDequeued(WORKER_ENTRY* entry, int queue_depth_after_dequeue) override;
    void ApplyPendingSpatialMaskPolicyAtFrameBoundary();

    std::atomic<bool> m_dump_next_frame;

    YOLOv8* yolov8_instance_;
    CameraParams* associated_camera_params_;
    CameraEachSelect* associated_camera_select_;
    CameraControl* camera_control_;

    EnetContext* enet_host_context_;
    ENetPeer* enet_target_peer_;
    flatbuffers::FlatBufferBuilder* fb_builder_;

    FrameGPU frame_original_gpu_;
    Debayer debayer_gpu_;
    unsigned char* d_rgb_yolo_input_gpu_;

    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_;
    std::atomic<double> current_fps_;

    shaman::SharedBoxQueue* shaman_ipc_queue_;
    COpenGLDisplay* m_display_worker = nullptr;
    CropProducerWorker* m_crop_worker = nullptr;
    VelocityTracker velocity_tracker_;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
    std::unique_ptr<yolo_perf::YoloPerfLogger> perf_logger_;
    std::unique_ptr<yolo_event_log::YoloEventLogger> event_logger_;
    uint64_t perf_sample_counter_ = 0;
    int perf_sample_rate_ = 1;
    std::string perf_log_folder_;

    mutable std::mutex spatial_mask_mutex_;
    std::condition_variable spatial_mask_cv_;
    orange::analytics_mask::Policy pending_spatial_mask_policy_;
    std::uint64_t pending_spatial_mask_generation_ = 0;
    std::atomic<std::uint64_t> requested_spatial_mask_generation_{0};
    std::atomic<std::uint64_t> applied_spatial_mask_generation_{0};
    // Worker-thread-owned after frame-boundary application.
    orange::analytics_mask::Policy active_spatial_mask_policy_;
    std::shared_ptr<const nlohmann::json> active_spatial_mask_policy_json_;
};

#endif // YOLO_WORKER_H
