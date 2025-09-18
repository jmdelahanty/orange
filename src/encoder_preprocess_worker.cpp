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
      encoder_pitch_(encoder_pitch),
      last_fps_update_time_(std::chrono::steady_clock::now())  // Initialize FPS timer
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));

    // Initialize GPU resources needed for color conversion
    initalize_gpu_frame(&frame_original_gpu_, camera_params_);
    initialize_gpu_debayer(&debayer_gpu_, camera_params_);

    if (camera_params_->color) {
        ck(cudaMalloc(&d_rgb_temp_, (size_t)camera_params_->width * camera_params_->height * 3));
    } else {
        size_t uv_plane_size = (size_t)encoder_pitch_ * camera_params_->height / 2;
        ck(cudaMalloc(&d_uv_default_plane_, uv_plane_size));
        ck(cudaMemset(d_uv_default_plane_, 128, uv_plane_size));
    }

    // Create a pool of buffers that will be passed to the hardware encoder
    size_t prepared_frame_size = (size_t)encoder_pitch_ * camera_params_->height * 3 / 2;
    for (int i = 0; i < ENCODER_ENTRY_POOL_SIZE; ++i) {
        ck(cudaMalloc(&encoder_entry_pool_[i].d_prepared_frame, prepared_frame_size));
        free_encoder_entries_.push(&encoder_entry_pool_[i]);
    }
    
    // --- Initialize the Event Pool ---
    event_pool_.resize(EVENT_POOL_SIZE);
    for (int i = 0; i < EVENT_POOL_SIZE; ++i) {
        ck(cudaEventCreateWithFlags(&event_pool_[i], cudaEventDisableTiming));
        free_events_.push(&event_pool_[i]);
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

    // --- Clean up the Event Pool ---
    for (auto& event : event_pool_) {
        if (event) cudaEventDestroy(event);
    }
    event_pool_.clear();
}

void EncoderPreprocessWorker::SetHwWorker(EncoderHwWorker* hw_worker)
{
    m_hw_worker_ = hw_worker;
}

bool EncoderPreprocessWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    auto start_time = std::chrono::steady_clock::now();

    // If there's no entry or we're not recording, just recycle and move on.
    if (!entry || !camera_control_->record_video) {
        if (entry && entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
             if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            m_recycle_queue_.push(entry);
        }
        return false;
    }

    // Track successful frame processing
    frame_counter_++;
    
    // FPS calculation and logging every second
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_fps_update_time_;
    // if (elapsed.count() >= 1.0) {
    //     current_fps_ = frame_counter_.load() / elapsed.count();
        
    //     std::cout << "[" << threadName << "] GPU " << camera_params_->gpu_id 
    //               << " Camera " << camera_params_->camera_serial
    //               << " | FPS: " << std::fixed << std::setprecision(1) << current_fps_
    //               << " | Queue: " << GetCountQueueInSize()
    //               << " | Free Buffers: " << available_buffers_.load()
    //               << " | Free Events: " << available_events_.load()
    //               << " | Dropped: " << frames_dropped_
    //               << " | Waits: " << resource_waits_
    //               << std::endl;
                  
    //     frame_counter_ = 0;
    //     last_fps_update_time_ = now;
    // }

    ck(cudaSetDevice(camera_params_->gpu_id));
    nppSetStream(m_stream);

    // Acquire resources for the next stage with retry logic
    ENCODER_WORKER_ENTRY* encoder_entry = nullptr;
    cudaEvent_t* event = nullptr;
    
    int retry_count = 0;
    while ((!free_encoder_entries_.pop(encoder_entry) || !free_events_.pop(event)) && retry_count < 3) {
        if (encoder_entry) {
            free_encoder_entries_.push(encoder_entry);
            available_buffers_++;
            encoder_entry = nullptr;
        }
        if (event) {
            free_events_.push(event);
            available_events_++;
            event = nullptr;
        }
        resource_waits_++;
        retry_count++;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    if (!encoder_entry || !event) {
        frames_dropped_++;
        std::cerr << "[PERF WARNING] " << threadName 
                  << ": Dropping frame - no resources after " << retry_count << " retries"
                  << " (Free buffers: ~" << available_buffers_.load() 
                  << ", Free events: ~" << available_events_.load() << ")" << std::endl;
                  
        if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            m_recycle_queue_.push(entry);
        }
        return false;
    }
    
    // Successfully acquired resources - update counters
    available_buffers_--;
    available_events_--;

    encoder_entry->preprocess_complete_event = event;

    // Wait for the raw frame data to be ready from the acquisition thread.
    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    // --- Perform the copy and color conversion ---
    if (camera_params_->color) {
        // Full Color Pipeline: RAW -> RGBA -> RGB -> NV12 (Planar YUV with interleaved UV)
        frame_original_gpu_.d_orig = entry->d_image;
        
        // 1. Debayer RAW Bayer to RGBA
        debayer_frame_gpu(camera_params_, &frame_original_gpu_, &debayer_gpu_);
        
        // 2. Convert RGBA to RGB (removes alpha channel)
        rgba2rgb_convert(d_rgb_temp_, debayer_gpu_.d_debayer, camera_params_->width, camera_params_->height, m_stream);

        // 3. Convert RGB to NV12 for the encoder using our new optimized kernel
        launch_rgb_to_nv12_kernel(d_rgb_temp_, encoder_entry->d_prepared_frame,
                                  camera_params_->width, camera_params_->height,
                                  encoder_pitch_, m_stream);

    } else {
        // Monochrome path: Copy Y plane and fill UV planes to create an NV12-compatible frame
        unsigned char* d_y_plane_dst = encoder_entry->d_prepared_frame;
        unsigned char* d_uv_plane_dst = d_y_plane_dst + ((size_t)encoder_pitch_ * camera_params_->height);

        ck(cudaMemcpy2DAsync(d_y_plane_dst, encoder_pitch_, entry->d_image, camera_params_->width, 
                             camera_params_->width, camera_params_->height, cudaMemcpyDeviceToDevice, m_stream));
        
        size_t uv_plane_size = (size_t)encoder_pitch_ * camera_params_->height / 2;
        ck(cudaMemcpyAsync(d_uv_plane_dst, d_uv_default_plane_, uv_plane_size, cudaMemcpyDeviceToDevice, m_stream));
    }
    
    // Record an event in the stream once all the above GPU work is queued.
    ck(cudaEventRecord(*encoder_entry->preprocess_complete_event, m_stream));

    // Release the main WORKER_ENTRY immediately.
    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }
        m_recycle_queue_.push(entry);
    }

    // Pass the prepared frame and its completion event to the hardware encoder.
    if (m_hw_worker_) {
        encoder_entry->recording_frame_id = entry->recording_frame_id;
        encoder_entry->timestamp = entry->timestamp;
        encoder_entry->timestamp_sys = entry->timestamp_sys;
        
        m_hw_worker_->PutObjectToQueueIn(encoder_entry);
    } else {
        // If there's no hardware worker, recycle the resources.
        free_encoder_entries_.push(encoder_entry);
        free_events_.push(event);
        available_buffers_++;
        available_events_++;
    }

    // Measure total preprocessing time
    auto preprocess_end = std::chrono::steady_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(preprocess_end - start_time).count();
    
    // Log slow frames (> 12.5ms for 80fps target)
    // if (duration_us > 12500) {
    //     std::cout << "[PERF WARNING] " << threadName 
    //               << " Camera " << camera_params_->camera_serial
    //               << " slow frame: " << duration_us << "μs"
    //               << " (target: <12500μs for 80fps)" << std::endl;
    // }

    return false; // This worker never passes items to its own output queue.
}