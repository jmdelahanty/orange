#include "crop_preview_worker.h"

#include "crop_producer_worker.h"
#include "kernel.cuh"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace {
constexpr int kDisplayGpuId = 0;
constexpr const char* kCropPreviewDisableEnv = "ORANGE_CROP_PREVIEW_DISABLE";
constexpr const char* kCropPreviewMaxFpsEnv = "ORANGE_CROP_PREVIEW_MAX_FPS";

uint64_t steady_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

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

int resolve_crop_preview_max_fps(const CameraParams* camera_params,
                                 const char* worker_name,
                                 std::string* source_out)
{
    int configured = camera_params
        ? camera_params->crop_pipeline.preview_max_fps
        : CameraCropPipelineConfig::kDefaultPreviewMaxFps;
    const char* source = "camera_config";

    const char* env_value = std::getenv(kCropPreviewMaxFpsEnv);
    if (env_value && *env_value) {
        char* end = nullptr;
        const long parsed = std::strtol(env_value, &end, 10);
        if (end != env_value && end && *end == '\0') {
            if (parsed > std::numeric_limits<int>::max()) {
                configured = std::numeric_limits<int>::max();
            } else if (parsed < std::numeric_limits<int>::min()) {
                configured = std::numeric_limits<int>::min();
            } else {
                configured = static_cast<int>(parsed);
            }
            source = kCropPreviewMaxFpsEnv;
        } else {
            std::cerr << "[CropPreviewWorker] Ignoring invalid "
                      << kCropPreviewMaxFpsEnv << "='" << env_value << "'";
            if (worker_name) {
                std::cerr << " for " << worker_name;
            }
            std::cerr << std::endl;
        }
    }

    if (source_out) {
        *source_out = source;
    }
    return sanitize_camera_crop_preview_max_fps(configured);
}

}  // namespace

CropPreviewWorker::CropPreviewWorker(
    const char* name,
    CameraParams* camera_params,
    unsigned char* display_buffer_pbo,
    CropProducer* crop_producer,
    int crop_size_px)
    : CThreadWorker<CropPreviewJob>(name),
      camera_params_(camera_params),
      crop_producer_(crop_producer),
      crop_width_(sanitize_camera_crop_size_px(crop_size_px)),
      crop_height_(sanitize_camera_crop_size_px(crop_size_px)),
      d_display_buffer_pbo_(display_buffer_pbo)
{
    std::cout << "[CropPreviewWorker] Initializing " << name
              << " on GPU " << camera_params_->gpu_id << std::endl;

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));

    std::string preview_fps_source;
    preview_cadence_.SetMaxFps(
        resolve_crop_preview_max_fps(camera_params_, name, &preview_fps_source));

    if (env_flag_enabled(kCropPreviewDisableEnv)) {
        display_preview_disabled_.store(true, std::memory_order_release);
        d_display_buffer_pbo_ = nullptr;
        std::cout << "[CropPreviewWorker] Crop live preview CUDA path disabled for "
                  << name << " via " << kCropPreviewDisableEnv << std::endl;
    }

    std::cout << "[CropPreviewWorker] Crop live preview for "
              << name
              << ": enabled=" << (crop_preview_available() ? "true" : "false")
              << " max_fps=" << preview_cadence_.MaxFps()
              << " source=" << preview_fps_source
              << std::endl;

    if (d_display_buffer_pbo_) {
        ck(cudaMalloc(&d_cropped_rgba_, crop_preview_bytes()));
    }

    if (d_display_buffer_pbo_ && camera_params_->gpu_id != kDisplayGpuId) {
        ck(cudaHostAlloc(&h_display_crop_, crop_preview_bytes(), cudaHostAllocDefault));
        ck(cudaSetDevice(kDisplayGpuId));
        ck(cudaStreamCreateWithFlags(&display_stream_, cudaStreamNonBlocking));
        ck(cudaSetDevice(camera_params_->gpu_id));
    }
}

