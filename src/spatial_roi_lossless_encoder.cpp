#include "spatial_roi_lossless_encoder.h"

#include "FFmpegWriter.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"
#include "video_container_finalization.h"
#include "json.hpp"

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace orange::spatial_roi::encoder {
namespace {

constexpr std::size_t kMaxQueueFrames = 4096;
constexpr std::uint32_t kMaxFps = 1000;
constexpr std::uint32_t kInputSlotWaitMs = 5000;
constexpr std::uint32_t kMaxOperationTimeoutMs = 60000;
constexpr std::size_t kMaxWriterQueuePackets = 4096;
constexpr std::size_t kMaxWriterQueueBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxFrameResultFailureReasonBytes = 4096;
constexpr std::size_t kMetadataRowMaxBytes = 256;
constexpr std::size_t kMaxTerminalSidecarBytes = 16ULL * 1024ULL * 1024ULL;

void set_error(std::string* output, const std::string& value)
{
    if (output) {
        *output = value;
    }
}

using ArtifactBundle = SpatialRoiLosslessEncoderArtifactBundle;
using ArtifactFile = recording::SpatialRoiRecorderArtifactFile;
using ArtifactFileAccess = recording::SpatialRoiRecorderArtifactFileAccess;

struct ExpectedArtifactPaths {
    std::string video;
    std::string metadata_csv;
    std::string keyframes_json;
    std::string finalization_json;
};

// Keep this derivation identical to the verified recorder contract's
// expected_artifact_stem(). It is a fixed contract rule, not output pathname
// authority supplied by the encoder caller.
ExpectedArtifactPaths expected_artifact_paths(
    const ipc::SpatialRoiIpcStreamIdentity& stream)
{
    const std::string stem =
        "Cam" + stream.camera_serial + "_spatial_roi_" + stream.roi_id;
    return {stem + ".mp4",
            stem + "_meta.csv",
            stem + "_keyframe.json",
            stem + ".mp4.finalization.json"};
}

bool descriptor_is_read_write_regular_file(const int fd)
{
    if (fd < 0) {
        return false;
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDWR) {
        return false;
    }
    struct stat descriptor_stat {};
    return ::fstat(fd, &descriptor_stat) == 0 &&
           S_ISREG(descriptor_stat.st_mode);
}

bool distinct_inode(const ArtifactFile& lhs, const ArtifactFile& rhs) noexcept
{
    return lhs.identity() != rhs.identity();
}

bool validate_artifact_bundle(const SpatialRoiLosslessEncoderConfig& config,
                              std::string* error_out)
{
    auto reject = [&](const std::string& reason) {
        set_error(error_out,
                  "spatial ROI lossless encoder artifact bundle invalid: " +
                      reason);
        return false;
    };
    const ArtifactBundle& bundle = config.artifacts;
    if (!bundle.artifact_root || !bundle.artifact_root->valid()) {
        return reject("artifact root is missing or invalid");
    }
    const ExpectedArtifactPaths expected = expected_artifact_paths(config.stream);
    if (!bundle.artifact_root->IsAllowed(expected.video) ||
        !bundle.artifact_root->IsAllowed(expected.metadata_csv) ||
        !bundle.artifact_root->IsAllowed(expected.keyframes_json) ||
        !bundle.artifact_root->IsAllowed(expected.finalization_json)) {
        return reject("one or more expected contract artifacts are not authorized");
    }
    const ArtifactFile* files[] = {
        bundle.video.get(), bundle.metadata_csv.get(),
        bundle.keyframes_json.get(), bundle.finalization_json.get()};
    const char* names[] = {"video", "metadata CSV", "keyframes JSON",
                           "finalization JSON"};
    const std::string expected_paths[] = {
        expected.video, expected.metadata_csv, expected.keyframes_json,
        expected.finalization_json};
    for (std::size_t index = 0; index < 4; ++index) {
        if (!files[index] || !files[index]->valid() ||
            !descriptor_is_read_write_regular_file(files[index]->borrowed_fd())) {
            return reject(std::string(names[index]) +
                          " handle must be a valid read-write regular file");
        }
        if (files[index]->access() != ArtifactFileAccess::kReadWrite) {
            return reject(std::string(names[index]) +
                          " handle does not retain read-write authority");
        }
        if (files[index]->artifact_root_identity() !=
            bundle.artifact_root->artifact_root_identity()) {
            return reject(std::string(names[index]) +
                          " handle belongs to a different artifact root identity");
        }
        if (files[index]->relative_path() != expected_paths[index]) {
            return reject(std::string(names[index]) +
                          " handle has the wrong exact contract-relative path");
        }
        std::string binding_error;
        if (!files[index]->VerifyCurrentBinding(&binding_error)) {
            return reject(std::string(names[index]) +
                          " current binding is not authorized" +
                          (binding_error.empty() ? std::string() : ": " + binding_error));
        }
    }
    if (!distinct_inode(*bundle.video, *bundle.metadata_csv) ||
        !distinct_inode(*bundle.video, *bundle.keyframes_json) ||
        !distinct_inode(*bundle.video, *bundle.finalization_json) ||
        !distinct_inode(*bundle.metadata_csv, *bundle.keyframes_json) ||
        !distinct_inode(*bundle.metadata_csv, *bundle.finalization_json) ||
        !distinct_inode(*bundle.keyframes_json, *bundle.finalization_json)) {
        return reject("all four artifact handles must name distinct inodes");
    }
    return true;
}

bool read_fd_bounded(const int fd,
                     const std::size_t max_bytes,
                     std::string* bytes_out,
                     std::string* error_out)
{
    if (!bytes_out || fd < 0) {
        set_error(error_out, "terminal artifact descriptor is unavailable");
        return false;
    }
    struct stat descriptor_stat {};
    if (::fstat(fd, &descriptor_stat) != 0 || descriptor_stat.st_size < 0) {
        set_error(error_out, "terminal artifact descriptor stat failed");
        return false;
    }
    const auto size = static_cast<std::uint64_t>(descriptor_stat.st_size);
    if (size > max_bytes || size > std::numeric_limits<std::size_t>::max()) {
        set_error(error_out, "terminal artifact exceeds bounded read size");
        return false;
    }
    bytes_out->assign(static_cast<std::size_t>(size), '\0');
    std::size_t offset = 0;
    while (offset < bytes_out->size()) {
        const ssize_t count = ::pread(fd,
                                      bytes_out->data() + offset,
                                      bytes_out->size() - offset,
                                      static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            set_error(error_out, "terminal artifact descriptor read failed");
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool same_identity(const ipc::SpatialRoiIpcStreamIdentity& lhs,
                   const ipc::SpatialRoiIpcStreamIdentity& rhs) noexcept
{
    return lhs.recording_id == rhs.recording_id &&
           lhs.recording_identity_token == rhs.recording_identity_token &&
           lhs.producer_generation == rhs.producer_generation &&
           lhs.camera_id == rhs.camera_id &&
           lhs.camera_serial == rhs.camera_serial &&
           lhs.roi_id == rhs.roi_id &&
           lhs.region_id == rhs.region_id &&
           lhs.arena_group_id == rhs.arena_group_id &&
           lhs.arena_id == rhs.arena_id &&
           lhs.logical_stream_id == rhs.logical_stream_id &&
           lhs.spatial_roi_plan_sha256 == rhs.spatial_roi_plan_sha256;
}

bool same_geometry(const ipc::SpatialRoiRecorderCudaDetachGeometry& lhs,
                   const ipc::SpatialRoiRecorderCudaDetachGeometry& rhs) noexcept
{
    return lhs.native_raster.width == rhs.native_raster.width &&
           lhs.native_raster.height == rhs.native_raster.height &&
           lhs.content_rect.x == rhs.content_rect.x &&
           lhs.content_rect.y == rhs.content_rect.y &&
           lhs.content_rect.width == rhs.content_rect.width &&
           lhs.content_rect.height == rhs.content_rect.height &&
           lhs.encoded_raster.width == rhs.encoded_raster.width &&
           lhs.encoded_raster.height == rhs.encoded_raster.height &&
           lhs.encoded_content_rect.x == rhs.encoded_content_rect.x &&
           lhs.encoded_content_rect.y == rhs.encoded_content_rect.y &&
           lhs.encoded_content_rect.width == rhs.encoded_content_rect.width &&
           lhs.encoded_content_rect.height == rhs.encoded_content_rect.height &&
           lhs.padding.left == rhs.padding.left &&
           lhs.padding.top == rhs.padding.top &&
           lhs.padding.right == rhs.padding.right &&
           lhs.padding.bottom == rhs.padding.bottom &&
           lhs.padding.value_mono8 == rhs.padding.value_mono8 &&
           lhs.routing_policy == rhs.routing_policy;
}

std::uint64_t checked_nv12_bytes(const std::uint32_t width,
                                 const std::uint32_t height)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels == 0 || pixels > std::numeric_limits<std::uint64_t>::max() / 3U * 2U) {
        throw std::invalid_argument("spatial ROI encoder raster size overflows");
    }
    return pixels + pixels / 2U;
}

SpatialRoiLosslessFrameMetadata metadata_from_descriptor(
    const SpatialRoiFrameDescriptor& descriptor)
{
    SpatialRoiLosslessFrameMetadata metadata;
    metadata.correlation = ipc::spatial_roi_ipc_correlation_from_descriptor(descriptor);
    metadata.camera_timestamp_ns = descriptor.camera_timestamp_ns;
    metadata.timestamp_sys_ns = descriptor.timestamp_sys_ns;
    metadata.source_gpu_id = descriptor.source_gpu_id;
    metadata.assigned_gpu_id = descriptor.assigned_gpu_id;
    metadata.assigned_shard_id = descriptor.assigned_shard_id;
    metadata.width = descriptor.encoded_raster.width;
    metadata.height = descriptor.encoded_raster.height;
    metadata.byte_length = checked_nv12_bytes(metadata.width, metadata.height);
    metadata.row_pitch_bytes = metadata.width;
    return metadata;
}

std::string cuda_error(const char* operation, const cudaError_t status)
{
    return std::string(operation) + " failed: " + cudaGetErrorString(status);
}

std::string bounded_failure_reason(const std::string& input)
{
    const std::string& source = input.empty() ?
        std::string("spatial ROI encoder operation failed") : input;
    std::string result;
    result.reserve(std::min(source.size(), kMaxFrameResultFailureReasonBytes));
    for (const unsigned char value : source) {
        if (result.size() == kMaxFrameResultFailureReasonBytes) {
            break;
        }
        result.push_back(value >= 0x20U && value != 0x7fU
                             ? static_cast<char>(value)
                             : ' ');
    }
    return result;
}

bool has_unsafe_text(const std::string& value) noexcept
{
    if (value.size() > kMaxFrameResultFailureReasonBytes) {
        return true;
    }
    return std::any_of(value.begin(), value.end(), [](const unsigned char byte) {
        return byte < 0x20U || byte == 0x7fU;
    });
}

bool is_start_code(const std::uint8_t* data,
                   const std::size_t size,
                   std::size_t* length_out) noexcept
{
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        *length_out = 3;
        return true;
    }
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
        data[3] == 1) {
        *length_out = 4;
        return true;
    }
    return false;
}

std::uint32_t read_be32(const std::uint8_t* data) noexcept
{
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

bool packet_has_hevc_idr(const std::uint8_t* data,
                         const std::size_t size) noexcept
{
    if (!data || size == 0) {
        return false;
    }
    bool found_start_code = false;
    for (std::size_t index = 0; index + 3 < size; ++index) {
        std::size_t start_length = 0;
        if (!is_start_code(data + index, size - index, &start_length)) {
            continue;
        }
        found_start_code = true;
        const std::size_t nal_start = index + start_length;
        if (nal_start >= size) {
            break;
        }
        const std::uint8_t nal_type = (data[nal_start] >> 1U) & 0x3fU;
        if (nal_type == 19U || nal_type == 20U) {
            return true;
        }
        index = nal_start;
    }
    if (found_start_code) {
        return false;
    }

    std::size_t offset = 0;
    while (offset + 4 <= size) {
        const std::uint32_t nal_length = read_be32(data + offset);
        offset += 4;
        if (nal_length == 0 || nal_length > size - offset) {
            break;
        }
        const std::uint8_t nal_type = (data[offset] >> 1U) & 0x3fU;
        if (nal_type == 19U || nal_type == 20U) {
            return true;
        }
        offset += nal_length;
    }
    return false;
}

}  // namespace

const char* spatial_roi_lossless_frame_result_status_name(
    const SpatialRoiLosslessFrameResultStatus status) noexcept
{
    switch (status) {
        case SpatialRoiLosslessFrameResultStatus::Encoded:
            return "encoded";
        case SpatialRoiLosslessFrameResultStatus::Failed:
            return "failed";
    }
    return "unknown";
}

bool validate_spatial_roi_lossless_frame_result(
    const SpatialRoiLosslessFrameResult& result,
    std::string* error_out)
{
    auto reject = [&](const std::string& reason) {
        set_error(error_out, "spatial ROI lossless frame result invalid: " + reason);
        return false;
    };
    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = result.correlation.stream.recording_id;
    descriptor.recording_identity_token =
        result.correlation.stream.recording_identity_token;
    descriptor.producer_generation =
        result.correlation.stream.producer_generation;
    descriptor.camera_id = result.correlation.stream.camera_id;
    descriptor.camera_serial = result.correlation.stream.camera_serial;
    descriptor.local_frame_id = result.correlation.local_frame_id;
    descriptor.camera_frame_id = result.correlation.camera_frame_id;
    descriptor.recording_frame_id = result.correlation.recording_frame_id;
    descriptor.roi_stream_frame_index =
        result.correlation.roi_stream_frame_index;
    descriptor.camera_timestamp_ns = result.camera_timestamp_ns;
    descriptor.timestamp_sys_ns = result.timestamp_sys_ns;
    descriptor.roi_id = result.correlation.stream.roi_id;
    descriptor.region_id = result.correlation.stream.region_id;
    descriptor.arena_group_id = result.correlation.stream.arena_group_id;
    descriptor.arena_id = result.correlation.stream.arena_id;
    descriptor.logical_stream_id = result.correlation.stream.logical_stream_id;
    descriptor.spatial_roi_plan_sha256 =
        result.correlation.stream.spatial_roi_plan_sha256;
    descriptor.native_raster = result.geometry.native_raster;
    descriptor.content_rect = result.geometry.content_rect;
    descriptor.encoded_raster = result.geometry.encoded_raster;
    descriptor.encoded_content_rect = result.geometry.encoded_content_rect;
    descriptor.padding = result.geometry.padding;
    descriptor.source_pixel_format = kSpatialRoiMono8PixelFormat;
    descriptor.bytes =
        static_cast<std::uint64_t>(descriptor.encoded_raster.width) *
        descriptor.encoded_raster.height;
    descriptor.source_gpu_id = result.source_gpu_id;
    descriptor.assigned_gpu_id = result.assigned_gpu_id;
    descriptor.assigned_shard_id = result.assigned_shard_id;
    descriptor.routing_policy = result.geometry.routing_policy;
    std::string descriptor_error;
    if (!validate_spatial_roi_frame_descriptor(descriptor, &descriptor_error)) {
        return reject(descriptor_error.empty() ? "descriptor identity is invalid"
                                                : descriptor_error);
    }
    if ((descriptor.encoded_raster.width & 1U) != 0 ||
        (descriptor.encoded_raster.height & 1U) != 0 ||
        descriptor.routing_policy != "single_shard") {
        return reject("encoded geometry is not valid for strict NV12 routing");
    }
    switch (result.status) {
        case SpatialRoiLosslessFrameResultStatus::Encoded:
            if (!result.failure_reason.empty()) {
                return reject("encoded result cannot contain a failure reason");
            }
            if (!result.nvenc_pts_assigned || result.output_frame_index == 0 ||
                result.nvenc_pts == std::numeric_limits<std::uint64_t>::max() ||
                result.output_frame_index != result.nvenc_pts + 1U ||
                result.output_frame_index !=
                    result.correlation.roi_stream_frame_index ||
                result.packet_count != 1 || result.encoded_bytes == 0 ||
                !result.keyframe) {
                return reject(
                    "encoded result requires one keyframe packet and exact one-based output identity");
            }
            break;
        case SpatialRoiLosslessFrameResultStatus::Failed:
            if (result.failure_reason.empty() ||
                has_unsafe_text(result.failure_reason)) {
                return reject("failed result requires a bounded safe reason");
            }
            if (result.output_frame_index != 0 || result.packet_count != 0 ||
                result.encoded_bytes != 0 || result.keyframe) {
                return reject("failed result cannot contain encoded packet evidence");
            }
            if (!result.nvenc_pts_assigned && result.nvenc_pts != 0) {
                return reject(
                    "failed result cannot invent an unassigned internal NVENC PTS");
            }
            break;
        default:
            return reject("status is not recognized");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_lossless_encoder_config(
    const SpatialRoiLosslessEncoderConfig& config,
    std::string* error_out)
{
    auto reject = [&](const std::string& reason) {
        set_error(error_out, "spatial ROI lossless encoder config invalid: " + reason);
        return false;
    };

    std::string identity_error;
    if (!ipc::validate_spatial_roi_ipc_stream_identity(config.stream,
                                                        &identity_error)) {
        return reject(identity_error.empty() ? "stream identity is invalid"
                                              : identity_error);
    }
    if (config.source_gpu_id < 0 || config.recorder_gpu_id < 0 ||
        config.assigned_shard_id < 0) {
        return reject("GPU and assigned shard IDs must be non-negative");
    }
    const auto& geometry = config.geometry;
    if (geometry.native_raster.width == 0 || geometry.native_raster.height == 0 ||
        geometry.encoded_raster.width == 0 || geometry.encoded_raster.height == 0 ||
        (geometry.encoded_raster.width & 1U) != 0 ||
        (geometry.encoded_raster.height & 1U) != 0) {
        return reject("encoded raster must be positive and even for NV12");
    }
    if (geometry.encoded_raster.width >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        geometry.encoded_raster.height >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return reject("encoded raster exceeds the NVENC int dimension bound");
    }
    if (geometry.routing_policy != "single_shard") {
        return reject("routing_policy must be single_shard");
    }
    if (config.codec != "hevc" || config.tuning != "lossless" || !config.lossless ||
        config.gop_length != 1 || config.input_format != "mono8" ||
        config.encoded_format != "nv12" || !config.no_resize ||
        !config.luma_preserved_exactly || config.neutral_chroma_value != 128) {
        return reject("profile must be strict HEVC lossless GOP-1 Mono8-to-NV12");
    }
    if (config.fps == 0 || config.fps > kMaxFps) {
        return reject("fps is outside the bounded positive range");
    }
    std::uint64_t nv12_bytes = 0;
    try {
        nv12_bytes = checked_nv12_bytes(geometry.encoded_raster.width,
                                         geometry.encoded_raster.height);
    } catch (const std::exception& exception) {
        return reject(exception.what());
    }
    if (config.queue_capacity == 0 || config.queue_capacity > kMaxQueueFrames) {
        return reject("queue_capacity is outside the bounded range");
    }
    if (config.max_queue_bytes == 0 ||
        static_cast<std::uint64_t>(config.queue_capacity) >
            std::numeric_limits<std::uint64_t>::max() / nv12_bytes ||
        static_cast<std::uint64_t>(config.queue_capacity) * nv12_bytes >
            config.max_queue_bytes) {
        return reject("max_queue_bytes cannot represent the bounded input queue");
    }
    if (config.writer_queue_max_packets == 0 ||
        config.writer_queue_max_packets > kMaxWriterQueuePackets ||
        config.writer_queue_max_bytes == 0 ||
        config.writer_queue_max_bytes > kMaxWriterQueueBytes) {
        return reject("writer queue bounds are outside the bounded range");
    }
    if (!config.frame_result_callback) {
        return reject("frame_result_callback is required");
    }
    if (config.operation_timeout_ms == 0 ||
        config.operation_timeout_ms > kMaxOperationTimeoutMs) {
        return reject("operation_timeout_ms is outside the bounded range");
    }
    if (config.max_frames_per_stream == 0 ||
        config.max_frames_per_stream >
            orange::session::spatial_roi::kMaxFramesPerStream) {
        return reject(
            "max_frames_per_stream is outside the recorder implementation bound");
    }
    if (config.max_media_bytes_per_stream == 0 ||
        config.max_media_bytes_per_stream >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return reject("max_media_bytes_per_stream must be nonzero and representable by off_t");
    }
    std::uint64_t metadata_max_bytes = 0;
    if (config.max_frames_per_stream >
            (std::numeric_limits<std::uint64_t>::max() - 256U) /
                kMetadataRowMaxBytes ||
        !validate_artifact_bundle(config, error_out)) {
        if (config.max_frames_per_stream >
            (std::numeric_limits<std::uint64_t>::max() - 256U) /
                kMetadataRowMaxBytes) {
            return reject("max_frames_per_stream overflows the metadata byte bound");
        }
        return false;
    }
    metadata_max_bytes = 256U +
                         config.max_frames_per_stream * kMetadataRowMaxBytes;
    (void)metadata_max_bytes;
    // Reuse only the host-side frame contract validator. This checks the
    // recording token, stream naming, geometry, Mono8 byte count, and routing
    // grammar without invoking any legacy external-recorder parser.
    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = config.stream.recording_id;
    descriptor.recording_identity_token = config.stream.recording_identity_token;
    descriptor.producer_generation = config.stream.producer_generation;
    descriptor.camera_id = config.stream.camera_id;
    descriptor.camera_serial = config.stream.camera_serial;
    descriptor.local_frame_id = 1;
    descriptor.camera_frame_id = 1;
    descriptor.recording_frame_id = 1;
    descriptor.roi_stream_frame_index = 1;
    descriptor.camera_timestamp_ns = 1;
    descriptor.timestamp_sys_ns = 1;
    descriptor.roi_id = config.stream.roi_id;
    descriptor.region_id = config.stream.region_id;
    descriptor.arena_group_id = config.stream.arena_group_id;
    descriptor.arena_id = config.stream.arena_id;
    descriptor.logical_stream_id = config.stream.logical_stream_id;
    descriptor.spatial_roi_plan_sha256 = config.stream.spatial_roi_plan_sha256;
    descriptor.native_raster = geometry.native_raster;
    descriptor.content_rect = geometry.content_rect;
    descriptor.encoded_raster = geometry.encoded_raster;
    descriptor.encoded_content_rect = geometry.encoded_content_rect;
    descriptor.padding = geometry.padding;
    descriptor.source_pixel_format = kSpatialRoiMono8PixelFormat;
    descriptor.bytes = static_cast<std::uint64_t>(geometry.encoded_raster.width) *
                       geometry.encoded_raster.height;
    descriptor.source_gpu_id = config.source_gpu_id;
    descriptor.assigned_gpu_id = config.recorder_gpu_id;
    descriptor.assigned_shard_id = config.assigned_shard_id;
    descriptor.routing_policy = geometry.routing_policy;
    std::string descriptor_error;
    if (!validate_spatial_roi_frame_descriptor(descriptor, &descriptor_error)) {
        return reject(descriptor_error.empty() ? "frame contract is invalid"
                                                : descriptor_error);
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

VideoEncodeProfile build_spatial_roi_lossless_encoder_profile(
    const SpatialRoiLosslessEncoderConfig& config)
{
    std::string error;
    if (!validate_spatial_roi_lossless_encoder_config(config, &error)) {
        throw std::invalid_argument(error);
    }
    VideoEncodeProfile profile;
    profile.name = "spatial_roi_hevc_lossless_gop1";
    profile.output_kind = "crop";
    profile.role = "recorder_owned_spatial_roi";
    profile.camera_serial = config.stream.camera_serial;
    profile.codec = config.codec;
    profile.preset = "p7";
    profile.tuning = config.tuning;
    profile.rate_control_mode = "cqp";
    profile.output_mode = "spatial_roi";
    profile.input_format = "nv12";
    profile.source_format = "mono8";
    profile.quality_value = 0;
    profile.requested_gop_length = 1;
    profile.resolved_gop_length = 1;
    profile.width = config.geometry.encoded_raster.width;
    profile.height = config.geometry.encoded_raster.height;
    // The immediate source handed to this core is the recorder's packed ROI
    // raster, not the upstream camera-native frame. Native geometry remains
    // provenance in the detach contract.
    profile.source_width = config.geometry.encoded_raster.width;
    profile.source_height = config.geometry.encoded_raster.height;
    profile.fps = config.fps;
    profile.downsample_factor = 1;
    profile.requested_output_width = static_cast<int>(profile.width);
    profile.requested_output_height = static_cast<int>(profile.height);
    profile.resize_enabled = false;
    profile.color = false;
    profile.source_gpu_id = config.source_gpu_id;
    profile.encode_gpu_id = config.recorder_gpu_id;
    profile.source_pixel_contract = resolve_video_source_pixel_contract(profile);
    return profile;
}

struct SpatialRoiLosslessEncoder::Impl final {
    // This holder is allocated before any frame copy. If source completion
    // becomes uncertain, ownership of the destination encoder and stream is
    // moved here and the holder is intentionally leaked. Destruction after an
    // unresolved copy could free a surface or stream still accessed by CUDA.
    struct UnsafeRuntimeLease {
        std::unique_ptr<NvEncoderCuda> encoder;
        cudaStream_t copy_stream = nullptr;
        CUcontext cu_context = nullptr;
    };

    enum class DrainState {
        NotStarted,
        Drained,
        SkippedUnsafe,
        Failed,
    };

    struct CopyAck {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool success = false;
        bool caller_timed_out = false;
        std::string error;
    };

    struct WorkItem {
        ipc::SpatialRoiRecorderDetachedFrame detached;
        SpatialRoiLosslessDeviceView view;
        std::shared_ptr<CopyAck> copy_ack;
        bool owns_detached = false;
        bool result_emitted = false;
        bool nvenc_pts_assigned = false;
        std::uint64_t nvenc_pts = 0;
    };

    struct AcceptedPacket {
        std::uint64_t nvenc_pts = 0;
        std::uint64_t bytes = 0;
        bool keyframe = false;
    };

    SpatialRoiLosslessEncoderConfig config;
    VideoEncodeProfile profile_value;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable state_cv;
    std::deque<std::unique_ptr<WorkItem>> queue;
    std::uint64_t queued_bytes = 0;
    bool accepting = true;
    bool stop_requested = false;
    bool finalized = false;
    bool finalize_started = false;
    bool worker_done = false;
    bool initialized = false;
    bool init_ok = false;
    bool failed_value = false;
    DrainState drain_state = DrainState::NotStarted;
    bool writer_overflow_seen = false;
    bool writer_thread_started = false;
    bool unsafe_teardown = false;
    bool artifacts_sealed = false;
    std::thread::id owner_thread_id;
    std::string error_value;
    SpatialRoiLosslessEncoderStats stats_value;
    // Public-attempt accounting must remain available even on allocation and
    // exception paths before a work item exists. Keep these three ingress
    // counters allocation-free and independent of the state mutex; they are
    // copied into stats_value at terminal snapshot construction.
    std::atomic<std::uint64_t> enqueue_attempted_count{0};
    std::atomic<std::uint64_t> rejected_count{0};
    std::atomic<std::uint64_t> queue_overflow_count{0};
    bool has_last_enqueued = false;
    std::uint64_t last_recording_frame_id = 0;
    std::uint64_t next_expected_roi_stream_frame_index = 1;
    std::uint64_t next_result_roi_stream_frame_index = 1;
    std::uint64_t encode_index = 0;
    std::int64_t last_mux_pts = -1;
    std::uint64_t next_expected_output_index = 0;
    SpatialRoiLosslessWriterTerminalSnapshot writer_terminal_value;
    std::shared_ptr<const SpatialRoiLosslessEncoderTerminalSnapshot>
        terminal_snapshot_value;
    // A source whose copy completion was not observed must never be released
    // by normal C++ destruction. The active WorkItem is heap-owned before the
    // copy starts; an uncertain item is intentionally leaked until process
    // exit, with no allocation or container push on the unsafe path.

    std::unique_ptr<NvEncoderCuda> encoder;
    std::unique_ptr<FFmpegWriter> writer;
    std::unique_ptr<UnsafeRuntimeLease> unsafe_runtime;
    int metadata_fd = -1;
    std::uint64_t metadata_bytes = 0;
    std::uint64_t metadata_max_bytes = 0;
    cudaStream_t copy_stream = nullptr;
    CUcontext cu_context = nullptr;
    explicit Impl(SpatialRoiLosslessEncoderConfig value)
        : config(std::move(value)),
          profile_value(build_spatial_roi_lossless_encoder_profile(config))
    {
        // Make the unsafe-copy quarantine allocation before the worker can
        // start a CUDA operation. The timeout path must not allocate.
        unsafe_runtime = std::make_unique<UnsafeRuntimeLease>();
    }

    ~Impl() = default;

    void start(const std::shared_ptr<Impl>& self)
    {
        if (!self || self.get() != this) {
            throw std::invalid_argument(
                "spatial ROI lossless encoder worker requires exact self ownership");
        }
        std::thread started;
        try {
            // The worker owns Impl independently of the public wrapper. This
            // is required because the owner-thread result callback is allowed
            // to release the last wrapper owner. A detached std::thread cannot
            // self-join or trigger std::terminate when Impl is finally
            // destroyed at the end of worker_loop.
            started = std::thread([self]() { self->worker_loop(); });
            started.detach();
        } catch (...) {
            if (started.joinable()) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    accepting = false;
                    stop_requested = true;
                }
                cv.notify_all();
                started.join();
            }
            throw;
        }
        std::unique_lock<std::mutex> lock(mutex);
        state_cv.wait(lock, [&]() { return initialized; });
        if (!init_ok) {
            accepting = false;
            stop_requested = true;
            lock.unlock();
            cv.notify_all();
            lock.lock();
            state_cv.wait(lock, [&]() { return worker_done; });
            throw std::runtime_error(error_value.empty()
                                         ? "spatial ROI lossless encoder initialization failed"
                                         : error_value);
        }
    }

    bool on_owner_thread() const noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(mutex);
            return owner_thread_id != std::thread::id{} &&
                   std::this_thread::get_id() == owner_thread_id;
        } catch (...) {
            return false;
        }
    }

    void request_stop_from_owner_destruction() noexcept
    {
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                accepting = false;
                stop_requested = true;
            }
            cv.notify_all();
        } catch (...) {
            // The detached worker's self-reference still prevents a
            // joinable-thread destructor or implementation UAF. Mutex failure
            // is not recoverable, but must not make wrapper destruction throw.
        }
    }

    void initialize_on_owner()
    {
        const cudaError_t set_device_status =
            cudaSetDevice(config.recorder_gpu_id);
        if (set_device_status != cudaSuccess) {
            throw std::runtime_error(cuda_error(
                "cudaSetDevice(spatial ROI lossless encoder)",
                set_device_status));
        }
        CUresult context_status = cuCtxGetCurrent(&cu_context);
        if (context_status != CUDA_SUCCESS || !cu_context) {
            throw std::runtime_error("spatial ROI lossless encoder has no current CUDA context");
        }
        const cudaError_t stream_status =
            cudaStreamCreateWithFlags(&copy_stream, cudaStreamNonBlocking);
        if (stream_status != cudaSuccess) {
            throw std::runtime_error(cuda_error(
                "cudaStreamCreateWithFlags(spatial ROI lossless encoder)",
                stream_status));
        }

        metadata_max_bytes =
            256U + config.max_frames_per_stream * kMetadataRowMaxBytes;
        std::string metadata_descriptor_error;
        if (!config.artifacts.metadata_csv->DuplicateFd(
                &metadata_fd, &metadata_descriptor_error) ||
            metadata_fd < 0) {
            throw std::runtime_error(
                metadata_descriptor_error.empty()
                    ? "spatial ROI lossless encoder could not duplicate metadata descriptor"
                    : metadata_descriptor_error);
        }
        if (::ftruncate(metadata_fd, 0) != 0 || ::lseek(metadata_fd, 0, SEEK_SET) < 0) {
            throw std::runtime_error(
                "spatial ROI lossless encoder could not truncate metadata descriptor");
        }
        const std::string metadata_header =
            "recording_frame_id,roi_stream_frame_index,output_frame_index,"
            "camera_timestamp_ns,timestamp_sys_ns,source_gpu_id,"
            "assigned_gpu_id,assigned_shard_id\n";
        if (!write_bounded_metadata(metadata_header)) {
            throw std::runtime_error(
                "spatial ROI lossless encoder metadata header write failed");
        }

        encoder = std::make_unique<NvEncoderCuda>(
            cu_context,
            profile_value.width,
            profile_value.height,
            NV_ENC_BUFFER_FORMAT_NV12,
            0);
        const VideoEncodeProfileNvencGuids guids =
            resolve_video_encode_profile_nvenc_guids(profile_value);
        const int minimum_width = encoder->GetCapabilityValue(
            guids.codec_guid, NV_ENC_CAPS_WIDTH_MIN);
        const int minimum_height = encoder->GetCapabilityValue(
            guids.codec_guid, NV_ENC_CAPS_HEIGHT_MIN);
        const int maximum_width = encoder->GetCapabilityValue(
            guids.codec_guid, NV_ENC_CAPS_WIDTH_MAX);
        const int maximum_height = encoder->GetCapabilityValue(
            guids.codec_guid, NV_ENC_CAPS_HEIGHT_MAX);
        if (minimum_width <= 0 || minimum_height <= 0 || maximum_width <= 0 ||
            maximum_height <= 0 ||
            profile_value.width < static_cast<std::uint32_t>(minimum_width) ||
            profile_value.height < static_cast<std::uint32_t>(minimum_height) ||
            profile_value.width > static_cast<std::uint32_t>(maximum_width) ||
            profile_value.height > static_cast<std::uint32_t>(maximum_height)) {
            std::ostringstream message;
            message << "spatial ROI lossless encoder raster "
                    << profile_value.width << 'x' << profile_value.height
                    << " is outside this GPU's HEVC NVENC capability range "
                    << minimum_width << 'x' << minimum_height << " through "
                    << maximum_width << 'x' << maximum_height;
            throw std::runtime_error(message.str());
        }
        NV_ENC_INITIALIZE_PARAMS initialize_params = {
            NV_ENC_INITIALIZE_PARAMS_VER};
        NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
        initialize_params.encodeConfig = &encode_config;
        encoder->CreateDefaultEncoderParams(&initialize_params,
                                            guids.codec_guid,
                                            guids.preset_guid,
                                            guids.tuning_info);
        apply_video_encode_profile_to_nvenc_config(
            profile_value, &initialize_params, &encode_config);
        encoder->CreateEncoder(&initialize_params);
        encoder->SetIOCudaStreams(
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&copy_stream),
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&copy_stream));

        const std::size_t writer_packets = config.writer_queue_max_packets;
        const std::size_t writer_bytes = config.writer_queue_max_bytes;
        const std::vector<std::pair<std::string, std::string>> tags =
            [&]() {
                auto result = build_video_encode_metadata_tags(profile_value);
                result.emplace_back("recording_id", config.stream.recording_id);
                result.emplace_back("recording_identity_token",
                                    config.stream.recording_identity_token);
                result.emplace_back("producer_generation",
                                    config.stream.producer_generation);
                result.emplace_back("camera_id",
                                    std::to_string(config.stream.camera_id));
                result.emplace_back("camera_serial", config.stream.camera_serial);
                result.emplace_back("logical_stream_id",
                                    config.stream.logical_stream_id);
                result.emplace_back("roi_id", config.stream.roi_id);
                result.emplace_back("region_id", config.stream.region_id);
                result.emplace_back("arena_group_id", config.stream.arena_group_id);
                if (!config.stream.arena_id.empty()) {
                    result.emplace_back("arena_id", config.stream.arena_id);
                }
                result.emplace_back("spatial_roi_plan_sha256",
                                    config.stream.spatial_roi_plan_sha256);
                result.emplace_back("source_gpu_id",
                                    std::to_string(config.source_gpu_id));
                result.emplace_back("recorder_gpu_id",
                                    std::to_string(config.recorder_gpu_id));
                result.emplace_back("assigned_shard_id",
                                    std::to_string(config.assigned_shard_id));
                return result;
            }();
        FFmpegWriterDescriptorOutputConfig output(
            config.artifacts.video->borrowed_fd(),
            config.artifacts.keyframes_json->borrowed_fd(),
            config.artifacts.finalization_json->borrowed_fd(),
            config.max_media_bytes_per_stream,
            config.artifacts.video->relative_path(),
            config.artifacts.keyframes_json->relative_path(),
            config.artifacts.finalization_json->relative_path());
        writer = std::make_unique<FFmpegWriter>(
            AV_CODEC_ID_HEVC,
            static_cast<int>(profile_value.width),
            static_cast<int>(profile_value.height),
            static_cast<int>(profile_value.fps),
            std::move(output),
            tags,
            FFmpegWriterQueueConfig{writer_packets, writer_bytes});
        writer->create_thread();
        writer_thread_started = true;
    }

    void mark_initialized(bool success, const std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        initialized = true;
        init_ok = success;
        if (!success) {
            failed_value = true;
            accepting = false;
            stop_requested = true;
            error_value = error;
            stats_value.failed = true;
        }
        state_cv.notify_all();
    }

    void latch_failure(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failed_value) {
            failed_value = true;
            error_value = error;
        }
        stats_value.failed = true;
        accepting = false;
        stop_requested = true;
        cv.notify_all();
    }

    void note_enqueue_attempt() noexcept
    {
        enqueue_attempted_count.fetch_add(1, std::memory_order_relaxed);
    }

    bool reject_attempt(std::string* error_out,
                        const std::string& reason,
                        const bool queue_overflow = false) noexcept
    {
        rejected_count.fetch_add(1, std::memory_order_relaxed);
        if (queue_overflow) {
            queue_overflow_count.fetch_add(1, std::memory_order_relaxed);
        }
        try {
            set_error(error_out, reason);
        } catch (...) {
            // Accounting is authoritative even when optional diagnostics
            // cannot allocate.
        }
        return false;
    }

