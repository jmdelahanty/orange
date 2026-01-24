// src/opengldisplay.cpp

#include "opengldisplay.h"
#include "enet_thread.h"
#include "cuda_context_debug.h"
#include <vector>
#include "kernel.cuh"
#include "shaman.h"
#include "npp_utils.h"
#include <cuda_runtime.h>
#include <iostream>
#include <cuda.h>
#include "global.h"
#include <npp.h> // For NPP APIs
#include "yolo_worker.h"
#include <cstring>

#define display_gpu_id 0 

namespace {
bool SkipDisplayYoloWait()
{
    static const bool enabled = []() {
        const char* env = std::getenv("ORANGE_DISPLAY_SKIP_YOLO_WAIT");
        const bool on = env && *env && std::strcmp(env, "0") != 0;
        if (on) {
            std::cout << "[OPENGL_DISPLAY] Skipping YOLO CPU wait for overlays." << std::endl;
        }
        return on;
    }();
    return enabled;
}
}  // namespace

COpenGLDisplay::COpenGLDisplay(const char* name, CameraParams *camera_params, CameraEachSelect *camera_select, unsigned char *display_buffer_cuda_pbo, INDIGOSignalBuilder* indigo_signal_builder, SafeQueue<WORKER_ENTRY*>& recycle_queue)
    : CThreadWorker(name),
      camera_params(camera_params),
      camera_select(camera_select),
      display_buffer_pbo_cuda_ptr_(display_buffer_cuda_pbo),
      indigo_signal_builder_(indigo_signal_builder),
      h_p2p_copy_buffer_(nullptr),
      d_detections_for_drawing_(nullptr), // Changed from d_points_for_drawing_
      d_skeleton_for_drawing_(nullptr),
      d_display_resize_buffer_(nullptr),
      m_stream(nullptr),
      m_recycle_queue(recycle_queue),
      last_display_log_time_(std::chrono::steady_clock::now())
{
    std::cout << "[OPENGL_DISPLAY] CONSTRUCTOR for " << camera_params->camera_name << " on display GPU " << display_gpu_id << std::endl;
    ck(cudaSetDevice(display_gpu_id));
    ck(cudaStreamCreate(&m_stream));

    initalize_gpu_frame(&frame_original_gpu_, camera_params);
    initialize_gpu_debayer(&debayer_gpu_, camera_params);
    
    // UPDATED: Allocate buffer for pose::Object structs, not floats
    ck(cudaMalloc(&d_detections_for_drawing_, sizeof(pose::Object) * shaman::MAX_OBJECTS));
    
    ck(cudaMalloc(&d_skeleton_for_drawing_, sizeof(unsigned int) * 4 * 2));
    ck(cudaMalloc(&d_display_resize_buffer_, (size_t)camera_params->width * camera_params->height * 4));

    size_t staging_buffer_size = (size_t)camera_params->width * camera_params->height * 4;
    ck(cudaHostAlloc(&h_p2p_copy_buffer_, staging_buffer_size, cudaHostAllocDefault));
    std::cout << "[OPENGL_DISPLAY] Constructor completed for " << camera_params->camera_name << std::endl;
}

COpenGLDisplay::~COpenGLDisplay()
{
    std::cout << "[OPENGL_DISPLAY] DESTRUCTOR for " << (camera_params ? camera_params->camera_name : "unknown") << std::endl;
    ck(cudaSetDevice(display_gpu_id));

    if (m_stream) cudaStreamDestroy(m_stream);
    if (h_p2p_copy_buffer_) cudaFreeHost(h_p2p_copy_buffer_);

    if (frame_original_gpu_.d_orig) cudaFree(frame_original_gpu_.d_orig);
    if (debayer_gpu_.d_debayer) cudaFree(debayer_gpu_.d_debayer);
    if (d_detections_for_drawing_) cudaFree(d_detections_for_drawing_);
    if (d_skeleton_for_drawing_) cudaFree(d_skeleton_for_drawing_);
    if (d_display_resize_buffer_) cudaFree(d_display_resize_buffer_);
}


