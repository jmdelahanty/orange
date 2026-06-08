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
}

bool SpatialSnapshotWorker::RequestSnapshot(
    const std::string& operation_id,
    uint64_t* request_id_out,
    std::string* error_out)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pending_ || in_flight_) {
        if (error_out) {
            *error_out = "A full-resolution stream snapshot is already pending for this camera.";
        }
        return false;
    }

    pending_ = true;
    pending_request_.request_id = ++next_request_id_;
    pending_request_.operation_id =
        operation_id.empty() ? "spatial_layout_full_resolution_stream_snapshot" : operation_id;
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

    ClaimedRequest request;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        request = current_claimed_request_locked();
    }

    SpatialSnapshotResult result;
    result.request_id = request.request_id;
    result.operation_id = request.operation_id;
    result.camera_serial = camera_params_ ? camera_params_->camera_serial : "";

    std::string error;
    {
        WorkerEntryRefGuard source_guard(
            recycle_queue_,
            entry,
            release_context,
            true);
        result.ok = copy_entry_to_rgba(*entry, &result, &error);
        if (!result.ok) {
            result.error = error.empty() ? "Full-resolution stream snapshot failed." : error;
        }
    }

    if (result.ok) {
        std::cout << "[SpatialSnapshotWorker] Captured full-resolution stream snapshot"
                  << " cam=" << result.camera_serial
                  << " frame=" << result.local_frame_id
                  << " camera_frame=" << result.camera_frame_id
                  << " size=" << result.width << "x" << result.height
                  << std::endl;
    } else {
        std::cerr << "[SpatialSnapshotWorker] Snapshot failed"
                  << " cam=" << result.camera_serial
                  << " frame=" << result.local_frame_id
                  << " error=" << result.error
                  << std::endl;
    }

    complete_result(std::move(result));
    return true;
}
