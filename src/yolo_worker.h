// src/yolo_worker.h
#ifndef YOLO_WORKER_H
#define YOLO_WORKER_H

#include "threadworker.h"
#include "yolov8_det.h"
#include "image_processing.h"
#include "camera.h"
#include "video_capture.h" // This now includes ProcessedFrame definition
#include "network_base.h"
#include "shaman.h"
#include <chrono>
#include <vector>
#include <atomic>
#include "common.hpp"

class COpenGLDisplay;
class CropAndEncodeWorker;

// This worker now processes the output of the FramePreprocessor
class YOLOv8Worker : public CThreadWorker<ProcessedFrame>
{
public:
    YOLOv8Worker(const char* name,
                 CameraParams* cam_params,
                 CameraEachSelect* cam_select,
                 SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
                 SafeQueue<ProcessedFrame*>& processed_recycle_queue);

    ~YOLOv8Worker() override;

    void SetENetTarget(EnetContext* host_ctx, ENetPeer* target_peer);
    void SetDisplayWorker(COpenGLDisplay* display_worker);
    void SetCropAndEncodeWorker(CropAndEncodeWorker* crop_worker);
    void DumpNextFrame() { m_dump_next_frame.store(true); }
    double get_fps() const;

private:
    bool WorkerFunction(ProcessedFrame* f) override; // Input is now ProcessedFrame
    void WorkerReset() override;

    std::atomic<bool> m_dump_next_frame;

    YOLOv8* yolov8_instance_;
    CameraParams* associated_camera_params_;
    CameraEachSelect* associated_camera_select_;

    EnetContext* enet_host_context_;
    ENetPeer* enet_target_peer_;
    flatbuffers::FlatBufferBuilder* fb_builder_;

    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_;
    std::atomic<double> current_fps_;

    shaman::SharedBoxQueue* shaman_ipc_queue_;
    COpenGLDisplay* m_display_worker;
    CropAndEncodeWorker* m_crop_worker;

    // The worker now needs access to both recycle queues
    SafeQueue<WORKER_ENTRY*>& m_raw_recycle_queue;
    SafeQueue<ProcessedFrame*>& m_processed_recycle_queue;
};

#endif // YOLO_WORKER_H