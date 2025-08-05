// src/encoder_preprocess_worker.h

#ifndef ENCODER_PREPROCESS_WORKER_H
#define ENCODER_PREPROCESS_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "image_processing.h"
#include "encoder_pipeline.h" 
#include <cuda_runtime.h>

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
    ENCODER_WORKER_ENTRY encoder_entry_pool_[ENCODER_ENTRY_POOL_SIZE];
};

#endif // ENCODER_PREPROCESS_WORKER_H