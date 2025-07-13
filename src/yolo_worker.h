// src/yolo_worker.h
#ifndef YOLO_WORKER_H
#define YOLO_WORKER_H

#include "threadworker.h"
#include "yolov8_det.h"
#include "image_processing.h"
#include "camera.h"
#include "video_capture.h"
#include "network_base.h"
#include "shaman.h"
#include <chrono>
#include <vector>
#include <atomic>
#include "common.hpp"

class COpenGLDisplay;
class CropAndEncodeWorker;

class YOLOv8Worker : public CThreadWorker<ProcessedFrame>
{
public:
    YOLOv8Worker(const char* name,
                 CameraParams* cam_params,
                 CameraEachSelect* cam_select,
                 // This worker receives frames from the preprocessor via this queue
                 SafeQueue<ProcessedFrame*>* input_queue,
                 SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
                 SafeQueue<ProcessedFrame*>& processed_recycle_queue);
    ~YOLOv8Worker() override;

    void SetENetTarget(EnetContext* host_ctx, ENetPeer* target_peer);
    void SetDisplayWorker(COpenGLDisplay* display_worker);
    void SetCropAndEncodeWorker(CropAndEncodeWorker* crop_worker);
    void DumpNextFrame() { m_dump_next_frame.store(true); }
    CameraParams* GetCameraParams() const { return associated_camera_params_; }
    double get_fps() const { return current_fps_.load(std::memory_order_relaxed); }

private:
    bool WorkerFunction(ProcessedFrame* f) override;
    void WorkerReset() override;
    // Overriding the base class's loop to use our own input source.
    void ThreadRunning() override;

    std::atomic<bool> m_dump_next_frame;
    YOLOv8* yolov8_instance_;
    CameraParams* associated_camera_params_;
    CameraEachSelect* associated_camera_select_;
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
    CropAndEncodeWorker* m_crop_worker = nullptr;

    // Pointer to the shared input queue.
    SafeQueue<ProcessedFrame*>* m_input_queue;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
    SafeQueue<ProcessedFrame*>& m_processed_recycle_queue;
};

#endif // YOLO_WORKER_H