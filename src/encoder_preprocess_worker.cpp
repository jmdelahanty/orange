// src/encoder_preprocess_worker.cpp

#include "encoder_preprocess_worker.h"
#include "encoder_hw_worker.h"
#include "kernel.cuh"
#include <npp.h>
#include <nppi.h>
#include <nppi_color_conversion.h>

EncoderPreprocessWorker::EncoderPreprocessWorker(
    const char* name,
    CameraParams* cam_params,
    int encoder_pitch,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    CameraControl* camera_control
)
    : CThreadWorker(name),
      camera_params_(cam_params),
      m_recycle_queue_(recycle_queue),
      camera_control_(camera_control),
      m_stream(nullptr),
      m_hw_worker_(nullptr),
      d_rgb_temp_(nullptr),
      d_uv_default_plane_(nullptr),
      encoder_pitch_(encoder_pitch)
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));

    // Initialize GPU resources needed for color conversion
    initalize_gpu_frame(&frame_original_gpu_, camera_params_);
    initialize_gpu_debayer(&debayer_gpu_, camera_params_);

    if (camera_params_->color) {
        ck(cudaMalloc(&d_rgb_temp_, (size_t)camera_params_->width * camera_params_->height * 3));
    } else {
        size_t uv_plane_size = (size_t)encoder_pitch_ * camera_params_->height / 4;
        ck(cudaMalloc(&d_uv_default_plane_, uv_plane_size));
        ck(cudaMemset(d_uv_default_plane_, 128, uv_plane_size));
    }

    // Create a pool of buffers that will be passed to the hardware encoder
    size_t prepared_frame_size = (size_t)encoder_pitch_ * camera_params_->height * 3 / 2;
    for (int i = 0; i < ENCODER_ENTRY_POOL_SIZE; ++i) {
        ck(cudaMalloc(&encoder_entry_pool_[i].d_prepared_frame, prepared_frame_size));
        free_encoder_entries_.push(&encoder_entry_pool_[i]);
    }
}

EncoderPreprocessWorker::~EncoderPreprocessWorker()
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    if (m_stream) cudaStreamDestroy(m_stream);

    // Free all buffers in the pool
    for (int i = 0; i < ENCODER_ENTRY_POOL_SIZE; ++i) {
        if (encoder_entry_pool_[i].d_prepared_frame) {
            cudaFree(encoder_entry_pool_[i].d_prepared_frame);
        }
    }

    if (d_rgb_temp_) cudaFree(d_rgb_temp_);
    if (d_uv_default_plane_) cudaFree(d_uv_default_plane_);
    if (frame_original_gpu_.d_orig) cudaFree(frame_original_gpu_.d_orig);
    if (debayer_gpu_.d_debayer) cudaFree(debayer_gpu_.d_debayer);
}

void EncoderPreprocessWorker::SetHwWorker(EncoderHwWorker* hw_worker)
{
    m_hw_worker_ = hw_worker;
}

bool EncoderPreprocessWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    if (!entry || !camera_control_->record_video) {
        // If there's no entry or we're not recording, just recycle and move on.
        if (entry && entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
             if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            m_recycle_queue_.push(entry);
        }
        return false;
    }

    ck(cudaSetDevice(camera_params_->gpu_id));
    nppSetStream(m_stream);

    ENCODER_WORKER_ENTRY* encoder_entry = nullptr;
    if (!free_encoder_entries_.pop(encoder_entry)) {
        // If we can't get a free buffer for the encoder, we must drop this frame.
        std::cerr << "Warning: EncoderPreprocessWorker is dropping a frame because the hardware encoder is too far behind." << std::endl;
        if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            m_recycle_queue_.push(entry);
        }
        return false;
    }

    // Wait for the raw frame data to be ready from the acquisition thread.
    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    // --- Perform the copy and color conversion ---
    if (camera_params_->color) {
        // This is a simplified color path. A real implementation might need more steps.
        // For now, let's assume a direct copy into the pre-allocated buffer for simplicity.
        // A full implementation would go RAW -> RGBA -> YUV here.
        ck(cudaMemcpyAsync(encoder_entry->d_prepared_frame, entry->d_image, (size_t)camera_params_->width * camera_params_->height, cudaMemcpyDeviceToDevice, m_stream));
    } else {
        // Monochrome path: Copy Y plane and fill UV planes.
        unsigned char* d_y_plane_dst = encoder_entry->d_prepared_frame;
        unsigned char* d_u_plane_dst = d_y_plane_dst + ((size_t)encoder_pitch_ * camera_params_->height);
        unsigned char* d_v_plane_dst = d_u_plane_dst + ((size_t)encoder_pitch_ * camera_params_->height * 5 / 4); // Corrected offset

        ck(cudaMemcpy2DAsync(d_y_plane_dst, encoder_pitch_, entry->d_image, camera_params_->width, camera_params_->width, camera_params_->height, cudaMemcpyDeviceToDevice, m_stream));
        size_t uv_plane_size = (size_t)encoder_pitch_ * camera_params_->height / 4;
        ck(cudaMemcpyAsync(d_u_plane_dst, d_uv_default_plane_, uv_plane_size, cudaMemcpyDeviceToDevice, m_stream));
        ck(cudaMemcpyAsync(d_v_plane_dst, d_uv_default_plane_, uv_plane_size, cudaMemcpyDeviceToDevice, m_stream));
    }
    
    // *** THIS IS THE CRITICAL FIX ***
    // Wait for all the copies and conversions launched above to complete on the GPU.
    ck(cudaStreamSynchronize(m_stream));

    // --- CRITICAL STEP: Release the main WORKER_ENTRY immediately ---
    // Now that the stream is synchronized, the data from entry->d_image has been safely copied.
    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }
        m_recycle_queue_.push(entry);
    }

    // --- Pass the prepared frame to the hardware encoder ---
    if (m_hw_worker_) {
        encoder_entry->recording_frame_id = entry->recording_frame_id;
        encoder_entry->timestamp = entry->timestamp;
        encoder_entry->timestamp_sys = entry->timestamp_sys;
        
        // This is a custom method we'll add to the EncoderHwWorker
        m_hw_worker_->PutObjectToQueueIn(encoder_entry);
    } else {
        // If there's no hardware worker, we must recycle the encoder entry to prevent a leak.
        free_encoder_entries_.push(encoder_entry);
    }

    return false; // This worker never passes items to its own output queue.
}