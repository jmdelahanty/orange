// src/crop_and_encode_worker.cpp

#include "crop_and_encode_worker.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include "project.h" // Add this include
#include "fsuid_guard.h"
#include <nppi.h>
#include <npp.h>
#include <nppi_color_conversion.h>
#include <nppi_geometry_transforms.h>
#include <algorithm> // For std::max_element
#include <cctype>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
constexpr int kDisplayGpuId = 0;
constexpr int kCropSourceReleaseEventPoolSize = 256;
constexpr int kCropFramePoolSize = 8;

uint64_t steady_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

double elapsed_ms(uint64_t start_ns, uint64_t end_ns)
{
    if (end_ns < start_ns) {
        return 0.0;
    }
    return static_cast<double>(end_ns - start_ns) / 1000000.0;
}

size_t encoded_packet_bytes(const std::vector<std::vector<uint8_t>>& packets)
{
    size_t total = 0;
    for (const auto& packet : packets) {
        total += packet.size();
    }
    return total;
}

bool env_flag_enabled(const char* name, bool default_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return default_value;
    }

    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized != "0" &&
           normalized != "false" &&
           normalized != "off" &&
           normalized != "no";
}
}

int CropAndEncodeWorker::SanitizeCropSize(int requested_size_px)
{
    return sanitize_camera_crop_size_px(requested_size_px);
}

CropAndEncodeWorker::CropAndEncodeWorker(
    const char* name,
    CameraParams* camera_params,
    const std::string& folder_name,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    unsigned char* display_buffer_pbo,
    CameraControl* camera_control,
    int crop_size_px
):CThreadWorker(name),
camera_params_(camera_params),
base_folder_name_(folder_name),
crop_width_(SanitizeCropSize(crop_size_px)),
crop_height_(SanitizeCropSize(crop_size_px)),
m_recycle_queue(recycle_queue),
d_display_buffer_pbo_(display_buffer_pbo),
camera_control_(camera_control),
d_cropped_rgba_(nullptr)
{

    std::cout << "[CropAndEncodeWorker] Initializing " << name << " on GPU " << camera_params_->gpu_id << std::endl;

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreateWithFlags(&m_stream, cudaStreamNonBlocking));
    ck(cudaStreamCreateWithFlags(&m_crop_producer_stream, cudaStreamNonBlocking));
    crop_copy_timing_enabled_ = env_flag_enabled("ORANGE_CROP_COPY_TIMING", true);
    crop_copy_kernel_enabled_ = env_flag_enabled("ORANGE_CROP_COPY_KERNEL", false);
    crop_source_stage_enabled_ = env_flag_enabled("ORANGE_CROP_STAGE_SOURCE", false);
    std::cout << "[CropAndEncodeWorker] Crop copy mode "
              << (crop_copy_kernel_enabled_ ? "kernel" : "memcpy2d")
              << ", source stage "
              << (crop_source_stage_enabled_ ? "enabled" : "disabled")
              << ", GPU timing "
              << (crop_copy_timing_enabled_ ? "enabled" : "disabled")
              << " for " << threadName << std::endl;

    source_release_event_pool_.resize(kCropSourceReleaseEventPoolSize);
    for (int i = 0; i < kCropSourceReleaseEventPoolSize; ++i) {
        ck(cudaEventCreateWithFlags(&source_release_event_pool_[i], cudaEventDisableTiming));
        free_source_release_events_.push(&source_release_event_pool_[i]);
    }

    crop_frame_pool_.resize(kCropFramePoolSize);
    for (auto& crop_frame : crop_frame_pool_) {
        ck(cudaMalloc(&crop_frame.d_crop_mono, crop_mono_bytes()));
        if (crop_copy_timing_enabled_) {
            ck(cudaEventCreate(&crop_frame.crop_copy_start_event));
            ck(cudaEventCreate(&crop_frame.crop_copy_stop_event));
        }
        ck(cudaEventCreateWithFlags(&crop_frame.crop_ready_event, cudaEventDisableTiming));
        ck(cudaEventCreateWithFlags(&crop_frame.recycle_event, cudaEventDisableTiming));
        free_crop_frames_.push(&crop_frame);
    }

    // Transitional crop buffer: this will become the shared crop payload for pose.
    ck(cudaMalloc(&d_cropped_rgba_, crop_preview_bytes()));

    if (d_display_buffer_pbo_ && camera_params_->gpu_id != kDisplayGpuId) {
        // OpenGL PBOs are mapped on the display GPU. For cross-GPU cameras,
        // stage only the small preview crop through host memory.
        ck(cudaHostAlloc(&h_display_crop_, crop_preview_bytes(), cudaHostAllocDefault));
        ck(cudaSetDevice(kDisplayGpuId));
        ck(cudaStreamCreateWithFlags(&m_display_stream, cudaStreamNonBlocking));
        ck(cudaSetDevice(camera_params_->gpu_id));
    }

    try {
        CUcontext cuContext;
        ck(cuCtxGetCurrent(&cuContext));

        encoder_ = new NvEncoderCuda(cuContext, crop_width_, crop_height_, NV_ENC_BUFFER_FORMAT_NV12);

        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
        initializeParams.encodeConfig = &encodeConfig;

        GUID codecGuid = NV_ENC_CODEC_HEVC_GUID;
        GUID presetGuid = NV_ENC_PRESET_P7_GUID;
        NV_ENC_TUNING_INFO tuningInfo = NV_ENC_TUNING_INFO_LOSSLESS;

        std::cout << "[CropAndEncodeWorker] CONFIGURING FOR LOSSLESS (HEVC), HIGH-QUALITY RECORDING." << std::endl;

        encoder_->CreateDefaultEncoderParams(&initializeParams, codecGuid, presetGuid, tuningInfo);

        encodeConfig.gopLength = 1; // Keyframe every frame for lossless
        encodeConfig.frameIntervalP = 1;
        initializeParams.frameRateNum = camera_params_->frame_rate;
        initializeParams.frameRateDen = 1;
        initializeParams.enablePTD = 1;
        encodeConfig.rcParams.constQP = { 0, 0, 0 };

        encoder_->CreateEncoder(&initializeParams);
        encoder_->SetIOCudaStreams((NV_ENC_CUSTREAM_PTR)&m_stream, (NV_ENC_CUSTREAM_PTR)&m_stream);
        
        const NvEncInputFrame *tempFrame = encoder_->GetNextInputFrame();
        encoder_pitch_ = tempFrame->pitch;

        // Allocate and initialize the blank frame buffer
        const size_t encoder_buffer_size = static_cast<size_t>(encoder_pitch_) * crop_height_ * 3 / 2;
        ck(cudaMalloc(&d_blank_frame_, encoder_buffer_size));

        // --- Correct YUV Initialization for a Black Frame ---
        // 1. Set the Y (luma) plane to 0 for black.
        size_t luma_size = static_cast<size_t>(encoder_pitch_) * crop_height_;
        ck(cudaMemsetAsync(d_blank_frame_, 0, luma_size, m_stream));

        // 2. Set the UV (chroma) plane to 128 for neutral color.
        size_t chroma_size = static_cast<size_t>(encoder_pitch_) * crop_height_ / 2;
        unsigned char* d_uv_plane = d_blank_frame_ + luma_size;
        ck(cudaMemsetAsync(d_uv_plane, 128, chroma_size, m_stream));

    } catch (const std::exception& e) {
        std::cerr << "[CropAndEncodeWorker] Failed to initialize encoder: " << e.what() << std::endl;
        if (encoder_) {
            delete encoder_;
            encoder_ = nullptr;
        }
        throw;
    }
}

