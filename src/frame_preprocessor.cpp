// src/frame_preprocessor.cpp

#include "frame_preprocessor.h"
#include "npp.h"
#include "cuda_context_debug.h"
#include "nvtx_profiling.h"

FramePreprocessor::FramePreprocessor(
    const char* name,
    CameraParams* cam_params,
    SafeQueue<ProcessedFrame*>* yolo_queue,
    SafeQueue<ProcessedFrame*>* encoder_queue,
    SafeQueue<ProcessedFrame*>* display_queue,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    SafeQueue<ProcessedFrame*>& processed_recycle_queue,
    SafeQueue<cudaEvent_t*>& free_events_queue)
    : CThreadWorker(name),
      camera_params_(cam_params),
      m_yolo_queue(yolo_queue),
      m_encoder_queue(encoder_queue),
      m_display_queue(display_queue),
      m_recycle_queue(recycle_queue),
      m_processed_recycle_queue(processed_recycle_queue),
      m_free_events_queue(free_events_queue),
      m_stream(nullptr)
{
    // Constructor no longer touches CUDA. It only stores parameters.
    // This makes it safe to call from any thread.
    debayer_gpu_.d_debayer = nullptr;
    std::cout << "[FramePreprocessor] Constructor for " << name << " on GPU " << camera_params_->gpu_id << std::endl;
}

FramePreprocessor::~FramePreprocessor() {
    // Make sure the stream is destroyed if it was created
    if (m_stream) {
        // It's good practice to set the device context before cleanup
        cudaSetDevice(camera_params_->gpu_id);
        cudaStreamDestroy(m_stream);
    }
}

bool FramePreprocessor::WorkerFunction(WORKER_ENTRY* entry) {
    if (!entry) return false;

    // 1. Establish the CUDA context and stream ON THIS THREAD.
    // This block will only run once, the first time a frame is processed.
    if (!m_stream) {
        ck(cudaSetDevice(camera_params_->gpu_id));
        ck(cudaStreamCreate(&m_stream));
        CUDA_CTX_LOG("PREPROCESSOR: Context and stream created on worker thread.");
    }

    CUDA_CTX_LOG("PREPROCESSOR: Starting WorkerFunction");
    NVTX_RANGE("FramePreprocessor_Worker");
    nppSetStream(m_stream); // Bind NPP operations to our stream

    // Lazily initialize debayer resources on the first frame
    if (!debayer_gpu_.d_debayer) {
        initialize_gpu_debayer(&debayer_gpu_, camera_params_);
    }
    
    // Wait for the raw frame data to be ready
    if (entry->event_ptr) {
        CUDA_SYNC_LOG("PREPROCESSOR: Waiting on acquisition event", *entry->event_ptr, entry->frame_id);
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));

        m_free_events_queue.push(entry->event_ptr);
    }

    // Perform the single, unified pre-processing step
    if (camera_params_->color) {
        debayer_frame_gpu(camera_params_, (FrameGPU*)&(entry->d_image), &debayer_gpu_);
    } else {
        duplicate_channel_gpu(camera_params_, (FrameGPU*)&(entry->d_image), &debayer_gpu_);
    }

    // Get a recycled ProcessedFrame or create a new one
    ProcessedFrame* processed_frame = nullptr;
    if (!m_processed_recycle_queue.pop(processed_frame)) {
        processed_frame = new ProcessedFrame();
        ck(cudaMalloc(&processed_frame->d_processed_image, (size_t)entry->width * entry->height * 4)); 
    }
    
    // Copy the processed data from our internal buffer to the frame's dedicated buffer
    ck(cudaMemcpyAsync(processed_frame->d_processed_image, debayer_gpu_.d_debayer, (size_t)entry->width * entry->height * 4, cudaMemcpyDeviceToDevice, m_stream));

    // Populate the new struct
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
    if (m_display_queue) { dispatch_count++; }

    if (dispatch_count > 0) {
        processed_frame->ref_count.store(dispatch_count);
        if (m_yolo_queue) { m_yolo_queue->push(processed_frame); }
        if (m_encoder_queue) { m_encoder_queue->push(processed_frame); }
        if (m_display_queue) { m_display_queue->push(processed_frame); }
    } else {
        // If no downstream workers, recycle everything immediately.
        m_processed_recycle_queue.push(processed_frame);
        m_recycle_queue.push(entry);
    }
    
    return false;
}