// src/encoder_preprocess_worker.cpp

#include "encoder_preprocess_worker.h"
#include "encoder_hw_worker.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include <npp.h>
#include <nppi.h>
#include <nppi_color_conversion.h>
#include <stdexcept>

#ifndef PIPELINE_PROFILE
#if defined(YOLO_PROFILE) && YOLO_PROFILE
#define PIPELINE_PROFILE 1
#else
#define PIPELINE_PROFILE 0
#endif
#endif
#if PIPELINE_PROFILE
static constexpr int kEncProfileLogEvery = 60;
#endif

namespace {
void check_npp_status(NppStatus status, const char* operation)
{
    if (status != NPP_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with NPP status " + std::to_string(status));
    }
}
} // namespace

EncoderPreprocessWorker::EncoderPreprocessWorker(
    const char* name,
    CameraParams* cam_params,
    const RecordingOutputConfig& recording_output_config,
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
      d_rgba_resize_(nullptr),
      d_uv_default_plane_(nullptr),
      recording_output_config_(recording_output_config),
      encoder_pitch_(encoder_pitch),
      output_width_(recording_output_config_.resolved_width > 0 ? recording_output_config_.resolved_width : static_cast<int>(camera_params_->width)),
      output_height_(recording_output_config_.resolved_height > 0 ? recording_output_config_.resolved_height : static_cast<int>(camera_params_->height)),
      resize_source_size_{static_cast<int>(camera_params_->width), static_cast<int>(camera_params_->height)},
      resize_source_roi_{0, 0, static_cast<int>(camera_params_->width), static_cast<int>(camera_params_->height)},
      resize_output_size_{output_width_, output_height_},
      resize_output_roi_{0, 0, output_width_, output_height_},
      last_fps_update_time_(std::chrono::steady_clock::now())  // Initialize FPS timer
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));

    // Initialize GPU resources needed for color conversion
    frame_original_gpu_.d_orig = nullptr;
    frame_original_gpu_.size_pic = static_cast<int>(camera_params_->width * camera_params_->height * sizeof(unsigned char));
    initialize_gpu_debayer(&debayer_gpu_, camera_params_);

    if (camera_params_->color) {
        if (recording_output_config_.resize_enabled) {
            ck(cudaMalloc(&d_rgba_resize_, static_cast<size_t>(output_width_) * output_height_ * 4));
        }
    } else {
        size_t uv_plane_size = static_cast<size_t>(encoder_pitch_) * output_height_ / 2;
        ck(cudaMalloc(&d_uv_default_plane_, uv_plane_size));
        ck(cudaMemset(d_uv_default_plane_, 128, uv_plane_size));
    }

    // Create a pool of buffers that will be passed to the hardware encoder
    size_t prepared_frame_size = static_cast<size_t>(encoder_pitch_) * output_height_ * 3 / 2;
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

    if (d_rgba_resize_) cudaFree(d_rgba_resize_);
    if (d_uv_default_plane_) cudaFree(d_uv_default_plane_);
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

double EncoderPreprocessWorker::get_hw_fps() const
{
    return m_hw_worker_ ? m_hw_worker_->get_fps() : 0.0;
}

uint64_t EncoderPreprocessWorker::get_hw_encode_failures() const
{
    return m_hw_worker_ ? m_hw_worker_->get_encode_failures() : 0;
}

uint64_t EncoderPreprocessWorker::get_hw_slow_frames() const
{
    return m_hw_worker_ ? m_hw_worker_->get_slow_frames() : 0;
}

int EncoderPreprocessWorker::get_hw_queue_depth() const
{
    return m_hw_worker_ ? m_hw_worker_->get_queue_depth() : -1;
}

bool EncoderPreprocessWorker::IsDrained()
{
    return (in_flight_.load(std::memory_order_relaxed) == 0) &&
           (GetCountQueueInSize() == 0);
}

bool EncoderPreprocessWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    auto start_time = std::chrono::steady_clock::now();

    if (!entry) {
        return false;
    }

    const bool recording_enabled = camera_control_->record_video;
    const bool draining = camera_control_->recording_draining;

    // If we're not recording and not draining, just recycle and move on.
    if (!recording_enabled && !draining) {
        if (entry && entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
             if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            m_recycle_queue_.push(entry);
        }
        return false;
    }

    in_flight_.fetch_add(1, std::memory_order_relaxed);

    // Track successful frame processing
    frame_counter_.fetch_add(1, std::memory_order_relaxed);
    
    // FPS calculation and logging every second
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_fps_update_time_;
    if (elapsed.count() >= 1.0) {
        const int frames = frame_counter_.exchange(0, std::memory_order_relaxed);
        current_fps_.store(frames / elapsed.count(), std::memory_order_relaxed);
        last_fps_update_time_ = now;
    }

    ck(cudaSetDevice(camera_params_->gpu_id));
    EnsureNppStream(m_stream);

