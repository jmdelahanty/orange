#include "spatial_snapshot_worker.h"

#include "worker_entry_release.h"

#include <cuda_runtime.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

constexpr uint32_t kMaxSpatialSnapshotAverageFrames = 256;

struct OutstandingFrameGuard {
    std::atomic<uint32_t>* count = nullptr;
    ~OutstandingFrameGuard()
    {
        if (count != nullptr) {
            count->fetch_sub(1, std::memory_order_release);
        }
    }
};

size_t spatial_snapshot_frame_byte_count(int pixel_type, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    switch (pixel_type) {
        case GVSP_PIX_MONO8:
        case GVSP_PIX_BAYRG8:
        case GVSP_PIX_BAYGB8:
            return pixel_count;
        case GVSP_PIX_RGB8:
        case GVSP_PIX_BGR8:
            return pixel_count * 3u;
        default:
            return 0;
    }
}

bool convert_snapshot_bytes_to_rgba(
    int pixel_type,
    int width,
    int height,
    const std::vector<unsigned char>& bytes,
    std::vector<unsigned char>* rgba_out,
    std::string* error_out)
{
    if (rgba_out == nullptr || width <= 0 || height <= 0 || bytes.empty()) {
        if (error_out) {
            *error_out = "Invalid full-resolution snapshot buffer.";
        }
        return false;
    }

    const unsigned char* data = bytes.data();
    try {
        cv::Mat rgba;
        switch (pixel_type) {
            case GVSP_PIX_MONO8: {
                cv::Mat gray(height, width, CV_8UC1, const_cast<unsigned char*>(data));
                cv::cvtColor(gray, rgba, cv::COLOR_GRAY2RGBA);
                break;
            }
            case GVSP_PIX_BAYRG8: {
                cv::Mat raw(height, width, CV_8UC1, const_cast<unsigned char*>(data));
                cv::cvtColor(raw, rgba, cv::COLOR_BayerRG2RGBA);
                break;
            }
            case GVSP_PIX_BAYGB8: {
                cv::Mat raw(height, width, CV_8UC1, const_cast<unsigned char*>(data));
                cv::cvtColor(raw, rgba, cv::COLOR_BayerGB2RGBA);
                break;
            }
            case GVSP_PIX_RGB8: {
                cv::Mat rgb(height, width, CV_8UC3, const_cast<unsigned char*>(data));
                cv::cvtColor(rgb, rgba, cv::COLOR_RGB2RGBA);
                break;
            }
            case GVSP_PIX_BGR8: {
                cv::Mat bgr(height, width, CV_8UC3, const_cast<unsigned char*>(data));
                cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
                break;
            }
            default:
                if (error_out) {
                    *error_out = "Unsupported pixel format for full-resolution stream snapshot.";
                }
                return false;
        }
        rgba_out->assign(rgba.data, rgba.data + rgba.total() * rgba.elemSize());
        return true;
    } catch (const cv::Exception& ex) {
        if (error_out) {
            *error_out = ex.what();
        }
        return false;
    }
}

std::string cuda_error_string(const char* operation, cudaError_t status)
{
    std::ostringstream oss;
    oss << operation << " failed: " << cudaGetErrorString(status);
    return oss.str();
}

} // namespace

SpatialSnapshotWorker::SpatialSnapshotWorker(
    const char* name,
    CameraParams* camera_params,
    SafeQueue<WORKER_ENTRY*>& recycle_queue)
    : CThreadWorker<WORKER_ENTRY>(name),
      camera_params_(camera_params),
      recycle_queue_(&recycle_queue)
{
    // Preview/diagnostic-only output queue: WorkerFunction returns true after
    // handling a snapshot frame, pushing the entry pointer onto the
    // base-class output queue, but results are delivered via
    // complete_result() and nothing in-tree drains the queue. Dropping is
    // always acceptable. No ReleaseDroppedQueueOutEntry override is needed:
    // WorkerFunction's WorkerEntryRefGuard releases the entry's pool
    // reference BEFORE returning true, so pointers on the output queue own no
    // pool reference and the base-class no-op release is correct.
    SetMaxQueueOutSize(8);
}

