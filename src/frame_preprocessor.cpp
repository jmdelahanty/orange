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

    // This setup code is correct
    if (!m_stream) {
        ck(cudaSetDevice(camera_params_->gpu_id));
        ck(cudaStreamCreate(&m_stream));
        initialize_gpu_debayer(&debayer_gpu_, camera_params_);
    }

    NVTX_RANGE("FramePreprocessor_Worker");
    nppSetStream(m_stream);

    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    // Debayering/duplication is also correct
    FrameGPU frame_original_gpu;
    frame_original_gpu.d_orig = entry->d_image;
    if (camera_params_->color) {
        debayer_frame_gpu(camera_params_, &frame_original_gpu, &debayer_gpu_);
    } else {
        duplicate_channel_gpu(camera_params_, &frame_original_gpu, &debayer_gpu_);
    }

    // Get a recycled ProcessedFrame or create a new one
    ProcessedFrame* processed_frame = nullptr;
    if (!m_processed_recycle_queue.pop(processed_frame)) {
        processed_frame = new ProcessedFrame();
        ck(cudaMalloc(&processed_frame->d_processed_image, (size_t)entry->width * entry->height * 4)); 
    }
    
    // Copy the processed RGBA data
    ck(cudaMemcpyAsync(processed_frame->d_processed_image, debayer_gpu_.d_debayer, (size_t)entry->width * entry->height * 4, cudaMemcpyDeviceToDevice, m_stream));

    // Populate the processed frame's metadata
    processed_frame->width = entry->width;
    processed_frame->height = entry->height;
    processed_frame->timestamp = entry->timestamp;
    processed_frame->frame_id = entry->frame_id;
    processed_frame->timestamp_sys = entry->timestamp_sys;
    processed_frame->has_detections = entry->has_detections;
    processed_frame->detections_ready.store(false);
    processed_frame->original_entry = entry;

    // Create a new event for this processed frame
    cudaEvent_t* processed_event = new cudaEvent_t();
    ck(cudaEventCreate(processed_event)); 
    ck(cudaEventRecord(*processed_event, m_stream));
    processed_frame->processed_event_ptr = processed_event;

    // COUNT ONLY THE ACTIVE (NON-NULL) DOWNSTREAM WORKERS
    int active_worker_count = 0;
    if (m_yolo_queue != nullptr) { active_worker_count++; }
    if (m_encoder_queue != nullptr) { active_worker_count++; }
    if (m_display_queue != nullptr) { active_worker_count++; }

    if (active_worker_count > 0) {
        // Set reference count to match the number of active downstream workers
        processed_frame->ref_count.store(active_worker_count, std::memory_order_relaxed);
        
        // Only push to the queues that are actually active (non-null)
        if (m_yolo_queue != nullptr) {
            m_yolo_queue->push(processed_frame);
        }
        if (m_encoder_queue != nullptr) {
            m_encoder_queue->push(processed_frame);
        }
        if (m_display_queue != nullptr) {
            m_display_queue->push(processed_frame);
        }
    } else {
        // If there are no downstream workers, we must recycle everything now.
        m_processed_recycle_queue.push(processed_frame);
        m_recycle_queue.push(entry);
    }
    
    return false; // This worker does not pass items to its own output queue
} 