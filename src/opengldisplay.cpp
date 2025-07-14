// src/opengldisplay.cpp

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
    SafeQueue<ProcessedFrame*>& processed_recycle_queue
) : CThreadWorker<ProcessedFrame>(name), // <-- Template type is ProcessedFrame
      camera_params(camera_params),
      camera_select(camera_select),
      display_buffer_pbo_cuda_ptr_(display_buffer_cuda_pbo),
      indigo_signal_builder_(indigo_signal_builder),
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

    // This worker no longer needs frame_original_gpu_ or debayer_gpu_.
    
    // Allocate buffer for detection data.
    ck(cudaMalloc(&d_detections_for_drawing_, sizeof(pose::Object) * shaman::MAX_OBJECTS));
    
    // Allocate buffer for downsampling.
    size_t resize_buffer_size = (size_t)camera_params->width * camera_params->height * 4;
    ck(cudaMalloc(&d_display_resize_buffer_, resize_buffer_size));

    // Allocate host-pinned buffer for P2P copies if needed.
    ck(cudaHostAlloc(&h_p2p_copy_buffer_, resize_buffer_size, cudaHostAllocDefault));

    std::cout << "[OPENGL_DISPLAY] Constructor completed for " << camera_params->camera_name << std::endl;
}

// Corrected Destructor
COpenGLDisplay::~COpenGLDisplay()
{
    std::cout << "[OPENGL_DISPLAY] DESTRUCTOR for " << (camera_params ? camera_params->camera_name : "unknown") << std::endl;
    ck(cudaSetDevice(display_gpu_id));

    if (m_stream) cudaStreamDestroy(m_stream);
    if (h_p2p_copy_buffer_) cudaFreeHost(h_p2p_copy_buffer_);
    if (d_detections_for_drawing_) cudaFree(d_detections_for_drawing_);
    if (d_display_resize_buffer_) cudaFree(d_display_resize_buffer_);
}

// Corrected WorkerFunction
bool COpenGLDisplay::WorkerFunction(ProcessedFrame* f)
{
    if (!f) return false;

    // This worker now processes one frame at a time, so the loop to drain the queue is removed.
    // That logic belongs in the acquisition thread.

    ck(cudaSetDevice(display_gpu_id));
    nppSetStream(m_stream);

    // Wait for the FramePreprocessor to be done with this frame.
    if (f->processed_event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *f->processed_event_ptr, 0));
    }
    
    // The frame is already debayered. We receive an RGBA image in f->d_processed_image.
    // We create a temporary pointer to it for clarity.
    unsigned char* processed_image_ptr = f->d_processed_image;

    // --- GPU-ACCELERATED DRAWING ---
    if (f->has_detections && !f->detections.empty()) {
        // Wait for YOLO detections to be ready before trying to draw them.
        while (!f->detections_ready.load(std::memory_order_acquire)) {
            // Spin-wait is acceptable here for low-latency.
        }

        // 1. Copy the detection data to our dedicated buffer for drawing.
        ck(cudaMemcpyAsync(d_detections_for_drawing_, 
                           f->detections.data(), 
                           f->detections.size() * sizeof(pose::Object), 
                           cudaMemcpyHostToDevice, 
                           m_stream));

        // 2. Launch the kernel to draw rectangles directly onto the processed RGBA image.
        gpu_draw_box(
            processed_image_ptr, // Draw directly on the processed frame
            f->width,
            f->height,
            d_detections_for_drawing_,
            f->detections.size(),
            m_stream);
    }
    
    // --- DOWNSAMPLING AND DISPLAY ---
    if (camera_select->downsample > 1) {
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
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, d_display_resize_buffer_, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    } else {
        size_t copy_size = (size_t)f->width * f->height * 4;
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, processed_image_ptr, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    }

    ck(cudaStreamSynchronize(m_stream));
    
    // --- Corrected Recycling Logic ---
    if (f->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // This is the last worker. Recycle both the raw and processed entries.
        m_raw_recycle_queue.push(f->original_entry);
        m_processed_recycle_queue.push(f);
    }

    return false; 
}