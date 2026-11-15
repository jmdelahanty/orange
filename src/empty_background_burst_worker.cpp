// src/empty_background_burst_worker.cpp (see empty_background_burst_worker.h)
#include "empty_background_burst_worker.h"

#include "empty_background_median.h"
#include "worker_entry_release.h"

#include <cuda_runtime.h>

#include <chrono>
#include <cstring>
#include <sstream>

namespace {

std::uint64_t now_ns()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::string cuda_failure(const std::string& what, cudaError_t status)
{
    return what + " failed: " + cudaGetErrorString(status);
}

void lower_to(std::atomic<int>* target, int value)
{
    // Written by the acquisition thread only; read after the burst.
    const int current = target->load(std::memory_order_relaxed);
    if (current < 0 || value < current) target->store(value, std::memory_order_relaxed);
}

}  // namespace

EmptyBackgroundBurstWorker::EmptyBackgroundBurstWorker(const char* name, CameraParams* camera_params,
                                                       SafeQueue<WORKER_ENTRY*>& recycle_queue)
    : CThreadWorker<WORKER_ENTRY>(name), camera_params_(camera_params), recycle_queue_(&recycle_queue)
{
}

EmptyBackgroundBurstWorker::~EmptyBackgroundBurstWorker()
{
    ReleaseReservation();
}

bool EmptyBackgroundBurstWorker::Reserve(std::string* error_out)
{
    // The caller's current device is restored: stream start runs this on a
    // thread that has its own device (the GUI thread's OpenGL interop GPU).
    int caller_device = -1;
    const bool restore_device = cudaGetDevice(&caller_device) == cudaSuccess;
    struct RestoreDevice {
        bool active;
        int device;
        ~RestoreDevice() { if (active) cudaSetDevice(device); }
    } restore{restore_device, caller_device};
    auto fail = [&](const std::string& reason) {
        ReleaseReservation();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reserve_error_ = reason;
        }
        if (error_out) *error_out = reason;
        return false;
    };
    if (reserved_) return true;
    if (!camera_params_) return fail("no camera parameters");
    if (camera_params_->pixel_format != "Mono8")
        return fail("pixel format " + camera_params_->pixel_format + " is not Mono8");
    const std::uint64_t pixels = static_cast<std::uint64_t>(camera_params_->width) * camera_params_->height;
    if (pixels == 0 || pixels > orange::empty_background::kMaxFrameBytes)
        return fail("raster " + std::to_string(camera_params_->width) + " x " + std::to_string(camera_params_->height) +
                    " is outside the burst limits");
    gpu_id_ = camera_params_->gpu_id;
    width_ = static_cast<int>(camera_params_->width);
    height_ = static_cast<int>(camera_params_->height);
    frame_bytes_ = static_cast<std::size_t>(pixels);

    cudaError_t status = cudaSetDevice(gpu_id_);
    if (status != cudaSuccess) return fail(cuda_failure("cudaSetDevice", status));
    const std::size_t slot_bytes = static_cast<std::size_t>(kFrames) * frame_bytes_;
    if ((status = cudaMalloc(reinterpret_cast<void**>(&d_slots_), slot_bytes)) != cudaSuccess)
        return fail(cuda_failure("cudaMalloc burst staging (" + std::to_string(slot_bytes) + " bytes)", status));
    if ((status = cudaMalloc(reinterpret_cast<void**>(&d_median_), frame_bytes_)) != cudaSuccess)
        return fail(cuda_failure("cudaMalloc median", status));
    if ((status = cudaMalloc(reinterpret_cast<void**>(&d_saturated_), sizeof(unsigned long long))) != cudaSuccess)
        return fail(cuda_failure("cudaMalloc saturated count", status));
    // The median plus the saturated count, pinned, so the device-to-host copy is one DMA.
    if ((status = cudaHostAlloc(reinterpret_cast<void**>(&h_median_), frame_bytes_ + sizeof(unsigned long long),
                                cudaHostAllocDefault)) != cudaSuccess)
        return fail(cuda_failure("cudaHostAlloc median", status));
    int least_priority = 0, greatest_priority = 0;
    if ((status = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority)) != cudaSuccess)
        return fail(cuda_failure("cudaDeviceGetStreamPriorityRange", status));
    // The lowest priority: a burst copy yields to detect, pose and the recorder copies.
    if ((status = cudaStreamCreateWithPriority(&stream_, cudaStreamNonBlocking, least_priority)) != cudaSuccess)
        return fail(cuda_failure("cudaStreamCreateWithPriority", status));
    // Blocking sync: this thread sleeps on the copy instead of spinning a CPU.
    if ((status = cudaEventCreateWithFlags(&copy_done_, cudaEventDisableTiming | cudaEventBlockingSync)) !=
        cudaSuccess)
        return fail(cuda_failure("cudaEventCreateWithFlags", status));
    reserved_bytes_ = slot_bytes + frame_bytes_ + sizeof(unsigned long long);
    reserved_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reserve_error_.clear();
    }
    return true;
}

