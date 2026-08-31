#pragma once

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace orange::spatial_roi {

// This is an Orange-side contract for a spatial ROI handoff. It is deliberately
// separate from external_recorder_ipc_protocol.h: the existing protocol v1 and
// its full-frame/crop clients remain unchanged until a future integration slice
// is ready to carry this descriptor.
inline constexpr const char* kSpatialRoiFrameDescriptorSchemaId =
    "orange.spatial_roi.frame_descriptor";
inline constexpr int kSpatialRoiFrameDescriptorSchemaVersion = 1;
inline constexpr const char* kSpatialRoiFrameMetadataSchemaId =
    "orange.spatial_roi.frame_metadata";
inline constexpr int kSpatialRoiFrameMetadataSchemaVersion = 1;
inline constexpr const char* kSpatialRoiMono8PixelFormat = "mono8";

struct SpatialRoiFrameRaster {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct SpatialRoiFrameRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct SpatialRoiFramePadding {
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;
    std::uint8_t value_mono8 = 0;
};

// The logical stream is part of the frame key. recording_frame_id is only
// unique in the parent camera/session, so it cannot identify one of several
// ROI outputs by itself.
struct SpatialRoiFrameKey {
    std::string logical_stream_id;
    std::uint64_t recording_frame_id = 0;

    bool operator==(const SpatialRoiFrameKey& other) const noexcept
    {
        return logical_stream_id == other.logical_stream_id &&
               recording_frame_id == other.recording_frame_id;
    }
};

struct SpatialRoiFrameKeyHash {
    std::size_t operator()(const SpatialRoiFrameKey& key) const noexcept;
};

// One descriptor is emitted for one ROI output frame. The source identity is
// repeated here rather than reconstructed from a session or stream name so an
// independently routed ROI remains self-identifying after handoff.
struct SpatialRoiFrameDescriptor {
    std::string recording_id;
    std::string recording_identity_token;
    std::string producer_generation;
    int camera_id = -1;
    std::string camera_serial;
    std::uint64_t local_frame_id = 0;
    std::uint64_t camera_frame_id = 0;
    std::uint64_t recording_frame_id = 0;
    std::uint64_t roi_stream_frame_index = 0;
    std::uint64_t camera_timestamp_ns = 0;
    std::uint64_t timestamp_sys_ns = 0;

    std::string roi_id;
    std::string region_id;
    std::string arena_group_id;
    // Empty means the verified plan did not bind an arena id. It is serialized
    // explicitly to keep this schema closed and deterministic.
    std::string arena_id;
    std::string logical_stream_id;
    std::string spatial_roi_plan_sha256;

    SpatialRoiFrameRaster native_raster;
    SpatialRoiFrameRect content_rect;
    SpatialRoiFrameRaster encoded_raster;
    SpatialRoiFrameRect encoded_content_rect;
    SpatialRoiFramePadding padding;

    std::string source_pixel_format = kSpatialRoiMono8PixelFormat;
    std::uint64_t bytes = 0;
    int source_gpu_id = -1;
    int assigned_gpu_id = -1;
    int assigned_shard_id = 0;
    std::string routing_policy = "single_shard";

    SpatialRoiFrameKey key() const
    {
        return {logical_stream_id, recording_frame_id};
    }
};

// Metadata is written for a descriptor admitted to a per-ROI recorder queue.
// output_frame_index advances at queue admission, not when encoding completes;
// this makes a failed encoder distinguishable from an unsubmitted source.
struct SpatialRoiFrameMetadata {
    SpatialRoiFrameDescriptor frame;
    std::string submission_outcome = "accepted";
    std::uint64_t output_frame_index = 0;
    std::uint64_t packet_count = 0;
    std::uint64_t encoded_bytes = 0;
};

bool validate_spatial_roi_frame_descriptor(
    const SpatialRoiFrameDescriptor& descriptor,
    std::string* error_out = nullptr);

bool validate_spatial_roi_frame_metadata(
    const SpatialRoiFrameMetadata& metadata,
    std::string* error_out = nullptr);

nlohmann::json spatial_roi_frame_descriptor_to_json(
    const SpatialRoiFrameDescriptor& descriptor);

bool spatial_roi_frame_descriptor_from_json(
    const nlohmann::json& value,
    SpatialRoiFrameDescriptor* descriptor_out,
    std::string* error_out = nullptr);

nlohmann::json spatial_roi_frame_metadata_to_json(
    const SpatialRoiFrameMetadata& metadata);

bool spatial_roi_frame_metadata_from_json(
    const nlohmann::json& value,
    SpatialRoiFrameMetadata* metadata_out,
    std::string* error_out = nullptr);

// Records admitted frame identities and rejects duplicate stream/frame pairs.
// It is intentionally small and host-only so both the future IPC client and
// finalizer can use the same collision check without depending on CUDA.
class SpatialRoiFrameIdentityRegistry {
public:
    bool note(const SpatialRoiFrameKey& key, std::string* error_out = nullptr);
    bool note(const SpatialRoiFrameDescriptor& descriptor,
              std::string* error_out = nullptr);
    bool contains(const SpatialRoiFrameKey& key) const;
    std::size_t size() const noexcept { return keys_.size(); }
    bool empty() const noexcept { return keys_.empty(); }

private:
    std::unordered_set<SpatialRoiFrameKey, SpatialRoiFrameKeyHash> keys_;
};

}  // namespace orange::spatial_roi
