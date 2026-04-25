// src/crop_and_encode_worker.cpp

#include "crop_and_encode_worker.h"
#include "crop_producer_worker.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include "project.h" // Add this include
#include "fsuid_guard.h"
#include <nppi.h>
#include <npp.h>
#include <nppi_color_conversion.h>
#include <nppi_geometry_transforms.h>
#include <algorithm> // For std::max_element
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
constexpr int kDisplayGpuId = 0;
constexpr const char* kCropPreviewDisableEnv = "ORANGE_CROP_PREVIEW_DISABLE";

bool env_flag_enabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return false;
    }
    return std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0 &&
           std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "OFF") != 0 &&
           std::strcmp(value, "no") != 0 &&
           std::strcmp(value, "NO") != 0;
}

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

    if (env_flag_enabled(kCropPreviewDisableEnv)) {
        display_preview_disabled_ = true;
        d_display_buffer_pbo_ = nullptr;
        std::cout << "[CropAndEncodeWorker] Crop live preview CUDA path disabled for "
                  << name << " via " << kCropPreviewDisableEnv << std::endl;
    }

    if (d_display_buffer_pbo_) {
        ck(cudaMalloc(&d_cropped_rgba_, crop_preview_bytes()));
    }

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
    if (d_blank_frame_) {
        cudaFree(d_blank_frame_);
        d_blank_frame_ = nullptr;
    }
    if (h_display_crop_) {
        cudaFreeHost(h_display_crop_);
        h_display_crop_ = nullptr;
    }
    if (m_stream) {
        cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
    if (m_display_stream) {
        cudaSetDevice(kDisplayGpuId);
        cudaStreamDestroy(m_display_stream);
        m_display_stream = nullptr;
    }

    std::cout << "[CropAndEncodeWorker] Summary for " << threadName
              << " jobs_enqueued=" << jobs_enqueued_.load(std::memory_order_relaxed)
              << " queue_full_drops=" << queue_full_drops_.load(std::memory_order_relaxed)
              << " queue_high_water=" << queue_high_water_.load(std::memory_order_relaxed)
              << " preview_frames=" << preview_frames_
              << " encoded_frames=" << encoded_frames_
              << " blank_frames=" << blank_frames_encoded_
              << " dropped_frames=" << dropped_frames_
              << std::endl;
}

void CropAndEncodeWorker::reset_recording_counters()
{
    run_jobs_enqueued_ = 0;
    run_queue_full_drops_ = 0;
    run_queue_high_water_ = 0;
}

void CropAndEncodeWorker::SetMaxQueueSize(int size)
{
    max_queue_size_ = std::max(1, size);
    CThreadWorker<CropEncodeJob>::SetMaxQueueSize(max_queue_size_);
}

bool CropAndEncodeWorker::TryEnqueueJob(CropEncodeJob* job)
{
    if (!job) {
        return false;
    }

    const int queue_depth = GetCountQueueInSize();
    queue_high_water_.store(
        std::max(queue_high_water_.load(std::memory_order_relaxed), queue_depth + 1),
        std::memory_order_relaxed);
    const bool record_active =
        job->frame.recording_frame_id > 0 && !job->frame.recording_folder.empty();
    if (record_active) {
        run_queue_high_water_ = std::max(run_queue_high_water_, queue_depth + 1);
    }
    if (queue_depth >= max_queue_size_) {
        queue_full_drops_.fetch_add(1, std::memory_order_relaxed);
        if (record_active) {
            ++run_queue_full_drops_;
        }
        return false;
    }

    jobs_enqueued_.fetch_add(1, std::memory_order_relaxed);
    if (record_active) {
        ++run_jobs_enqueued_;
    }
    PutObjectToQueueIn(job);
    return true;
}