#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
    void inject_test_fault(
        const SpatialRoiLosslessEncoderTestFaultPoint point) const
    {
        if (config.test_fault_injector) {
            config.test_fault_injector(point);
        }
    }
#endif

    void reject_admitted_exception(const std::string& reason) noexcept
    {
        try {
            latch_failure(reason);
        } catch (...) {
            // The attempt remains admitted and must never be reclassified as
            // rejected. The owner thread will still observe its work item.
        }
    }

    void reject_admitted_exception() noexcept
    {
        reject_admitted_exception(
            "spatial ROI encoder failed after frame admission");
    }

    bool reject_unadmitted_exception(std::string* error_out,
                                     const char* fallback) noexcept
    {
        return reject_attempt(error_out, fallback ? fallback :
            "spatial ROI encoder enqueue failed before admission");
    }

    bool reject_unadmitted_exception(std::string* error_out,
                                     const std::exception& exception,
                                     const char* prefix) noexcept
    {
        try {
            return reject_attempt(error_out,
                                  std::string(prefix ? prefix : "") +
                                      exception.what());
        } catch (...) {
            return reject_unadmitted_exception(
                error_out,
                "spatial ROI encoder enqueue failed before admission");
        }
    }

    void set_rejection_error(std::string* error_out,
                             const char* reason) noexcept
    {
        try {
            set_error(error_out, reason ? reason : "encoder rejected frame");
        } catch (...) {
        }
    }

    bool reject_locked(std::string* error_out,
                       const char* reason,
                       const bool queue_overflow = false) noexcept
    {
        // Caller holds mutex, so do not recursively enter reject_attempt().
        rejected_count.fetch_add(1, std::memory_order_relaxed);
        if (queue_overflow) {
            queue_overflow_count.fetch_add(1, std::memory_order_relaxed);
        }
        set_rejection_error(error_out, reason);
        return false;
    }

    bool validate_metadata(const SpatialRoiLosslessFrameMetadata& metadata,
                           std::string* error) const
    {
        std::string correlation_error;
        if (!ipc::validate_spatial_roi_ipc_correlation(metadata.correlation,
                                                        &correlation_error)) {
            set_error(error, correlation_error.empty() ? "frame correlation is invalid"
                                                        : correlation_error);
            return false;
        }
        if (!same_identity(metadata.correlation.stream, config.stream)) {
            set_error(error, "frame stream identity does not match encoder contract");
            return false;
        }
        if (metadata.source_gpu_id != config.source_gpu_id ||
            metadata.assigned_gpu_id != config.recorder_gpu_id ||
            metadata.assigned_shard_id != config.assigned_shard_id) {
            set_error(error, "frame GPU/shard binding does not match encoder contract");
            return false;
        }
        if (metadata.width != profile_value.width ||
            metadata.height != profile_value.height ||
            metadata.row_pitch_bytes != profile_value.width ||
            metadata.byte_length != checked_nv12_bytes(profile_value.width,
                                                       profile_value.height)) {
            set_error(error, "frame NV12 geometry or byte span does not match encoder contract");
            return false;
        }
        if (metadata.camera_timestamp_ns == 0 || metadata.timestamp_sys_ns == 0 ||
            metadata.correlation.local_frame_id == 0 ||
            metadata.correlation.camera_frame_id == 0 ||
            metadata.correlation.recording_frame_id == 0 ||
            metadata.correlation.roi_stream_frame_index == 0) {
            set_error(error, "frame IDs and timestamps must be positive");
            return false;
        }
        return true;
    }

    bool admit_metadata(const SpatialRoiLosslessFrameMetadata& metadata,
                        std::string* error)
    {
        if (!validate_metadata(metadata, error)) {
            return false;
        }
        return true;
    }

    bool write_bounded_metadata(const std::string& bytes)
    {
        if (metadata_fd < 0 || bytes.size() > metadata_max_bytes ||
            metadata_bytes > metadata_max_bytes - bytes.size()) {
            return false;
        }
        struct stat metadata_stat {};
        if (::fstat(metadata_fd, &metadata_stat) != 0 ||
            metadata_stat.st_size < 0 ||
            static_cast<std::uint64_t>(metadata_stat.st_size) != metadata_bytes ||
            metadata_bytes > metadata_max_bytes) {
            return false;
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = ::write(metadata_fd,
                                          bytes.data() + offset,
                                          bytes.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        metadata_bytes += bytes.size();
        return true;
    }

    bool enqueue_item(std::unique_ptr<WorkItem> item, std::string* error) noexcept
    {
        if (!item) {
            return reject_attempt(error, "encoder work item is unavailable");
        }
        const auto& metadata = item->view.metadata;
        try {
#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
            inject_test_fault(
                SpatialRoiLosslessEncoderTestFaultPoint::BeforeQueueInsertion);
#endif
            std::lock_guard<std::mutex> lock(mutex);
            if (std::this_thread::get_id() == owner_thread_id) {
                return reject_locked(
                    error,
                    "reentrant Enqueue from the encoder owner callback is rejected");
            }
            if (!accepting || stop_requested || failed_value || finalized) {
                return reject_locked(error, "encoder is stopping or has failed");
            }
            // This check precedes dense-index consumption below. A frame
            // beyond the authenticated long-run ceiling is rejected while
            // leaving the expected stream index unchanged.
            if (stats_value.enqueued >= config.max_frames_per_stream ||
                metadata.correlation.roi_stream_frame_index >
                    config.max_frames_per_stream) {
                return reject_locked(
                    error, "max_frames_per_stream admission limit reached");
            }
            if (has_last_enqueued &&
                metadata.correlation.recording_frame_id <=
                    last_recording_frame_id) {
                return reject_locked(
                    error, "recording_frame_id must increase strictly");
            }
            if (metadata.correlation.roi_stream_frame_index !=
                next_expected_roi_stream_frame_index) {
                return reject_locked(
                    error,
                    "roi_stream_frame_index must be dense and one-based on admission");
            }
            if (queue.size() >= config.queue_capacity ||
                queued_bytes > config.max_queue_bytes - metadata.byte_length) {
                return reject_locked(
                    error, "spatial ROI encoder input queue is full", true);
            }
            const std::uint64_t item_bytes = metadata.byte_length;
            const std::uint64_t recording_frame_id =
                metadata.correlation.recording_frame_id;
            const std::uint64_t roi_stream_frame_index =
                metadata.correlation.roi_stream_frame_index;
            queue.push_back(std::move(item));
            queued_bytes += item_bytes;
            ++stats_value.enqueued;
            stats_value.peak_queue_depth = std::max<std::uint64_t>(
                stats_value.peak_queue_depth, queue.size());
            has_last_enqueued = true;
            last_recording_frame_id = recording_frame_id;
            next_expected_roi_stream_frame_index =
                roi_stream_frame_index == std::numeric_limits<std::uint64_t>::max()
                    ? 0
                    : roi_stream_frame_index + 1U;
        } catch (const std::exception& exception) {
            return reject_unadmitted_exception(
                error, exception,
                "spatial ROI encoder queue allocation failed: ");
        } catch (...) {
            return reject_unadmitted_exception(
                error, "spatial ROI encoder queue allocation failed");
        }
        cv.notify_one();
        return true;
    }

    static void complete_ack(const std::shared_ptr<CopyAck>& ack,
                             const bool success,
                             const std::string& error) noexcept
    {
        if (!ack) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(ack->mutex);
            if (ack->done) {
                return;
            }
            ack->success = success;
            try {
                ack->error = error;
            } catch (...) {
                // An acknowledgement is still useful as a terminal wakeup if
                // diagnostic allocation fails. The source-safety result is
                // carried by success/done, not by this optional string.
                ack->error.clear();
            }
            ack->done = true;
        }
        ack->cv.notify_all();
    }

    SpatialRoiLosslessFrameResult base_result_for(
        const WorkItem& item) const
    {
        SpatialRoiLosslessFrameResult result;
        result.correlation = item.view.metadata.correlation;
        result.geometry = config.geometry;
        result.camera_timestamp_ns = item.view.metadata.camera_timestamp_ns;
        result.timestamp_sys_ns = item.view.metadata.timestamp_sys_ns;
        result.source_gpu_id = item.view.metadata.source_gpu_id;
        result.assigned_gpu_id = item.view.metadata.assigned_gpu_id;
        result.assigned_shard_id = item.view.metadata.assigned_shard_id;
        result.nvenc_pts_assigned = item.nvenc_pts_assigned;
        result.nvenc_pts = item.nvenc_pts;
        return result;
    }

    SpatialRoiLosslessFrameResult failed_result_for(
        const WorkItem& item,
        const std::string& reason) const
    {
        SpatialRoiLosslessFrameResult result = base_result_for(item);
        result.status = SpatialRoiLosslessFrameResultStatus::Failed;
        result.failure_reason = bounded_failure_reason(reason);
        return result;
    }

    bool emit_result(SpatialRoiLosslessFrameResult result,
                     std::string* delivery_error) noexcept
    {
        std::string validation_error;
        bool result_valid = true;
        try {
            if (!validate_spatial_roi_lossless_frame_result(result,
                                                             &validation_error)) {
                result_valid = false;
                result.status = SpatialRoiLosslessFrameResultStatus::Failed;
                result.output_frame_index = 0;
                result.packet_count = 0;
                result.encoded_bytes = 0;
                result.keyframe = false;
                result.failure_reason = bounded_failure_reason(
                    "internal frame result validation failed: " + validation_error);
            }
        } catch (...) {
            result_valid = false;
            // Preserve the exact correlation and still attempt one terminal
            // callback if diagnostic construction itself runs out of memory.
            result.status = SpatialRoiLosslessFrameResultStatus::Failed;
            result.output_frame_index = 0;
            result.packet_count = 0;
            result.encoded_bytes = 0;
            result.keyframe = false;
            try {
                result.failure_reason = "internal frame result validation failed";
            } catch (...) {
            }
        }

        bool ordering_ok = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ordering_ok = result.correlation.roi_stream_frame_index ==
                          next_result_roi_stream_frame_index;
            if (ordering_ok) {
                next_result_roi_stream_frame_index =
                    next_result_roi_stream_frame_index ==
                            std::numeric_limits<std::uint64_t>::max()
                        ? 0
                        : next_result_roi_stream_frame_index + 1U;
            }
            ++stats_value.frame_results_emitted;
            if (result.status == SpatialRoiLosslessFrameResultStatus::Encoded) {
                ++stats_value.encoded_results;
                ++stats_value.encoded_frames;
            } else {
                ++stats_value.failed_results;
            }
        }

        std::string callback_error;
        bool callback_ok = false;
        if (!ordering_ok) {
            callback_error =
                "frame result order does not match dense admission order";
        } else {
            try {
                callback_ok = config.frame_result_callback(result, &callback_error);
            } catch (const std::exception& exception) {
                try {
                    callback_error =
                        std::string("frame result callback threw: ") + exception.what();
                } catch (...) {
                    callback_error = "frame result callback threw";
                }
            } catch (...) {
                callback_error = "frame result callback threw a non-standard exception";
            }
        }
        if (ordering_ok && callback_ok && result_valid) {
            return true;
        }

        if (!result_valid) {
            callback_error = validation_error.empty()
                ? "internal frame result validation failed"
                : "internal frame result validation failed: " + validation_error;
        }

        callback_error = bounded_failure_reason(
            callback_error.empty() ? "frame result callback rejected completion"
                                   : callback_error);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++stats_value.result_callback_failures;
            failed_value = true;
            stats_value.failed = true;
            accepting = false;
            stop_requested = true;
            if (error_value.empty()) {
                try {
                    error_value = callback_error;
                } catch (...) {
                }
            }
            cv.notify_all();
        }
        try {
            set_error(delivery_error, callback_error);
        } catch (...) {
        }
        return false;
    }

    bool emit_failed_result(WorkItem& item,
                            const std::string& reason,
                            std::string* delivery_error = nullptr) noexcept
    {
        if (item.result_emitted) {
            return true;
        }
        item.result_emitted = true;
        try {
            return emit_result(failed_result_for(item, reason), delivery_error);
        } catch (...) {
            // A completion construction failure is explicit and terminal even
            // when the callback object cannot be invoked.
            std::lock_guard<std::mutex> lock(mutex);
            ++stats_value.result_callback_failures;
            failed_value = true;
            stats_value.failed = true;
            accepting = false;
            stop_requested = true;
            if (error_value.empty()) {
                try {
                    error_value = "frame result construction failed";
                } catch (...) {
                }
            }
            cv.notify_all();
            return false;
        }
    }

    void release_pending_items(std::deque<std::unique_ptr<WorkItem>>& pending,
                               const std::string& reason)
    {
        while (!pending.empty()) {
            std::unique_ptr<WorkItem> item = std::move(pending.front());
            pending.pop_front();
            if (!item) {
                continue;
            }
            if (item->owns_detached) {
                item->detached.Release();
                item->owns_detached = false;
            }
            item->view.lifetime.reset();
            complete_ack(item->copy_ack, false, reason);
            (void)emit_failed_result(*item, reason);
        }
    }

    void quarantine_item(std::unique_ptr<WorkItem>& item,
                         const std::string& reason)
    {
        // Do not destroy this object: its destructor would call
        // SpatialRoiRecorderDetachedFrame::Release after a CUDA copy for
        // which completion was not observed. The owner allocated it before
        // the copy began. Release the owner before any diagnostic/state work;
        // the uncertain path must remain source-safe even if a future mutex
        // implementation reports an exceptional lock failure.
        WorkItem* retained = item.release();
        quarantine_runtime();
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++stats_value.source_quarantines;
            stats_value.source_release_safe = false;
        }
        complete_ack(retained ? retained->copy_ack : nullptr, false, reason);
        if (retained) {
            (void)emit_failed_result(*retained, reason);
        }
    }

    void quarantine_runtime() noexcept
    {
        if (unsafe_teardown) {
            return;
        }
        unsafe_teardown = true;
        if (unsafe_runtime) {
            unsafe_runtime->encoder = std::move(encoder);
            unsafe_runtime->copy_stream = copy_stream;
            unsafe_runtime->cu_context = cu_context;
            copy_stream = nullptr;
            cu_context = nullptr;
            unsafe_runtime.release();
        } else {
            // The holder is allocated in the constructor, but keep this
            // fallback leak-only path allocation-free if construction is ever
            // changed.  Never destroy a destination after uncertain CUDA
            // completion.
            (void)encoder.release();
            copy_stream = nullptr;
            cu_context = nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex);
        ++stats_value.destination_quarantines;
        stats_value.source_release_safe = false;
    }

    bool wait_for_copy_completion(
        const std::chrono::steady_clock::time_point deadline) const
    {
        for (;;) {
            const auto before_query = std::chrono::steady_clock::now();
            if (before_query >= deadline) {
                return false;
            }
            const cudaError_t query_status = cudaStreamQuery(copy_stream);
            const auto after_query = std::chrono::steady_clock::now();
            if (query_status == cudaSuccess) {
                return after_query < deadline;
            }
            if (query_status != cudaErrorNotReady || after_query >= deadline) {
                return false;
            }
            const auto remaining = deadline - after_query;
            const auto sleep_for = std::min(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                std::chrono::milliseconds(1));
            if (sleep_for.count() <= 0) {
                return false;
            }
            std::this_thread::sleep_for(sleep_for);
        }
    }

    void note_writer_state()
    {
        if (!writer || writer_overflow_seen) {
            return;
        }

        const bool queue_overflowed = writer->has_queue_overflowed();
        const bool thread_failed = writer->writer_thread_failed();
        const bool writer_failed = writer->failed();
        if (!queue_overflowed && !thread_failed && !writer_failed) {
            return;
        }

        FFmpegWriterFailureStats failures;
        try {
            failures = writer->failure_stats();
        } catch (...) {
            // The failure latch itself is still authoritative if diagnostic
            // snapshot allocation fails.
            failures.failed = writer_failed;
        }

        std::string reason;
        try {
            if (!failures.last_error.empty()) {
                reason = "FFmpegWriter failed: " + failures.last_error;
            } else if (failures.video_size_limit_failures != 0) {
                reason = "FFmpegWriter media byte limit exceeded";
            } else if (queue_overflowed) {
                reason = "FFmpegWriter packet queue overflowed";
            } else if (thread_failed) {
                reason = "FFmpegWriter thread failed";
            } else {
                reason = "FFmpegWriter failure latch set";
            }
        } catch (...) {
            reason = "FFmpegWriter failure latch set";
        }

        writer_overflow_seen = true;
        std::lock_guard<std::mutex> lock(mutex);
        const std::uint64_t failure_count =
            failures.total_failures == 0 ? 1 : failures.total_failures;
        stats_value.writer_failures += failure_count;
        if (queue_overflowed) {
            const std::uint64_t overflow_count = writer->queue_overflow_events();
            stats_value.writer_queue_overflows +=
                overflow_count == 0 ? 1 : overflow_count;
        }
        failed_value = true;
        stats_value.failed = true;
        if (error_value.empty()) {
            error_value = std::move(reason);
        }
        accepting = false;
        stop_requested = true;
        cv.notify_all();
    }

    void capture_writer_terminal_state() noexcept
    {
        SpatialRoiLosslessWriterTerminalSnapshot snapshot;
        snapshot.observed = writer != nullptr;
        if (!writer) {
            snapshot.failure_latched = true;
            snapshot.first_failure_reason = "FFmpegWriter is unavailable";
            writer_terminal_value = std::move(snapshot);
            return;
        }
        try {
            const FFmpegWriterFailureStats failures = writer->failure_stats();
            snapshot.failure_latched = failures.failed;
            snapshot.packet_allocation_failures =
                failures.packet_allocation_failures;
            snapshot.packet_enqueue_failures = failures.packet_enqueue_failures;
            snapshot.packet_write_failures = failures.packet_write_failures;
            snapshot.muxer_flush_failures = failures.muxer_flush_failures;
            snapshot.sidecar_write_failures = failures.sidecar_write_failures;
            snapshot.video_size_limit_failures =
                failures.video_size_limit_failures;
            snapshot.thread_failures = failures.thread_failures;
            snapshot.total_failures = failures.total_failures;
            snapshot.last_error_code = failures.last_error_code;
            snapshot.first_failure_reason = failures.last_error;
            snapshot.packet_write_error_latched =
                failures.packet_write_failures != 0;
            snapshot.writer_thread_failure_latched =
                writer->writer_thread_failed();
            snapshot.queue_overflow_latched = writer->has_queue_overflowed();
            snapshot.queue_overflow_events = writer->queue_overflow_events();
            snapshot.failure_latched = snapshot.failure_latched ||
                                       snapshot.writer_thread_failure_latched ||
                                       snapshot.queue_overflow_latched ||
                                       snapshot.video_size_limit_failures != 0;
        } catch (...) {
            snapshot.failure_latched = true;
            try {
                snapshot.first_failure_reason =
                    "FFmpegWriter terminal failure snapshot failed";
            } catch (...) {
            }
        }
        try {
            writer_terminal_value = std::move(snapshot);
        } catch (...) {
            writer_terminal_value.observed = true;
            writer_terminal_value.failure_latched = true;
        }
    }

    std::string writer_failure_reason() const
    {
        if (!writer) {
            return "FFmpegWriter is unavailable";
        }
        try {
            const FFmpegWriterFailureStats failures = writer->failure_stats();
            if (!failures.last_error.empty()) {
                return "FFmpegWriter rejected packet: " + failures.last_error;
            }
        } catch (...) {
            // Use the bounded generic fallback below.
        }
        return "FFmpegWriter rejected packet";
    }

    AcceptedPacket push_exact_frame_packet(
        const std::vector<std::vector<std::uint8_t>>& packets,
        const std::vector<std::uint64_t>& timestamps,
        const std::uint64_t expected_nvenc_pts)
    {
        if (!writer) {
            throw std::runtime_error("spatial ROI lossless encoder writer is unavailable");
        }
        if (packets.size() != 1 || timestamps.size() != 1) {
            throw std::runtime_error(
                "strict GOP-1 NVENC encode must return exactly one packet and timestamp per frame");
        }
        note_writer_state();
        if (writer->failed() || writer->has_queue_overflowed() ||
            writer->writer_thread_failed()) {
            throw std::runtime_error(writer_failure_reason());
        }
        if (packets.front().empty()) {
            throw std::runtime_error(
                "NVENC returned an empty packet with a timestamp slot");
        }
        const std::uint64_t output_index = timestamps.front();
        if (output_index != expected_nvenc_pts ||
            output_index != next_expected_output_index ||
            output_index > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error(
                "NVENC returned a non-dense, duplicate, or mismatched frame timestamp");
        }
        const std::int64_t pts = static_cast<std::int64_t>(output_index);
        if (pts <= last_mux_pts) {
            throw std::runtime_error(
                "NVENC returned non-monotonic dense frame timestamps");
        }
        if (packets.front().size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("NVENC packet exceeds FFmpegWriter size bound");
        }
        const bool keyframe = packet_has_hevc_idr(
            packets.front().data(), packets.front().size());
        if (!keyframe) {
            throw std::runtime_error(
                "strict GOP-1 NVENC packet is not an HEVC IDR keyframe");
        }
        const bool accepted = writer->push_packet(
            const_cast<std::uint8_t*>(packets.front().data()),
            static_cast<int>(packets.front().size()),
            pts,
            output_index,
            true);
        if (!accepted || writer->failed() || writer->has_queue_overflowed() ||
            writer->writer_thread_failed()) {
            note_writer_state();
            throw std::runtime_error(writer_failure_reason());
        }
        last_mux_pts = pts;
        ++next_expected_output_index;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++stats_value.encoded_packets;
            stats_value.encoded_bytes += packets.front().size();
        }
        note_writer_state();
        AcceptedPacket result;
        result.nvenc_pts = output_index;
        result.bytes = packets.front().size();
        result.keyframe = keyframe;
        return result;
    }

    void write_frame_mapping(const SpatialRoiLosslessFrameMetadata& metadata,
                             const std::uint64_t output_frame_index)
    {
        std::ostringstream row;
        row << metadata.correlation.recording_frame_id << ','
            << metadata.correlation.roi_stream_frame_index << ','
            << output_frame_index << ','
            << metadata.camera_timestamp_ns << ','
            << metadata.timestamp_sys_ns << ','
            << metadata.source_gpu_id << ','
            << metadata.assigned_gpu_id << ','
            << metadata.assigned_shard_id << '\n';
        if (row.str().size() > kMetadataRowMaxBytes ||
            !write_bounded_metadata(row.str())) {
            throw std::runtime_error(
                "spatial ROI lossless encoder frame metadata write failed");
        }
    }

    void process_item(std::unique_ptr<WorkItem>& item_owner)
    {
        WorkItem& item = *item_owner;
        enum class Stage { Copy, Encode };
        Stage stage = Stage::Copy;
        bool copy_started = false;
        bool copy_completed = false;
        try {
            // This ordering is contractual: never call GetNextInputFrame until
            // the bounded wait has proved that NvEncoderCuda's ring slot is safe.
            const std::uint32_t input_wait_ms = std::min(
                kInputSlotWaitMs, config.operation_timeout_ms);
            if (!encoder->WaitForNextInputFrameAvailable(input_wait_ms)) {
                throw std::runtime_error("NVENC input slot did not become available within the bound");
            }
            const NvEncInputFrame* input_frame = encoder->GetNextInputFrame();
            if (!input_frame || !input_frame->inputPtr) {
                throw std::runtime_error("NvEncoderCuda returned no input surface");
            }
            // From this call until cudaStreamQuery reports success, retain the
            // source regardless of the exception path: the helper may have
            // submitted work before surfacing a CUDA error.  This is one
            // absolute deadline for launch and completion.  A CUDA driver
            // call that never returns cannot be made interruptible here; the
            // process supervisor must apply the outer bound in that case.
            const auto copy_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(
                                           config.operation_timeout_ms);
            copy_started = true;
            NvEncoderCuda::CopyToDeviceFrame(
                cu_context,
                const_cast<unsigned char*>(item.view.device_nv12),
                static_cast<std::uint32_t>(item.view.metadata.row_pitch_bytes),
                reinterpret_cast<CUdeviceptr>(input_frame->inputPtr),
                input_frame->pitch,
                static_cast<int>(profile_value.width),
                static_cast<int>(profile_value.height),
                CU_MEMORYTYPE_DEVICE,
                NV_ENC_BUFFER_FORMAT_NV12,
                input_frame->chromaOffsets,
                input_frame->numChromaPlanes,
                false,
                reinterpret_cast<CUstream>(copy_stream));
            if (!wait_for_copy_completion(copy_deadline)) {
                throw std::runtime_error(
                    "spatial ROI input copy did not complete within the bound");
            }
            copy_completed = true;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++stats_value.copy_completed;
            }
            if (item.owns_detached) {
                item.detached.Release();
                item.owns_detached = false;
            }
            item.view.lifetime.reset();
            {
                std::lock_guard<std::mutex> lock(mutex);
                // Both detached handles and immutable-view lifetime tokens
                // reach their source-safe boundary here; count the admitted
                // source exactly once regardless of transport form.
                ++stats_value.source_releases;
            }
            complete_ack(item.copy_ack, true, {});

            stage = Stage::Encode;
            if (item.copy_ack) {
                std::lock_guard<std::mutex> ack_lock(item.copy_ack->mutex);
                if (item.copy_ack->caller_timed_out) {
                    throw std::runtime_error(
                        "device view caller timed out before encode ownership transfer");
                }
            }

            std::vector<std::vector<std::uint8_t>> packets;
            std::vector<std::uint64_t> timestamps;
            if (encode_index > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    "spatial ROI lossless encoder dense frame index overflowed uint32");
            }
            const std::uint64_t dense_frame_index = encode_index;
            if (dense_frame_index ==
                    std::numeric_limits<std::uint64_t>::max() ||
                item.view.metadata.correlation.roi_stream_frame_index !=
                    dense_frame_index + 1U) {
                throw std::runtime_error(
                    "dense ROI stream identity does not match the internal NVENC timeline");
            }
            item.nvenc_pts_assigned = true;
            item.nvenc_pts = dense_frame_index;
            NV_ENC_PIC_PARAMS picture = {NV_ENC_PIC_PARAMS_VER};
            picture.frameIdx = static_cast<std::uint32_t>(dense_frame_index);
            // NVENC timestamps are the local dense output identity. Source
            // recording IDs are retained by the frame descriptor/evidence and
            // must not produce gaps in the MP4 timeline.
            picture.inputTimeStamp = dense_frame_index;
            picture.inputDuration = 1;
            ++encode_index;
            encoder->EncodeFrame(packets, &picture, nullptr, &timestamps);
            const AcceptedPacket packet = push_exact_frame_packet(
                packets, timestamps, dense_frame_index);

            // The source/output mapping is part of the durable frame result.
            // Never tell the evidence callback that a frame was encoded until
            // its exact mapping row has been accepted by the bounded metadata
            // artifact. If this throws, the common failure path emits exactly
            // one Failed result for the admitted frame.
