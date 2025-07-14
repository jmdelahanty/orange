#include "opengldisplay.h"
#include "cuda_context_debug.h"
#include "shaman.h"
#include "kernel.cuh"
#include <npp.h> // For nppSetStream and nppiResize_8u_C4R
#include <iostream>
#include <vector>
#include "common.hpp" // For shaman::MAX_OBJECTS

#define display_gpu_id 0

// Corrected Constructor
COpenGLDisplay::COpenGLDisplay(
    const char* name,
    CameraParams* camera_params,
    CameraEachSelect* camera_select,
    unsigned char* display_buffer_cuda_pbo,
    INDIGOSignalBuilder* indigo_signal_builder,
    SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
    SafeQueue<ProcessedFrame*>& processed_recycle_queue,
    CameraResources* resources
) : CThreadWorker<ProcessedFrame>(name),
      camera_params(camera_params),
      camera_select(camera_select),
      display_buffer_pbo_cuda_ptr_(display_buffer_cuda_pbo),
      indigo_signal_builder_(indigo_signal_builder),
      m_resources(resources),
      m_stream(nullptr),
      d_detections_for_drawing_(nullptr),
      d_display_resize_buffer_(nullptr),
      h_p2p_copy_buffer_(nullptr),
      m_raw_recycle_queue(raw_recycle_queue),
      m_processed_recycle_queue(processed_recycle_queue)
{
    std::cout << "[OPENGL_DISPLAY] CONSTRUCTOR for " << camera_params->camera_name
              << " on display GPU " << display_gpu_id << std::endl;

    ck(cudaSetDevice(display_gpu_id));
    ck(cudaStreamCreate(&m_stream));
    
    ck(cudaMalloc(&d_detections_for_drawing_, sizeof(pose::Object) * shaman::MAX_OBJECTS));
    
    size_t resize_buffer_size = (size_t)camera_params->width * camera_params->height * 4;
    ck(cudaMalloc(&d_display_resize_buffer_, resize_buffer_size));

    ck(cudaHostAlloc(&h_p2p_copy_buffer_, resize_buffer_size, cudaHostAllocDefault));

    std::cout << "[OPENGL_DISPLAY] Constructor completed for " << camera_params->camera_name << std::endl;
}

COpenGLDisplay::~COpenGLDisplay()
{
    std::cout << "[OPENGL_DISPLAY] DESTRUCTOR for " << (camera_params ? camera_params->camera_name : "unknown") << std::endl;
    ck(cudaSetDevice(display_gpu_id));

    if (m_stream) cudaStreamDestroy(m_stream);
    if (h_p2p_copy_buffer_) cudaFreeHost(h_p2p_copy_buffer_);
    if (d_detections_for_drawing_) cudaFree(d_detections_for_drawing_);
    if (d_display_resize_buffer_) cudaFree(d_display_resize_buffer_);
}

bool COpenGLDisplay::WorkerFunction(ProcessedFrame* f)
{
    if (!f) return false;

    ProcessedFrame* newest_frame = f;
    ProcessedFrame* next_frame = nullptr;

    SafeQueue<ProcessedFrame*>* input_queue = this->GetInputQueue();

    // Continuously pop from the queue until it's empty
    while (input_queue && input_queue->pop(next_frame)) {
        // We got a newer frame, so the current 'newest_frame' must be discarded.
        if (newest_frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_raw_recycle_queue.push(newest_frame->original_entry);
            m_processed_recycle_queue.push(newest_frame);
        }
        // The newly popped frame is now the newest one.
        newest_frame = next_frame;
    }
    
    f = newest_frame; // Process the newest frame we found

    std::cout << "[DISPLAY] Camera " << camera_params->camera_serial
              << "Processing freshest frame " << f->frame_id << std::endl;

    ck(cudaSetDevice(display_gpu_id));
    CUDA_CTX_LOG("DISPLAY_WORKER: Set device for frame " + std::to_string(f->frame_id));
    nppSetStream(m_stream);

    if (f->processed_event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *f->processed_event_ptr, 0));
    }
    CUDA_SYNC_LOG("Preprocessor event signaled", m_stream, f->frame_id);

    unsigned char* processed_image_ptr = f->d_processed_image;

    if (f->detections_ready.load() && !f->detections.empty()) {
        CUDA_RT_LOG("Drawing detections on frame");
        
        size_t detection_data_size = f->detections.size() * sizeof(pose::Object);
        CUDA_MEM_LOG("Copying detection data for drawing", f->detections.data(), detection_data_size, f->frame_id);
        ck(cudaMemcpyAsync(d_detections_for_drawing_,
                           f->detections.data(),
                           detection_data_size,
                           cudaMemcpyHostToDevice,
                           m_stream));

        CUDA_RT_LOG("Launching gpu_draw_box kernel");
        gpu_draw_box(
            processed_image_ptr,
            f->width,
            f->height,
            d_detections_for_drawing_,
            f->detections.size(),
            m_stream);
    }

    if (camera_select->downsample > 1) {
        CUDA_RT_LOG("Downsampling for display");
        NppiSize input_size = { f->width, f->height };
        NppiRect input_roi = { 0, 0, f->width, f->height };

        int out_width = f->width / camera_select->downsample;
        int out_height = f->height / camera_select->downsample;
        NppiSize output_size = { out_width, out_height };
        NppiRect output_roi = { 0, 0, out_width, out_height };

        nppiResize_8u_C4R(processed_image_ptr, f->width * 4, input_size, input_roi,
                            d_display_resize_buffer_, out_width * 4, output_size,
                            output_roi, NPPI_INTER_SUPER);

        size_t copy_size = (size_t)out_width * out_height * 4;
        CUDA_MEM_LOG("Copying downsampled image to PBO", d_display_resize_buffer_, copy_size, f->frame_id);
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, d_display_resize_buffer_, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    } else {
        size_t copy_size = (size_t)f->width * f->height * 4;
        CUDA_MEM_LOG("Copying full-size image to PBO", processed_image_ptr, copy_size, f->frame_id);
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, processed_image_ptr, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    }

    CUDA_SYNC_LOG("Synchronizing stream after display copy", m_stream, f->frame_id);
    ck(cudaStreamSynchronize(m_stream));
    CUDA_SYNC_LOG("Stream synchronized", m_stream, f->frame_id);

    if (f->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // This is the last worker for this frame. Recycle everything.
        
        // ** THE FIX: Return the event to the free pool **
        if (f->original_entry && f->original_entry->event_ptr) {
            m_resources->free_events_queue->push(f->original_entry->event_ptr);
        }

        // Recycle the raw and processed frame structs
        m_raw_recycle_queue.push(f->original_entry);
        m_processed_recycle_queue.push(f);
    }

    return false;
}