CropAndEncodeWorker::~CropAndEncodeWorker() {
    std::cout << "[CropAndEncodeWorker] Destructor for " << threadName << std::endl;

    if (camera_params_) {
        ck(cudaSetDevice(camera_params_->gpu_id));
    }

    drain_pending_source_releases(true);
    drain_pending_crop_frames(true);

    if (is_recording_) {
        finalize_recording();
    } else {
        // Always flush and close the writer and encoder.
        flush_and_close();
    }

    // Explicitly delete the encoder to release its resources.
    if (encoder_) {
        delete encoder_;
        encoder_ = nullptr;
    }

    if (d_cropped_rgba_) {
        cudaFree(d_cropped_rgba_);
        d_cropped_rgba_ = nullptr;
    }
    if (d_source_stage_mono_) {
        cudaFree(d_source_stage_mono_);
        d_source_stage_mono_ = nullptr;
    }
    if (d_blank_frame_) {
        cudaFree(d_blank_frame_);
        d_blank_frame_ = nullptr;
    }
    if (h_display_crop_) {
        cudaFreeHost(h_display_crop_);
        h_display_crop_ = nullptr;
    }
    for (auto& event : source_release_event_pool_) {
        if (event) {
            cudaEventDestroy(event);
        }
    }
    source_release_event_pool_.clear();
    for (auto& crop_frame : crop_frame_pool_) {
        if (crop_frame.d_crop_mono) {
            cudaFree(crop_frame.d_crop_mono);
            crop_frame.d_crop_mono = nullptr;
        }
        if (crop_frame.crop_copy_start_event) {
            cudaEventDestroy(crop_frame.crop_copy_start_event);
            crop_frame.crop_copy_start_event = nullptr;
        }
        if (crop_frame.crop_copy_stop_event) {
            cudaEventDestroy(crop_frame.crop_copy_stop_event);
            crop_frame.crop_copy_stop_event = nullptr;
        }
        if (crop_frame.crop_ready_event) {
            cudaEventDestroy(crop_frame.crop_ready_event);
            crop_frame.crop_ready_event = nullptr;
        }
        if (crop_frame.recycle_event) {
            cudaEventDestroy(crop_frame.recycle_event);
            crop_frame.recycle_event = nullptr;
        }
    }
    crop_frame_pool_.clear();
    if (m_stream) {
        cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
    if (m_crop_producer_stream) {
        cudaStreamDestroy(m_crop_producer_stream);
        m_crop_producer_stream = nullptr;
    }
    if (m_display_stream) {
        cudaSetDevice(kDisplayGpuId);
        cudaStreamDestroy(m_display_stream);
        m_display_stream = nullptr;
    }
}

bool CropAndEncodeWorker::ensure_recording_started(const std::string& recording_folder) {
    if (is_recording_) {
        return true;
    }

    if (recording_folder.empty()) {
        std::cerr << "[CropAndEncodeWorker] Refusing to start crop recording for "
                  << threadName
                  << ": frame has no recording folder." << std::endl;
        return false;
    }

    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        make_folder(recording_folder);

        writer_.video_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop.mp4";
        writer_.keyframe_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_keyframe.json";
        writer_.metadata_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_meta.csv";
        crop_perf_file_ = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_perf.csv";

        writer_.video = new FFmpegWriter(
            AV_CODEC_ID_HEVC,
            crop_width_,
            crop_height_,
            camera_params_->frame_rate,
            writer_.video_file.c_str(),
            writer_.keyframe_file.c_str());
        writer_.video->create_thread();

        writer_.metadata = new std::ofstream();
        writer_.metadata->open(writer_.metadata_file.c_str());
        if (!(*writer_.metadata)) {
            std::cout << "[CropAndEncodeWorker] Warning: Could not open metadata file!" << std::endl;
        } else {
            *writer_.metadata
                << "recording_frame_id,local_frame_id,camera_frame_id,timestamp,timestamp_sys,"
                << "has_detection,blank_frame,detection_confidence,"
                << "crop_x,crop_y,crop_w,crop_h,"
                << "detection_x,detection_y,detection_w,detection_h\n";
        }

        crop_perf_.open(crop_perf_file_.c_str());
        if (!crop_perf_) {
            std::cout << "[CropAndEncodeWorker] Warning: Could not open crop perf file!" << std::endl;
        } else {
            crop_perf_
                << "recording_frame_id,local_frame_id,camera_frame_id,"
                << "worker_start_steady_ns,queue_depth_start,encode_active,"
                << "has_detection,blank_frame,dropped,drop_reason,"
                << "crop_x,crop_y,crop_w,crop_h,"
                << "packet_count,encoded_bytes,"
                << "event_wait_cpu_ms,crop_pool_wait_ms,crop_producer_cpu_ms,"
                << "crop_source_wait_enqueue_cpu_ms,source_stage_enqueue_cpu_ms,"
                << "crop_copy_start_event_record_cpu_ms,"
                << "crop_roi_copy_enqueue_cpu_ms,"
                << "crop_ready_event_record_cpu_ms,source_release_event_record_cpu_ms,"
                << "crop_copy_gpu_ms,"
                << "crop_preview_cpu_ms,encode_submit_cpu_ms,"
                << "metadata_cpu_ms,stream_sync_ms,display_sync_ms,total_ms\n";
        }
    }

    last_frame_id_used_ = 0;
    encoder_flushed_ = false;
    camera_control_->active_recorders.fetch_add(1, std::memory_order_relaxed);
    is_recording_ = true;
    return true;
}