bool SpatialSnapshotWorker::RequestSnapshot(
    const std::string& operation_id,
    uint64_t* request_id_out,
    std::string* error_out,
    uint32_t frame_count,
    SpatialSnapshotAlignmentPlan alignment_plan)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pending_ || in_flight_ || average_accumulator_.request_id != 0 ||
        outstanding_frame_count_.load(std::memory_order_acquire) != 0) {
        if (error_out) {
            *error_out = "A full-resolution stream snapshot is already pending for this camera.";
        }
        return false;
    }

    pending_ = true;
    claimed_frame_count_ = 0;
    pending_request_.request_id = ++next_request_id_;
    pending_request_.operation_id =
        operation_id.empty() ? "spatial_layout_full_resolution_stream_snapshot" : operation_id;
    pending_request_.target_frame_count =
        std::clamp<uint32_t>(frame_count, 1u, kMaxSpatialSnapshotAverageFrames);
    pending_request_.alignment_plan = alignment_plan;
    if (request_id_out) {
        *request_id_out = pending_request_.request_id;
    }
    request_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SpatialSnapshotWorker::HasPendingRequest(uint64_t camera_timestamp_ns) const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!pending_ || in_flight_) {
        return false;
    }
    if (!pending_request_.alignment_plan.enabled()) {
        return true;
    }
    uint64_t expected_timestamp_ns = 0;
    if (!spatial_snapshot_expected_frame_timestamp(
            pending_request_.alignment_plan,
            claimed_frame_count_,
            &expected_timestamp_ns)) {
        return true;  // Claim a frame and fail explicitly.
    }
    return spatial_snapshot_frame_decision(
               camera_timestamp_ns,
               expected_timestamp_ns,
               pending_request_.alignment_plan.tolerance_ns) !=
           SpatialSnapshotFrameDecision::wait;
}

bool SpatialSnapshotWorker::TryClaimNextFrame(uint64_t camera_timestamp_ns)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!pending_ || in_flight_) {
        return false;
    }

    uint64_t expected_timestamp_ns = 0;
    if (pending_request_.alignment_plan.enabled()) {
        if (!spatial_snapshot_expected_frame_timestamp(
                pending_request_.alignment_plan,
                claimed_frame_count_,
                &expected_timestamp_ns)) {
            // Claim the frame so WorkerFunction can terminate the request with
            // an explicit error instead of leaving it pending indefinitely.
            expected_timestamp_ns = std::numeric_limits<uint64_t>::max();
        } else if (spatial_snapshot_frame_decision(
                       camera_timestamp_ns,
                       expected_timestamp_ns,
                       pending_request_.alignment_plan.tolerance_ns) ==
                   SpatialSnapshotFrameDecision::wait) {
            return false;
        }
    }

    in_flight_ = true;
    in_flight_request_ = pending_request_;
    in_flight_request_.expected_frame_timestamp_ns = expected_timestamp_ns;
    if (pending_request_.alignment_plan.enabled()) {
        ++claimed_frame_count_;
        pending_ = claimed_frame_count_ < pending_request_.target_frame_count;
    } else {
        pending_ = false;
        pending_request_ = ClaimedRequest{};
    }
    outstanding_frame_count_.fetch_add(1, std::memory_order_release);
    return true;
}

bool SpatialSnapshotWorker::UndoClaimAfterEnqueueFailure()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (in_flight_request_.alignment_plan.enabled() && claimed_frame_count_ > 0) {
        --claimed_frame_count_;
        pending_ = true;
        in_flight_ = false;
        in_flight_request_ = ClaimedRequest{};
        outstanding_frame_count_.fetch_sub(1, std::memory_order_release);
        enqueue_rejected_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void SpatialSnapshotWorker::CompleteClaimedRequestWithError(const std::string& error)
{
    ClaimedRequest request;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!in_flight_) {
            return;
        }
        request = in_flight_request_;
        in_flight_ = false;
        in_flight_request_ = ClaimedRequest{};
        outstanding_frame_count_.fetch_sub(1, std::memory_order_release);
    }

    SpatialSnapshotResult result;
    result.ok = false;
    result.request_id = request.request_id;
    result.operation_id = request.operation_id;
    result.camera_serial = camera_params_ ? camera_params_->camera_serial : "";
    result.error = error;
    enqueue_rejected_count_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        reset_active_request_locked();
    }
    complete_result(std::move(result));
}

bool SpatialSnapshotWorker::PopCompletedSnapshot(SpatialSnapshotResult* result_out)
{
    if (result_out == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!has_completed_result_) {
        return false;
    }
    *result_out = std::move(completed_result_);
    completed_result_ = SpatialSnapshotResult{};
    has_completed_result_ = false;
    return true;
}

SpatialSnapshotWorker::ClaimedRequest
SpatialSnapshotWorker::current_claimed_request_locked() const
{
    // TryClaimNextFrame freezes the exact schedule entry in this request.
    // Do not derive it again from the image accumulator: that accumulator is
    // worker-owned and deliberately updated without holding state_mutex_.
    return in_flight_request_;
}

