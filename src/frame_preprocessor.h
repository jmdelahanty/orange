// src/frame_preprocessor.h

#ifndef FRAME_PREPROCESSOR_H
#define FRAME_PREPROCESSOR_H

#include "threadworker.h"
#include "video_capture.h"
#include "image_processing.h"
#include "yolo_worker.h"
#include "gpu_video_encoder.h"

class FramePreprocessor : public CThreadWorker<WORKER_ENTRY> {
public:
    FramePreprocessor(
        const char* name,
        CameraParams* cam_params,
        SafeQueue<ProcessedFrame*>* yolo_queue,
        SafeQueue<ProcessedFrame*>* encoder_queue,
        SafeQueue<WORKER_ENTRY*>& recycle_queue,
        SafeQueue<ProcessedFrame*>& processed_recycle_queue);

    ~FramePreprocessor() override;

private:
    bool WorkerFunction(WORKER_ENTRY* f) override;

    CameraParams* camera_params_;
    Debayer debayer_gpu_;
    cudaStream_t m_stream;

    // Queues for dispatching processed frames
    SafeQueue<ProcessedFrame*>* m_yolo_queue;
    SafeQueue<ProcessedFrame*>* m_encoder_queue;

    // Recycle queues
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
    SafeQueue<ProcessedFrame*>& m_processed_recycle_queue;
};

#endif // FRAME_PREPROCESSOR_H