CropPreviewWorker::~CropPreviewWorker()
{
    StopThread();

    // Destructor must not throw (docs/error_handling_convention.md):
    // best-effort device selection during teardown.
    if (camera_params_ && cudaSetDevice(camera_params_->gpu_id) != cudaSuccess) {
        std::cerr << "[CropPreviewWorker] destructor: cudaSetDevice failed; continuing teardown" << std::endl;
    }

    if (d_cropped_rgba_) {
        cudaFree(d_cropped_rgba_);
        d_cropped_rgba_ = nullptr;
    }
    if (h_display_crop_) {
        cudaFreeHost(h_display_crop_);
        h_display_crop_ = nullptr;
    }
    if (stream_) {
        cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }
    if (display_stream_) {
        cudaSetDevice(kDisplayGpuId);
        cudaStreamDestroy(display_stream_);
        display_stream_ = nullptr;
        if (camera_params_) {
            cudaSetDevice(camera_params_->gpu_id);
        }
    }

    std::cout << "[CropPreviewWorker] Summary for " << threadName
              << " offered=" << frames_offered_.load(std::memory_order_relaxed)
              << " updated=" << frames_updated_.load(std::memory_order_relaxed)
              << " skipped_by_cadence="
              << frames_skipped_by_cadence_.load(std::memory_order_relaxed)
              << " clears=" << clears_updated_.load(std::memory_order_relaxed)
              << " queue_full_drops="
              << queue_full_drops_.load(std::memory_order_relaxed)
              << " queue_high_water="
              << queue_high_water_.load(std::memory_order_relaxed)
              << " serial=" << preview_serial_.load(std::memory_order_relaxed)
              << std::endl;
}

void CropPreviewWorker::SetMaxQueueSize(int size)
{
    max_queue_size_ = std::max(1, size);
    CThreadWorker<CropPreviewJob>::SetMaxQueueSize(max_queue_size_);
}

void CropPreviewWorker::SetPreviewDisplayEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(preview_mutex_);
    preview_cadence_.SetDisplayEnabled(enabled);
}

CropPreviewCadence::Decision CropPreviewWorker::EvaluateOffer(bool blank_preview)
{
    std::lock_guard<std::mutex> lock(preview_mutex_);
    const CropPreviewCadence::Decision decision =
        preview_cadence_.ShouldUpdate(crop_preview_available(), blank_preview, steady_now_ns());
    if (decision.offered) {
        frames_offered_.fetch_add(1, std::memory_order_relaxed);
    }
    if (decision.skipped_by_cadence) {
        frames_skipped_by_cadence_.fetch_add(1, std::memory_order_relaxed);
    }
    return decision;
}

bool CropPreviewWorker::TryEnqueuePreview(CropPreviewJob* job)
{
    if (!job) {
        return false;
    }

    const int queue_depth = GetCountQueueInSize();
    queue_high_water_.store(
        std::max(queue_high_water_.load(std::memory_order_relaxed), queue_depth + 1),
        std::memory_order_relaxed);
    if (queue_depth >= max_queue_size_) {
        queue_full_drops_.fetch_add(1, std::memory_order_relaxed);
        release_job(job);
        return false;
    }

    if (!PutObjectToQueueIn(job)) {
        std::cerr << "[CropPreviewWorker] enqueue rejected after stop"
                  << " worker=" << threadName
                  << " frame=" << job->frame.local_frame_id
                  << " recording_frame=" << job->frame.recording_frame_id
                  << std::endl;
        release_job(job);
        return false;
    }
    return true;
}

void CropPreviewWorker::ResetRunCounters()
{
    frames_offered_.store(0, std::memory_order_relaxed);
    frames_updated_.store(0, std::memory_order_relaxed);
    frames_skipped_by_cadence_.store(0, std::memory_order_relaxed);
    clears_updated_.store(0, std::memory_order_relaxed);
    queue_full_drops_.store(0, std::memory_order_relaxed);
    queue_high_water_.store(0, std::memory_order_relaxed);
}