bool COpenGLDisplay::WorkerFunction(WORKER_ENTRY* f)
{
    if (!f) return false;

    // This logic to get the latest frame is good, let's keep it.
    WORKER_ENTRY* latest_frame = f;
    WORKER_ENTRY* discarded_frame = nullptr;

    while ((discarded_frame = GetObjectFromQueueIn()) != nullptr) {
        if (latest_frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (latest_frame->gpu_direct_mode && latest_frame->camera_instance && latest_frame->camera_frame_struct) {
                EVT_CameraQueueFrame(latest_frame->camera_instance, latest_frame->camera_frame_struct);
            }
            m_recycle_queue.push(latest_frame);
        }
        latest_frame = discarded_frame;
    }

    ck(cudaSetDevice(display_gpu_id));
    EnsureNppStream(m_stream);

    // Wait for the data to be ready from the acquisition thread
    if (latest_frame->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *latest_frame->event_ptr, 0));
    }
    
    // Also wait for YOLO detections if they exist for this frame, so we can draw them
    if (latest_frame->has_detections && latest_frame->yolo_completion_event) {
        ck(cudaStreamWaitEvent(m_stream, *latest_frame->yolo_completion_event, 0));
    }
    
    // Spin-wait until the YOLO worker has finished its CPU-side post-processing
    bool allow_overlay = latest_frame->has_detections && camera_select->yolo;
    if (allow_overlay) {
        if (!SkipDisplayYoloWait()) {
            while (!latest_frame->detections_ready.load(std::memory_order_acquire)) {
                // Busy-wait to keep overlays in sync with the current frame.
            }
        } else if (!latest_frame->detections_ready.load(std::memory_order_acquire)) {
            allow_overlay = false;
        }
    }

    // --- Perform image processing on the GPU ---
    size_t frame_size = (size_t)camera_params->width * camera_params->height;

    // Handle P2P copy if the acquisition GPU is different from the display GPU
    if (camera_params->gpu_id != display_gpu_id) {
        ck(cudaMemcpyAsync(h_p2p_copy_buffer_, latest_frame->d_image, frame_size, cudaMemcpyDeviceToHost, m_stream));
        ck(cudaMemcpyAsync(frame_original_gpu_.d_orig, h_p2p_copy_buffer_, frame_size, cudaMemcpyHostToDevice, m_stream));
        display_cross_gpu_frames_++;
    } else {
        ck(cudaMemcpyAsync(frame_original_gpu_.d_orig, latest_frame->d_image, frame_size, cudaMemcpyDeviceToDevice, m_stream));
        display_same_gpu_frames_++;
    }

    // Debayer or duplicate mono channel to get a 4-channel RGBA image in debayer_gpu_.d_debayer
    if (camera_params->color){
        debayer_frame_gpu(camera_params, &frame_original_gpu_, &debayer_gpu_);
    } else {
        duplicate_channel_gpu(camera_params, &frame_original_gpu_, &debayer_gpu_);
    }

    // --- Draw detections directly on the GPU buffer ---
    if (allow_overlay && !latest_frame->detections.empty()) {
        // Copy detection data to the GPU
        ck(cudaMemcpyAsync(d_detections_for_drawing_, 
                           latest_frame->detections.data(), 
                           latest_frame->detections.size() * sizeof(pose::Object), 
                           cudaMemcpyHostToDevice, 
                           m_stream));

        // Launch the kernel to draw boxes directly onto the debayered RGBA image
        gpu_draw_box(
            debayer_gpu_.d_debayer, // Draw onto the RGBA buffer
            camera_params->width,
            camera_params->height,
            d_detections_for_drawing_,
            latest_frame->detections.size(),
            m_stream);
    }
    
    // --- FINAL GPU-to-GPU COPY
    // Instead of copying to CPU, we now copy from our final GPU buffer (debayer_gpu_.d_debayer)
    // directly to the PBO's mapped CUDA pointer (display_buffer_pbo_cuda_ptr_).

    unsigned char* final_image_source = debayer_gpu_.d_debayer;
    size_t copy_size = (size_t)camera_params->width * camera_params->height * 4;

    // Handle downsampling if needed
    if (camera_select->downsample > 1) {
        output_display_size_.width = camera_params->width / camera_select->downsample;
        output_display_size_.height = camera_params->height / camera_select->downsample;
        NppiSize input_size = {static_cast<int>(camera_params->width), static_cast<int>(camera_params->height)};
        NppiRect input_roi = {0, 0, static_cast<int>(camera_params->width), static_cast<int>(camera_params->height)};
        NppiRect output_roi = {0, 0, output_display_size_.width, output_display_size_.height};
        
        // Resize the RGBA image
        nppiResize_8u_C4R(debayer_gpu_.d_debayer, camera_params->width * 4, input_size, input_roi,
                            d_display_resize_buffer_, output_display_size_.width * 4, output_display_size_,
                            output_roi, NPPI_INTER_SUPER);
        
        // Update the source and size for the final copy
        final_image_source = d_display_resize_buffer_;
        copy_size = (size_t)output_display_size_.width * output_display_size_.height * 4;
    }
    
    // Perform the efficient GPU->GPU copy into the PBO buffer
    ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, final_image_source, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    
    // Synchronize this worker's stream to ensure the copy is complete before OpenGL uses it
    ck(cudaStreamSynchronize(m_stream));

    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> log_elapsed = now - last_display_log_time_;
    if (log_elapsed.count() >= 1.0) {
        std::cout << "[DISPLAY] Cam " << camera_params->camera_serial
                  << " GPU " << camera_params->gpu_id
                  << " display_gpu " << display_gpu_id
                  << " same_gpu=" << display_same_gpu_frames_
                  << " cross_gpu=" << display_cross_gpu_frames_
                  << std::endl;
        display_same_gpu_frames_ = 0;
        display_cross_gpu_frames_ = 0;
        last_display_log_time_ = now;
    }
    
    // --- Cleanup and recycle ---
    if (latest_frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (latest_frame->gpu_direct_mode && latest_frame->camera_instance && latest_frame->camera_frame_struct) {
            EVT_CameraQueueFrame(latest_frame->camera_instance, latest_frame->camera_frame_struct);
        }
        m_recycle_queue.push(latest_frame);
    }

    return false; // This worker doesn't pass items to its own output queue
}
