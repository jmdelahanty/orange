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
#include <chrono>
#include <vector>
#include <chrono>
#include <cuda.h>
#include <atomic>
#include <memory>
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
    void WorkerReset() override;

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
};

#endif // YOLO_WORKER_H