void CropPreviewWorker::WaitUntilIdle(int timeout_ms)
{
    const uint64_t start_ns = steady_now_ns();
    const uint64_t timeout_ns =
        timeout_ms <= 0 ? 0 : static_cast<uint64_t>(timeout_ms) * 1000000ull;
    while (GetCountQueueInSize() > 0 ||
           active_jobs_.load(std::memory_order_acquire) > 0) {
        if (timeout_ns > 0 && steady_now_ns() - start_ns >= timeout_ns) {
            std::cerr << "[CropPreviewWorker] Timed out waiting for preview queue to drain for "
                      << threadName << std::endl;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (crop_producer_) {
        crop_producer_->DrainPending(false);
    }
}

CropPreviewWorker::Summary CropPreviewWorker::GetSummary() const
{
    std::lock_guard<std::mutex> lock(preview_mutex_);
    Summary summary;
    summary.max_fps = preview_cadence_.MaxFps();
    summary.available = crop_preview_available();
    summary.display_enabled = preview_cadence_.DisplayEnabled();
    summary.frames_offered = frames_offered_.load(std::memory_order_relaxed);
    summary.frames_updated = frames_updated_.load(std::memory_order_relaxed);
    summary.frames_skipped_by_cadence =
        frames_skipped_by_cadence_.load(std::memory_order_relaxed);
    summary.clears_updated = clears_updated_.load(std::memory_order_relaxed);
    summary.queue_full_drops = queue_full_drops_.load(std::memory_order_relaxed);
    summary.queue_high_water = queue_high_water_.load(std::memory_order_relaxed);
    summary.serial = preview_serial_.load(std::memory_order_relaxed);
    return summary;
}

void CropPreviewWorker::OnQueueInDequeued(CropPreviewJob* job, int /*queue_depth*/)
{
    if (job) {
        active_jobs_.fetch_add(1, std::memory_order_acq_rel);
    }
}

bool CropPreviewWorker::WorkerFunction(CropPreviewJob* raw_job)
{
    if (!raw_job) {
        return false;
    }
    struct JobGuard {
        CropPreviewWorker* owner = nullptr;
        CropPreviewJob* job = nullptr;
        ~JobGuard()
        {
            if (owner && job) {
                owner->release_job(job);
            }
        }
    } job_guard{this, raw_job};
    CropPreviewJob* job = raw_job;
    struct ActiveJobGuard {
        std::atomic<int>* active_jobs = nullptr;
        ~ActiveJobGuard()
        {
            if (active_jobs) {
                active_jobs->fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    } active_guard{&active_jobs_};

    try {
        ck(cudaSetDevice(camera_params_->gpu_id));
        if (job->blank_preview) {
            clear_display_preview();
            synchronize_display_preview();
            if (crop_preview_active()) {
                mark_display_preview_updated(true);
            }
            return false;
        }

        if (!job->crop_frame) {
            return false;
        }

        ck(cudaStreamWaitEvent(stream_, job->crop_frame->crop_ready_event, 0));
        pose::Rect crop_rect = {
            0.0f,
            0.0f,
            static_cast<float>(crop_width_),
            static_cast<float>(crop_height_)};
        gpu_crop_and_resize_rgba(
            job->crop_frame->d_crop_mono,
            d_cropped_rgba_,
            crop_width_,
            crop_height_,
            crop_rect,
            crop_width_,
            crop_height_,
            stream_);
        copy_crop_to_display_preview();
        synchronize_display_preview();
        if (crop_preview_active()) {
            mark_display_preview_updated(false);
        }
    } catch (const std::exception& e) {
        std::cerr << "[CropPreviewWorker] Exception updating preview for frame "
                  << job->frame.local_frame_id << ": " << e.what() << std::endl;
    }

    if (crop_producer_) {
        crop_producer_->DrainPending(false);
    }
    return false;
}

size_t CropPreviewWorker::crop_preview_bytes() const
{
    return static_cast<size_t>(crop_width_) * static_cast<size_t>(crop_height_) * 4;
}

bool CropPreviewWorker::crop_preview_available() const
{
    return d_display_buffer_pbo_ &&
           !display_preview_disabled_.load(std::memory_order_acquire);
}

bool CropPreviewWorker::crop_preview_active() const
{
    return crop_preview_available() && preview_cadence_.DisplayEnabled();
}

void CropPreviewWorker::mark_display_preview_updated(bool blank_preview)
{
    std::lock_guard<std::mutex> lock(preview_mutex_);
    preview_cadence_.MarkUpdated(blank_preview, steady_now_ns());
    frames_updated_.fetch_add(1, std::memory_order_relaxed);
    if (blank_preview) {
        clears_updated_.fetch_add(1, std::memory_order_relaxed);
    }
    preview_serial_.fetch_add(1, std::memory_order_release);
}

bool CropPreviewWorker::display_cuda_ok(cudaError_t status, const char* operation)
{
    if (status == cudaSuccess) {
        return true;
    }

    if (!display_preview_disabled_.exchange(true, std::memory_order_acq_rel)) {
        std::cerr << "[CropPreviewWorker] Disabling crop preview for "
                  << threadName << ": " << operation << " failed: "
                  << cudaGetErrorString(status) << std::endl;
    }

    if (camera_params_) {
        cudaSetDevice(camera_params_->gpu_id);
    }
    return false;
}

void CropPreviewWorker::copy_crop_to_display_preview()
{
    if (!d_display_buffer_pbo_ ||
        display_preview_disabled_.load(std::memory_order_acquire)) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        display_cuda_ok(
            cudaMemcpyAsync(
                d_display_buffer_pbo_,
                d_cropped_rgba_,
                crop_preview_bytes(),
                cudaMemcpyDeviceToDevice,
                stream_),
            "cudaMemcpyAsync(crop preview same GPU)");
        return;
    }

    if (!h_display_crop_ || !display_stream_) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview cross-GPU staging");
        return;
    }

    if (!display_cuda_ok(
            cudaMemcpyAsync(
                h_display_crop_,
                d_cropped_rgba_,
                crop_preview_bytes(),
                cudaMemcpyDeviceToHost,
                stream_),
            "cudaMemcpyAsync(crop preview device-to-host)")) {
        return;
    }
    if (!display_cuda_ok(cudaStreamSynchronize(stream_), "cudaStreamSynchronize(crop preview camera stream)")) {
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
            display_stream_),
        "cudaMemcpyAsync(crop preview host-to-display)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropPreviewWorker::clear_display_preview()
{
    if (!d_display_buffer_pbo_ ||
        display_preview_disabled_.load(std::memory_order_acquire)) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        display_cuda_ok(
            cudaMemsetAsync(d_display_buffer_pbo_, 0, crop_preview_bytes(), stream_),
            "cudaMemsetAsync(crop preview same GPU)");
        return;
    }

    if (!display_stream_) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview display stream");
        return;
    }

    if (!display_cuda_ok(cudaSetDevice(kDisplayGpuId), "cudaSetDevice(crop preview clear display GPU)")) {
        return;
    }
    display_cuda_ok(
        cudaMemsetAsync(d_display_buffer_pbo_, 0, crop_preview_bytes(), display_stream_),
        "cudaMemsetAsync(crop preview display GPU)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropPreviewWorker::synchronize_display_preview()
{
    if (!d_display_buffer_pbo_ ||
        display_preview_disabled_.load(std::memory_order_acquire)) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        display_cuda_ok(cudaStreamSynchronize(stream_), "cudaStreamSynchronize(crop preview same GPU)");
        return;
    }

    if (!display_stream_) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview display stream synchronize");
        return;
    }

    if (!display_cuda_ok(cudaSetDevice(kDisplayGpuId), "cudaSetDevice(crop preview synchronize display GPU)")) {
        return;
    }
    display_cuda_ok(cudaStreamSynchronize(display_stream_), "cudaStreamSynchronize(crop preview display stream)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropPreviewWorker::release_job(CropPreviewJob* job)
{
    if (!job) {
        return;
    }
    if (job->crop_frame && crop_producer_) {
        CropFrameLease crop_frame_lease(crop_producer_, job->crop_frame);
        job->crop_frame = nullptr;
        try {
            ck(cudaSetDevice(camera_params_->gpu_id));
            ck(cudaStreamWaitEvent(stream_, crop_frame_lease.get()->crop_ready_event, 0));
            crop_frame_lease.ReleaseAfterStream(stream_);
        } catch (const std::exception& e) {
            std::cerr << "[CropPreviewWorker] Failed to release pooled preview job frame "
                      << job->frame.local_frame_id
                      << ": " << e.what()
                      << "; returning crop frame immediately." << std::endl;
            crop_frame_lease.ReleaseNow();
        }
    }
    if (crop_producer_worker_) {
        crop_producer_worker_->ReturnCropPreviewJob(job);
    } else {
        std::cerr << "[CropPreviewWorker] No CropProducerWorker set; pooled CropPreviewJob "
                  << "for frame " << job->frame.local_frame_id
                  << " cannot be returned." << std::endl;
    }
}
