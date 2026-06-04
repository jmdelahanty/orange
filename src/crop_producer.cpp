#include "crop_producer.h"
#include "worker_entry_release.h"

#include "kernel.cuh"
#include "pose_worker.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
constexpr int kCropSourceReleaseEventPoolSize = 256;
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

int env_int_or_default(const char* name, int default_value, int min_value, int max_value)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || (end && *end != '\0')) {
        std::cerr << "[CropProducer] Ignoring invalid " << name << "='" << raw << "'"
                  << std::endl;
        return default_value;
    }
    if (parsed < min_value || parsed > max_value) {
        std::cerr << "[CropProducer] Ignoring out-of-range " << name << "=" << parsed
                  << " (expected " << min_value << "-" << max_value << ")"
                  << std::endl;
        return default_value;
    }
    return static_cast<int>(parsed);
}
}  // namespace

CropProducer::CropProducer(
    CameraParams* camera_params,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    int crop_width,
    int crop_height)
    : camera_params_(camera_params),
      recycle_queue_(recycle_queue),
      crop_width_(crop_width),
      crop_height_(crop_height)
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreateWithFlags(&producer_stream_, cudaStreamNonBlocking));

    crop_copy_timing_enabled_ = env_flag_enabled("ORANGE_CROP_COPY_TIMING", false);
    crop_copy_kernel_enabled_ = env_flag_enabled("ORANGE_CROP_COPY_KERNEL", false);
    crop_source_stage_enabled_ = env_flag_enabled("ORANGE_CROP_STAGE_SOURCE", true);
    crop_early_owned_frame_enabled_ = env_flag_enabled("ORANGE_ANALYTICS_EARLY_OWNED_FRAME", true);
    crop_frame_pool_size_ =
        env_int_or_default(
            "ORANGE_CROP_FRAME_POOL_SIZE",
            kDefaultCropFramePoolSize,
            kMinCropFramePoolSize,
            kMaxCropFramePoolSize);

    std::cout << "[CropProducer] Crop copy mode "
              << (crop_copy_kernel_enabled_ ? "kernel" : "memcpy2d")
              << ", source stage "
              << (crop_source_stage_enabled_ ? "enabled" : "disabled")
              << ", analytics early owned frame "
              << (crop_early_owned_frame_enabled_ ? "enabled" : "disabled")
              << ", crop frame pool size " << crop_frame_pool_size_
              << ", GPU timing "
              << (crop_copy_timing_enabled_ ? "enabled" : "disabled")
              << " for camera " << camera_params_->camera_serial << std::endl;

    source_release_event_pool_.resize(kCropSourceReleaseEventPoolSize);
    for (int i = 0; i < kCropSourceReleaseEventPoolSize; ++i) {
        ck(cudaEventCreateWithFlags(&source_release_event_pool_[i], cudaEventDisableTiming));
        free_source_release_events_.push(&source_release_event_pool_[i]);
    }

    crop_frame_pool_.resize(crop_frame_pool_size_);
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
}