#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
            inject_test_fault(SpatialRoiLosslessEncoderTestFaultPoint::
                                  BeforeMetadataMappingWrite);
#endif
            write_frame_mapping(item.view.metadata, dense_frame_index + 1U);

            SpatialRoiLosslessFrameResult result = base_result_for(item);
            result.status = SpatialRoiLosslessFrameResultStatus::Encoded;
            result.output_frame_index = dense_frame_index + 1U;
            result.packet_count = 1;
            result.encoded_bytes = packet.bytes;
            result.keyframe = packet.keyframe;
            item.result_emitted = true;
            std::string callback_error;
            const bool callback_ok = emit_result(std::move(result),
                                                 &callback_error);
            if (!callback_ok) {
                throw std::runtime_error(
                    callback_error.empty()
                        ? "frame result callback rejected completion"
                        : callback_error);
            }
        } catch (const std::exception& exception) {
            if (stage == Stage::Copy && copy_started && !copy_completed) {
                quarantine_item(item_owner, exception.what());
            } else {
                if (stage == Stage::Copy && item.owns_detached) {
                    item.detached.Release();
                    item.owns_detached = false;
                }
                if (stage == Stage::Copy) {
                    item.view.lifetime.reset();
                    complete_ack(item.copy_ack, false, exception.what());
                }
                (void)emit_failed_result(item, exception.what());
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stage == Stage::Copy) {
                    ++stats_value.copy_failures;
                } else {
                    ++stats_value.encode_failures;
                }
            }
            throw;
        }
    }

    void finalize_on_owner()
    {
        if (drain_state != DrainState::NotStarted) {
            return;
        }
        // Set the state before entering EndEncode.  If a future owner-thread
        // callback re-enters finalization, it must not submit a second EOS.
        if (unsafe_teardown) {
            drain_state = DrainState::SkippedUnsafe;
            return;
        }
        drain_state = DrainState::Failed;
        try {
            if (encoder && writer) {
                std::vector<std::vector<std::uint8_t>> packets;
                std::vector<std::uint64_t> timestamps;
                encoder->EndEncode(packets, nullptr, &timestamps);
                if (!packets.empty() || !timestamps.empty()) {
                    throw std::runtime_error(
                        "strict GOP-1 NVENC drain returned a delayed packet without a retained frame result");
                }
                if (next_expected_output_index != encode_index) {
                    throw std::runtime_error(
                        "NVENC did not return exactly one packet for every submitted GOP-1 frame");
                }
            }
            drain_state = DrainState::Drained;
        } catch (...) {
            // Do not retry EndEncode after any exception; NvEncoder's shared
            // EOS latch and this owner-local state together make draining
            // idempotent. Normal DestroyEncoder teardown may therefore invoke
            // its internal FlushEncoder without sending a second EOS.
            drain_state = DrainState::Failed;
            throw;
        }
    }

    bool validate_and_seal_terminal_artifacts()
    {
        const ArtifactBundle& bundle = config.artifacts;
        ArtifactFile* files[] = {
            bundle.video.get(), bundle.metadata_csv.get(),
            bundle.keyframes_json.get(), bundle.finalization_json.get()};
        const char* labels[] = {"video", "metadata CSV", "keyframes JSON",
                                "finalization JSON"};
        for (std::size_t index = 0; index < 4; ++index) {
            std::string binding_error;
            if (!files[index] ||
                !files[index]->VerifyCurrentBinding(&binding_error)) {
                latch_failure(std::string("terminal ") + labels[index] +
                              " artifact binding changed" +
                              (binding_error.empty()
                                   ? std::string()
                                   : ": " + binding_error));
                return false;
            }
        }

        std::string keyframe_bytes;
        std::string keyframe_error;
        if (!read_fd_bounded(bundle.keyframes_json->borrowed_fd(),
                             kMaxTerminalSidecarBytes,
                             &keyframe_bytes,
                             &keyframe_error)) {
            latch_failure("terminal keyframe artifact read failed: " +
                          (keyframe_error.empty() ? std::string("unknown")
                                                   : keyframe_error));
            return false;
        }
        std::string finalization_bytes;
        std::string finalization_error;
        if (!read_fd_bounded(bundle.finalization_json->borrowed_fd(),
                             kMaxTerminalSidecarBytes,
                             &finalization_bytes,
                             &finalization_error)) {
            latch_failure("terminal finalization artifact read failed: " +
                          (finalization_error.empty() ? std::string("unknown")
                                                       : finalization_error));
            return false;
        }

        std::string evidence_error;
        try {
            const nlohmann::json keyframes = nlohmann::json::parse(keyframe_bytes);
            if (!keyframes.is_object() || keyframes.size() != 8 ||
                keyframes.value("schema_id", std::string()) !=
                    "orange.spatial_roi_keyframe_summary" ||
                keyframes.value("schema_version", 0) != 1 ||
                !keyframes.value("terminal", false) ||
                keyframes.value("codec", std::string()) != "hevc" ||
                keyframes.value("fps", 0U) != config.fps ||
                keyframes.value("total_frames", std::uint64_t(0)) !=
                    encode_index ||
                !keyframes.contains("frame_index_sequence") ||
                !keyframes.at("frame_index_sequence").is_object() ||
                keyframes.at("frame_index_sequence").size() != 3 ||
                !keyframes.contains("keyframe_policy") ||
                !keyframes.at("keyframe_policy").is_object() ||
                keyframes.at("keyframe_policy").size() != 4) {
                throw std::runtime_error(
                    "terminal keyframe artifact does not prove exact GOP-1 evidence");
            }
            const auto& sequence = keyframes.at("frame_index_sequence");
            const auto& policy = keyframes.at("keyframe_policy");
            if (encode_index == 0 ||
                !sequence.at("first").is_number_integer() ||
                sequence.at("first").get<std::int64_t>() != 0 ||
                !sequence.at("last").is_number_integer() ||
                sequence.at("last").get<std::uint64_t>() != encode_index - 1U ||
                !sequence.at("zero_based_contiguous").is_boolean() ||
                !sequence.at("zero_based_contiguous").get<bool>() ||
                policy.value("name", std::string()) != "all_frames_idr" ||
                policy.value("keyframe_frames", std::uint64_t(0)) !=
                    encode_index ||
                policy.value("non_keyframe_frames", std::uint64_t(1)) != 0 ||
                !policy.at("satisfied").is_boolean() ||
                !policy.at("satisfied").get<bool>()) {
                throw std::runtime_error(
                    "terminal keyframe artifact does not prove a dense all-IDR stream");
            }

            const nlohmann::json finalization =
                nlohmann::json::parse(finalization_bytes);
            if (!finalization.is_object() ||
                finalization.value("schema_id", std::string()) !=
                    "orange.video_container_finalization" ||
                finalization.value("schema_version", 0) != 1 ||
                !finalization.value("terminal", false) ||
                finalization.value("status", std::string()) != "complete") {
                throw std::runtime_error(
                    "terminal finalization artifact schema/status is invalid");
            }
            const nlohmann::json& container = finalization.at("container");
            if (!container.is_object() || !container.value("header_written", false) ||
                !container.value("trailer_written", false) ||
                !container.value("output_closed", false) ||
                !container.value("finalized", false)) {
                throw std::runtime_error(
                    "terminal finalization artifact does not prove a closed container");
            }
            const nlohmann::json& playback_intent =
                finalization.at("quicktime_full_frame_rate_playback_intent");
            if (!playback_intent.is_object() ||
                !playback_intent.value("patch_applied", false)) {
                throw std::runtime_error(
                    "terminal finalization artifact does not prove the playback-intent patch");
            }
        } catch (const std::exception& exception) {
            evidence_error = exception.what();
        } catch (...) {
            evidence_error = "terminal artifact JSON validation failed";
        }
        if (!evidence_error.empty()) {
            writer_terminal_value.close_finalization_failure_latched = true;
            writer_terminal_value.close_finalization_failure_reason =
                bounded_failure_reason(evidence_error);
            latch_failure(evidence_error);
            return false;
        }

        for (std::size_t index = 0; index < 4; ++index) {
            std::string seal_error;
            if (!files[index]->Seal(&seal_error)) {
                latch_failure(std::string("terminal ") + labels[index] +
                              " artifact seal failed" +
                              (seal_error.empty() ? std::string()
                                                  : ": " + seal_error));
                return false;
            }
        }
        artifacts_sealed = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stats_value.artifacts_sealed = true;
            stats_value.media_finalization_validated = true;
        }
        return true;
    }

    void cleanup_on_owner()
    {
        // The core's source-ID mapping is part of the recording contract. It
        // must be flushed before FFmpegWriter's explicit finalization emits
        // terminal media evidence; otherwise media could be reported complete
        // while metadata is still buffered or failed.
        bool metadata_ok = false;
        if (metadata_fd >= 0) {
            metadata_ok = ::fsync(metadata_fd) == 0;
            if (::close(metadata_fd) != 0) {
                metadata_ok = false;
            }
            metadata_fd = -1;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stats_value.metadata_flushed = metadata_ok;
        }
        if (!metadata_ok) {
            latch_failure(
                "spatial ROI lossless encoder frame metadata flush failed");
        }

        if (writer) {
            // FFmpeg mux/close calls are not made interruptible by this core;
            // the process supervisor owns the outer Finalize/join deadline.
            const bool writer_finalized = writer->finalize();
            note_writer_state();
            capture_writer_terminal_state();
            if (!writer_finalized) {
                latch_failure(
                    "FFmpegWriter explicit terminal finalization failed");
            }
            writer.reset();
            writer_thread_started = false;

            // FFmpegWriter writes both terminal sidecars from descriptor
            // duplicates. Validate those exact held inodes, then seal all four
            // contract artifacts before a successful terminal snapshot can be
            // published.
            if (encode_index == 0) {
                latch_failure(
                    "zero-frame spatial ROI stream cannot finalize successfully");
            } else if (validate_and_seal_terminal_artifacts()) {
                std::lock_guard<std::mutex> lock(mutex);
                writer_terminal_value.close_finalization_validated = true;
            }
        }
        if (!unsafe_teardown) {
            if (encoder) {
                try {
                    // DestroyEncoder releases input resources through
                    // FlushEncoder. The NvEncoder EOS latch makes this safe
                    // after our one explicit EndEncode call.
                    encoder->DestroyEncoder();
                } catch (...) {
                    latch_failure("NvEncoderCuda DestroyEncoder failed during finalize");
                }
                encoder.reset();
            }
            if (copy_stream) {
                (void)cudaStreamDestroy(copy_stream);
                copy_stream = nullptr;
            }
        } else {
            // quarantine_runtime() intentionally moved the encoder and stream
            // into a leaked lease. Never call a destructor or cudaStreamDestroy
            // while the destination copy may still be executing.
        }
    }

    void worker_loop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            owner_thread_id = std::this_thread::get_id();
        }
        try {
            initialize_on_owner();
            mark_initialized(true, {});
        } catch (const std::exception& exception) {
            mark_initialized(false, exception.what());
            cleanup_on_owner();
#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
            try {
                inject_test_fault(
                    SpatialRoiLosslessEncoderTestFaultPoint::AfterOwnerCleanup);
            } catch (...) {
            }
#endif
            std::lock_guard<std::mutex> lock(mutex);
            worker_done = true;
            state_cv.notify_all();
            return;
        }

        try {
            for (;;) {
                std::unique_ptr<WorkItem> item;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&]() { return stop_requested || !queue.empty(); });
                    if (queue.empty()) {
                        if (stop_requested) {
                            break;
                        }
                        continue;
                    }
                    if (failed_value) {
                        std::deque<std::unique_ptr<WorkItem>> pending;
                        pending.swap(queue);
                        queued_bytes = 0;
                        lock.unlock();
                        release_pending_items(
                            pending,
                            error_value.empty() ? "encoder failed" : error_value);
                        lock.lock();
                        break;
                    }
                    item = std::move(queue.front());
                    queue.pop_front();
                    queued_bytes -= item->view.metadata.byte_length;
                    ++stats_value.dequeued;
                }
                try {
                    process_item(item);
                } catch (const std::exception& exception) {
                    latch_failure(exception.what());
                    std::deque<std::unique_ptr<WorkItem>> pending;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        pending.swap(queue);
                        queued_bytes = 0;
                    }
                    release_pending_items(pending, exception.what());
                    break;
                }
            }
            finalize_on_owner();
        } catch (const std::exception& exception) {
            latch_failure(exception.what());
        }

        cleanup_on_owner();
