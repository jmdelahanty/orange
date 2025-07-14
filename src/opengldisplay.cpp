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
    CUcontext pCudaContext, // The CUDA context for this worker
    CameraParams* camera_params,
    CameraEachSelect* camera_select,
    GL_Texture* gl_texture,
    INDIGOSignalBuilder* indigo_signal_builder,
    SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
    SafeQueue<ProcessedFrame*>& processed_recycle_queue
) :
      camera_params_(camera_params),
      m_cuContext(pCudaContext),
      camera_select(camera_select),
      m_gl_texture(gl_texture),
      indigo_signal_builder_(indigo_signal_builder),
      m_stream(nullptr),
      d_detections_for_drawing_(nullptr),
      d_display_resize_buffer_(nullptr),
      h_p2p_copy_buffer_(nullptr),
      m_raw_recycle_queue(raw_recycle_queue),
      m_processed_recycle_queue(processed_recycle_queue)
{
    std::cout << "[OPENGL_DISPLAY] CONSTRUCTOR for " << camera_params_->camera_name
              << " on display GPU " << display_gpu_id << std::endl;

    ck(cuCtxPushCurrent(m_cuContext));

    ck(cudaSetDevice(display_gpu_id));
    ck(cudaStreamCreate(&m_stream));

    // Allocate buffer for detection data.
    ck(cudaMalloc(&d_detections_for_drawing_, sizeof(pose::Object) * shaman::MAX_OBJECTS));

    // Allocate buffer for downsampling.
    size_t resize_buffer_size = (size_t)camera_params_->width * camera_params_->height * 4;
    ck(cudaMalloc(&d_display_resize_buffer_, resize_buffer_size));

    // Allocate host-pinned buffer for P2P copies if needed.
    ck(cudaHostAlloc(&h_p2p_copy_buffer_, resize_buffer_size, cudaHostAllocDefault));

    std::cout << "[OPENGL_DISPLAY] Constructor completed for " << camera_params_->camera_name << std::endl;

    ck(cuCtxPopCurrent(NULL));
}

// Corrected Destructor
COpenGLDisplay::~COpenGLDisplay()
{
    std::cout << "[OPENGL_DISPLAY] DESTRUCTOR for " << (camera_params_ ? camera_params_->camera_name : "unknown") << std::endl;
    ck(cudaSetDevice(display_gpu_id));

    if (m_stream) cudaStreamDestroy(m_stream);
    if (h_p2p_copy_buffer_) cudaFreeHost(h_p2p_copy_buffer_);
    if (d_detections_for_drawing_) cudaFree(d_detections_for_drawing_);
    if (d_display_resize_buffer_) cudaFree(d_display_resize_buffer_);
}

void COpenGLDisplay::update_texture(ProcessedFrame* f)
{
    if (!f) return;

    // This will now correctly use the main thread's active context
    ck(cuCtxPushCurrent(m_cuContext));
    ck(cudaSetDevice(display_gpu_id));
    nppSetStream(m_stream);

    if (f->processed_event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *f->processed_event_ptr, 0));
    }

    unsigned char* processed_image_ptr = f->d_processed_image;

    // --- GPU-ACCELERATED DRAWING ---
    if (f->has_detections && !f->detections.empty()) {
        while (!f->detections_ready.load(std::memory_order_acquire)) {
            // Spin-wait
        }
        ck(cudaMemcpyAsync(d_detections_for_drawing_,
                           f->detections.data(),
                           f->detections.size() * sizeof(pose::Object),
                           cudaMemcpyHostToDevice,
                           m_stream));
        gpu_draw_box(
            processed_image_ptr,
            f->width,
            f->height,
            d_detections_for_drawing_,
            f->detections.size(),
            m_stream);
    }

    // 1. Map the OpenGL resource to get a safe CUDA pointer for the PBO
    ck(cudaGraphicsMapResources(1, &m_gl_texture->cuda_resource, m_stream));
    unsigned char* pbo_cuda_ptr = nullptr;
    size_t num_bytes;
    ck(cudaGraphicsResourceGetMappedPointer((void**)&pbo_cuda_ptr, &num_bytes, m_gl_texture->cuda_resource));

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
        ck(cudaMemcpyAsync(pbo_cuda_ptr, d_display_resize_buffer_, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    } else {
        size_t copy_size = (size_t)f->width * f->height * 4;
        ck(cudaMemcpyAsync(pbo_cuda_ptr, processed_image_ptr, copy_size, cudaMemcpyDeviceToDevice, m_stream));
    }

    // 2. Unmap the resource so OpenGL can now safely use it
    ck(cudaGraphicsUnmapResources(1, &m_gl_texture->cuda_resource, m_stream));

    // --- Corrected Recycling Logic ---
    if (f->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_raw_recycle_queue.push(f->original_entry);
        m_processed_recycle_queue.push(f);
    }
    ck(cuCtxPopCurrent(NULL));
}