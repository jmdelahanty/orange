// src/encoder_preprocess_worker.h

#ifndef ENCODER_PREPROCESS_WORKER_H
#define ENCODER_PREPROCESS_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "image_processing.h"
#include "encoder_pipeline.h" 
#include <cuda_runtime.h>
#include <atomic>
#include <chrono>

class EncoderHwWorker; // Forward declaration is sufficient here

class EncoderPreprocessWorker : public CThreadWorker<WORKER_ENTRY>
{
public:
    EncoderPreprocessWorker(
        const char* name,
        CameraParams* cam_params,
        int encoder_pitch,
        SafeQueue<WORKER_ENTRY*>& recycle_queue,
        CameraControl* camera_control
    );
    ~EncoderPreprocessWorker() override;

    void SetHwWorker(EncoderHwWorker* hw_worker);

    // This queue is public so the HW worker can return buffers
    SafeQueue<ENCODER_WORKER_ENTRY*> free_encoder_entries_;
    SafeQueue<cudaEvent_t*> free_events_;  // Assuming this is also public
    
    // Public atomic counters for resource tracking
    std::atomic<int> available_buffers_{ENCODER_ENTRY_POOL_SIZE};
    std::atomic<int> available_events_{EVENT_POOL_SIZE};
    
    // Performance monitoring getters
    double get_fps() const { return current_fps_.load(); }
    uint64_t get_frames_dropped() const { return frames_dropped_.load(); }
    uint64_t get_resource_waits() const { return resource_waits_.load(); }

protected:
    bool WorkerFunction(WORKER_ENTRY* entry) override;

private:
    CameraParams* camera_params_;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue_;
    CameraControl* camera_control_;
    cudaStream_t m_stream;
    EncoderHwWorker* m_hw_worker_;

    FrameGPU frame_original_gpu_;
    Debayer debayer_gpu_;
    unsigned char* d_rgb_temp_;
    unsigned char* d_uv_default_plane_;
    int encoder_pitch_;

    static const int ENCODER_ENTRY_POOL_SIZE = 120; 
    static const int EVENT_POOL_SIZE = 120;
    ENCODER_WORKER_ENTRY encoder_entry_pool_[ENCODER_ENTRY_POOL_SIZE];
    std::vector<cudaEvent_t> event_pool_;
    
    // Performance monitoring members
    std::chrono::steady_clock::time_point last_fps_update_time_;
    std::atomic<int> frame_counter_{0};
    std::atomic<double> current_fps_{0.0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> resource_waits_{0};
};

#endif // ENCODER_PREPROCESS_WORKER_H