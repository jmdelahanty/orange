#include "spatial_snapshot_worker.h"

#include "worker_entry_release.h"

#include <cuda_runtime.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

constexpr uint32_t kMaxSpatialSnapshotAverageFrames = 256;

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
    // Results are delivered only through PopCompletedSnapshot(). Source
    // WORKER_ENTRY pointers are never forwarded to the base output queue.
}

bool SpatialSnapshotWorker::RequestSnapshot(
    const std::string& operation_id,
    uint64_t* request_id_out,
    std::string* error_out,
    uint32_t frame_count)
{
    return request_snapshot(
        operation_id,
        request_id_out,
        error_out,
        frame_count,
        SpatialSnapshotRepresentation::kRgba8);
}

bool SpatialSnapshotWorker::RequestNativeSnapshot(
    const std::string& operation_id,
    uint64_t* request_id_out,
    std::string* error_out)
{
    return request_snapshot(
        operation_id,
        request_id_out,
        error_out,
        1,
        SpatialSnapshotRepresentation::kNativeBytes);
}

bool SpatialSnapshotWorker::request_snapshot(
    const std::string& operation_id,
    uint64_t* request_id_out,
    std::string* error_out,
    uint32_t frame_count,
    const SpatialSnapshotRepresentation representation)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pending_ || in_flight_ || average_accumulator_.request_id != 0) {
        if (error_out) {
            *error_out = "A full-resolution stream snapshot is already pending for this camera.";
        }
        return false;
    }

    pending_ = true;
    pending_request_.request_id = ++next_request_id_;
    pending_request_.operation_id =
        operation_id.empty() ? "spatial_layout_full_resolution_stream_snapshot" : operation_id;
    pending_request_.target_frame_count =
        std::clamp<uint32_t>(frame_count, 1u, kMaxSpatialSnapshotAverageFrames);
    pending_request_.representation = representation;
    if (request_id_out) {
        *request_id_out = pending_request_.request_id;
    }
    request_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SpatialSnapshotWorker::HasPendingRequest() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return pending_ && !in_flight_;
}

bool SpatialSnapshotWorker::TryClaimNextFrame()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!pending_ || in_flight_) {
        return false;
    }

    in_flight_ = true;
    pending_ = false;
    in_flight_request_ = pending_request_;
    pending_request_ = ClaimedRequest{};
    return true;
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
    return in_flight_request_;
}

void SpatialSnapshotWorker::reset_active_request_locked()
{
    pending_ = false;
    in_flight_ = false;
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
        return true;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
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
        reset_active_request_locked();
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
        pending_ = true;
        in_flight_ = false;
        pending_request_ = request;
        in_flight_request_ = ClaimedRequest{};
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
    reset_active_request_locked();
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

bool SpatialSnapshotWorker::copy_entry_to_native(
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

    cudaEvent_t* ready_event =
        const_cast<WORKER_ENTRY&>(entry).delayed_consumer_event();
    if (ready_event != nullptr && *ready_event != nullptr) {
        const cudaError_t event_status = cudaEventSynchronize(*ready_event);
        if (event_status != cudaSuccess) {
            if (error_out) {
                *error_out =
                    cuda_error_string("cudaEventSynchronize", event_status);
            }
            cudaGetLastError();
            return false;
        }
    }

    size_t byte_count = entry.source_buffer_bytes;
    if (byte_count == 0) {
        byte_count = spatial_snapshot_frame_byte_count(
            entry.pixelFormat, entry.width, entry.height);
    }
    if (byte_count == 0) {
        if (error_out) {
            *error_out =
                "Unsupported pixel format for native full-resolution stream snapshot.";
        }
        return false;
    }

    result->native_bytes.resize(byte_count);
    cudaPointerAttributes attrs{};
    const cudaError_t attr_status = cudaPointerGetAttributes(&attrs, source);
    if (attr_status == cudaSuccess && attrs.type == cudaMemoryTypeDevice) {
        const int device = entry.image_gpu_id >= 0
                               ? entry.image_gpu_id
                               : (camera_params_ ? camera_params_->gpu_id
                                                 : attrs.device);
        if (device >= 0) {
            const cudaError_t set_status = cudaSetDevice(device);
            if (set_status != cudaSuccess) {
                if (error_out) {
                    *error_out = cuda_error_string("cudaSetDevice", set_status);
                }
                cudaGetLastError();
                result->native_bytes.clear();
                return false;
            }
        }
        const cudaError_t copy_status = cudaMemcpy(
            result->native_bytes.data(),
            source,
            byte_count,
            cudaMemcpyDeviceToHost);
        if (copy_status != cudaSuccess) {
            if (error_out) {
                *error_out = cuda_error_string(
                    "cudaMemcpy(DeviceToHost)", copy_status);
            }
            cudaGetLastError();
            result->native_bytes.clear();
            return false;
        }
    } else {
        if (attr_status != cudaSuccess) {
            cudaGetLastError();
        }
        std::memcpy(result->native_bytes.data(), source, byte_count);
    }

    result->capture_mode = "full_resolution_native_stream_snapshot";
    result->capture_representation = "native_bytes";
    result->width = entry.width;
    result->height = entry.height;
    result->pixel_format = entry.pixelFormat;
    result->local_frame_id = entry.frame_id;
    result->camera_frame_id = entry.camera_frame_id;
    result->recording_frame_id = entry.recording_frame_id;
    result->camera_timestamp_ns = entry.timestamp;
    result->timestamp_sys_ns = entry.timestamp_sys;
    result->requested_frame_count = 1;
    result->completed_frame_count = 1;
    result->first_local_frame_id = entry.frame_id;
    result->last_local_frame_id = entry.frame_id;
    result->first_camera_frame_id = entry.camera_frame_id;
    result->last_camera_frame_id = entry.camera_frame_id;
    return true;
}

bool SpatialSnapshotWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    if (entry == nullptr) {
        return false;
    }

    const WorkerEntryReleaseContext release_context{
        camera_params_ ? camera_params_->camera_serial.c_str() : nullptr,
        "spatial_snapshot"};
    ClaimedRequest request;
    SpatialSnapshotResult frame_result;
    std::string error;
    {
        // Own the retained acquisition reference before taking locks or
        // copying strings. Release it immediately after the source copy so
        // CPU accumulation/logging cannot hold a camera or ring entry.
        WorkerEntryRefGuard source_guard(
            recycle_queue_,
            entry,
            release_context,
            true);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            request = current_claimed_request_locked();
        }
        frame_result.request_id = request.request_id;
        frame_result.operation_id = request.operation_id;
        frame_result.camera_serial =
            camera_params_ ? camera_params_->camera_serial : "";
        frame_result.requested_frame_count =
            std::max<uint32_t>(1u, request.target_frame_count);
        frame_result.ok =
            request.representation ==
                    SpatialSnapshotRepresentation::kNativeBytes
                ? copy_entry_to_native(*entry, &frame_result, &error)
                : copy_entry_to_rgba(*entry, &frame_result, &error);
        if (!frame_result.ok) {
            frame_result.error = error.empty()
                ? "Full-resolution stream snapshot failed."
                : error;
        }
    }

    SpatialSnapshotResult completed_result;
    bool completed = true;
    if (frame_result.ok &&
        request.representation == SpatialSnapshotRepresentation::kRgba8) {
        completed = accumulate_frame_or_complete(
            request,
            frame_result,
            &completed_result,
            &error);
    } else if (frame_result.ok) {
        completed_result = std::move(frame_result);
    } else {
        completed_result = std::move(frame_result);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            reset_active_request_locked();
        }
    }

    if (!completed) {
        return false;
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
    return false;
}