CropProducer::~CropProducer()
{
    if (camera_params_) {
        ck(cudaSetDevice(camera_params_->gpu_id));
    }

    drain_pending_source_releases(true);
    drain_pending_crop_frames(true);

    if (d_source_stage_mono_) {
        cudaFree(d_source_stage_mono_);
        d_source_stage_mono_ = nullptr;
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

    if (producer_stream_) {
        cudaStreamDestroy(producer_stream_);
        producer_stream_ = nullptr;
    }

    std::cout << "[CropProducer] Summary for camera " << camera_params_->camera_serial
              << " produced=" << frames_produced_.load(std::memory_order_relaxed)
              << " recycled=" << frames_recycled_.load(std::memory_order_relaxed)
              << " lease_releases=" << crop_frame_release_count_.load(std::memory_order_relaxed)
              << " recording_offered=" << recording_frames_offered_.load(std::memory_order_relaxed)
              << " recording_accepted=" << recording_frames_accepted_.load(std::memory_order_relaxed)
              << " recording_dropped=" << recording_frames_dropped_.load(std::memory_order_relaxed)
              << " preview_offered=" << preview_frames_offered_.load(std::memory_order_relaxed)
              << " preview_accepted=" << preview_frames_accepted_.load(std::memory_order_relaxed)
              << " preview_dropped=" << preview_frames_dropped_.load(std::memory_order_relaxed)
              << " pose_offered=" << pose_frames_offered_.load(std::memory_order_relaxed)
              << " pose_accepted=" << pose_frames_accepted_.load(std::memory_order_relaxed)
              << " pose_dropped=" << pose_frames_dropped_.load(std::memory_order_relaxed)
              << " source_release_event_misses=" << source_release_event_misses_.load(std::memory_order_relaxed)
              << " crop_pool_misses=" << crop_frame_pool_misses_.load(std::memory_order_relaxed)
              << std::endl;
}

void CropProducer::release_entry(WORKER_ENTRY* entry)
{
    release_worker_entry_to_recycle(recycle_queue_, entry);
}

void CropProducer::ReleaseSourceEntry(WORKER_ENTRY*& entry)
{
    if (!entry) {
        return;
    }
    release_entry(entry);
    entry = nullptr;
}

void CropProducer::SetPoseWorker(PoseWorker* pose_worker)
{
    pose_worker_ = pose_worker;
}

void CropProducer::ResetRunFanoutCounters()
{
    pose_frames_offered_.store(0, std::memory_order_relaxed);
    pose_frames_accepted_.store(0, std::memory_order_relaxed);
    pose_frames_dropped_.store(0, std::memory_order_relaxed);
    preview_frames_offered_.store(0, std::memory_order_relaxed);
    preview_frames_accepted_.store(0, std::memory_order_relaxed);
    preview_frames_dropped_.store(0, std::memory_order_relaxed);
    recording_frames_offered_.store(0, std::memory_order_relaxed);
    recording_frames_accepted_.store(0, std::memory_order_relaxed);
    recording_frames_dropped_.store(0, std::memory_order_relaxed);
}

void CropProducer::NoteConsumerOffered(Consumer consumer)
{
    switch (consumer) {
    case Consumer::kRecording:
        recording_frames_offered_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Consumer::kPreview:
        preview_frames_offered_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Consumer::kPose:
        pose_frames_offered_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void CropProducer::NoteConsumerAccepted(Consumer consumer)
{
    switch (consumer) {
    case Consumer::kRecording:
        recording_frames_accepted_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Consumer::kPreview:
        preview_frames_accepted_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Consumer::kPose:
        pose_frames_accepted_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void CropProducer::NoteConsumerDropped(Consumer consumer)
{
    switch (consumer) {
    case Consumer::kRecording:
        recording_frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Consumer::kPreview:
        preview_frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Consumer::kPose:
        pose_frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

CropProducer::FanoutCounters CropProducer::GetFanoutCounters() const
{
    FanoutCounters counters;
    counters.recording_crop_frame_offered =
        recording_frames_offered_.load(std::memory_order_relaxed);
    counters.recording_crop_frame_accepted =
        recording_frames_accepted_.load(std::memory_order_relaxed);
    counters.recording_crop_frame_dropped =
        recording_frames_dropped_.load(std::memory_order_relaxed);
    counters.preview_crop_frame_offered =
        preview_frames_offered_.load(std::memory_order_relaxed);
    counters.preview_crop_frame_accepted =
        preview_frames_accepted_.load(std::memory_order_relaxed);
    counters.preview_crop_frame_dropped =
        preview_frames_dropped_.load(std::memory_order_relaxed);
    counters.pose_crop_frame_offered =
        pose_frames_offered_.load(std::memory_order_relaxed);
    counters.pose_crop_frame_accepted =
        pose_frames_accepted_.load(std::memory_order_relaxed);
    counters.pose_crop_frame_dropped =
        pose_frames_dropped_.load(std::memory_order_relaxed);
    counters.frames_produced_total =
        frames_produced_.load(std::memory_order_relaxed);
    counters.frames_recycled_total =
        frames_recycled_.load(std::memory_order_relaxed);
    counters.crop_frame_release_total =
        crop_frame_release_count_.load(std::memory_order_relaxed);
    counters.crop_frame_pool_misses_total =
        crop_frame_pool_misses_.load(std::memory_order_relaxed);
    counters.source_release_event_misses_total =
        source_release_event_misses_.load(std::memory_order_relaxed);
    counters.pending_source_releases =
        pending_source_release_count_.load(std::memory_order_relaxed);
    counters.pending_crop_frame_recycles =
        pending_crop_frame_recycle_count_.load(std::memory_order_relaxed);
    return counters;
}

void CropProducer::RetainLease(CropFrame* crop_frame)
{
    if (!crop_frame) {
        return;
    }
    crop_frame->active_leases.fetch_add(1, std::memory_order_acq_rel);
}

cudaEvent_t* CropProducer::acquire_source_release_event()
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
    std::cerr << "[CropProducer] No source-release CUDA event available for camera "
              << camera_params_->camera_serial
              << "; falling back to producer-stream synchronization." << std::endl;
    return nullptr;
}

void CropProducer::defer_source_release(WORKER_ENTRY* entry, cudaEvent_t* event)
{
    if (!entry || !event) {
        if (event) {
            free_source_release_events_.push(event);
        }
        release_entry(entry);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pending_source_releases_mutex_);
        pending_source_releases_.push_back({entry, event});
        pending_source_release_count_.store(
            static_cast<int>(pending_source_releases_.size()),
            std::memory_order_relaxed);
    }
}

void CropProducer::drain_pending_source_releases(bool synchronize_all)
{
    std::lock_guard<std::mutex> lock(pending_source_releases_mutex_);
    for (auto it = pending_source_releases_.begin();
         it != pending_source_releases_.end();) {
        cudaError_t status = synchronize_all
            ? cudaEventSynchronize(*it->event)
            : cudaEventQuery(*it->event);

        if (status == cudaErrorNotReady) {
            ++it;
            continue;
        }

        if (status != cudaSuccess) {
            std::cerr << "[CropProducer] Source-release event wait/query failed for camera "
                      << camera_params_->camera_serial
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

CropFrame* CropProducer::acquire_crop_frame()
{
    CropFrame* crop_frame = nullptr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        drain_pending_crop_frames(false);
        if (free_crop_frames_.pop(crop_frame)) {
            crop_frame->frame = CropFrameSnapshot{};
            crop_frame->active_leases.store(0, std::memory_order_relaxed);
            return crop_frame;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    crop_frame_pool_misses_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[CropProducer] No free CropFrame available for camera "
              << camera_params_->camera_serial
              << "; dropping crop output for this frame." << std::endl;
    return nullptr;
}

void CropProducer::recycle_crop_frame(CropFrame* crop_frame)
{
    if (!crop_frame) {
        return;
    }
    crop_frame->frame = CropFrameSnapshot{};
    crop_frame->active_leases.store(0, std::memory_order_relaxed);
    free_crop_frames_.push(crop_frame);
    frames_recycled_.fetch_add(1, std::memory_order_relaxed);
}

void CropProducer::release_crop_frame_lease(CropFrame* crop_frame)
{
    if (!crop_frame) {
        return;
    }

    const int previous = crop_frame->active_leases.fetch_sub(1, std::memory_order_acq_rel);
    crop_frame_release_count_.fetch_add(1, std::memory_order_relaxed);
    if (previous <= 0) {
        crop_frame->active_leases.store(0, std::memory_order_relaxed);
        std::cerr << "[CropProducer] Lease underflow for camera "
                  << camera_params_->camera_serial
                  << " frame " << crop_frame->frame.local_frame_id << std::endl;
        return;
    }

    if (previous == 1) {
        recycle_crop_frame(crop_frame);
    }
}

void CropProducer::RecycleNow(CropFrame* crop_frame)
{
    release_crop_frame_lease(crop_frame);
}

void CropProducer::defer_crop_frame_recycle(CropFrame* crop_frame)
{
    if (!crop_frame) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pending_crop_frame_recycles_mutex_);
        pending_crop_frame_recycles_.push_back({crop_frame});
        pending_crop_frame_recycle_count_.store(
            static_cast<int>(pending_crop_frame_recycles_.size()),
            std::memory_order_relaxed);
    }
}

void CropProducer::drain_pending_crop_frames(bool synchronize_all)
{
    std::lock_guard<std::mutex> lock(pending_crop_frame_recycles_mutex_);
    for (auto it = pending_crop_frame_recycles_.begin();
         it != pending_crop_frame_recycles_.end();) {
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
            std::cerr << "[CropProducer] CropFrame recycle event wait/query failed for camera "
                      << camera_params_->camera_serial
                      << ": " << cudaGetErrorString(status) << std::endl;
            cudaGetLastError();
        }

        release_crop_frame_lease(crop_frame);
        it = pending_crop_frame_recycles_.erase(it);
        pending_crop_frame_recycle_count_.store(
            static_cast<int>(pending_crop_frame_recycles_.size()),
            std::memory_order_relaxed);
    }

    pending_crop_frame_recycle_count_.store(
        static_cast<int>(pending_crop_frame_recycles_.size()),
        std::memory_order_relaxed);
}

size_t CropProducer::crop_mono_bytes() const
{
    return static_cast<size_t>(crop_width_) * static_cast<size_t>(crop_height_);
}

void CropProducer::ensure_source_stage_buffer(int width, int height)
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

CropProducer::ProduceResult CropProducer::Produce(
    WORKER_ENTRY*& entry,
    const CropFrameSnapshot& frame,
    int crop_x,
    int crop_y,
    bool needs_crop_frame,
    CropProducerPerfSample* perf,
    bool release_source_entry)
{
    ProduceResult result;
    ck(cudaSetDevice(camera_params_->gpu_id));
    DrainPending(false);

    if (!entry) {
        return result;
    }

    if (!needs_crop_frame) {
        if (release_source_entry) {
            ReleaseSourceEntry(entry);
        }
        return result;
    }

    const uint64_t crop_pool_wait_start_ns = steady_now_ns();
    CropFrame* active_crop_frame = acquire_crop_frame();
    if (perf) {
        perf->crop_pool_wait_ms = elapsed_ms(crop_pool_wait_start_ns, steady_now_ns());
    }
    if (!active_crop_frame) {
        result.dropped = true;
        result.drop_reason = "crop_frame_pool_empty";
        if (release_source_entry) {
            ReleaseSourceEntry(entry);
        }
        return result;
    }

    active_crop_frame->frame = frame;
    active_crop_frame->active_leases.store(1, std::memory_order_relaxed);

    auto defer_source_after_stream_work = [&](WORKER_ENTRY*& source_entry) {
        if (!source_entry) {
            return;
        }
        if (!release_source_entry) {
            return;
        }

        cudaEvent_t* source_release_event = acquire_source_release_event();
        if (source_release_event) {
            if (perf) {
                const uint64_t source_release_event_record_start_ns = steady_now_ns();
                ck(cudaEventRecord(*source_release_event, producer_stream_));
                perf->source_release_event_record_cpu_ms += elapsed_ms(
                    source_release_event_record_start_ns,
                    steady_now_ns());
            } else {
                ck(cudaEventRecord(*source_release_event, producer_stream_));
            }
            defer_source_release(source_entry, source_release_event);
            source_entry = nullptr;
            return;
        }

        ck(cudaStreamSynchronize(producer_stream_));
        ReleaseSourceEntry(source_entry);
    };

    const bool use_analytics_owned_frame =
        crop_early_owned_frame_enabled_ &&
        entry->analytics_owned_frame_valid &&
        entry->d_analytics_image != nullptr &&
        entry->analytics_ready_event != nullptr;
    const bool use_entry_owned_source_frame =
        !entry->gpu_direct_mode &&
        entry->d_image != nullptr &&
        entry->d_image == entry->d_image_pool;

    if (use_analytics_owned_frame) {
        const uint64_t analytics_wait_start_ns = steady_now_ns();
        ck(cudaStreamWaitEvent(producer_stream_, entry->analytics_ready_event, 0));
        if (perf) {
            perf->analytics_owned_wait_cpu_ms = elapsed_ms(analytics_wait_start_ns, steady_now_ns());
            perf->crop_source_wait_enqueue_cpu_ms = perf->analytics_owned_wait_cpu_ms;
        }
    } else if (entry->event_ptr) {
        const uint64_t event_wait_start_ns = steady_now_ns();
        ck(cudaStreamWaitEvent(producer_stream_, *entry->event_ptr, 0));
        if (perf) {
            perf->event_wait_cpu_ms = elapsed_ms(event_wait_start_ns, steady_now_ns());
            perf->crop_source_wait_enqueue_cpu_ms = perf->event_wait_cpu_ms;
        }
    }

    const unsigned char* crop_source_ptr =
        use_analytics_owned_frame ? entry->d_analytics_image : entry->d_image;
    int crop_source_pitch = entry->width;
    const bool needs_source_stage_copy =
        crop_source_stage_enabled_ &&
        !use_analytics_owned_frame &&
        !use_entry_owned_source_frame;
    if (needs_source_stage_copy) {
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
            producer_stream_));
        if (perf) {
            perf->source_stage_enqueue_cpu_ms = elapsed_ms(
                source_stage_enqueue_start_ns,
                steady_now_ns());
        }
        if (release_source_entry) {
            defer_source_after_stream_work(entry);
        }
        crop_source_ptr = d_source_stage_mono_;
        crop_source_pitch = frame.source_width;
    }

    const uint64_t crop_producer_start_ns = steady_now_ns();
    const uint64_t crop_copy_start_event_record_start_ns = steady_now_ns();
    if (crop_copy_timing_enabled_ && active_crop_frame->crop_copy_start_event) {
        ck(cudaEventRecord(active_crop_frame->crop_copy_start_event, producer_stream_));
    }
    if (perf) {
        perf->crop_copy_start_event_record_cpu_ms = elapsed_ms(
            crop_copy_start_event_record_start_ns,
            steady_now_ns());
    }

    const uint64_t crop_roi_copy_enqueue_start_ns = steady_now_ns();
    if (crop_copy_kernel_enabled_) {
        launch_mono_roi_copy_kernel(
            crop_source_ptr,
            active_crop_frame->d_crop_mono,
            crop_source_pitch,
            crop_x,
            crop_y,
            crop_width_,
            crop_height_,
            producer_stream_);
    } else {
        ck(cudaMemcpy2DAsync(
            active_crop_frame->d_crop_mono,
            crop_width_,
            crop_source_ptr + (crop_y * crop_source_pitch + crop_x),
            crop_source_pitch,
            crop_width_,
            crop_height_,
            cudaMemcpyDeviceToDevice,
            producer_stream_));
    }
    if (perf) {
        perf->crop_roi_copy_enqueue_cpu_ms = elapsed_ms(
            crop_roi_copy_enqueue_start_ns,
            steady_now_ns());
    }

    const uint64_t crop_ready_event_record_start_ns = steady_now_ns();
    if (crop_copy_timing_enabled_ && active_crop_frame->crop_copy_stop_event) {
        ck(cudaEventRecord(active_crop_frame->crop_copy_stop_event, producer_stream_));
    }
    ck(cudaEventRecord(active_crop_frame->crop_ready_event, producer_stream_));
    if (perf) {
        perf->crop_ready_event_record_cpu_ms = elapsed_ms(
            crop_ready_event_record_start_ns,
            steady_now_ns());
        perf->crop_producer_cpu_ms = elapsed_ms(crop_producer_start_ns, steady_now_ns());
    }
    active_crop_frame->frame.crop_ready_host_ns = steady_now_ns();

    if (pose_worker_) {
        NoteConsumerOffered(Consumer::kPose);
        active_crop_frame->active_leases.fetch_add(1, std::memory_order_acq_rel);
        if (pose_worker_->TryEnqueueCrop(active_crop_frame)) {
            NoteConsumerAccepted(Consumer::kPose);
        } else {
            release_crop_frame_lease(active_crop_frame);
            NoteConsumerDropped(Consumer::kPose);
        }
    }

    if (!needs_source_stage_copy && release_source_entry) {
        defer_source_after_stream_work(entry);
    }

    frames_produced_.fetch_add(1, std::memory_order_relaxed);
    result.crop_frame = active_crop_frame;
    return result;
}

void CropProducer::RecycleAfterConsumerStream(CropFrame* crop_frame, cudaStream_t consumer_stream)
{
    if (!crop_frame) {
        return;
    }
    ck(cudaEventRecord(crop_frame->recycle_event, consumer_stream));
    defer_crop_frame_recycle(crop_frame);
}

void CropProducer::QueryCopyTiming(CropFrame* crop_frame, CropProducerPerfSample* perf)
{
    if (!perf || !crop_copy_timing_enabled_ || !crop_frame ||
        !crop_frame->crop_copy_start_event || !crop_frame->crop_copy_stop_event) {
        return;
    }

    cudaError_t copy_done = cudaEventQuery(crop_frame->crop_copy_stop_event);
    if (copy_done == cudaSuccess) {
        float copy_gpu_ms = 0.0f;
        cudaError_t elapsed_status = cudaEventElapsedTime(
            &copy_gpu_ms,
            crop_frame->crop_copy_start_event,
            crop_frame->crop_copy_stop_event);
        if (elapsed_status == cudaSuccess) {
            perf->crop_copy_gpu_ms = static_cast<double>(copy_gpu_ms);
        } else {
            std::cerr << "[CropProducer] Crop copy timing failed for camera "
                      << camera_params_->camera_serial
                      << " frame " << crop_frame->frame.local_frame_id
                      << ": " << cudaGetErrorString(elapsed_status) << std::endl;
            cudaGetLastError();
        }
    } else if (copy_done != cudaErrorNotReady) {
        std::cerr << "[CropProducer] Crop copy timing query failed for camera "
                  << camera_params_->camera_serial
                  << " frame " << crop_frame->frame.local_frame_id
                  << ": " << cudaGetErrorString(copy_done) << std::endl;
        cudaGetLastError();
    }
}

void CropProducer::DrainPending(bool synchronize_all)
{
    drain_pending_source_releases(synchronize_all);
    drain_pending_crop_frames(synchronize_all);
}

bool CropProducer::DrainReady()
{
    DrainPending(false);
    return pending_source_release_count_.load(std::memory_order_relaxed) == 0 &&
           pending_crop_frame_recycle_count_.load(std::memory_order_relaxed) == 0;
}
