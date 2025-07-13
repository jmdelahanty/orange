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
      m_processed_recycle_queue(processed_recycle_queue)
{
    std::cout << "[OPENGL_DISPLAY] CONSTRUCTOR for " << camera_params->camera_name << " on display GPU " << display_gpu_id << std::endl;
    ck(cudaSetDevice(display_gpu_id));
    ck(cudaStreamCreate(&m_stream));
    
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
    if (d_detections_for_drawing_) cudaFree(d_detections_for_drawing_);
    if (d_skeleton_for_drawing_) cudaFree(d_skeleton_for_drawing_);
    if (d_display_resize_buffer_) cudaFree(d_display_resize_buffer_);
}

void COpenGLDisplay::ThreadRunning()
{
    printf("OpenGLDisplay Thread Start %d\n", GetID());
    CUDA_CTX_LOG("DISPLAY: ThreadRunning Start");
    while (IsMachineOn())
    {
        ProcessedFrame* latest_frame = nullptr;
        // Step 1: Wait for and get the first available frame.
        if (m_input_queue && m_input_queue->pop(latest_frame))
        {
            CUDA_CTX_LOG("DISPLAY: Popped initial frame");
            // Step 2: Aggressively drain the queue to find the newest frame.
            ProcessedFrame* newer_frame = nullptr;
            while(m_input_queue->pop(newer_frame))
            {
                CUDA_CTX_LOG("DISPLAY: Discarding older frame");
                // A newer frame was available. The 'latest_frame' we were holding is now old.
                // We must decrement its reference count. If we were the last one
                // holding onto it, its resources will be properly recycled.
                if (latest_frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    CUDA_MEM_LOG("DISPLAY: Recycling discarded frame", latest_frame->original_entry, 0, latest_frame->frame_id);
                    m_recycle_queue.push(latest_frame->original_entry);
                    m_processed_recycle_queue.push(latest_frame);
                }
                // The frame we just popped is now the latest one.
                latest_frame = newer_frame;
            }

            // Step 3: After the loop, latest_frame holds the absolute most
            // recent frame. Process it.
            if (latest_frame)
            {
                CUDA_CTX_LOG("DISPLAY: Processing latest frame");
                WorkerFunction(latest_frame);
            }
        }
        else
        {
            // If the queue was empty, sleep briefly.
            usleep(1000);
        }
    }
    printf("OpenGLDisplay Thread DONE %d\n", GetID());
}

bool COpenGLDisplay::WorkerFunction(ProcessedFrame* frame)
{
    if (!frame) return false;

    CUDA_CTX_LOG("DISPLAY: WorkerFunction Start");
    ck(cudaSetDevice(display_gpu_id));
    nppSetStream(m_stream);

    // --- GPU-ACCELERATED DRAWING ---
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

    if (camera_select->downsample > 1) {
        output_display_size_.width = frame->width / camera_select->downsample;
        output_display_size_.height = frame->height / camera_select->downsample;
        
        NppiSize input_size = {frame->width, frame->height};
        NppiRect input_roi = {0, 0, frame->width, frame->height};
        NppiRect output_roi = {0, 0, output_display_size_.width, output_display_size_.height};

        nppiResize_8u_C4R(frame->d_processed_image, frame->width * 4, input_size, input_roi,
                            d_display_resize_buffer_, output_display_size_.width * 4, output_display_size_,
                            output_roi, NPPI_INTER_SUPER);

        size_t copy_size = (size_t)output_display_size_.width * (size_t)output_display_size_.height * 4;
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, d_display_resize_buffer_, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    } else {
        size_t copy_size = (size_t)frame->width * (size_t)frame->height * 4;
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, frame->d_processed_image, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    }

    CUDA_SYNC_LOG("DISPLAY: Waiting on stream sync", m_stream, frame->frame_id);
    ck(cudaStreamSynchronize(m_stream));
    CUDA_SYNC_LOG("DISPLAY: Stream sync complete", m_stream, frame->frame_id);

    // --- Final Step: Reference Counting and Recycling ---
    if (frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        CUDA_MEM_LOG("DISPLAY: Recycling processed frame", frame->original_entry, 0, frame->frame_id);
        m_recycle_queue.push(frame->original_entry);
        m_processed_recycle_queue.push(frame);
    }

    return false;
}