void SpatialSnapshotWorker::reset_active_request_locked()
{
    pending_ = false;
    in_flight_ = false;
    claimed_frame_count_ = 0;
    pending_request_ = ClaimedRequest{};
    in_flight_request_ = ClaimedRequest{};
    average_accumulator_ = AverageAccumulator{};
}

void SpatialSnapshotWorker::complete_result(SpatialSnapshotResult result)
{
    if (result.ok) {
        completed_count_.fetch_add(1, std::memory_order_relaxed);
    } else {
        failed_count_.fetch_add(1, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    completed_result_ = std::move(result);
    has_completed_result_ = true;
    in_flight_ = false;
    in_flight_request_ = ClaimedRequest{};
}

bool SpatialSnapshotWorker::accumulate_frame_or_complete(
    const ClaimedRequest& request,
    const SpatialSnapshotResult& frame,
    SpatialSnapshotResult* completed_result,
    std::string* error_out)
{
    if (completed_result == nullptr) {
        if (error_out) {
            *error_out = "Snapshot completion destination is null.";
        }
        return false;
    }
    if (request.target_frame_count <= 1) {
        *completed_result = frame;
        completed_result->capture_mode = "full_resolution_stream_snapshot";
        completed_result->requested_frame_count = 1;
        completed_result->completed_frame_count = 1;
        completed_result->first_local_frame_id = frame.local_frame_id;
        completed_result->last_local_frame_id = frame.local_frame_id;
        completed_result->first_camera_frame_id = frame.camera_frame_id;
        completed_result->last_camera_frame_id = frame.camera_frame_id;
        if (request.alignment_plan.enabled()) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            reset_active_request_locked();
        }
        return true;
    }

    // The potentially hundreds-of-megabytes accumulation below is owned by
    // this worker thread.  In particular, do not hold state_mutex_ while
    // allocating or walking the image: the GUI completion poll and the camera
    // acquisition path both need that mutex for short request-state checks.
    if (average_accumulator_.request_id == 0) {
        average_accumulator_.request_id = request.request_id;
        average_accumulator_.operation_id = request.operation_id;
        average_accumulator_.target_frame_count = request.target_frame_count;
        average_accumulator_.width = frame.width;
        average_accumulator_.height = frame.height;
        average_accumulator_.pixel_format = frame.pixel_format;
        average_accumulator_.rgba_sums.assign(frame.rgba.size(), 0u);
    }

    if (average_accumulator_.request_id != request.request_id ||
        average_accumulator_.width != frame.width ||
        average_accumulator_.height != frame.height ||
        average_accumulator_.pixel_format != frame.pixel_format ||
        average_accumulator_.rgba_sums.size() != frame.rgba.size()) {
        completed_result->ok = false;
        completed_result->request_id = request.request_id;
        completed_result->operation_id = request.operation_id;
        completed_result->camera_serial = camera_params_ ? camera_params_->camera_serial : "";
        completed_result->error =
            "Averaged full-resolution snapshot frame shape changed during capture.";
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            reset_active_request_locked();
        }
        if (error_out) {
            *error_out = completed_result->error;
        }
        return true;
    }

    if (average_accumulator_.captured_frame_count == 0) {
        average_accumulator_.first_local_frame_id = frame.local_frame_id;
        average_accumulator_.first_camera_frame_id = frame.camera_frame_id;
        average_accumulator_.first_camera_timestamp_ns = frame.camera_timestamp_ns;
        average_accumulator_.first_timestamp_sys_ns = frame.timestamp_sys_ns;
    }
    for (size_t i = 0; i < frame.rgba.size(); ++i) {
        average_accumulator_.rgba_sums[i] += static_cast<uint32_t>(frame.rgba[i]);
    }
    average_accumulator_.captured_frame_count++;
    average_accumulator_.last_local_frame_id = frame.local_frame_id;
    average_accumulator_.last_camera_frame_id = frame.camera_frame_id;
    average_accumulator_.last_camera_timestamp_ns = frame.camera_timestamp_ns;
    average_accumulator_.last_timestamp_sys_ns = frame.timestamp_sys_ns;

    if (average_accumulator_.captured_frame_count < average_accumulator_.target_frame_count) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (request.alignment_plan.enabled()) {
                // Keep the original shared schedule pending, but do not admit
                // its next timestamp until this full-resolution copy and
                // accumulation have completed.
                in_flight_ = false;
                in_flight_request_ = ClaimedRequest{};
            } else {
                pending_ = true;
                in_flight_ = false;
                pending_request_ = request;
                in_flight_request_ = ClaimedRequest{};
            }
        }
        return false;
    }

    completed_result->ok = true;
    completed_result->request_id = request.request_id;
    completed_result->operation_id = request.operation_id;
    completed_result->camera_serial = camera_params_ ? camera_params_->camera_serial : "";
    completed_result->capture_mode = "temporal_mean_stream_frames_v1";
    completed_result->source_array_role = "images_full";
    completed_result->width = average_accumulator_.width;
    completed_result->height = average_accumulator_.height;
    completed_result->pixel_format = average_accumulator_.pixel_format;
    completed_result->local_frame_id = average_accumulator_.last_local_frame_id;
    completed_result->camera_frame_id = average_accumulator_.last_camera_frame_id;
    completed_result->camera_timestamp_ns = average_accumulator_.last_camera_timestamp_ns;
    completed_result->timestamp_sys_ns = average_accumulator_.last_timestamp_sys_ns;
    completed_result->requested_frame_count = average_accumulator_.target_frame_count;
    completed_result->completed_frame_count = average_accumulator_.captured_frame_count;
    completed_result->first_local_frame_id = average_accumulator_.first_local_frame_id;
    completed_result->last_local_frame_id = average_accumulator_.last_local_frame_id;
    completed_result->first_camera_frame_id = average_accumulator_.first_camera_frame_id;
    completed_result->last_camera_frame_id = average_accumulator_.last_camera_frame_id;
    completed_result->rgba.resize(average_accumulator_.rgba_sums.size());
    for (size_t i = 0; i < average_accumulator_.rgba_sums.size(); ++i) {
        const uint32_t rounded =
            average_accumulator_.rgba_sums[i] +
            average_accumulator_.captured_frame_count / 2u;
        completed_result->rgba[i] = static_cast<unsigned char>(
            std::min<uint32_t>(
                255u,
                rounded / std::max<uint32_t>(1u, average_accumulator_.captured_frame_count)));
    }
    // Move the large accumulator out while holding the state lock, then let
    // its storage be released after the lock is gone.  Moving the vector is
    // constant-time; freeing hundreds of megabytes need not block the GUI or
    // acquisition thread either.
    AverageAccumulator retired_accumulator;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        retired_accumulator = std::move(average_accumulator_);
        reset_active_request_locked();
    }
    return true;
}