void CropAndEncodeWorker::RotateRecordingFolder(const std::string& recording_folder)
{
    if (recording_folder.empty()) {
        return;
    }

    if (current_sidecar_recording_folder_ == recording_folder) {
        return;
    }

    if (!current_sidecar_recording_folder_.empty()) {
        write_sidecar_summary();
        reset_recording_counters();
    }

    current_sidecar_recording_folder_ = recording_folder;

    crop_sidecar_perf_file_ =
        recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_sidecar_perf.csv";

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream sidecar_perf(crop_sidecar_perf_file_.c_str(), std::ios::out | std::ios::trunc);
    if (!sidecar_perf) {
        std::cerr << "[CropAndEncodeWorker] Warning: Could not open crop sidecar perf file for "
                  << threadName << ": " << crop_sidecar_perf_file_ << std::endl;
        return;
    }

    sidecar_perf
        << "camera_serial,gpu_id,worker,queue_size,"
        << "producer_jobs_offered,producer_jobs_enqueued,producer_queue_full_drops,"
        << "producer_blank_jobs_offered,producer_blank_jobs_enqueued,"
        << "producer_dropped_jobs_offered,producer_dropped_jobs_enqueued,"
        << "consumer_jobs_enqueued,consumer_queue_full_drops,consumer_queue_high_water\n";
}