void EmptyBackgroundBurstWorker::ReleaseReservation() noexcept
{
    if (!d_slots_ && !d_median_ && !d_saturated_ && !h_median_ && !stream_ && !copy_done_) {
        reserved_ = false;
        return;
    }
    int caller_device = -1;
    const bool restore_device = cudaGetDevice(&caller_device) == cudaSuccess;
    if (gpu_id_ >= 0) cudaSetDevice(gpu_id_);
    if (stream_) cudaStreamSynchronize(stream_);
    if (copy_done_) cudaEventDestroy(copy_done_);
    if (stream_) cudaStreamDestroy(stream_);
    if (h_median_) cudaFreeHost(h_median_);
    if (d_saturated_) cudaFree(d_saturated_);
    if (d_median_) cudaFree(d_median_);
    if (d_slots_) cudaFree(d_slots_);
    copy_done_ = nullptr;
    stream_ = nullptr;
    h_median_ = nullptr;
    d_saturated_ = nullptr;
    d_median_ = nullptr;
    d_slots_ = nullptr;
    reserved_bytes_ = 0;
    reserved_ = false;
    if (restore_device) cudaSetDevice(caller_device);
}

std::string EmptyBackgroundBurstWorker::reserve_error() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return reserve_error_;
}

bool EmptyBackgroundBurstWorker::Request(std::uint64_t* request_id_out, std::string* error_out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto refuse = [&](const std::string& reason) {
        if (error_out) *error_out = reason;
        return false;
    };
    if (!reserved_)
        return refuse(reserve_error_.empty() ? "no burst staging area was reserved at stream start"
                                             : "no burst staging area: " + reserve_error_);
    if (active_request_id_ != 0 || median_running_ || outstanding_.load(std::memory_order_acquire) != 0 ||
        has_result_)
        return refuse("a background burst is already outstanding for this camera");
    active_request_id_ = ++next_request_id_;
    active_failed_ = false;
    copied_ = 0;
    last_stream_frame_id_ = 0;
    building_ = EmptyBackgroundBurstResult{};
    building_.request_id = active_request_id_;
    building_.camera_serial = camera_params_ ? camera_params_->camera_serial : "";
    building_.width = width_;
    building_.height = height_;
    building_.gpu_id = gpu_id_;
    offer_failure_.clear();
    claimed_.store(0, std::memory_order_relaxed);
    min_free_entries_.store(-1, std::memory_order_relaxed);
    min_free_events_.store(-1, std::memory_order_relaxed);
    max_queue_depth_.store(0, std::memory_order_relaxed);
    first_offer_ns_.store(0, std::memory_order_relaxed);
    offer_failed_.store(false, std::memory_order_relaxed);
    claiming_.store(true, std::memory_order_release);
    if (request_id_out) *request_id_out = active_request_id_;
    return true;
}

void EmptyBackgroundBurstWorker::Abort(const std::string& reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    fail_locked(reason);
}

bool EmptyBackgroundBurstWorker::Busy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_request_id_ != 0 || median_running_ || outstanding_.load(std::memory_order_acquire) != 0;
}

bool EmptyBackgroundBurstWorker::PopResult(EmptyBackgroundBurstResult* out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_result_) return false;
    if (out) *out = std::move(result_);
    result_ = EmptyBackgroundBurstResult{};
    has_result_ = false;
    return true;
}