bool SpatialSnapshotWorker::copy_entry_to_rgba(
    const WORKER_ENTRY& entry,
    SpatialSnapshotResult* result,
    std::string* error_out)
{
    if (result == nullptr) {
        if (error_out) {
            *error_out = "Snapshot result pointer is null.";
        }
        return false;
    }
    if (entry.width <= 0 || entry.height <= 0) {
        if (error_out) {
            *error_out = "Snapshot source frame dimensions are invalid.";
        }
        return false;
    }

    const unsigned char* source = entry.delayed_consumer_image();
    if (source == nullptr) {
        if (error_out) {
            *error_out = "Snapshot source frame pointer is null.";
        }
        return false;
    }

    cudaEvent_t* ready_event = const_cast<WORKER_ENTRY&>(entry).delayed_consumer_event();
    if (ready_event != nullptr && *ready_event != nullptr) {
        const cudaError_t event_status = cudaEventSynchronize(*ready_event);
        if (event_status != cudaSuccess) {
            if (error_out) {
                *error_out = cuda_error_string("cudaEventSynchronize", event_status);
            }
            cudaGetLastError();
            return false;
        }
    }

    size_t byte_count = entry.source_buffer_bytes;
    if (byte_count == 0) {
        byte_count = spatial_snapshot_frame_byte_count(entry.pixelFormat, entry.width, entry.height);
    }
    if (byte_count == 0) {
        if (error_out) {
            *error_out = "Unsupported pixel format for full-resolution stream snapshot.";
        }
        return false;
    }

    std::vector<unsigned char> host_bytes(byte_count);
    cudaPointerAttributes attrs{};
    const cudaError_t attr_status = cudaPointerGetAttributes(&attrs, source);
    if (attr_status == cudaSuccess && attrs.type == cudaMemoryTypeDevice) {
        const int device = entry.image_gpu_id >= 0
                               ? entry.image_gpu_id
                               : (camera_params_ ? camera_params_->gpu_id : attrs.device);
        if (device >= 0) {
            const cudaError_t set_status = cudaSetDevice(device);
            if (set_status != cudaSuccess) {
                if (error_out) {
                    *error_out = cuda_error_string("cudaSetDevice", set_status);
                }
                cudaGetLastError();
                return false;
            }
        }
        const cudaError_t copy_status =
            cudaMemcpy(host_bytes.data(), source, byte_count, cudaMemcpyDeviceToHost);
        if (copy_status != cudaSuccess) {
            if (error_out) {
                *error_out = cuda_error_string("cudaMemcpy(DeviceToHost)", copy_status);
            }
            cudaGetLastError();
            return false;
        }
    } else {
        if (attr_status != cudaSuccess) {
            cudaGetLastError();
        }
        std::memcpy(host_bytes.data(), source, byte_count);
    }

    result->width = entry.width;
    result->height = entry.height;
    result->pixel_format = entry.pixelFormat;
    result->local_frame_id = entry.frame_id;
    result->camera_frame_id = entry.camera_frame_id;
    result->recording_frame_id = entry.recording_frame_id;
    result->camera_timestamp_ns = entry.timestamp;
    result->timestamp_sys_ns = entry.timestamp_sys;
    return convert_snapshot_bytes_to_rgba(
        entry.pixelFormat,
        entry.width,
        entry.height,
        host_bytes,
        &result->rgba,
        error_out);
}

