// src/preprocess_worker.h

#ifndef PREPROCESS_WORKER_H
#define PREPROCESS_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "image_processing.h"
#include <cuda_runtime.h>

class PreprocessWorker : public CThreadWorker<WORKER_ENTRY>
{
public:
    PreprocessWorker(
        const char* name,
        CameraParams* cam_params,
        SafeQueue<WORKER_ENTRY*>& recycle_queue);
    ~PreprocessWorker() override;

protected:
    // The main worker function that processes the raw frame.
    bool WorkerFunction(WORKER_ENTRY* entry) override;

private:
    CameraParams* camera_params_;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue_;
    cudaStream_t m_stream;

    // A reusable GPU buffer for the RGB conversion result within this worker.
    unsigned char* d_rgb_buffer_; 
};

#endif // PREPROCESSING_WORKER_H