#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
        try {
            inject_test_fault(
                SpatialRoiLosslessEncoderTestFaultPoint::AfterOwnerCleanup);
        } catch (...) {
        }
#endif
        std::lock_guard<std::mutex> lock(mutex);
        worker_done = true;
        state_cv.notify_all();
    }

    bool enqueue_detached(ipc::SpatialRoiRecorderDetachedFrame&& detached,
                          const SpatialRoiFrameDescriptor& descriptor,
                          std::string* error) noexcept
    {
        try {
            note_enqueue_attempt();
        } catch (...) {
            return false;
        }
        try {
            if (!detached.valid() || !detached.device_nv12()) {
                return reject_attempt(
                    error, "detached frame has no valid NV12 device view");
            }
            SpatialRoiLosslessFrameMetadata metadata;
            std::string descriptor_error;
            if (!validate_spatial_roi_frame_descriptor(descriptor,
                                                        &descriptor_error)) {
                throw std::invalid_argument(
                    descriptor_error.empty() ? "detached frame descriptor is invalid"
                                             : descriptor_error);
            }
            ipc::SpatialRoiRecorderCudaDetachGeometry descriptor_geometry;
            descriptor_geometry.native_raster = descriptor.native_raster;
            descriptor_geometry.content_rect = descriptor.content_rect;
            descriptor_geometry.encoded_raster = descriptor.encoded_raster;
            descriptor_geometry.encoded_content_rect =
                descriptor.encoded_content_rect;
            descriptor_geometry.padding = descriptor.padding;
            descriptor_geometry.routing_policy = descriptor.routing_policy;
            if (!same_geometry(descriptor_geometry, config.geometry)) {
                throw std::invalid_argument(
                    "detached frame descriptor geometry does not match encoder contract");
            }
            metadata = metadata_from_descriptor(descriptor);
            if (metadata.byte_length != detached.nv12_bytes() ||
                metadata.width != detached.width() ||
                metadata.height != detached.height() ||
                metadata.row_pitch_bytes != detached.row_pitch_bytes()) {
                return reject_attempt(
                    error,
                    "detached frame NV12 view does not match its descriptor");
            }
            std::string correlation_error;
            if (!ipc::spatial_roi_ipc_correlation_matches_descriptor(
                    detached.correlation(), descriptor, &correlation_error)) {
                return reject_attempt(
                    error,
                    correlation_error.empty()
                        ? "detached frame correlation does not match descriptor"
                        : correlation_error);
            }
            if (!admit_metadata(metadata, error)) {
                const std::string reason =
                    error && !error->empty()
                        ? *error
                        : "detached frame metadata is invalid";
                return reject_attempt(error, reason);
            }
#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
            inject_test_fault(SpatialRoiLosslessEncoderTestFaultPoint::
                                  BeforeDetachedWorkItemAllocation);
#endif
            auto item = std::make_unique<WorkItem>();
            item->view.device_nv12 = detached.device_nv12();
            item->view.metadata = metadata;
            item->detached = std::move(detached);
            item->owns_detached = true;
            return enqueue_item(std::move(item), error);
        } catch (const std::exception& exception) {
            return reject_unadmitted_exception(
                error, exception,
                "spatial ROI encoder enqueue failed before admission: ");
        } catch (...) {
            return reject_unadmitted_exception(
                error, "spatial ROI encoder enqueue failed before admission");
        }
    }

    bool enqueue_view(const SpatialRoiLosslessDeviceView& view,
                      std::string* error) noexcept
    {
        try {
            note_enqueue_attempt();
        } catch (...) {
            return false;
        }
        bool admitted = false;
        try {
            if (!view.lifetime) {
                return reject_attempt(
                    error, "immutable device view requires a lifetime token");
            }
            if (!view.device_nv12) {
                return reject_attempt(
                    error, "immutable device view has a null device pointer");
            }
            if (!admit_metadata(view.metadata, error)) {
                const std::string reason =
                    error && !error->empty()
                        ? *error
                        : "device view metadata is invalid";
                return reject_attempt(error, reason);
            }
#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
            inject_test_fault(SpatialRoiLosslessEncoderTestFaultPoint::
                                  BeforeDeviceViewWorkItemAllocation);
#endif
            auto item = std::make_unique<WorkItem>();
            item->view = view;
#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
            inject_test_fault(SpatialRoiLosslessEncoderTestFaultPoint::
                                  BeforeDeviceViewAckAllocation);
#endif
            item->copy_ack = std::make_shared<CopyAck>();
            const std::shared_ptr<CopyAck> ack = item->copy_ack;
            admitted = enqueue_item(std::move(item), error);
            if (!admitted) {
                return false;
            }
            std::unique_lock<std::mutex> lock(ack->mutex);
            if (!ack->cv.wait_for(
                    lock,
                    std::chrono::milliseconds(config.operation_timeout_ms),
                    [&]() { return ack->done; })) {
                ack->caller_timed_out = true;
                lock.unlock();
                {
                    std::lock_guard<std::mutex> stats_lock(mutex);
                    // The caller has not observed the copy acknowledgement.
                    // Keep this conservative even if the owner later observes
                    // completion; Finalize is terminal after this point.
                    stats_value.source_release_safe = false;
                }
                latch_failure(
                    "device view copy acknowledgement exceeded operation timeout");
                set_error(
                    error,
                    "device view copy acknowledgement exceeded operation timeout");
                return false;
            }
            if (!ack->success) {
                set_error(error,
                          ack->error.empty() ? "device view copy failed"
                                             : ack->error);
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            if (!admitted) {
                return reject_unadmitted_exception(
                    error, exception,
                    "spatial ROI encoder enqueue failed before admission: ");
            }
            reject_admitted_exception(exception.what());
            try {
                set_error(error, exception.what());
            } catch (...) {
            }
            return false;
        } catch (...) {
            if (!admitted) {
                return reject_unadmitted_exception(
                    error, "spatial ROI encoder enqueue failed before admission");
            }
            reject_admitted_exception();
            return false;
        }
    }

    bool finalize() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (std::this_thread::get_id() == owner_thread_id) {
            return false;
        }
        if (finalized) {
            return terminal_snapshot_value && terminal_snapshot_value->successful;
        }
        if (!finalize_started) {
            finalize_started = true;
            ++stats_value.finalize_calls;
            accepting = false;
            stop_requested = true;
            lock.unlock();
            cv.notify_all();
            lock.lock();
            state_cv.wait(lock, [&]() { return worker_done; });
            stats_value.enqueue_attempted =
                enqueue_attempted_count.load(std::memory_order_acquire);
            stats_value.rejected =
                rejected_count.load(std::memory_order_acquire);
            stats_value.queue_overflows =
                queue_overflow_count.load(std::memory_order_acquire);
            const bool attempts_accounted =
                stats_value.enqueue_attempted >= stats_value.enqueued &&
                stats_value.enqueue_attempted - stats_value.enqueued ==
                    stats_value.rejected;
            const bool admitted_results_emitted =
                stats_value.frame_results_emitted == stats_value.enqueued &&
                stats_value.encoded_results + stats_value.failed_results ==
                    stats_value.frame_results_emitted;
            const bool successful_frame_cardinality =
                stats_value.enqueued > 0 && attempts_accounted &&
                admitted_results_emitted &&
                stats_value.encoded_frames == stats_value.encoded_results &&
                stats_value.encoded_packets == stats_value.encoded_results &&
                stats_value.rejected == 0 &&
                stats_value.queue_overflows == 0 &&
                stats_value.dequeued == stats_value.enqueued &&
                stats_value.copy_completed == stats_value.enqueued &&
                stats_value.source_releases == stats_value.enqueued &&
                stats_value.failed_results == 0;
            if (!successful_frame_cardinality) {
                failed_value = true;
                stats_value.failed = true;
                if (error_value.empty()) {
                    try {
                        if (stats_value.enqueued == 0) {
                            error_value =
                                "zero-frame spatial ROI stream cannot finalize successfully";
                        } else if (!attempts_accounted) {
                            error_value =
                                "public enqueue attempts are not exactly partitioned into admitted and rejected attempts";
                        } else if (!admitted_results_emitted) {
                            error_value =
                                "admitted frame/result terminal counts do not match";
                        } else {
                            error_value =
                                "successful frame/copy/packet terminal counts do not match";
                        }
                    } catch (...) {
                    }
                }
            }
            const bool writer_clear = writer_terminal_value.observed &&
                                      !writer_terminal_value.failure_latched &&
                                      !writer_terminal_value.packet_write_error_latched &&
                                      writer_terminal_value.close_finalization_validated &&
                                      !writer_terminal_value.close_finalization_failure_latched &&
                                      writer_terminal_value.video_size_limit_failures == 0;
            // Finalization is successful only when the exact writer snapshot,
            // both terminal artifacts, result cardinality, and quarantine
            // state all prove completion.
            stats_value.finalized = !failed_value &&
                                    successful_frame_cardinality &&
                                    drain_state == DrainState::Drained &&
                                    writer_clear &&
                                    stats_value.metadata_flushed &&
                                    stats_value.media_finalization_validated &&
                                    stats_value.artifacts_sealed &&
                                    stats_value.source_release_safe &&
                                    stats_value.source_quarantines == 0 &&
                                    stats_value.destination_quarantines == 0;
            try {
                auto snapshot =
                    std::make_shared<SpatialRoiLosslessEncoderTerminalSnapshot>();
                snapshot->stream = config.stream;
                snapshot->terminal = true;
                snapshot->successful = stats_value.finalized;
                snapshot->drain_completed = drain_state == DrainState::Drained;
                snapshot->metadata_flushed = stats_value.metadata_flushed;
                snapshot->media_finalization_validated =
                    stats_value.media_finalization_validated;
                snapshot->artifacts_sealed = stats_value.artifacts_sealed;
                snapshot->all_enqueue_attempts_accounted = attempts_accounted;
                snapshot->nonempty_stream = stats_value.enqueued > 0;
                snapshot->all_admitted_results_emitted =
                    admitted_results_emitted;
                snapshot->source_release_safe = stats_value.source_release_safe;
                snapshot->source_quarantined =
                    stats_value.source_quarantines != 0;
                snapshot->destination_quarantined =
                    stats_value.destination_quarantines != 0;
                snapshot->terminal_reason = stats_value.finalized
                    ? "complete"
                    : bounded_failure_reason(
                          error_value.empty()
                              ? "spatial ROI lossless encoder finalization failed"
                              : error_value);
                snapshot->counts = stats_value;
                snapshot->writer = writer_terminal_value;
                terminal_snapshot_value = std::move(snapshot);
            } catch (...) {
                failed_value = true;
                stats_value.failed = true;
                stats_value.finalized = false;
                terminal_snapshot_value.reset();
                if (error_value.empty()) {
                    try {
                        error_value = "terminal encoder snapshot allocation failed";
                    } catch (...) {
                    }
                }
            }
            finalized = true;
            const bool success = terminal_snapshot_value &&
                                 terminal_snapshot_value->successful;
            state_cv.notify_all();
            return success;
        }
        state_cv.wait(lock, [&]() { return finalized; });
        return terminal_snapshot_value && terminal_snapshot_value->successful;
    }

    bool valid() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return init_ok && !failed_value;
    }

    bool failed() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return failed_value;
    }

    std::string error() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return error_value;
    }

    SpatialRoiLosslessEncoderStats stats() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        SpatialRoiLosslessEncoderStats snapshot = stats_value;
        snapshot.enqueue_attempted =
            enqueue_attempted_count.load(std::memory_order_acquire);
        snapshot.rejected = rejected_count.load(std::memory_order_acquire);
        snapshot.queue_overflows =
            queue_overflow_count.load(std::memory_order_acquire);
        return snapshot;
    }

    std::shared_ptr<const SpatialRoiLosslessEncoderTerminalSnapshot>
    terminal_snapshot() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return terminal_snapshot_value;
    }
};