void CropAndEncodeWorker::push_encoded_packets(
    std::vector<std::vector<uint8_t>>& packets,
    const std::vector<uint64_t>& output_timestamps,
    uint64_t fallback_zero_based_frame)
{
    if (!writer_.video) {
        if (!packets.empty()) {
            std::cerr << "[CropAndEncodeWorker] Warning: dropping "
                      << packets.size()
                      << " encoded packets because crop writer is not open." << std::endl;
        }
        return;
    }

    for (size_t i = 0; i < packets.size(); ++i) {
        const int64_t sample_index = i < output_timestamps.size()
            ? static_cast<int64_t>(output_timestamps[i])
            : static_cast<int64_t>(fallback_zero_based_frame);
        writer_.video->push_packet(
            packets[i].data(),
            static_cast<int>(packets[i].size()),
            sample_index);
        last_frame_id_used_ = std::max<uint64_t>(
            last_frame_id_used_,
            static_cast<uint64_t>(sample_index + 1));
    }
}

void CropAndEncodeWorker::write_metadata_row(const CropFrameSnapshot& frame)
{
    if (!writer_.metadata || !writer_.metadata->is_open()) {
        return;
    }

    *writer_.metadata << frame.recording_frame_id << ','
                      << frame.local_frame_id << ','
                      << frame.camera_frame_id << ','
                      << frame.timestamp << ','
                      << frame.timestamp_sys << ','
                      << (frame.has_detection ? 1 : 0) << ','
                      << (frame.blank_frame ? 1 : 0) << ','
                      << frame.detection_confidence << ','
                      << frame.crop_x << ','
                      << frame.crop_y << ','
                      << frame.crop_w << ','
                      << frame.crop_h << ','
                      << frame.detection_x << ','
                      << frame.detection_y << ','
                      << frame.detection_w << ','
                      << frame.detection_h << '\n';
}

void CropAndEncodeWorker::write_perf_row(const CropFrameSnapshot& frame, const CropPerfSample& sample)
{
    if (!crop_perf_.is_open()) {
        return;
    }

    crop_perf_ << frame.recording_frame_id << ','
               << frame.local_frame_id << ','
               << frame.camera_frame_id << ','
               << sample.worker_start_steady_ns << ','
               << sample.queue_depth_start << ','
               << (sample.encode_active ? 1 : 0) << ','
               << (sample.has_detection ? 1 : 0) << ','
               << (sample.blank_frame ? 1 : 0) << ','
               << (sample.dropped ? 1 : 0) << ','
               << sample.drop_reason << ','
               << sample.crop_x << ','
               << sample.crop_y << ','
               << sample.crop_w << ','
               << sample.crop_h << ','
               << sample.packet_count << ','
               << sample.encoded_bytes << ','
               << sample.event_wait_cpu_ms << ','
               << sample.crop_pool_wait_ms << ','
               << sample.crop_producer_cpu_ms << ','
               << sample.crop_source_wait_enqueue_cpu_ms << ','
               << sample.source_stage_enqueue_cpu_ms << ','
               << sample.crop_copy_start_event_record_cpu_ms << ','
               << sample.crop_roi_copy_enqueue_cpu_ms << ','
               << sample.crop_ready_event_record_cpu_ms << ','
               << sample.source_release_event_record_cpu_ms << ','
               << sample.crop_copy_gpu_ms << ','
               << sample.crop_preview_cpu_ms << ','
               << sample.encode_submit_cpu_ms << ','
               << sample.metadata_cpu_ms << ','
               << sample.stream_sync_ms << ','
               << sample.display_sync_ms << ','
               << sample.total_ms << '\n';
}

void CropAndEncodeWorker::release_entry(WORKER_ENTRY* entry) {
    if (!entry) {
        return;
    }

    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }
        m_recycle_queue.push(entry);
    }
}

