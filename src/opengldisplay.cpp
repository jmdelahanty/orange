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
#include <npp.h> // For nppSetStream
#include "yolo_worker.h"

#define display_gpu_id 0

// CHANGE 1: The constructor now accepts and stores the input queue
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
      m_input_queue(input_queue), // <-- Store the passed queue
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

// CHANGE 2: Implement the corrected ThreadRunning loop
void COpenGLDisplay::ThreadRunning()
{
    printf("OpenGLDisplay Thread Start %d\n", GetID());
    // Use the public IsMachineOn() to control the loop
    while (IsMachineOn())
    {
        ProcessedFrame* f = nullptr;
        // Use the new m_input_queue, not the private base class one
        if (m_input_queue && m_input_queue->pop(f))
        {
            if (f)
            {
                // The main logic is still in WorkerFunction.
                // It returns false, so we don't need to do anything with the return value.
                WorkerFunction(f);
            }
        }
        else
        {
            // Sleep if the queue is empty.
            // We use a hardcoded value since the base 'interval' is private.
            // 1ms is a reasonable default.
            usleep(1000);
        }
    }

    // Process any remaining items after the stop signal.
    while (true)
    {
        ProcessedFrame* f = nullptr;
        if (m_input_queue && m_input_queue->pop(f))
        {
            if (f)
            {
                WorkerFunction(f);
            }
        }
        else
        {
            break; // Exit when the queue is empty.
        }
    }
    printf("OpenGLDisplay Thread DONE %d\n", GetID());
}


bool COpenGLDisplay::WorkerFunction(ProcessedFrame* frame)
{
    if (!frame) return false;

    // This logic to get only the latest frame is good and remains.
    ProcessedFrame* latest_frame = frame;
    ProcessedFrame* discarded_frame = nullptr;

    while (m_input_queue && m_input_queue->pop(discarded_frame)) {
        if (latest_frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_recycle_queue.push(latest_frame->original_entry);
            m_processed_recycle_queue.push(latest_frame);
        }
        latest_frame = discarded_frame;
    }

    ck(cudaSetDevice(display_gpu_id));
    nppSetStream(m_stream);

    if (latest_frame->has_detections && latest_frame->detections_ready.load(std::memory_order_acquire) && !latest_frame->detections.empty()) {
        ck(cudaMemcpyAsync(d_detections_for_drawing_,
                           latest_frame->detections.data(),
                           latest_frame->detections.size() * sizeof(pose::Object),
                           cudaMemcpyHostToDevice,
                           m_stream));

        gpu_draw_box(
            latest_frame->d_processed_image,
            latest_frame->width,
            latest_frame->height,
            d_detections_for_drawing_,
            latest_frame->detections.size(),
            m_stream);
    }

    if (camera_select->downsample > 1) {
        output_display_size_.width = latest_frame->width / camera_select->downsample;
        output_display_size_.height = latest_frame->height / camera_select->downsample;
        
        NppiSize input_size = {latest_frame->width, latest_frame->height};
        NppiRect input_roi = {0, 0, latest_frame->width, latest_frame->height};
        NppiRect output_roi = {0, 0, output_display_size_.width, output_display_size_.height};

        nppiResize_8u_C4R(latest_frame->d_processed_image, latest_frame->width * 4, input_size, input_roi,
                            d_display_resize_buffer_, output_display_size_.width * 4, output_display_size_,
                            output_roi, NPPI_INTER_SUPER);

        size_t copy_size = (size_t)output_display_size_.width * (size_t)output_display_size_.height * 4;
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, d_display_resize_buffer_, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    } else {
        size_t copy_size = (size_t)latest_frame->width * (size_t)latest_frame->height * 4;
        ck(cudaMemcpyAsync(display_buffer_pbo_cuda_ptr_, latest_frame->d_processed_image, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    }

    ck(cudaStreamSynchronize(m_stream));

    if (latest_frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_recycle_queue.push(latest_frame->original_entry);
        m_processed_recycle_queue.push(latest_frame);
    }

    return false;
}