SpatialRoiLosslessEncoder::SpatialRoiLosslessEncoder(
    SpatialRoiLosslessEncoderConfig config)
{
    auto implementation = std::make_shared<Impl>(std::move(config));
    implementation->start(implementation);
    impl_ = std::move(implementation);
}

SpatialRoiLosslessEncoder::~SpatialRoiLosslessEncoder()
{
    auto implementation = std::move(impl_);
    if (!implementation) {
        return;
    }
    if (implementation->on_owner_thread()) {
        implementation->request_stop_from_owner_destruction();
        return;
    }
    (void)implementation->finalize();
}

bool SpatialRoiLosslessEncoder::valid() const noexcept
{
    return impl_ && impl_->valid();
}

bool SpatialRoiLosslessEncoder::failed() const noexcept
{
    return !impl_ || impl_->failed();
}

std::string SpatialRoiLosslessEncoder::error() const
{
    return impl_ ? impl_->error() : "encoder implementation is unavailable";
}

const VideoEncodeProfile& SpatialRoiLosslessEncoder::profile() const noexcept
{
    return impl_->profile_value;
}

SpatialRoiLosslessEncoderStats SpatialRoiLosslessEncoder::stats() const noexcept
{
    return impl_ ? impl_->stats() : SpatialRoiLosslessEncoderStats{};
}

std::shared_ptr<const SpatialRoiLosslessEncoderTerminalSnapshot>
SpatialRoiLosslessEncoder::terminal_snapshot() const noexcept
{
    return impl_ ? impl_->terminal_snapshot() : nullptr;
}

bool SpatialRoiLosslessEncoder::Enqueue(
    ipc::SpatialRoiRecorderDetachedFrame&& detached,
    const SpatialRoiFrameDescriptor& descriptor) noexcept
{
    auto implementation = impl_;
    if (!implementation) {
        return false;
    }
    std::string error;
    return implementation->enqueue_detached(
        std::move(detached), descriptor, &error);
}

bool SpatialRoiLosslessEncoder::Enqueue(
    const SpatialRoiLosslessDeviceView& view) noexcept
{
    auto implementation = impl_;
    if (!implementation) {
        return false;
    }
    std::string error;
    return implementation->enqueue_view(view, &error);
}

bool SpatialRoiLosslessEncoder::Finalize() noexcept
{
    auto implementation = impl_;
    if (!implementation) {
        return false;
    }
    try {
        return implementation->finalize();
    } catch (...) {
        return false;
    }
}

}  // namespace orange::spatial_roi::encoder