cudaEvent_t* CropAndEncodeWorker::acquire_source_release_event()
{
    cudaEvent_t* event = nullptr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        drain_pending_source_releases(false);
        if (free_source_release_events_.pop(event)) {
            return event;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    source_release_event_misses_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[CropAndEncodeWorker] No source-release CUDA event available for "
              << threadName
              << "; falling back to source-stream synchronization." << std::endl;
    return nullptr;
}

void CropAndEncodeWorker::defer_source_release(WORKER_ENTRY* entry, cudaEvent_t* event)
{
    if (!entry || !event) {
        if (event) {
            free_source_release_events_.push(event);
        }
        release_entry(entry);
        return;
    }

    pending_source_releases_.push_back({entry, event});
    pending_source_release_count_.store(
        static_cast<int>(pending_source_releases_.size()),
        std::memory_order_relaxed);
}

void CropAndEncodeWorker::drain_pending_source_releases(bool synchronize_all)
{
    for (auto it = pending_source_releases_.begin();
         it != pending_source_releases_.end(); ) {
        cudaError_t status = synchronize_all
            ? cudaEventSynchronize(*it->event)
            : cudaEventQuery(*it->event);

        if (status == cudaErrorNotReady) {
            ++it;
            continue;
        }

        if (status != cudaSuccess) {
            std::cerr << "[CropAndEncodeWorker] Source-release event wait/query failed for "
                      << threadName
                      << ": " << cudaGetErrorString(status) << std::endl;
            cudaGetLastError();
        }

        release_entry(it->entry);
        free_source_release_events_.push(it->event);
        it = pending_source_releases_.erase(it);
        pending_source_release_count_.store(
            static_cast<int>(pending_source_releases_.size()),
            std::memory_order_relaxed);
    }
}

CropAndEncodeWorker::CropFrame* CropAndEncodeWorker::acquire_crop_frame()
{
    CropFrame* crop_frame = nullptr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        drain_pending_crop_frames(false);
        if (free_crop_frames_.pop(crop_frame)) {
            crop_frame->frame = CropFrameSnapshot{};
            return crop_frame;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    crop_frame_pool_misses_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[CropAndEncodeWorker] No free CropFrame available for "
              << threadName
              << "; dropping crop output for this frame." << std::endl;
    return nullptr;
}

void CropAndEncodeWorker::recycle_crop_frame(CropFrame* crop_frame)
{
    if (!crop_frame) {
        return;
    }
    crop_frame->frame = CropFrameSnapshot{};
    free_crop_frames_.push(crop_frame);
}

void CropAndEncodeWorker::defer_crop_frame_recycle(CropFrame* crop_frame)
{
    if (!crop_frame) {
        return;
    }
    pending_crop_frame_recycles_.push_back({crop_frame});
    pending_crop_frame_recycle_count_.store(
        static_cast<int>(pending_crop_frame_recycles_.size()),
        std::memory_order_relaxed);
}

void CropAndEncodeWorker::drain_pending_crop_frames(bool synchronize_all)
{
    for (auto it = pending_crop_frame_recycles_.begin();
         it != pending_crop_frame_recycles_.end(); ) {
        CropFrame* crop_frame = it->crop_frame;
        if (!crop_frame || !crop_frame->recycle_event) {
            it = pending_crop_frame_recycles_.erase(it);
            continue;
        }

        cudaError_t status = synchronize_all
            ? cudaEventSynchronize(crop_frame->recycle_event)
            : cudaEventQuery(crop_frame->recycle_event);

        if (status == cudaErrorNotReady) {
            ++it;
            continue;
        }

        if (status != cudaSuccess) {
            std::cerr << "[CropAndEncodeWorker] CropFrame recycle event wait/query failed for "
                      << threadName
                      << ": " << cudaGetErrorString(status) << std::endl;
            cudaGetLastError();
        }

        recycle_crop_frame(crop_frame);
        it = pending_crop_frame_recycles_.erase(it);
        pending_crop_frame_recycle_count_.store(
            static_cast<int>(pending_crop_frame_recycles_.size()),
            std::memory_order_relaxed);
    }

    pending_crop_frame_recycle_count_.store(
        static_cast<int>(pending_crop_frame_recycles_.size()),
        std::memory_order_relaxed);
}

size_t CropAndEncodeWorker::crop_preview_bytes() const
{
    return static_cast<size_t>(crop_width_) * static_cast<size_t>(crop_height_) * 4;
}

size_t CropAndEncodeWorker::crop_mono_bytes() const
{
    return static_cast<size_t>(crop_width_) * static_cast<size_t>(crop_height_);
}

void CropAndEncodeWorker::ensure_source_stage_buffer(int width, int height)
{
    if (!crop_source_stage_enabled_) {
        return;
    }

    if (d_source_stage_mono_ &&
        source_stage_width_ == width &&
        source_stage_height_ == height) {
        return;
    }

    if (d_source_stage_mono_) {
        ck(cudaFree(d_source_stage_mono_));
        d_source_stage_mono_ = nullptr;
    }

    ck(cudaMalloc(&d_source_stage_mono_, static_cast<size_t>(width) * static_cast<size_t>(height)));
    source_stage_width_ = width;
    source_stage_height_ = height;
}

bool CropAndEncodeWorker::display_cuda_ok(cudaError_t status, const char* operation)
{
    if (status == cudaSuccess) {
        return true;
    }

    if (!display_preview_disabled_) {
        std::cerr << "[CropAndEncodeWorker] Disabling crop preview for "
                  << threadName << ": " << operation << " failed: "
                  << cudaGetErrorString(status) << std::endl;
    }

    display_preview_disabled_ = true;
    d_display_buffer_pbo_ = nullptr;
    if (camera_params_) {
        cudaSetDevice(camera_params_->gpu_id);
    }
    return false;
}

void CropAndEncodeWorker::copy_crop_to_display_preview()
{
    if (!d_display_buffer_pbo_ || display_preview_disabled_) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        display_cuda_ok(
            cudaMemcpyAsync(
                d_display_buffer_pbo_,
                d_cropped_rgba_,
                crop_preview_bytes(),
                cudaMemcpyDeviceToDevice,
                m_stream),
            "cudaMemcpyAsync(crop preview same GPU)");
        return;
    }

    if (!h_display_crop_ || !m_display_stream) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview cross-GPU staging");
        return;
    }

    if (!display_cuda_ok(
            cudaMemcpyAsync(
                h_display_crop_,
                d_cropped_rgba_,
                crop_preview_bytes(),
                cudaMemcpyDeviceToHost,
                m_stream),
            "cudaMemcpyAsync(crop preview device-to-host)")) {
        return;
    }
    if (!display_cuda_ok(cudaStreamSynchronize(m_stream), "cudaStreamSynchronize(crop preview camera stream)")) {
        return;
    }
    if (!display_cuda_ok(cudaSetDevice(kDisplayGpuId), "cudaSetDevice(crop preview display GPU)")) {
        return;
    }
    display_cuda_ok(
        cudaMemcpyAsync(
            d_display_buffer_pbo_,
            h_display_crop_,
            crop_preview_bytes(),
            cudaMemcpyHostToDevice,
            m_display_stream),
        "cudaMemcpyAsync(crop preview host-to-display)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropAndEncodeWorker::clear_display_preview()
{
    if (!d_display_buffer_pbo_ || display_preview_disabled_) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        display_cuda_ok(
            cudaMemsetAsync(d_display_buffer_pbo_, 0, crop_preview_bytes(), m_stream),
            "cudaMemsetAsync(crop preview same GPU)");
        return;
    }

    if (!m_display_stream) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview display stream");
        return;
    }

    if (!display_cuda_ok(cudaSetDevice(kDisplayGpuId), "cudaSetDevice(crop preview clear display GPU)")) {
        return;
    }
    display_cuda_ok(
        cudaMemsetAsync(d_display_buffer_pbo_, 0, crop_preview_bytes(), m_display_stream),
        "cudaMemsetAsync(crop preview display GPU)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropAndEncodeWorker::synchronize_display_preview()
{
    if (!d_display_buffer_pbo_ || display_preview_disabled_) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        display_cuda_ok(cudaStreamSynchronize(m_stream), "cudaStreamSynchronize(crop preview same GPU)");
        return;
    }

    if (!m_display_stream) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview display stream synchronize");
        return;
    }

    if (!display_cuda_ok(cudaSetDevice(kDisplayGpuId), "cudaSetDevice(crop preview synchronize display GPU)")) {
        return;
    }
    display_cuda_ok(cudaStreamSynchronize(m_display_stream), "cudaStreamSynchronize(crop preview display stream)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropAndEncodeWorker::flush_and_close() {
    std::cout << "[CropAndEncodeWorker] Flushing and closing for " << threadName << std::endl;

    if (encoder_ && writer_.video && !encoder_flushed_) {
        std::vector<std::vector<uint8_t>> packets;
        std::vector<uint64_t> output_timestamps;
        encoder_->EndEncode(packets, nullptr, &output_timestamps);
        push_encoded_packets(packets, output_timestamps, last_frame_id_used_);
        encoder_flushed_ = true;
        std::cout << "[CropAndEncodeWorker] Encoder flushed." << std::endl;
    }

    if (writer_.video) {
        std::cout << "[CropAndEncodeWorker] Closing video writer for " << threadName
                  << " queued_packets=" << writer_.video->queued_packets()
                  << " queued_bytes=" << writer_.video->queued_bytes()
                  << std::endl;
        writer_.video->quit_thread();
        std::cout << "[CropAndEncodeWorker] Video writer quit signal sent." << std::endl;
        writer_.video->join_thread();
        std::cout << "[CropAndEncodeWorker] Video writer thread joined." << std::endl;
        delete writer_.video;
        writer_.video = nullptr;
        std::cout << "[CropAndEncodeWorker] Video writer closed." << std::endl;
    }
    
    if (writer_.metadata) {
        if (writer_.metadata->is_open()) {
            writer_.metadata->close();
        }
        delete writer_.metadata;
        writer_.metadata = nullptr;
    }

    if (crop_perf_.is_open()) {
        crop_perf_.close();
    }
}

void CropAndEncodeWorker::finalize_recording()
{
    if (!is_recording_) {
        return;
    }

    flush_and_close();
    is_recording_ = false;

    if (camera_control_) {
        int remaining = camera_control_->active_recorders.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (remaining == 0) {
            if (camera_control_->recording_draining) {
                camera_control_->recording_draining = false;
            }
            camera_control_->stop_record = false;
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            camera_control_->recording_folder.clear();
        }
    }
}

bool CropAndEncodeWorker::drain_ready()
{
    drain_pending_source_releases(false);
    drain_pending_crop_frames(false);
    return (GetCountQueueInSize() == 0) &&
           (pending_source_release_count_.load(std::memory_order_relaxed) == 0) &&
           (pending_crop_frame_recycle_count_.load(std::memory_order_relaxed) == 0);
}


bool CropAndEncodeWorker::WorkerFunction(WORKER_ENTRY* entry) {
    // Set the correct CUDA device for this worker's operations.
    ck(cudaSetDevice(camera_params_->gpu_id));
    EnsureNppStream(m_stream);
    drain_pending_source_releases(false);
    drain_pending_crop_frames(false);

    if (!entry) {
        return false;
    }

    CropFrameSnapshot frame;
    frame.recording_frame_id = entry->recording_frame_id;
    frame.local_frame_id = entry->frame_id;
    frame.camera_frame_id = entry->camera_frame_id;
    frame.timestamp = entry->timestamp;
    frame.timestamp_sys = entry->timestamp_sys;
    frame.source_width = entry->width;
    frame.source_height = entry->height;

    CropPerfSample perf;
    perf.worker_start_steady_ns = steady_now_ns();
    perf.queue_depth_start = GetCountQueueInSize();

    bool source_entry_released = false;
    bool source_work_queued = false;
    cudaStream_t source_release_stream = m_stream;
    auto release_source_now = [&]() {
        if (!source_entry_released && entry) {
            release_entry(entry);
            source_entry_released = true;
            entry = nullptr;
        }
    };

    auto defer_source_after_stream_work = [&](cudaStream_t stream) {
        if (source_entry_released || !entry) {
            return;
        }

        cudaEvent_t* source_release_event = acquire_source_release_event();
        if (source_release_event) {
            const uint64_t source_release_event_record_start_ns = steady_now_ns();
            ck(cudaEventRecord(*source_release_event, stream));
            perf.source_release_event_record_cpu_ms += elapsed_ms(
                source_release_event_record_start_ns,
                steady_now_ns());
            defer_source_release(entry, source_release_event);
            source_entry_released = true;
            entry = nullptr;
            return;
        }

        // Event pool exhaustion is unexpected. Keep correctness by waiting only
        // for the stream that last touched the source, then release the entry.
        const uint64_t stream_sync_start_ns = steady_now_ns();
        ck(cudaStreamSynchronize(stream));
        perf.stream_sync_ms += elapsed_ms(stream_sync_start_ns, steady_now_ns());
        release_source_now();
    };

    CropFrame* active_crop_frame = nullptr;
    CropFrame* timing_crop_frame = nullptr;
    bool crop_frame_work_queued = false;
    auto recycle_active_crop_frame_now = [&]() {
        if (active_crop_frame) {
            recycle_crop_frame(active_crop_frame);
            active_crop_frame = nullptr;
            crop_frame_work_queued = false;
        }
    };

    auto defer_active_crop_frame_after_stream_work = [&]() {
        if (!active_crop_frame) {
            return;
        }

        ck(cudaEventRecord(active_crop_frame->recycle_event, m_stream));
        defer_crop_frame_recycle(active_crop_frame);
        active_crop_frame = nullptr;
        crop_frame_work_queued = false;
    };

    // YOLO reaches this worker after acquisition, so global record_video can
    // change before a frame is processed. Use the per-frame recording identity
    // to avoid encoding stale pre-record frames or dropping valid late frames.
    const bool frame_should_encode =
        entry->recording_frame_id > 0 && !entry->recording_folder.empty();

    if (frame_should_encode && !is_recording_) {
        ensure_recording_started(entry->recording_folder);
    } else if (!frame_should_encode && is_recording_ && !camera_control_->record_video) {
        // The queue has reached post-recording preview frames; close the run.
        finalize_recording();
    }

    const bool encode_this_frame = frame_should_encode && is_recording_;
    perf.encode_active = encode_this_frame;

    try {
        if (entry->width < crop_width_ || entry->height < crop_height_) {
            std::cerr << "[CropAndEncodeWorker] Dropping crop frame for camera "
                      << camera_params_->camera_serial
                      << ": source frame "
                      << entry->width << "x" << entry->height
                      << " is smaller than crop "
                      << crop_width_ << "x" << crop_height_ << std::endl;
            if (encode_this_frame) {
                const uint64_t now_ns = steady_now_ns();
                perf.dropped = true;
                perf.drop_reason = "source_smaller_than_crop";
                perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, now_ns);
                write_perf_row(frame, perf);
            }
            release_source_now();
            return false;
        }
        
        bool has_detection = !entry->detections.empty();
        perf.has_detection = has_detection;
        frame.has_detection = has_detection;

        if (has_detection) {
            // --- DETECTION IS FOUND ---
            pose::Object best_detection = *std::max_element(
                entry->detections.begin(), entry->detections.end(),
                [](const pose::Object& a, const pose::Object& b) { return a.prob < b.prob; });
            
            const int CROP_W = crop_width_;
            const int CROP_H = crop_height_;
            float cx = best_detection.rect.x + best_detection.rect.width * 0.5f;
            float cy = best_detection.rect.y + best_detection.rect.height * 0.5f;
            int ix = std::clamp(static_cast<int>(cx) - CROP_W / 2, 0, entry->width - CROP_W);
            int iy = std::clamp(static_cast<int>(cy) - CROP_H / 2, 0, entry->height - CROP_H);
            perf.crop_x = ix;
            perf.crop_y = iy;
            perf.crop_w = CROP_W;
            perf.crop_h = CROP_H;
            frame.detection_confidence = best_detection.prob;
            frame.crop_x = ix;
            frame.crop_y = iy;
            frame.crop_w = CROP_W;
            frame.crop_h = CROP_H;
            frame.detection_x = best_detection.rect.x;
            frame.detection_y = best_detection.rect.y;
            frame.detection_w = best_detection.rect.width;
            frame.detection_h = best_detection.rect.height;

            const bool needs_crop_frame = d_display_buffer_pbo_ || encode_this_frame;
            if (needs_crop_frame) {
                const uint64_t crop_pool_wait_start_ns = steady_now_ns();
                active_crop_frame = acquire_crop_frame();
                perf.crop_pool_wait_ms = elapsed_ms(crop_pool_wait_start_ns, steady_now_ns());

                if (!active_crop_frame) {
                    perf.dropped = true;
                    perf.drop_reason = "crop_frame_pool_empty";
                    perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
                    if (encode_this_frame) {
                        write_perf_row(frame, perf);
                    }
                    release_source_now();
                    return false;
                }
                active_crop_frame->frame = frame;
                timing_crop_frame = active_crop_frame;

                if (entry->event_ptr) {
                    const uint64_t event_wait_start_ns = steady_now_ns();
                    ck(cudaStreamWaitEvent(m_crop_producer_stream, *entry->event_ptr, 0));
                    perf.event_wait_cpu_ms = elapsed_ms(event_wait_start_ns, steady_now_ns());
                    perf.crop_source_wait_enqueue_cpu_ms = perf.event_wait_cpu_ms;
                }

                const unsigned char* crop_source_ptr = entry->d_image;
                int crop_source_pitch = entry->width;
                if (crop_source_stage_enabled_) {
                    ensure_source_stage_buffer(entry->width, entry->height);
                    const uint64_t source_stage_enqueue_start_ns = steady_now_ns();
                    ck(cudaMemcpy2DAsync(
                        d_source_stage_mono_,
                        entry->width,
                        entry->d_image,
                        entry->width,
                        entry->width,
                        entry->height,
                        cudaMemcpyDeviceToDevice,
                        m_crop_producer_stream));
                    perf.source_stage_enqueue_cpu_ms = elapsed_ms(
                        source_stage_enqueue_start_ns,
                        steady_now_ns());
                    source_work_queued = true;
                    source_release_stream = m_crop_producer_stream;
                    // In staged-source mode the GPUDirect frame is detached once
                    // the full-frame stage copy completes; the ROI crop below
                    // reads only from ordinary device memory.
                    defer_source_after_stream_work(m_crop_producer_stream);
                    crop_source_ptr = d_source_stage_mono_;
                    crop_source_pitch = frame.source_width;
                }

                const uint64_t crop_producer_start_ns = steady_now_ns();
                const uint64_t crop_copy_start_event_record_start_ns = steady_now_ns();
                if (crop_copy_timing_enabled_ && active_crop_frame->crop_copy_start_event) {
                    ck(cudaEventRecord(active_crop_frame->crop_copy_start_event, m_crop_producer_stream));
                }
                perf.crop_copy_start_event_record_cpu_ms = elapsed_ms(
                    crop_copy_start_event_record_start_ns,
                    steady_now_ns());
                const uint64_t crop_roi_copy_enqueue_start_ns = steady_now_ns();
                if (crop_copy_kernel_enabled_) {
                    launch_mono_roi_copy_kernel(
                        crop_source_ptr,
                        active_crop_frame->d_crop_mono,
                        crop_source_pitch,
                        ix,
                        iy,
                        CROP_W,
                        CROP_H,
                        m_crop_producer_stream);
                } else {
                    ck(cudaMemcpy2DAsync(active_crop_frame->d_crop_mono, CROP_W,
                                         crop_source_ptr + (iy * crop_source_pitch + ix),
                                         crop_source_pitch, CROP_W, CROP_H,
                                         cudaMemcpyDeviceToDevice, m_crop_producer_stream));
                }
                perf.crop_roi_copy_enqueue_cpu_ms = elapsed_ms(
                    crop_roi_copy_enqueue_start_ns,
                    steady_now_ns());
                if (!crop_source_stage_enabled_) {
                    source_work_queued = true;
                    source_release_stream = m_crop_producer_stream;
                }
                crop_frame_work_queued = true;
                const uint64_t crop_ready_event_record_start_ns = steady_now_ns();
                if (crop_copy_timing_enabled_ && active_crop_frame->crop_copy_stop_event) {
                    ck(cudaEventRecord(active_crop_frame->crop_copy_stop_event, m_crop_producer_stream));
                }
                ck(cudaEventRecord(active_crop_frame->crop_ready_event, m_crop_producer_stream));
                perf.crop_ready_event_record_cpu_ms = elapsed_ms(
                    crop_ready_event_record_start_ns,
                    steady_now_ns());
                perf.crop_producer_cpu_ms = elapsed_ms(crop_producer_start_ns, steady_now_ns());

                if (!crop_source_stage_enabled_) {
                    // The source GPUDirect frame is no longer needed once the ROI
                    // copy completes. Preview/encode consume the crop-owned frame
                    // below.
                    defer_source_after_stream_work(m_crop_producer_stream);
                }
            } else {
                release_source_now();
            }
            
            const uint64_t zero_based_recording_frame =
                frame.recording_frame_id > 0 ? frame.recording_frame_id - 1 : 0;
            const NvEncInputFrame* encIn = nullptr;
            NV_ENC_PIC_PARAMS pic_params = { NV_ENC_PIC_PARAMS_VER };
            bool encode_prepared = false;

            if (active_crop_frame) {
                ck(cudaStreamWaitEvent(m_stream, active_crop_frame->crop_ready_event, 0));
            }

            if (d_display_buffer_pbo_ && active_crop_frame) {
                const uint64_t preview_start_ns = steady_now_ns();
                pose::Rect crop_rect = {0.0f, 0.0f, (float)CROP_W, (float)CROP_H};
                gpu_crop_and_resize_rgba(active_crop_frame->d_crop_mono, d_cropped_rgba_, CROP_W, CROP_H,
                                         crop_rect, CROP_W, CROP_H, m_stream);
                perf.crop_preview_cpu_ms = elapsed_ms(preview_start_ns, steady_now_ns());
            }
            
            if (encode_this_frame && active_crop_frame) {
                encIn = encoder_->GetNextInputFrame();
                unsigned char* d_nv12_dst = static_cast<unsigned char*>(encIn->inputPtr);
                pic_params.frameIdx = static_cast<uint32_t>(zero_based_recording_frame & 0xffffffffu);
                pic_params.inputTimeStamp = zero_based_recording_frame;
                pic_params.inputDuration = 1;

                ck(cudaMemcpy2DAsync(d_nv12_dst, encIn->pitch,
                                     active_crop_frame->d_crop_mono, CROP_W,
                                     CROP_W, CROP_H, cudaMemcpyDeviceToDevice, m_stream));
                
                unsigned char* d_uv_plane_dst = d_nv12_dst + encIn->pitch * CROP_H;
                ck(cudaMemset2DAsync(d_uv_plane_dst, encIn->pitch, 128, CROP_W, CROP_H / 2, m_stream));
                encode_prepared = true;
            }

            if (active_crop_frame && crop_frame_work_queued) {
                defer_active_crop_frame_after_stream_work();
            }

            // --- LIVE PREVIEW LOGIC (ALWAYS RUNS) ---
            if (d_display_buffer_pbo_) {
                copy_crop_to_display_preview();
                const uint64_t display_sync_start_ns = steady_now_ns();
                synchronize_display_preview();
                perf.display_sync_ms = elapsed_ms(display_sync_start_ns, steady_now_ns());
                drain_pending_source_releases(false);
                drain_pending_crop_frames(false);
            }

            // --- RECORDING LOGIC (ONLY RUNS IF RECORDING IS ON) ---
            if (encode_prepared) {
                const uint64_t encode_start_ns = steady_now_ns();

                // Encode and write the frame to file
                std::vector<std::vector<uint8_t>> packets;
                std::vector<uint64_t> output_timestamps;
                encoder_->EncodeFrame(packets, &pic_params, nullptr, &output_timestamps);
                perf.packet_count = packets.size();
                perf.encoded_bytes = encoded_packet_bytes(packets);
                push_encoded_packets(packets, output_timestamps, zero_based_recording_frame);
                perf.encode_submit_cpu_ms = elapsed_ms(encode_start_ns, steady_now_ns());

                // Write metadata to file
                const uint64_t metadata_start_ns = steady_now_ns();
                write_metadata_row(frame);
                perf.metadata_cpu_ms = elapsed_ms(metadata_start_ns, steady_now_ns());
            }
        } else {
            release_source_now();
            // --- NO DETECTION ---
            // Always show a blank screen for the preview if no detection
            if (d_display_buffer_pbo_) {
                const uint64_t preview_start_ns = steady_now_ns();
                clear_display_preview();
                perf.crop_preview_cpu_ms = elapsed_ms(preview_start_ns, steady_now_ns());
            }
            
            // Only encode a blank frame if recording is active
            if (encode_this_frame) {
                perf.blank_frame = true;
                frame.blank_frame = true;
                const uint64_t encode_start_ns = steady_now_ns();
                const NvEncInputFrame* encIn = encoder_->GetNextInputFrame();
                const uint64_t zero_based_recording_frame =
                    frame.recording_frame_id > 0 ? frame.recording_frame_id - 1 : 0;
                NV_ENC_PIC_PARAMS pic_params = { NV_ENC_PIC_PARAMS_VER };
                pic_params.frameIdx = static_cast<uint32_t>(zero_based_recording_frame & 0xffffffffu);
                pic_params.inputTimeStamp = zero_based_recording_frame;
                pic_params.inputDuration = 1;
                ck(cudaMemcpy2DAsync(encIn->inputPtr, encIn->pitch, d_blank_frame_,
                                     encoder_pitch_, encoder_pitch_, crop_height_ * 3 / 2,
                                     cudaMemcpyDeviceToDevice, m_stream));

                std::vector<std::vector<uint8_t>> packets;
                std::vector<uint64_t> output_timestamps;
                encoder_->EncodeFrame(packets, &pic_params, nullptr, &output_timestamps);
                perf.packet_count = packets.size();
                perf.encoded_bytes = encoded_packet_bytes(packets);
                push_encoded_packets(packets, output_timestamps, zero_based_recording_frame);
                perf.encode_submit_cpu_ms = elapsed_ms(encode_start_ns, steady_now_ns());
                const uint64_t metadata_start_ns = steady_now_ns();
                write_metadata_row(frame);
                perf.metadata_cpu_ms = elapsed_ms(metadata_start_ns, steady_now_ns());
            }
        }

        if (crop_copy_timing_enabled_ &&
            timing_crop_frame &&
            timing_crop_frame->crop_copy_start_event &&
            timing_crop_frame->crop_copy_stop_event) {
            cudaError_t copy_done = cudaEventQuery(timing_crop_frame->crop_copy_stop_event);
            if (copy_done == cudaSuccess) {
                float copy_gpu_ms = 0.0f;
                cudaError_t elapsed_status = cudaEventElapsedTime(
                    &copy_gpu_ms,
                    timing_crop_frame->crop_copy_start_event,
                    timing_crop_frame->crop_copy_stop_event);
                if (elapsed_status == cudaSuccess) {
                    perf.crop_copy_gpu_ms = static_cast<double>(copy_gpu_ms);
                } else {
                    std::cerr << "[CropAndEncodeWorker] Crop copy timing failed for frame "
                              << frame.local_frame_id
                              << ": " << cudaGetErrorString(elapsed_status) << std::endl;
                    cudaGetLastError();
                }
            } else if (copy_done != cudaErrorNotReady) {
                std::cerr << "[CropAndEncodeWorker] Crop copy timing query failed for frame "
                          << frame.local_frame_id
                          << ": " << cudaGetErrorString(copy_done) << std::endl;
                cudaGetLastError();
            }
        }

        if (encode_this_frame) {
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            write_perf_row(frame, perf);
        }

    } catch (const std::exception& e) {
        std::cerr << "[CropAndEncodeWorker] Exception processing frame " << frame.local_frame_id
                  << ": " << e.what() << std::endl;
        if (encode_this_frame) {
            perf.dropped = true;
            perf.drop_reason = "exception";
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            write_perf_row(frame, perf);
        }
    }

    // Cleanup and recycle the entry
    if (active_crop_frame) {
        if (crop_frame_work_queued) {
            try {
                defer_active_crop_frame_after_stream_work();
            } catch (const std::exception& e) {
                std::cerr << "[CropAndEncodeWorker] Failed to defer crop frame recycle for frame "
                          << frame.local_frame_id
                          << ": " << e.what()
                          << "; synchronizing crop stream before recycle." << std::endl;
                const uint64_t stream_sync_start_ns = steady_now_ns();
                cudaError_t status = cudaStreamSynchronize(m_stream);
                perf.stream_sync_ms += elapsed_ms(stream_sync_start_ns, steady_now_ns());
                if (status != cudaSuccess) {
                    std::cerr << "[CropAndEncodeWorker] CropFrame fallback sync failed for frame "
                              << frame.local_frame_id
                              << ": " << cudaGetErrorString(status) << std::endl;
                    cudaGetLastError();
                }
                recycle_active_crop_frame_now();
            }
        } else {
            recycle_active_crop_frame_now();
        }
    }

    if (!source_entry_released && source_work_queued) {
        try {
            defer_source_after_stream_work(source_release_stream);
        } catch (const std::exception& e) {
            std::cerr << "[CropAndEncodeWorker] Failed to defer source release for frame "
                      << frame.local_frame_id
                      << ": " << e.what()
                      << "; synchronizing source-use stream before release." << std::endl;
            const uint64_t stream_sync_start_ns = steady_now_ns();
            cudaError_t status = cudaStreamSynchronize(source_release_stream);
            perf.stream_sync_ms += elapsed_ms(stream_sync_start_ns, steady_now_ns());
            if (status != cudaSuccess) {
                std::cerr << "[CropAndEncodeWorker] Source-release fallback sync failed for frame "
                          << frame.local_frame_id
                          << ": " << cudaGetErrorString(status) << std::endl;
                cudaGetLastError();
            }
            release_source_now();
        }
    } else {
        release_source_now();
    }
    drain_pending_source_releases(false);
    drain_pending_crop_frames(false);

    return false; // This worker does not pass items to its own output queue
}
