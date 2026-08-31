#pragma once

#include "json.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orange::session::spatial_roi {

inline constexpr const char* kConfigSchemaId =
    "orange.spatial_roi_recording.config";
inline constexpr int kConfigSchemaVersion = 1;
inline constexpr const char* kPlanSchemaId =
    "orange.spatial_roi_recording.plan";
inline constexpr int kPlanSchemaVersion = 1;
inline constexpr const char* kPlanScope =
    "detector_independent_camera_native_spatial_rois";
inline constexpr const char* kCanonicalization =
    "canonical_json_utf8_sort_keys_compact_v1";
inline constexpr const char* kBackend =
    "independent_lossless_external_ipc";
inline constexpr const char* kSourceCadence =
    "every_recording_frame";
inline constexpr const char* kSourcePixelFormat = "mono8";

struct Raster {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Rect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ArtifactRef {
    std::string relative_path;
    std::string sha256;
};

struct RoiConfig {
    std::string roi_id;
    std::string region_id;
    bool has_arena_id = false;
    std::string arena_id;
    // Optional/best-effort ROI admission is not defined in schema v1.
    bool required = true;
    Rect content_rect;
    bool has_region_mask = false;
    ArtifactRef region_mask;
    std::string logical_stream_id;
    std::string artifact_stem;
};

struct AuthorityRef {
    std::string id;
    std::string sha256;
};

struct CameraConfig {
    int camera_id = -1;
    std::string camera_serial;
    Raster native_raster;
    std::uint32_t source_frame_rate = 0;
    std::string arena_group_id;
    AuthorityRef layout;
    AuthorityRef materialization;
    AuthorityRef registration;
    bool allow_roi_overlap = false;
    std::vector<RoiConfig> rois;
};

struct BufferingConfig {
    std::uint32_t pool_frames_per_stream = 32;
    std::uint32_t queue_frames_per_stream = 64;
};

struct AdmissionLimits {
    std::uint32_t max_rois_per_camera = 16;
    std::uint32_t max_total_rois = 16;
    std::uint64_t max_total_pixel_rate = 1000000000ULL;
    std::uint32_t max_total_encoder_streams = 16;
    std::uint64_t max_total_pool_bytes = 2147483648ULL;
    std::uint64_t max_total_queue_frames = 1024ULL;
};

struct Config {
    bool enabled = false;
    std::string backend = kBackend;
    // Schema v1 is fail-closed; strict=false has no supported runtime
    // semantics and is rejected by validate_config().
    bool strict = true;
    std::string source_cadence = kSourceCadence;
    std::string source_pixel_format = kSourcePixelFormat;
    bool no_resize = true;
    bool no_color_conversion = true;
    std::uint32_t output_alignment_px = 2;
    std::uint8_t padding_value_mono8 = 0;
    BufferingConfig buffering;
    AdmissionLimits admission;
    std::map<std::string, CameraConfig> cameras;
};

struct AdmissionUsage {
    std::uint32_t camera_count = 0;
    std::uint32_t roi_count = 0;
    std::uint32_t encoder_stream_count = 0;
    std::uint64_t content_pixel_rate = 0;
    std::uint64_t encoded_pixel_rate = 0;
    std::uint64_t pool_bytes = 0;
    std::uint64_t queue_frames = 0;
};

struct PlanContext {
    std::string recording_id;
    std::string recording_identity_token;
    std::string generated_at_utc;
    std::string producer_generation;
};

// The immutable, resolved portion of a verified plan consumed by a spatial
// ROI producer.  These descriptors intentionally retain the order emitted by
// the plan; order is part of the producer allocation contract.
struct SpatialRoiPlanRoiDescriptor {
    std::string roi_id;
    std::string region_id;
    bool has_arena_id = false;
    std::string arena_id;
    std::string arena_group_id;
    std::string logical_stream_id;
    Rect source_rect;
    Raster encoded_raster;
    Rect encoded_content_rect;
    std::uint64_t pool_bytes = 0;
};

struct SpatialRoiPlanCameraDescriptor {
    int camera_id = -1;
    std::string camera_serial;
    Raster native_raster;
    std::string arena_group_id;
    std::vector<SpatialRoiPlanRoiDescriptor> rois;
    std::uint64_t pool_bytes = 0;
};

struct SpatialRoiRecordingPlan {
    std::string plan_sha256;
    std::string recording_id;
    std::string recording_identity_token;
    std::string producer_generation;
    std::uint32_t pool_frames_per_stream = 0;
    AdmissionUsage admission_usage;
    std::map<std::string, SpatialRoiPlanCameraDescriptor> cameras;
};

// Safe defaults are deliberately disabled. A disabled config may keep valid
// camera/ROI definitions so an operator can preconfigure a rig without arming
// the output.
Config default_config();

std::string expected_logical_stream_id(const std::string& camera_serial,
                                       const std::string& roi_id);
std::string expected_artifact_stem(const std::string& camera_serial,
                                   const std::string& roi_id);

// Parse a closed schema-v1 object. Unknown fields, unsafe identifiers, and
// inconsistent derived stream/artifact names fail closed.
bool parse_config(const nlohmann::json& value,
                  Config* config_out,
                  std::string* error_out = nullptr);
nlohmann::json config_to_json(const Config& config);

// Validate geometry and resource admission. encoded_pixel_rate includes
// explicit right/bottom alignment padding and is the value gated by
// max_total_pixel_rate.
bool validate_config(const Config& config,
                     AdmissionUsage* usage_out = nullptr,
                     std::string* error_out = nullptr);
nlohmann::json admission_usage_to_json(const AdmissionUsage& usage);

// Build and verify an immutable digest-bound recording plan. The plan is a
// deterministic function of the normalized config and explicit context.
bool build_plan(const Config& config,
                const PlanContext& context,
                nlohmann::json* plan_out,
                AdmissionUsage* usage_out = nullptr,
                std::string* error_out = nullptr);
bool verify_plan(const nlohmann::json& plan,
                 std::string* error_out = nullptr);

// Verify the envelope, digest, normalized configuration, and resolved
// descriptors before exposing a plan to a runtime consumer.  The result is
// deterministic and contains no fields reconstructed from consumer input.
bool parse_verified_plan(const nlohmann::json& plan,
                         SpatialRoiRecordingPlan* plan_out,
                         std::string* error_out = nullptr);

}  // namespace orange::session::spatial_roi