void EmptyBackgroundBurstWorker::OfferFrame(WORKER_ENTRY* entry, const bool recording,
                                            const int free_entries_available, const int free_events_available)
{
    if (!claiming_.load(std::memory_order_acquire) || entry == nullptr) return;
    if (recording) {
        claiming_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        fail_locked("a recording started during the burst");
        return;
    }
    const std::uint32_t index = claimed_.fetch_add(1, std::memory_order_acq_rel);
    if (index >= kFrames) {
        claiming_.store(false, std::memory_order_release);
        return;
    }
    if (index + 1 == kFrames) claiming_.store(false, std::memory_order_release);
    if (index == 0) first_offer_ns_.store(now_ns(), std::memory_order_relaxed);
    lower_to(&min_free_entries_, free_entries_available);
    lower_to(&min_free_events_, free_events_available);
    const std::uint32_t depth = outstanding_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (depth > max_queue_depth_.load(std::memory_order_relaxed))
        max_queue_depth_.store(depth, std::memory_order_relaxed);
    const bool queued = retain_and_try_enqueue_worker_entry(
        this, recycle_queue_, entry,
        WorkerEntryReleaseContext{camera_params_ ? camera_params_->camera_serial.c_str() : nullptr,
                                  "empty_background_burst"},
        kMaxQueuedFrames);
    if (!queued) {
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        claiming_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        fail_locked("burst frame " + std::to_string(index + 1) + " of 17 could not be queued (stream frame " +
                    std::to_string(entry->frame_id) + ")");
    }
}

void EmptyBackgroundBurstWorker::fail_locked(const std::string& reason)
{
    claiming_.store(false, std::memory_order_release);
    if (active_request_id_ == 0) return;
    if (!active_failed_) {
        active_failed_ = true;
        building_.failure_reason = reason.substr(0, 1024);
    }
    if (outstanding_.load(std::memory_order_acquire) == 0) complete_locked();
}

void EmptyBackgroundBurstWorker::complete_locked()
{
    building_.ok = !active_failed_ && copied_ == kFrames;
    if (!building_.ok && building_.failure_reason.empty())
        building_.failure_reason = "burst ended after " + std::to_string(copied_) + " of 17 frames";
    if (!building_.ok) building_.median = orange::empty_background::BurstMedian{};
    building_.min_free_entries = min_free_entries_.load(std::memory_order_relaxed);
    building_.min_free_events = min_free_events_.load(std::memory_order_relaxed);
    building_.max_queue_depth = max_queue_depth_.load(std::memory_order_relaxed);
    result_ = std::move(building_);
    building_ = EmptyBackgroundBurstResult{};
    has_result_ = true;
    active_request_id_ = 0;
}

bool EmptyBackgroundBurstWorker::copy_into_slot(const WORKER_ENTRY& entry, const std::uint32_t slot,
                                                std::string* error)
{
    auto fail = [&](const std::string& reason) {
        *error = reason;
        return false;
    };
    if (entry.width != width_ || entry.height != height_ || entry.pixelFormat != GVSP_PIX_MONO8)
        return fail("frame raster or pixel format differs from the reserved " + std::to_string(width_) + " x " +
                    std::to_string(height_) + " Mono8");
    if (entry.image_gpu_id != gpu_id_)
        return fail("frame is on GPU " + std::to_string(entry.image_gpu_id) + ", the staging area on GPU " +
                    std::to_string(gpu_id_));
    if (entry.source_buffer_bytes < frame_bytes_) return fail("frame buffer is smaller than the raster");
    // The owned copy only, never the camera's GPUDirect buffer (it may be requeued
    // independently of the entry's reference count).
    const bool analytics_owned = entry.has_analytics_owned_source();
    const bool ring_owned = !entry.gpu_direct_mode && entry.owns_memory && entry.d_image_pool != nullptr &&
                            entry.d_image == entry.d_image_pool && entry.event_ptr != nullptr &&
                            *entry.event_ptr != nullptr;
    if (!analytics_owned && !ring_owned) return fail("frame has no owned copy (GPUDirect buffer refused)");
    if (analytics_owned && !entry.wait_delayed_consumer_ready(200))
        return fail("the frame's owned copy was not issued within 200 ms");
    const cudaEvent_t ready = analytics_owned ? entry.analytics_ready_event : *entry.event_ptr;
    const std::uint8_t* source = analytics_owned ? entry.d_analytics_image : entry.d_image_pool;
    cudaError_t status = cudaSetDevice(gpu_id_);
    if (status != cudaSuccess) return fail(cuda_failure("cudaSetDevice", status));
    if ((status = cudaStreamWaitEvent(stream_, ready, 0)) != cudaSuccess)
        return fail(cuda_failure("cudaStreamWaitEvent", status));
    if ((status = cudaMemcpyAsync(d_slots_ + static_cast<std::size_t>(slot) * frame_bytes_, source, frame_bytes_,
                                  cudaMemcpyDeviceToDevice, stream_)) != cudaSuccess)
        return fail(cuda_failure("burst slot copy", status));
    if ((status = cudaEventRecord(copy_done_, stream_)) != cudaSuccess)
        return fail(cuda_failure("cudaEventRecord", status));
    if ((status = cudaEventSynchronize(copy_done_)) != cudaSuccess)
        return fail(cuda_failure("cudaEventSynchronize", status));
    return true;
}

