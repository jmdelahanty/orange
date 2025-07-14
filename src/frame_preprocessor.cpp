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
    SafeQueue<ProcessedFrame*>& processed_recycle_queue)
    : CThreadWorker(name),
      camera_params_(cam_params),
      m_yolo_queue(yolo_queue),
      m_encoder_queue(encoder_queue),
      m_display_queue(display_queue),
      m_recycle_queue(recycle_queue),
      m_processed_recycle_queue(processed_recycle_queue),
      m_stream(nullptr)
{
    debayer_gpu_.d_debayer = nullptr; // Ensure it's null initially
    std::cout << "[FramePreprocessor] Constructor for " << name << " on GPU " << camera_params_->gpu_id << std::endl;
}

FramePreprocessor::~FramePreprocessor() {
    if (m_stream) {
        cudaSetDevice(camera_params_->gpu_id);
        if(debayer_gpu_.d_debayer) cudaFree(debayer_gpu_.d_debayer);
        cudaStreamDestroy(m_stream);
    }
}

bool FramePreprocessor::WorkerFunction(WORKER_ENTRY* entry) {
    if (!entry) return false;

    if (!m_stream) {
        ck(cudaSetDevice(camera_params_->gpu_id));
        ck(cudaStreamCreate(&m_stream));
        initialize_gpu_debayer(&debayer_gpu_, camera_params_);
        CUDA_CTX_LOG("PREPROCESSOR: Context and stream created on worker thread.");
    }

    NVTX_RANGE("FramePreprocessor_Worker");
    nppSetStream(m_stream);

    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    FrameGPU frame_original_gpu;
    frame_original_gpu.d_orig = entry->d_image;
    if (camera_params_->color) {
        debayer_frame_gpu(camera_params_, &frame_original_gpu, &debayer_gpu_);
    } else {
        duplicate_channel_gpu(camera_params_, &frame_original_gpu, &debayer_gpu_);
    }

    ProcessedFrame* processed_frame = nullptr;
    if (!m_processed_recycle_queue.pop(processed_frame)) {
        processed_frame = new ProcessedFrame();
        // Allocate the buffer for the processed image ONCE
        ck(cudaMalloc(&processed_frame->d_processed_image, (size_t)entry->width * entry->height * 4)); 
    }
    
    // Now we can safely access members because the full definition is included
    ck(cudaMemcpyAsync(processed_frame->d_processed_image, debayer_gpu_.d_debayer, (size_t)entry->width * entry->height * 4, cudaMemcpyDeviceToDevice, m_stream));

    processed_frame->width = entry->width;
    processed_frame->height = entry->height;
    processed_frame->timestamp = entry->timestamp;
    processed_frame->frame_id = entry->frame_id;
    processed_frame->timestamp_sys = entry->timestamp_sys;
    processed_frame->has_detections = entry->has_detections;
    processed_frame->detections_ready.store(false);
    processed_frame->original_entry = entry;

    cudaEvent_t processed_event; // Correctly declare as a value, not a pointer
    ck(cudaEventCreate(&processed_event)); 
    ck(cudaEventRecord(processed_event, m_stream));
    processed_frame->processed_event_ptr = new cudaEvent_t(processed_event); // Store a pointer to a new event

    // Dispatching logic remains the same...
    int dispatch_count = 0;
    if (m_yolo_queue) { dispatch_count++; }
    if (m_encoder_queue) { dispatch_count++; }
    if (m_display_queue) { dispatch_count++; }

    if (dispatch_count > 0) {
        processed_frame->ref_count.store(dispatch_count);
        if (m_yolo_queue)    m_yolo_queue->push(processed_frame);
        if (m_encoder_queue) m_encoder_queue->push(processed_frame);
        if (m_display_queue) m_display_queue->push(processed_frame);
    } else {
        m_processed_recycle_queue.push(processed_frame);
        m_recycle_queue.push(entry);
    }
    
    return false;
}