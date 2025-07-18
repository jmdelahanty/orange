// src/preprocess_worker.cpp

#include "preprocess_worker.h"
#include "kernel.cuh" // For mono_to_rgb_kernel
#include "nvtx_profiling.h"
#include <iostream>

PreprocessWorker::PreprocessWorker(
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

    // Allocate the intermediate RGB buffer once.
    // This buffer will be reused for every frame this worker processes.
    size_t rgb_buffer_size = (size_t)camera_params_->width * camera_params_->height * 3;
    ck(cudaMalloc(&d_rgb_buffer_, rgb_buffer_size));
}

PreprocessWorker::~PreprocessWorker()
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

bool PreprocessWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    if (!entry) {
        return false;
    }

    NVTX_RANGE("PreprocessWorker");
    ck(cudaSetDevice(camera_params_->gpu_id));

    // Wait for the raw data to be ready from the acquisition thread.
    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    // This is the core logic: convert the raw Mono8 frame to a 3-channel RGB frame.
    // We will need a new kernel or modify an existing one for this. For now, we'll
    // assume a 'mono_to_rgb_kernel' exists.
    launch_mono_to_rgb_kernel(
        entry->d_processed_rgb,
        entry->d_image,         
        camera_params_->width,
        camera_params_->height,
        m_stream
    );

    return true; // Return true to pass the entry to the output queue for the dispatcher
}