bool SpatialSnapshotWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    if (entry == nullptr) {
        return false;
    }

    const WorkerEntryReleaseContext release_context{
        camera_params_ ? camera_params_->camera_serial.c_str() : nullptr,
        "spatial_snapshot"};
    const OutstandingFrameGuard outstanding_guard{&outstanding_frame_count_};

    ClaimedRequest request;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        request = current_claimed_request_locked();
    }
    if (request.request_id == 0) {
        WorkerEntryRefGuard source_guard(
            recycle_queue_, entry, release_context, true);
        return false;  // A queued frame from an already failed request.
    }

    SpatialSnapshotResult frame_result;
    frame_result.request_id = request.request_id;
    frame_result.operation_id = request.operation_id;
    frame_result.camera_serial = camera_params_ ? camera_params_->camera_serial : "";
    frame_result.requested_frame_count = std::max<uint32_t>(1u, request.target_frame_count);

    std::string error;
    {
        WorkerEntryRefGuard source_guard(
            recycle_queue_,
            entry,
            release_context,
            true);
        if (request.alignment_plan.enabled() &&
            spatial_snapshot_frame_decision(
                entry->timestamp,
                request.expected_frame_timestamp_ns,
                request.alignment_plan.tolerance_ns) !=
                SpatialSnapshotFrameDecision::accept) {
            std::ostringstream message;
            message << "PTP grouped snapshot missed its common frame: expected="
                    << request.expected_frame_timestamp_ns
                    << " observed=" << entry->timestamp
                    << " tolerance_ns=" << request.alignment_plan.tolerance_ns;
            error = message.str();
            frame_result.ok = false;
        } else {
            frame_result.ok = copy_entry_to_rgba(*entry, &frame_result, &error);
        }
        if (!frame_result.ok) {
            frame_result.error = error.empty() ? "Full-resolution stream snapshot failed." : error;
        }
    }

    SpatialSnapshotResult completed_result;
    bool completed = true;
    if (frame_result.ok) {
        completed = accumulate_frame_or_complete(
            request,
            frame_result,
            &completed_result,
            &error);
    } else {
        completed_result = std::move(frame_result);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            reset_active_request_locked();
        }
    }

    if (!completed) {
        return true;
    }

    if (completed_result.ok) {
        if (completed_result.completed_frame_count > 1) {
            std::cout << "[SpatialSnapshotWorker] Captured averaged full-resolution stream snapshot"
                      << " cam=" << completed_result.camera_serial
                      << " frames=" << completed_result.completed_frame_count
                      << " local_frame_range=" << completed_result.first_local_frame_id
                      << "-" << completed_result.last_local_frame_id
                      << " camera_frame_range=" << completed_result.first_camera_frame_id
                      << "-" << completed_result.last_camera_frame_id
                      << " size=" << completed_result.width << "x" << completed_result.height
                      << std::endl;
        } else {
            std::cout << "[SpatialSnapshotWorker] Captured full-resolution stream snapshot"
                      << " cam=" << completed_result.camera_serial
                      << " frame=" << completed_result.local_frame_id
                      << " camera_frame=" << completed_result.camera_frame_id
                      << " size=" << completed_result.width << "x" << completed_result.height
                      << std::endl;
        }
    } else {
        std::cerr << "[SpatialSnapshotWorker] Snapshot failed"
                  << " cam=" << completed_result.camera_serial
                  << " frame=" << completed_result.local_frame_id
                  << " error=" << completed_result.error
                  << std::endl;
    }

    complete_result(std::move(completed_result));
    return true;
}
