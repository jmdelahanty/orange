// src/frame_preprocessor.cpp

#include "frame_preprocessor.h"
#include "nvtx_profiling.h"

FramePreprocessor::FramePreprocessor(
    const char* name,
    CameraParams* cam_params,
    SafeQueue<ProcessedFrame*>* yolo_queue,
    SafeQueue<ProcessedFrame*>* encoder_queue,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    SafeQueue<ProcessedFrame*>& processed_recycle_queue)
    : CThreadWorker(name),
      camera_params_(cam_params),
      m_yolo_queue(yolo_queue),
      m_encoder_queue(encoder_queue),
      m_recycle_queue(recycle_queue),
      m_processed_recycle_queue(processed_recycle_queue)
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));
    initialize_gpu_debayer(&debayer_gpu_, camera_params_);
}

FramePreprocessor::~FramePreprocessor() {
    if (m_stream) cudaStreamDestroy(m_stream);
    if (debayer_gpu_.d_debayer) cudaFree(debayer_gpu_.d_debayer);
}

bool FramePreprocessor::WorkerFunction(WORKER_ENTRY* entry) {
    if (!entry) return false;

    NVTX_RANGE("FramePreprocessor_Worker");
    ck(cudaSetDevice(camera_params_->gpu_id));

    // Wait for the raw frame data to be ready
    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    // This is the SINGLE pre-processing step
    if (camera_params_->color) {
        debayer_frame_gpu(camera_params_, (FrameGPU*) &entry->d_image, &debayer_gpu_);
    } else {
        duplicate_channel_gpu(camera_params_, (FrameGPU*) &entry->d_image, &debayer_gpu_);
    }

    // Get a recycled ProcessedFrame or create a new one
    ProcessedFrame* processed_frame = nullptr;
    if (!m_processed_recycle_queue.pop(processed_frame)) {
        processed_frame = new ProcessedFrame();
    }
    
    // Populate the new struct
    processed_frame->d_processed_image = debayer_gpu_.d_debayer;
    processed_frame->width = entry->width;
    processed_frame->height = entry->height;
    processed_frame->timestamp = entry->timestamp;
    processed_frame->frame_id = entry->frame_id;
    processed_frame->has_detections = entry->has_detections;
    processed_frame->detections = entry->detections;
    processed_frame->detections_ready.store(entry->detections_ready.load());
    processed_frame->original_entry = entry;

    // Dispatch to downstream workers
    int dispatch_count = 0;
    if (m_yolo_queue) { dispatch_count++; }
    if (m_encoder_queue) { dispatch_count++; }

    if (dispatch_count > 0) {
        processed_frame->ref_count.store(dispatch_count);
        if (m_yolo_queue) { m_yolo_queue->push(processed_frame); }
        if (m_encoder_queue) { m_encoder_queue->push(processed_frame); }
    } else {
        // If no one needs this frame, recycle it immediately
        m_processed_recycle_queue.push(processed_frame);
        // And also recycle the original entry
        m_recycle_queue.push(entry);
    }

    return false; // This worker does not have an output queue of its own
}