bool EmptyBackgroundBurstWorker::run_median(const std::uint64_t request_id, std::string* error)
{
    const std::uint64_t started = now_ns();
    cudaError_t status = cudaSetDevice(gpu_id_);
    if (status == cudaSuccess)
        status = orange::empty_background::LaunchBurstMedian17(d_slots_, frame_bytes_, frame_bytes_, d_median_,
                                                               d_saturated_, stream_);
    if (status == cudaSuccess)
        status = cudaMemcpyAsync(h_median_, d_median_, frame_bytes_, cudaMemcpyDeviceToHost, stream_);
    if (status == cudaSuccess)
        status = cudaMemcpyAsync(h_median_ + frame_bytes_, d_saturated_, sizeof(unsigned long long),
                                 cudaMemcpyDeviceToHost, stream_);
    if (status == cudaSuccess) status = cudaEventRecord(copy_done_, stream_);
    if (status == cudaSuccess) status = cudaEventSynchronize(copy_done_);
    if (status != cudaSuccess) {
        *error = cuda_failure("burst median", status);
        return false;
    }
    unsigned long long saturated = 0;
    std::memcpy(&saturated, h_median_ + frame_bytes_, sizeof(saturated));
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_request_id_ != request_id) return true;  // aborted meanwhile; its result is already out
    building_.median.median.assign(h_median_, h_median_ + frame_bytes_);
    building_.median.any_frame_saturated_pixels = saturated;
    building_.median_ms = static_cast<double>(now_ns() - started) / 1e6;
    return true;
}

bool EmptyBackgroundBurstWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    if (entry == nullptr) return false;
    bool burst_complete = false;
    std::uint64_t request_id = 0;
    {
        // Owns the retained reference; released at the end of this scope, right
        // after the slot copy finished (or at once when the burst already failed).
        WorkerEntryRefGuard lease(recycle_queue_, entry,
                                  WorkerEntryReleaseContext{
                                      camera_params_ ? camera_params_->camera_serial.c_str() : nullptr,
                                      "empty_background_burst"},
                                  true);
        std::uint32_t slot = 0;
        bool copy = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_request_id_ != 0 && !active_failed_) {
                if (copied_ > 0 && entry->frame_id != last_stream_frame_id_ + 1) {
                    fail_locked("burst frames are not consecutive (stream frame " +
                                std::to_string(last_stream_frame_id_) + " then " + std::to_string(entry->frame_id) +
                                ")");
                } else {
                    slot = copied_;
                    copy = slot < kFrames;
                }
            }
        }
        if (copy) {
            std::string error;
            const bool ok = copy_into_slot(*entry, slot, &error);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!ok) {
                fail_locked(error);
            } else if (active_request_id_ != 0 && !active_failed_) {
                building_.source_frames.push_back({static_cast<std::uint64_t>(entry->frame_id), entry->camera_frame_id,
                                                   static_cast<std::uint64_t>(entry->timestamp),
                                                   static_cast<std::uint64_t>(entry->timestamp_sys)});
                last_stream_frame_id_ = entry->frame_id;
                ++copied_;
                if (copied_ == kFrames) {
                    const std::uint64_t first = first_offer_ns_.load(std::memory_order_relaxed);
                    building_.burst_ms = first ? static_cast<double>(now_ns() - first) / 1e6 : 0.0;
                    burst_complete = true;
                    request_id = active_request_id_;
                    median_running_ = true;
                }
            }
        }
    }
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    if (burst_complete) {
        // Every frame is released before the median runs: no pool entry waits on it.
        std::string error;
        const bool ok = run_median(request_id, &error);
        std::lock_guard<std::mutex> lock(mutex_);
        median_running_ = false;
        if (active_request_id_ != request_id) return false;
        if (!ok) fail_locked(error);
        else if (active_request_id_ != 0 && outstanding_.load(std::memory_order_acquire) == 0) complete_locked();
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_request_id_ != 0 && active_failed_ && outstanding_.load(std::memory_order_acquire) == 0)
        complete_locked();
    return false;
}
