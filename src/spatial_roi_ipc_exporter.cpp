#include "spatial_roi_ipc_exporter.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <utility>

namespace orange::spatial_roi::ipc {
namespace {

static_assert(sizeof(cudaIpcMemHandle_t) == kSpatialRoiCudaIpcHandleBytes,
              "CUDA IPC memory handle ABI changed; update the wire contract");
static_assert(sizeof(cudaIpcEventHandle_t) == kSpatialRoiCudaIpcHandleBytes,
              "CUDA IPC event handle ABI changed; update the wire contract");

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

void set_error_noexcept(std::string* error_out, const char* message) noexcept
{
    if (!error_out) {
        return;
    }
    try {
        *error_out = message;
    } catch (...) {
        error_out->clear();
    }
}

std::string cuda_failure(const char* operation, cudaError_t status)
{
    std::ostringstream stream;
    stream << operation << " failed: " << cudaGetErrorString(status);
    return stream.str();
}

bool same_identity(const SpatialRoiFrameIdentity& lhs,
                   const SpatialRoiFrameIdentity& rhs)
{
    return lhs.recording_id == rhs.recording_id &&
           lhs.recording_identity_token == rhs.recording_identity_token &&
           lhs.producer_generation == rhs.producer_generation &&
           lhs.camera_id == rhs.camera_id &&
           lhs.camera_serial == rhs.camera_serial &&
           lhs.local_frame_id == rhs.local_frame_id &&
           lhs.camera_frame_id == rhs.camera_frame_id &&
           lhs.recording_frame_id == rhs.recording_frame_id &&
           lhs.camera_timestamp_ns == rhs.camera_timestamp_ns &&
           lhs.timestamp_sys_ns == rhs.timestamp_sys_ns;
}

bool same_rect(const SpatialRoiRect& lhs, const SpatialRoiRect& rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width &&
           lhs.height == rhs.height;
}

bool same_raster(const SpatialRoiRaster& lhs, const SpatialRoiRaster& rhs)
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool same_geometry(const SpatialRoiOutputGeometry& lhs,
                   const SpatialRoiPlanRoiBinding& rhs)
{
    return same_rect(lhs.content_rect, rhs.source_rect) &&
           same_raster(lhs.encoded_raster, rhs.encoded_raster) &&
           same_rect(lhs.encoded_content_rect, rhs.encoded_content_rect);
}

bool same_work_item(const SpatialRoiWorkItem& lhs,
                    const SpatialRoiWorkItem& rhs)
{
    return lhs.roi_id == rhs.roi_id && lhs.region_id == rhs.region_id &&
           lhs.arena_group_id == rhs.arena_group_id &&
           lhs.arena_id == rhs.arena_id &&
           lhs.logical_stream_id == rhs.logical_stream_id &&
           lhs.spatial_roi_plan_sha256 == rhs.spatial_roi_plan_sha256 &&
           same_identity(lhs.source, rhs.source) &&
           same_rect(lhs.geometry.content_rect, rhs.geometry.content_rect) &&
           same_raster(lhs.geometry.encoded_raster, rhs.geometry.encoded_raster) &&
           same_rect(lhs.geometry.encoded_content_rect,
                     rhs.geometry.encoded_content_rect);
}

bool rect_fits(const SpatialRoiRaster& raster, const SpatialRoiRect& rect)
{
    return rect.width != 0 && rect.height != 0 &&
           static_cast<std::uint64_t>(rect.x) + rect.width <= raster.width &&
           static_cast<std::uint64_t>(rect.y) + rect.height <= raster.height;
}

bool exact_item_against_plan(const SpatialRoiWorkItem& item,
                            const SpatialRoiPlanRoiBinding& expected,
                            const SpatialRoiBatchLimits& limits,
                            std::string* error_out)
{
    if (item.roi_id != expected.roi_id || item.region_id != expected.region_id ||
        item.arena_group_id != expected.arena_group_id ||
        item.arena_id != expected.arena_id ||
        item.logical_stream_id != expected.logical_stream_id ||
        item.spatial_roi_plan_sha256 != limits.expected_spatial_roi_plan_sha256) {
        return fail(error_out,
                    "lane work item identity does not exactly match the verified plan");
    }
    const SpatialRoiFrameIdentity& identity = item.source;
    if (identity.recording_id != limits.expected_recording_id ||
        identity.recording_identity_token !=
            limits.expected_recording_identity_token ||
        identity.producer_generation != limits.expected_producer_generation ||
        identity.camera_id != limits.expected_camera_id ||
        identity.camera_serial != limits.expected_camera_serial) {
        return fail(error_out,
                    "lane source identity does not exactly match the verified plan");
    }
    if (!same_geometry(item.geometry, expected)) {
        return fail(error_out,
                    "lane geometry does not exactly match the verified plan order");
    }
    if (!rect_fits(limits.expected_native_raster, item.geometry.content_rect)) {
        return fail(error_out,
                    "lane content rectangle does not fit the verified native raster");
    }
    if (expected.output_bytes !=
        static_cast<std::size_t>(item.geometry.encoded_raster.width) *
            item.geometry.encoded_raster.height) {
        return fail(error_out,
                    "verified ROI output byte count disagrees with encoded raster");
    }
    return true;
}

std::string handle_to_hex(const void* handle)
{
    static constexpr char kHex[] = "0123456789abcdef";
    const auto* bytes = static_cast<const unsigned char*>(handle);
    std::string encoded;
    encoded.reserve(kSpatialRoiCudaIpcHandleHexBytes);
    for (std::size_t index = 0; index < kSpatialRoiCudaIpcHandleBytes; ++index) {
        const unsigned char byte = bytes[index];
        encoded.push_back(kHex[(byte >> 4U) & 0x0fU]);
        encoded.push_back(kHex[byte & 0x0fU]);
    }
    return encoded;
}

bool checked_pixels(const SpatialRoiRaster& raster, std::uint64_t* out)
{
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(raster.width) * raster.height;
    if (pixels == 0 || pixels > kSpatialRoiIpcMaxPackedMono8Bytes) {
        return false;
    }
    if (out) {
        *out = pixels;
    }
    return true;
}

bool build_export_impl(
    const SpatialRoiIpcFrameExporter& exporter,
    std::size_t lane_index,
    const std::shared_ptr<const SpatialRoiBatchEnvelope>& envelope,
    std::uint64_t roi_stream_frame_index,
    int assigned_recorder_gpu_id,
    int assigned_shard_id,
    SpatialRoiIpcExport* export_out,
    std::string* error_out)
{
    if (!export_out) {
        return fail(error_out, "spatial ROI IPC export destination is null");
    }
    if (!exporter.valid()) {
        return fail(error_out,
                    "spatial ROI IPC exporter was constructed from an invalid plan: " +
                        exporter.error());
    }
    if (roi_stream_frame_index == 0) {
        return fail(error_out,
                    "roi_stream_frame_index must be positive and lane-assigned");
    }
    if (assigned_recorder_gpu_id < 0 || assigned_shard_id < 0) {
        return fail(error_out,
                    "assigned recorder GPU and shard must be non-negative");
    }

    const SpatialRoiBatchLimits& limits = exporter.limits();
    const std::size_t expected_lane_count = limits.expected_roi_descriptors.size();
    if (lane_index >= expected_lane_count) {
        return fail(error_out, "lane_index is outside the verified ROI order");
    }
    if (!envelope) {
        return fail(error_out, "spatial ROI IPC export envelope is null");
    }

    const std::vector<SpatialRoiWorkItem>& work_items = envelope->work_items();
    const SpatialRoiBatchResult& result = envelope->result();
    const std::vector<SpatialRoiOutputView>& outputs = result.outputs();
    if (work_items.size() != expected_lane_count ||
        outputs.size() != expected_lane_count ||
        envelope->required_lane_count() != expected_lane_count) {
        return fail(error_out,
                    "batch envelope lane count does not exactly match the verified plan");
    }
    if (!result.accepted()) {
        return fail(error_out,
                    "cannot export a batch result that was not accepted");
    }
    if (!result.completion_event()) {
        return fail(error_out, "accepted batch has no completion event");
    }

    for (std::size_t index = 0; index < expected_lane_count; ++index) {
        if (!exact_item_against_plan(work_items[index],
                                     limits.expected_roi_descriptors[index],
                                     limits,
                                     error_out)) {
            return false;
        }
        if (index != 0 && !same_identity(work_items[0].source,
                                         work_items[index].source)) {
            return fail(error_out,
                        "batch envelope contains mixed source identities");
        }
        if (!same_work_item(outputs[index].work_item, work_items[index])) {
            return fail(error_out,
                        "batch output identity does not match its lane work item");
        }
    }

    const SpatialRoiOutputView& output = outputs[lane_index];
    if (!output.device_data || output.pitch_bytes == 0 ||
        output.pitch_bytes !=
            static_cast<std::size_t>(output.work_item.geometry.encoded_raster.width)) {
        return fail(error_out,
                    "lane output is not the exact packed encoded raster allocation");
    }

    // The batch producer records one interprocess-capable event after all
    // zero-fill and D2D copies. Querying it here is intentional: exporting a
    // not-ready event would allow the recorder to observe an incomplete ROI.
    cudaError_t status = cudaSetDevice(exporter.producer_gpu_id());
    if (status != cudaSuccess) {
        return fail(error_out, cuda_failure("cudaSetDevice(spatial ROI export)", status));
    }
    status = cudaEventQuery(result.completion_event());
    if (status != cudaSuccess) {
        return fail(error_out,
                    cuda_failure("cudaEventQuery(spatial ROI batch completion)", status));
    }

    cudaIpcMemHandle_t memory_handle{};
    status = cudaIpcGetMemHandle(&memory_handle, output.device_data);
    if (status != cudaSuccess) {
        return fail(error_out,
                    cuda_failure("cudaIpcGetMemHandle(spatial ROI output)", status));
    }
    cudaIpcEventHandle_t event_handle{};
    status = cudaIpcGetEventHandle(&event_handle, result.completion_event());
    if (status != cudaSuccess) {
        return fail(error_out,
                    cuda_failure("cudaIpcGetEventHandle(spatial ROI completion)", status));
    }

    const SpatialRoiWorkItem& item = work_items[lane_index];
    std::uint64_t bytes = 0;
    if (!checked_pixels(item.geometry.encoded_raster, &bytes)) {
        return fail(error_out,
                    "encoded raster exceeds the bounded spatial ROI IPC span");
    }

    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = item.source.recording_id;
    descriptor.recording_identity_token = item.source.recording_identity_token;
    descriptor.producer_generation = item.source.producer_generation;
    descriptor.camera_id = item.source.camera_id;
    descriptor.camera_serial = item.source.camera_serial;
    descriptor.local_frame_id = item.source.local_frame_id;
    descriptor.camera_frame_id = item.source.camera_frame_id;
    descriptor.recording_frame_id = item.source.recording_frame_id;
    descriptor.roi_stream_frame_index = roi_stream_frame_index;
    descriptor.camera_timestamp_ns = item.source.camera_timestamp_ns;
    descriptor.timestamp_sys_ns = item.source.timestamp_sys_ns;
    descriptor.roi_id = item.roi_id;
    descriptor.region_id = item.region_id;
    descriptor.arena_group_id = item.arena_group_id;
    descriptor.arena_id = item.arena_id;
    descriptor.logical_stream_id = item.logical_stream_id;
    descriptor.spatial_roi_plan_sha256 = item.spatial_roi_plan_sha256;
    descriptor.native_raster = {
        limits.expected_native_raster.width,
        limits.expected_native_raster.height};
    descriptor.content_rect = {
        item.geometry.content_rect.x,
        item.geometry.content_rect.y,
        item.geometry.content_rect.width,
        item.geometry.content_rect.height};
    descriptor.encoded_raster = {
        item.geometry.encoded_raster.width,
        item.geometry.encoded_raster.height};
    descriptor.encoded_content_rect = {
        item.geometry.encoded_content_rect.x,
        item.geometry.encoded_content_rect.y,
        item.geometry.encoded_content_rect.width,
        item.geometry.encoded_content_rect.height};

    const std::uint64_t encoded_right =
        static_cast<std::uint64_t>(descriptor.encoded_content_rect.x) +
        descriptor.encoded_content_rect.width;
    const std::uint64_t encoded_bottom =
        static_cast<std::uint64_t>(descriptor.encoded_content_rect.y) +
        descriptor.encoded_content_rect.height;
    if (encoded_right > descriptor.encoded_raster.width ||
        encoded_bottom > descriptor.encoded_raster.height) {
        return fail(error_out,
                    "encoded content rectangle does not fit its plan raster");
    }
    descriptor.padding = {
        descriptor.encoded_content_rect.x,
        descriptor.encoded_content_rect.y,
        static_cast<std::uint32_t>(descriptor.encoded_raster.width - encoded_right),
        static_cast<std::uint32_t>(descriptor.encoded_raster.height - encoded_bottom),
        0};
    descriptor.source_pixel_format = kSpatialRoiMono8PixelFormat;
    descriptor.bytes = bytes;
    descriptor.source_gpu_id = exporter.producer_gpu_id();
    descriptor.assigned_gpu_id = assigned_recorder_gpu_id;
    descriptor.assigned_shard_id = assigned_shard_id;
    descriptor.routing_policy = "single_shard";

    SpatialRoiIpcExport built;
    built.frame.descriptor = std::move(descriptor);
    built.frame.cuda_buffer.memory_handle_hex = handle_to_hex(&memory_handle);
    built.frame.cuda_buffer.ready_event_handle_hex = handle_to_hex(&event_handle);
    built.frame.cuda_buffer.byte_offset = 0;
    built.frame.cuda_buffer.byte_length = bytes;
    built.frame.cuda_buffer.row_pitch_bytes =
        item.geometry.encoded_raster.width;
    built.frame.cuda_buffer.pixel_format = kSpatialRoiMono8PixelFormat;
    built.frame.cuda_buffer.layout = "packed_row_major";
    if (!validate_spatial_roi_ipc_frame(built.frame, error_out)) {
        return false;
    }
    built.envelope = envelope;
    *export_out = std::move(built);
    if (error_out) {
        error_out->clear();
    }
    return true;
}

}  // namespace

SpatialRoiIpcFrameExporter::SpatialRoiIpcFrameExporter(
    const nlohmann::json& verified_plan,
    std::string camera_serial,
    int producer_gpu_id) noexcept
    : camera_serial_(std::move(camera_serial)), producer_gpu_id_(producer_gpu_id)
{
    try {
        if (spatial_roi_batch_limits_from_verified_plan(verified_plan,
                                                        camera_serial_,
                                                        producer_gpu_id_,
                                                        &limits_,
                                                        &error_)) {
            valid_ = true;
            error_.clear();
        } else if (error_.empty()) {
            set_error_noexcept(
                &error_, "verified spatial ROI plan could not be materialized");
        }
    } catch (const std::exception& exception) {
        valid_ = false;
        (void)exception;
        set_error_noexcept(
            &error_, "spatial ROI IPC exporter construction failed");
    } catch (...) {
        valid_ = false;
        set_error_noexcept(
            &error_,
            "spatial ROI IPC exporter construction failed: unknown exception");
    }
}

bool SpatialRoiIpcFrameExporter::Build(
    const SpatialRoiLaneDelivery& delivery,
    int assigned_recorder_gpu_id,
    int assigned_shard_id,
    SpatialRoiIpcExport* export_out,
    std::string* error_out) const noexcept
{
    try {
        if (error_out) {
            error_out->clear();
        }
        return build_export_impl(*this,
                                 delivery.lane_index,
                                 delivery.envelope,
                                 delivery.roi_stream_frame_index,
                                 assigned_recorder_gpu_id,
                                 assigned_shard_id,
                                 export_out,
                                 error_out);
    } catch (const std::exception& exception) {
        (void)exception;
        set_error_noexcept(error_out, "spatial ROI IPC export failed");
        return false;
    } catch (...) {
        set_error_noexcept(
            error_out, "spatial ROI IPC export failed: unknown exception");
        return false;
    }
}

}  // namespace orange::spatial_roi::ipc