#if PIPELINE_PROFILE
    struct EncProfileEvents {
        cudaEvent_t start{};
        cudaEvent_t end{};
        int device = -1;
        bool initialized = false;

        void Init(int gpu_id) {
            if (initialized) {
                return;
            }
            device = gpu_id;
            ck(cudaSetDevice(device));
            ck(cudaEventCreate(&start));
            ck(cudaEventCreate(&end));
            initialized = true;
        }

        ~EncProfileEvents() {
            if (!initialized) {
                return;
            }
            if (device >= 0) {
                cudaSetDevice(device);
            }
            cudaEventDestroy(start);
            cudaEventDestroy(end);
        }
    };

    static thread_local EncProfileEvents enc_prof_events;
    static thread_local bool enc_prof_inflight = false;
    static thread_local int enc_prof_count = 0;
    enc_prof_events.Init(camera_params_->gpu_id);
    if (enc_prof_inflight) {
        cudaError_t enc_status = cudaEventQuery(enc_prof_events.end);
        if (enc_status == cudaSuccess) {
            float enc_ms = 0.0f;
            ck(cudaEventElapsedTime(&enc_ms, enc_prof_events.start, enc_prof_events.end));
            std::cout << "[ENC_PRE_TIME] Cam " << camera_params_->camera_serial
                      << " GPU " << camera_params_->gpu_id
                      << " ms=" << enc_ms
                      << " q=" << GetCountQueueInSize()
                      << std::endl;
            enc_prof_inflight = false;
        } else if (enc_status != cudaErrorNotReady) {
            std::cerr << "[ENC_PRE_TIME] Cam " << camera_params_->camera_serial
                      << " event query failed: " << cudaGetErrorString(enc_status)
                      << std::endl;
            enc_prof_inflight = false;
        }
    }
#endif

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

        if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            m_recycle_queue_.push(entry);
        }
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    
    // Successfully acquired resources - update counters
    available_buffers_--;
    available_events_--;
    encoder_entry->preprocess_complete_event = event;

#if PIPELINE_PROFILE
    bool sample_preprocess = false;
    if (!enc_prof_inflight) {
        enc_prof_count++;
        if (enc_prof_count % kEncProfileLogEvery == 0) {
            sample_preprocess = true;
        ck(cudaEventRecord(enc_prof_events.start, m_stream));
        }
    }
#endif

    // Wait for the raw frame data to be ready from the acquisition thread.
    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    // --- Perform the copy and color conversion ---
    if (camera_params_->color) {
        // Full Color Pipeline: RAW -> RGBA -> NV12
        frame_original_gpu_.d_orig = entry->d_image;
        
        // 1. Debayer RAW Bayer to RGBA
        debayer_frame_gpu(camera_params_, &frame_original_gpu_, &debayer_gpu_);

        unsigned char* rgba_source = debayer_gpu_.d_debayer;
        if (recording_output_config_.resize_enabled) {
            check_npp_status(
                nppiResize_8u_C4R(
                    debayer_gpu_.d_debayer,
                    static_cast<int>(camera_params_->width) * 4,
                    resize_source_size_,
                    resize_source_roi_,
                    d_rgba_resize_,
                    output_width_ * 4,
                    resize_output_size_,
                    resize_output_roi_,
                    NPPI_INTER_SUPER),
                "nppiResize_8u_C4R");
            rgba_source = d_rgba_resize_;
        }

        // 2. Convert RGBA directly to NV12 for the encoder.
        launch_rgba_to_nv12_kernel(
            rgba_source,
            encoder_entry->d_prepared_frame,
            output_width_,
            output_height_,
            encoder_pitch_,
            m_stream);

    } else {
        // Monochrome path: Copy Y plane and fill UV planes to create an NV12-compatible frame
        unsigned char* d_y_plane_dst = encoder_entry->d_prepared_frame;
        unsigned char* d_uv_plane_dst = d_y_plane_dst + (static_cast<size_t>(encoder_pitch_) * output_height_);

        if (recording_output_config_.resize_enabled) {
            check_npp_status(
                nppiResize_8u_C1R(
                    entry->d_image,
                    static_cast<int>(camera_params_->width),
                    resize_source_size_,
                    resize_source_roi_,
                    d_y_plane_dst,
                    encoder_pitch_,
                    resize_output_size_,
                    resize_output_roi_,
                    NPPI_INTER_SUPER),
                "nppiResize_8u_C1R");
        } else {
            ck(cudaMemcpy2DAsync(
                d_y_plane_dst,
                encoder_pitch_,
                entry->d_image,
                camera_params_->width,
                output_width_,
                output_height_,
                cudaMemcpyDeviceToDevice,
                m_stream));
        }

        size_t uv_plane_size = static_cast<size_t>(encoder_pitch_) * output_height_ / 2;
        ck(cudaMemcpyAsync(d_uv_plane_dst, d_uv_default_plane_, uv_plane_size, cudaMemcpyDeviceToDevice, m_stream));
    }
    
    // Record an event in the stream once all the above GPU work is queued.
    ck(cudaEventRecord(*encoder_entry->preprocess_complete_event, m_stream));
#if PIPELINE_PROFILE
    if (sample_preprocess) {
        ck(cudaEventRecord(enc_prof_events.end, m_stream));
        enc_prof_inflight = true;
    }
#endif

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
    in_flight_.fetch_sub(1, std::memory_order_relaxed);

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
