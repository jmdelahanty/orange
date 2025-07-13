// src/opengldisplay.cpp

#include "opengldisplay.h"
#include "enet_thread.h"
#include "cuda_context_debug.h"
#include <vector>
#include "kernel.cuh"
#include "shaman.h"
#include <cuda_runtime.h>
#include <iostream>
#include <cuda.h>
#include "global.h"
#include <npp.h>
#include "yolo_worker.h"
#include "global.h"

#define display_gpu_id 0

COpenGLDisplay::COpenGLDisplay(
    const char* name,
    CameraParams *camera_params,
    CameraEachSelect *camera_select,
    unsigned char *display_buffer_cuda_pbo,
    INDIGOSignalBuilder* indigo_signal_builder,
    SafeQueue<ProcessedFrame*>* input_queue,
    SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
    SafeQueue<ProcessedFrame*>& processed_recycle_queue)
    : CThreadWorker<ProcessedFrame>(name),
      m_input_queue(input_queue),
      camera_params(camera_params),
      camera_select(camera_select),
      display_buffer_pbo_cuda_ptr_(display_buffer_cuda_pbo),
      indigo_signal_builder_(indigo_signal_builder),
      h_p2p_copy_buffer_(nullptr),
      d_detections_for_drawing_(nullptr),
      d_skeleton_for_drawing_(nullptr),
      d_display_resize_buffer_(nullptr),
      m_stream(nullptr),
      m_recycle_queue(raw_recycle_queue),
      m_processed_recycle_queue(processed_recycle_queue),
      last_fps_update_time_(std::chrono::steady_clock::now()),
      frame_counter_(0)
{
    // Constructor now ONLY sets up parameters.
    // It does NOT interact with CUDA, as it runs on the main thread.
    std::cout << "[OPENGL_DISPLAY] CONSTRUCTOR for " << camera_params->camera_name << " on display GPU " << display_gpu_id << std::endl;
}

COpenGLDisplay::~COpenGLDisplay()
{
    std::cout << "[OPENGL_DISPLAY] DESTRUCTOR for " << (camera_params ? camera_params->camera_name : "unknown") << std::endl;
    // The thread should be stopped before destruction, so we can safely clean up resources.
    ck(cudaSetDevice(display_gpu_id)); // Set context for cleanup
    if (m_stream) cudaStreamDestroy(m_stream);
    if (d_detections_for_drawing_) cudaFree(d_detections_for_drawing_);
    if (d_skeleton_for_drawing_) cudaFree(d_skeleton_for_drawing_);
    if (d_display_resize_buffer_) cudaFree(d_display_resize_buffer_);
}

void COpenGLDisplay::ThreadRunning()
{
    // 1. Set the CUDA device FOR THIS THREAD. This creates the context.
    ck(cudaSetDevice(display_gpu_id));
    printf("OpenGLDisplay Thread Start %d\n", GetID());
    CUDA_CTX_LOG("DISPLAY: ThreadRunning Start - Context Established");

    // 2. Now that a context exists, we can create stream and buffers.
    ck(cudaStreamCreate(&m_stream));
    ck(cudaMalloc(&d_detections_for_drawing_, sizeof(pose::Object) * shaman::MAX_OBJECTS));
    ck(cudaMalloc(&d_skeleton_for_drawing_, sizeof(unsigned int) * 4 * 2));
    ck(cudaMalloc(&d_display_resize_buffer_, (size_t)camera_params->width * camera_params->height * 4));

    // Enable peer access between the acquisition GPU and the display GPU
    if (camera_params->gpu_id != display_gpu_id) {
        cudaSetDevice(display_gpu_id);
        cudaDeviceEnablePeerAccess(camera_params->gpu_id, 0);
    }
    
    // --- The rest of the loop is the same ---
    while (IsMachineOn())
    {
        ProcessedFrame* latest_frame = nullptr;
        if (m_input_queue && m_input_queue->pop(latest_frame))
        {
            ProcessedFrame* newer_frame = nullptr;
            while(m_input_queue->pop(newer_frame))
            {
                if (latest_frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    m_recycle_queue.push(latest_frame->original_entry);
                    m_processed_recycle_queue.push(latest_frame);
                }
                latest_frame = newer_frame;
            }

            if (latest_frame)
            {
                WorkerFunction(latest_frame);
            }
        }
        else
        {
            usleep(1000);
        }
    }
    printf("OpenGLDisplay Thread DONE %d\n", GetID());
}

bool COpenGLDisplay::WorkerFunction(ProcessedFrame* frame)
{
    if (!frame) return false;

    frame_counter_++;
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_fps_update_time_;
    if (elapsed.count() >= 1.0) {
        // Calculate FPS
        double fps = static_cast<double>(frame_counter_) / elapsed.count();
        // Update the global atomic variable
        streaming_fps.store(fps);
        // Reset counters for the next interval
        frame_counter_ = 0;
        last_fps_update_time_ = now;
    }

    nppSetStream(m_stream);

    if (frame->has_detections && frame->detections_ready.load(std::memory_order_acquire) && !frame->detections.empty()) {
        ck(cudaMemcpyAsync(d_detections_for_drawing_,
                           frame->detections.data(),
                           frame->detections.size() * sizeof(pose::Object),
                           cudaMemcpyHostToDevice,
                           m_stream));

        gpu_draw_box(
            frame->d_processed_image,
            frame->width,
            frame->height,
            d_detections_for_drawing_,
            frame->detections.size(),
            m_stream);
    }

    // --- Select the correct copy method based on GPU IDs ---
    unsigned char* source_buffer_for_copy = frame->d_processed_image;
    size_t copy_size = (size_t)frame->width * (size_t)frame->height * 4;

    if (camera_select->downsample > 1) {
        output_display_size_.width = frame->width / camera_select->downsample;
        output_display_size_.height = frame->height / camera_select->downsample;
        
        NppiSize input_size = {frame->width, frame->height};
        NppiRect input_roi = {0, 0, frame->width, frame->height};
        NppiRect output_roi = {0, 0, output_display_size_.width, output_display_size_.height};

        nppiResize_8u_C4R(frame->d_processed_image, frame->width * 4, input_size, input_roi,
                            d_display_resize_buffer_, output_display_size_.width * 4, output_display_size_,
                            output_roi, NPPI_INTER_SUPER);

        source_buffer_for_copy = d_display_resize_buffer_; // Update the source for the copy
        copy_size = (size_t)output_display_size_.width * (size_t)output_display_size_.height * 4;
    }

    // Now perform the copy using the correct method
    if (camera_params->gpu_id == display_gpu_id) {
        // Same GPU: Use standard device-to-device copy
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, source_buffer_for_copy, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    } else {
        // Different GPUs: Use peer-to-peer copy
        ck(cudaMemcpyPeerAsync(display_buffer_pbo_cuda_ptr_, display_gpu_id, 
                               source_buffer_for_copy, camera_params->gpu_id, 
                               copy_size, m_stream));
    }

    ck(cudaStreamSynchronize(m_stream));

    // --- Corrected Recycling Logic ---
    if (frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        WORKER_ENTRY* entry = frame->original_entry;

        // If the frame came from a GPU Direct buffer, requeue it with the camera SDK first.
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }

        // Now, recycle the software containers.
        m_recycle_queue.push(entry);
        m_processed_recycle_queue.push(frame);
    }

    return false;
}