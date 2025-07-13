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
      m_processed_recycle_queue(processed_recycle_queue)
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));
    debayer_gpu_.d_debayer = nullptr;
    // We no longer need to initialize debayer here, that happens in the worker
    // to ensure it's on the correct thread's context.
    std::cout << "[FramePreprocessor] Constructor for " << name << " on GPU " << camera_params_->gpu_id << std::endl;
}

FramePreprocessor::~FramePreprocessor() {
    if (m_stream) cudaStreamDestroy(m_stream);
}

bool FramePreprocessor::WorkerFunction(WORKER_ENTRY* entry) {
    if (!entry) return false;
    
    CUDA_CTX_LOG("PREPROCESSOR: Starting WorkerFunction");
    NVTX_RANGE("FramePreprocessor_Worker");
    ck(cudaSetDevice(camera_params_->gpu_id));
    nppSetStream(m_stream); // Bind NPP operations to our stream

    // Lazily initialize debayer resources on the first frame, on the correct thread
    if (!debayer_gpu_.d_debayer) {
        initialize_gpu_debayer(&debayer_gpu_, camera_params_);
    }
    
    // Wait for the raw frame data to be ready
    if (entry->event_ptr) {
        CUDA_SYNC_LOG("PREPROCESSOR: Waiting on acquisition event", *entry->event_ptr, entry->frame_id);
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    // Perform the single, unified pre-processing step
    if (camera_params_->color) {
        // This is a placeholder; the FrameGPU struct needs to be populated correctly
        FrameGPU temp_frame_gpu;
        temp_frame_gpu.d_orig = entry->d_image;
        temp_frame_gpu.size_pic = entry->width * entry->height;
        debayer_frame_gpu(camera_params_, &temp_frame_gpu, &debayer_gpu_);
    } else {
        FrameGPU temp_frame_gpu;
        temp_frame_gpu.d_orig = entry->d_image;
        temp_frame_gpu.size_pic = entry->width * entry->height;
        duplicate_channel_gpu(camera_params_, &temp_frame_gpu, &debayer_gpu_);
    }

    // Get a recycled ProcessedFrame or create a new one
    ProcessedFrame* processed_frame = nullptr;
    if (!m_processed_recycle_queue.pop(processed_frame)) {
        processed_frame = new ProcessedFrame();
        // Allocate buffer for the processed image data if it's a new frame
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
        if (m_yolo_queue) { m_yolo_queue->push(processed_frame); CUDA_CTX_LOG("PREPROCESSOR: Pushed to YOLO Queue"); }
        if (m_encoder_queue) { m_encoder_queue->push(processed_frame); CUDA_CTX_LOG("PREPROCESSOR: Pushed to Encoder Queue");}
        if (m_display_queue) { m_display_queue->push(processed_frame); CUDA_CTX_LOG("PREPROCESSOR: Pushed to Display Queue");}
    } else {
        m_processed_recycle_queue.push(processed_frame);
        m_recycle_queue.push(entry);
    }
    
    // This worker function from CThreadWorker base class should return false,
    // as it dispatches work itself and doesn't use the base class's output queue.
    return false;
}