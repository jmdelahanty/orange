#pragma once

#include "json.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orange::session::spatial_roi {

inline constexpr const char* kConfigSchemaId =
    "orange.spatial_roi_recording.config";
inline constexpr int kLegacyConfigSchemaVersion = 2;
inline constexpr int kConfigSchemaVersion = 3;
inline constexpr const char* kPlanSchemaId =
    "orange.spatial_roi_recording.plan";
inline constexpr int kLegacyPlanSchemaVersion = 2;
inline constexpr int kPlanSchemaVersion = 3;
inline constexpr const char* kPlanScope =
    "detector_independent_camera_native_spatial_rois";
inline constexpr const char* kCanonicalization =
    "canonical_json_utf8_sort_keys_compact_v1";
inline constexpr const char* kLegacyBackend =
    "independent_lossless_external_ipc";
inline constexpr const char* kBackend =
    "independent_hevc_external_ipc";
inline constexpr const char* kLegacyLosslessEncodeProfileName =
    "hevc_p7_lossless_cqp0_gop1_v1";
inline constexpr const char* kLegacyLowLatencyVbrGop1EncodeProfileName =
    "hevc_p1_low_latency_vbr_q20_gop1_v1";
inline constexpr const char* kLowLatencyVbrEncodeProfileName =
    "hevc_p1_low_latency_vbr_q20_gop25_v1";
inline constexpr const char* kSourceCadence =
    "every_recording_frame";
inline constexpr const char* kSourcePixelFormat = "mono8";

// These are both defaults and current implementation ceilings. The evidence
// stream validator is intentionally bounded to this frame count/file size, so
// config/plan admission must never mint a contract the recorder cannot honor.
inline constexpr std::uint64_t kMaxFramesPerStream = 4000000ULL;
// FFmpeg's descriptor-backed AVIO and the finalization sidecar use signed
// off_t lengths. Do not mint a verified plan whose media ceiling the recorder
// cannot represent on the supported 64-bit Linux runtime.
inline constexpr std::uint64_t kMaxMediaBytesPerStream =
    9223372036854775807ULL;
inline constexpr std::uint64_t kMaxEvidenceBytesPerStream =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultMaxFramesPerStream =
    kMaxFramesPerStream;
inline constexpr std::uint64_t kDefaultMaxMediaBytesPerStream =
    128ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultMaxEvidenceBytesPerStream =
    kMaxEvidenceBytesPerStream;
inline constexpr std::uint64_t kDefaultMaxTotalMediaBytes =
    16ULL * kDefaultMaxMediaBytesPerStream;
inline constexpr std::uint64_t kDefaultMaxTotalEvidenceBytes =
    16ULL * kDefaultMaxEvidenceBytesPerStream;

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
    // Optional/best-effort ROI admission is not defined in schema v2 or v3.
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

// Schema v3 names complete immutable encoder policies rather than allowing
// individual encoder knobs to drift independently. Schema v2 has no wire
// member for this object and is interpreted as the legacy lossless profile.
struct EncodeProfile {
    std::string name = kLowLatencyVbrEncodeProfileName;
    std::string codec = "hevc";
    std::string preset = "p1";
    std::string tuning = "ll";
    bool lossless = false;
    std::string rate_control_mode = "vbr";
    std::uint32_t quality_value = 20;
    std::uint32_t gop_length = 25;
    // Effective encoder controls are part of the immutable profile. They are
    // deliberately concrete values rather than tri-state runtime overrides.
    bool aq = false;
    bool temporal_aq = false;
    bool lookahead = false;
    std::uint32_t lookahead_depth = 0;
};

// Explicit long-run bounds. These are admission ceilings, not a duration or
// an estimate of HEVC compression. Every enabled ROI stream reserves exactly
// these frame/media/evidence budgets before the plan is armed.
struct RecordingLimits {
    std::uint64_t max_frames_per_stream = kDefaultMaxFramesPerStream;
    std::uint64_t max_media_bytes_per_stream =
        kDefaultMaxMediaBytesPerStream;
    std::uint64_t max_evidence_bytes_per_stream =
        kDefaultMaxEvidenceBytesPerStream;
};

struct AdmissionLimits {
    std::uint32_t max_rois_per_camera = 16;
    std::uint32_t max_total_rois = 16;
    std::uint64_t max_total_pixel_rate = 1000000000ULL;
    std::uint32_t max_total_encoder_streams = 16;
    std::uint64_t max_total_pool_bytes = 2147483648ULL;
    std::uint64_t max_total_queue_frames = 1024ULL;
    std::uint64_t max_total_media_bytes = kDefaultMaxTotalMediaBytes;
    std::uint64_t max_total_evidence_bytes = kDefaultMaxTotalEvidenceBytes;
};

struct Config {
    int schema_version = kConfigSchemaVersion;
    bool enabled = false;
    std::string backend = kBackend;
    // Schemas v2/v3 are fail-closed; strict=false has no supported runtime
    // semantics and is rejected by validate_config().
    bool strict = true;
    std::string source_cadence = kSourceCadence;
    std::string source_pixel_format = kSourcePixelFormat;
    bool no_resize = true;
    bool no_color_conversion = true;
    std::uint32_t output_alignment_px = 2;
    std::uint8_t padding_value_mono8 = 0;
    EncodeProfile encode_profile;
    BufferingConfig buffering;
    RecordingLimits recording_limits;
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
    std::uint64_t media_bytes = 0;
    std::uint64_t evidence_bytes = 0;
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
    std::uint32_t source_frame_rate = 0;
    std::string arena_group_id;
    std::vector<SpatialRoiPlanRoiDescriptor> rois;
    std::uint64_t pool_bytes = 0;
};

struct SpatialRoiRecordingPlan {
    int schema_version = 0;
    std::string backend;
    EncodeProfile encode_profile;
    std::string plan_sha256;
    std::string recording_id;
    std::string recording_identity_token;
    std::string producer_generation;
    std::uint32_t pool_frames_per_stream = 0;
    RecordingLimits recording_limits;
    AdmissionUsage admission_usage;
    std::map<std::string, SpatialRoiPlanCameraDescriptor> cameras;
};

// Safe defaults are deliberately disabled. A disabled config may keep valid
// camera/ROI definitions so an operator can preconfigure a rig without arming
// the output.
Config default_config();

// Canonical values for the only two profiles accepted by schema v3. These
// factories also make the profile inferred for schema v2 explicit to native
// callers without adding it to the legacy wire object.
EncodeProfile legacy_lossless_encode_profile();
EncodeProfile legacy_low_latency_vbr_gop1_encode_profile();
EncodeProfile low_latency_vbr_encode_profile();

std::string expected_logical_stream_id(const std::string& camera_serial,
                                       const std::string& roi_id);
std::string expected_artifact_stem(const std::string& camera_serial,
                                   const std::string& roi_id);

// Spatial-ROI sockets live beneath one recording-specific private runtime
// directory. The directory is created and owned by the camera-level
// supervisor with mode 0700; neither the plan nor the listener treats /tmp
// itself as the endpoint authority. Stream leaf names are fixed-length hashes
// because sockaddr_un has a small platform path limit. The complete stream
// identity remains authenticated by IPC-v2 HELLO and is never inferred from
// this opaque leaf.
std::string expected_socket_runtime_directory(
    const std::string& recording_identity_token);
std::string expected_socket_path(
    const std::string& recording_identity_token,
    const std::string& logical_stream_id);

// Parse a closed schema-v2 or schema-v3 object. Unknown fields, unsafe
// identifiers, and inconsistent derived stream/artifact names fail closed.
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
