// src/preprocessing_worker.h

#ifndef PREPROCESSING_WORKER_H
#define PREPROCESSING_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "image_processing.h"
#include <cuda_runtime.h>
#include "kernel.cuh" 
#include "nvtx_profiling.h"
#include <iostream>

class PreprocessingWorker : public CThreadWorker<WORKER_ENTRY>
{
public:
    inline PreprocessingWorker(
        const char* name,
        CameraParams* cam_params,
        SafeQueue<WORKER_ENTRY*>& recycle_queue);
    inline ~PreprocessingWorker() override;

protected:
    inline bool WorkerFunction(WORKER_ENTRY* entry) override;

private:
    CameraParams* camera_params_;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue_;
    cudaStream_t m_stream;
    unsigned char* d_rgb_buffer_; 
};

// --- IMPLEMENTATIONS ---

inline PreprocessingWorker::PreprocessingWorker(
    const char* name,
    CameraParams* cam_params,
    SafeQueue<WORKER_ENTRY*>& recycle_queue)
    : CThreadWorker(name),
      camera_params_(cam_params),
      m_recycle_queue_(recycle_queue),
      m_stream(nullptr),
      d_rgb_buffer_(nullptr)
{
    std::cout << "[PREPROCESS] CONSTRUCTOR for " << name << " on GPU " << camera_params_->gpu_id << std::endl;
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));

    size_t rgb_buffer_size = (size_t)camera_params_->width * camera_params_->height * 3;
    ck(cudaMalloc(&d_rgb_buffer_, rgb_buffer_size));
}

inline PreprocessingWorker::~PreprocessingWorker()
{
    std::cout << "[PREPROCESS] DESTRUCTOR for " << threadName << std::endl;
    ck(cudaSetDevice(camera_params_->gpu_id));
    if (m_stream) {
        cudaStreamDestroy(m_stream);
    }
    if (d_rgb_buffer_) {
        cudaFree(d_rgb_buffer_);
    }
}

inline bool PreprocessingWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    if (!entry) {
        return false;
    }

    NVTX_RANGE("PreprocessingWorker");
    ck(cudaSetDevice(camera_params_->gpu_id));

    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    launch_mono_to_rgb_kernel(
        entry->d_processed_rgb,
        entry->d_image,         
        camera_params_->width,
        camera_params_->height,
        m_stream
    );

    return true; // Pass the entry to the output queue for the dispatcher
}


#endif // PREPROCESSING_WORKER_H