void CropAndEncodeWorker::write_sidecar_summary()
{
    if (crop_sidecar_perf_file_.empty()) {
        return;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream sidecar_perf(crop_sidecar_perf_file_.c_str(), std::ios::out | std::ios::app);
    if (!sidecar_perf) {
        std::cerr << "[CropAndEncodeWorker] Warning: Could not append crop sidecar perf file for "
                  << threadName << ": " << crop_sidecar_perf_file_ << std::endl;
        return;
    }

    CropProducerWorker::RecordingCounters producer_counters;
    if (crop_producer_worker_) {
        producer_counters = crop_producer_worker_->GetRecordingCounters();
    }

    sidecar_perf
        << camera_params_->camera_serial << ','
        << camera_params_->gpu_id << ','
        << "CropAndEncodeWorker,"
        << max_queue_size_ << ','
        << producer_counters.jobs_offered << ','
        << producer_counters.jobs_enqueued << ','
        << producer_counters.queue_full_drops << ','
        << producer_counters.blank_jobs_offered << ','
        << producer_counters.blank_jobs_enqueued << ','
        << producer_counters.dropped_jobs_offered << ','
        << producer_counters.dropped_jobs_enqueued << ','
        << run_jobs_enqueued_ << ','
        << run_queue_full_drops_ << ','
        << run_queue_high_water_ << '\n';
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

    if (crop_sidecar_perf_file_.empty()) {
        RotateRecordingFolder(recording_folder);
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
                << "crop_source_wait_enqueue_cpu_ms,analytics_owned_wait_cpu_ms,source_stage_enqueue_cpu_ms,"
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

void CropAndEncodeWorker::write_perf_row(const CropFrameSnapshot& frame, const CropEncodePerfSample& sample)
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
               << sample.analytics_owned_wait_cpu_ms << ','
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

size_t CropAndEncodeWorker::crop_preview_bytes() const
{
    return static_cast<size_t>(crop_width_) * static_cast<size_t>(crop_height_) * 4;
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

    write_sidecar_summary();
    flush_and_close();
    is_recording_ = false;
    crop_sidecar_perf_file_.clear();
    current_sidecar_recording_folder_.clear();
    reset_recording_counters();
    if (crop_producer_worker_) {
        crop_producer_worker_->CloseRecording();
    }

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
    return GetCountQueueInSize() == 0;
}


bool CropAndEncodeWorker::WorkerFunction(CropEncodeJob* raw_job) {
    // Set the correct CUDA device for this worker's operations.
    ck(cudaSetDevice(camera_params_->gpu_id));
    EnsureNppStream(m_stream);

    std::unique_ptr<CropEncodeJob> job(raw_job);
    if (!job) {
        return false;
    }

    CropFrameSnapshot& frame = job->frame;
    CropEncodePerfSample& perf = job->perf;
    CropFrame* active_crop_frame = job->crop_frame;
    CropFrame* timing_crop_frame = active_crop_frame;

    const bool frame_should_encode =
        frame.recording_frame_id > 0 && !frame.recording_folder.empty();

    if (frame_should_encode && !is_recording_) {
        ensure_recording_started(frame.recording_folder);
    } else if (!frame_should_encode && is_recording_ && !camera_control_->record_video) {
        // The queue has reached post-recording preview frames; close the run.
        finalize_recording();
    }

    const bool encode_this_frame = frame_should_encode && is_recording_;
    perf.encode_active = encode_this_frame;

    try {
        if (frame.has_detection && perf.dropped) {
            dropped_frames_++;
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            if (encode_this_frame) {
                write_perf_row(frame, perf);
            }
            return false;
        }

        if (frame.has_detection) {
            const int CROP_W = crop_width_;
            const int CROP_H = crop_height_;
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
                preview_frames_++;
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

            if (active_crop_frame && crop_producer_) {
                crop_producer_->RecycleAfterConsumerStream(active_crop_frame, m_stream);
                active_crop_frame = nullptr;
            }

            // --- LIVE PREVIEW LOGIC (ALWAYS RUNS) ---
            if (d_display_buffer_pbo_) {
                copy_crop_to_display_preview();
                const uint64_t display_sync_start_ns = steady_now_ns();
                synchronize_display_preview();
                perf.display_sync_ms = elapsed_ms(display_sync_start_ns, steady_now_ns());
                if (crop_producer_) {
                    crop_producer_->DrainPending(false);
                }
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
                encoded_frames_++;

                // Write metadata to file
                const uint64_t metadata_start_ns = steady_now_ns();
                write_metadata_row(frame);
                perf.metadata_cpu_ms = elapsed_ms(metadata_start_ns, steady_now_ns());
            }
        } else {
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
                encoded_frames_++;
                blank_frames_encoded_++;
                const uint64_t metadata_start_ns = steady_now_ns();
                write_metadata_row(frame);
                perf.metadata_cpu_ms = elapsed_ms(metadata_start_ns, steady_now_ns());
            }
        }

        if (crop_producer_) {
            crop_producer_->QueryCopyTiming(timing_crop_frame, &perf);
        }

        if (encode_this_frame) {
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            write_perf_row(frame, perf);
        }

    } catch (const std::exception& e) {
        std::cerr << "[CropAndEncodeWorker] Exception processing frame " << frame.local_frame_id
                  << ": " << e.what() << std::endl;
        if (encode_this_frame) {
            dropped_frames_++;
            perf.dropped = true;
            perf.drop_reason = "exception";
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            write_perf_row(frame, perf);
        }
    }

    // Cleanup and recycle the entry
    if (active_crop_frame) {
        try {
            if (crop_producer_) {
                crop_producer_->RecycleAfterConsumerStream(active_crop_frame, m_stream);
            }
            active_crop_frame = nullptr;
        } catch (const std::exception& e) {
            std::cerr << "[CropAndEncodeWorker] Failed to defer crop frame recycle for frame "
                      << frame.local_frame_id
                      << ": " << e.what()
                      << "; synchronizing consumer stream before recycle." << std::endl;
            const uint64_t stream_sync_start_ns = steady_now_ns();
            cudaError_t status = cudaStreamSynchronize(m_stream);
            perf.stream_sync_ms += elapsed_ms(stream_sync_start_ns, steady_now_ns());
            if (status != cudaSuccess) {
                std::cerr << "[CropAndEncodeWorker] CropFrame fallback sync failed for frame "
                          << frame.local_frame_id
                          << ": " << cudaGetErrorString(status) << std::endl;
                cudaGetLastError();
            }
            if (crop_producer_) {
                crop_producer_->RecycleNow(active_crop_frame);
            }
            active_crop_frame = nullptr;
        }
    }
    if (crop_producer_) {
        crop_producer_->DrainPending(false);
    }

    return false; // This worker does not pass items to its own output queue
}
