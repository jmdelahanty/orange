#pragma once

#include "json.hpp"
#include "session/spatial_roi_recorder_contract.h"
#include "spatial_roi_frame_contract.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace orange::session::spatial_roi {

// This parser is intentionally separate from the legacy
// orange.external_recorder.contract parser.  It accepts only the closed,
// detector-independent spatial ROI external-recorder contract emitted by
// build_spatial_roi_recorder_contract().
struct SpatialRoiRecorderAuthorityView {
    std::string id;
    std::string sha256;
};

struct SpatialRoiRecorderGeometryView {
    SpatialRoiRecorderAuthorityView layout;
    SpatialRoiRecorderAuthorityView materialization;
    SpatialRoiRecorderAuthorityView registration;
    orange::spatial_roi::SpatialRoiFrameRaster native_raster;
    orange::spatial_roi::SpatialRoiFrameRect content_rect;
    orange::spatial_roi::SpatialRoiFrameRaster encoded_raster;
    orange::spatial_roi::SpatialRoiFrameRect encoded_content_rect;
    std::uint32_t content_offset_x = 0;
    std::uint32_t content_offset_y = 0;
    orange::spatial_roi::SpatialRoiFramePadding padding;
    std::string source_coordinate_space;
    std::string video_coordinate_space;
};

struct SpatialRoiRecorderEncodeProfileView {
    std::string profile_id;
    std::string codec;
    std::string preset;
    std::string tuning;
    bool lossless = false;
    std::string rate_control_mode;
    std::uint32_t quality_value = 0;
    std::uint32_t gop_length = 0;
    bool aq = false;
    bool temporal_aq = false;
    bool lookahead = false;
    std::uint32_t lookahead_depth = 0;
    std::uint32_t frame_rate = 0;
    std::string input_format;
    std::string encoded_format;
    bool no_resize = false;
    bool luma_preserved_exactly = false;
    std::uint32_t neutral_chroma_value = 0;
};

struct SpatialRoiRecorderArtifactPathView {
    // The contract currently emits absolute paths. relative_path is derived
    // from artifact_root after verifying that the absolute path is contained
    // by that root and has no unsafe components.
    std::string absolute_path;
    std::string relative_path;
};

struct SpatialRoiRecorderIpcView {
    std::string protocol;
    int version = 0;
    std::vector<std::string> features;
    std::string source_lifetime_mode;
    std::uint32_t queue_capacity_frames_per_stream = 0;
    std::uint32_t max_outstanding_frames_per_stream = 0;
    std::uint32_t max_queue_capacity_frames_per_stream = 0;
    std::uint64_t queue_capacity_frames_total = 0;
    std::uint64_t max_outstanding_frames_total = 0;
};

struct SpatialRoiRecorderStoragePreflightPolicyView {
    std::string schema_id;
    int schema_version = 0;
    bool required = false;
    std::uint64_t reserved_free_bytes = 0;
};

// Checked recorder-side totals over every stream in stream_order.  These
// values make process-level memory admission deterministic without asking the
// caller to re-derive (or silently enlarge) per-stream budgets.
struct SpatialRoiRecorderAggregateBoundsView {
    std::uint64_t max_detach_pool_bytes_total = 0;
    std::uint64_t max_queue_bytes_total = 0;
    std::uint64_t writer_queue_max_packets_total = 0;
    std::uint64_t writer_queue_max_bytes_total = 0;
    std::uint32_t operation_timeout_ms_per_stream = 0;
    std::uint64_t max_media_bytes_total = 0;
    std::uint64_t max_evidence_bytes_total = 0;
};

struct SpatialRoiRecorderStreamView {
    std::string stream_id;
    std::string logical_stream_id;
    std::string stream_kind;
    std::string output_kind;
    int camera_id = -1;
    std::string camera_serial;
    std::string env_key;
    std::string socket_path;
    int analytics_gpu_id = -1;
    int recorder_gpu_id = -1;
    int source_gpu_id = -1;
    int assigned_gpu_id = -1;
    std::string roi_id;
    std::string region_id;
    std::string arena_group_id;
    bool has_arena_id = false;
    std::string arena_id;
    std::string recording_id;
    std::string session_id;
    std::string recording_identity_token;
    std::string producer_generation;
    std::string spatial_roi_plan_sha256;
    std::vector<std::string> frame_identity_key_fields;
    std::string roi_stream_frame_index_mode;
    std::string recording_frame_id_source;
    SpatialRoiRecorderGeometryView geometry;
    SpatialRoiRecorderEncodeProfileView encode_profile;
    std::uint32_t encode_fps = 0;
    std::string codec;
    std::string tuning;
    std::string rate_control_mode;
    std::uint32_t quality_value = 0;
    std::uint32_t gop = 0;
    std::uint32_t encode_queue_depth = 0;
    std::uint32_t detach_pool_frames = 0;
    std::uint64_t max_detach_pool_bytes = 0;
    std::uint64_t max_queue_bytes = 0;
    std::uint64_t writer_queue_max_packets = 0;
    std::uint64_t writer_queue_max_bytes = 0;
    std::uint32_t operation_timeout_ms = 0;
    std::uint64_t max_frames_per_stream = 0;
    std::uint64_t max_media_bytes_per_stream = 0;
    std::uint64_t max_evidence_bytes_per_stream = 0;
    std::string routing_policy;
    std::vector<int> expected_shard_gpu_ids;
    std::map<std::string, SpatialRoiRecorderArtifactPathView> artifacts;
};

struct SpatialRoiRecorderContractView {
    std::string schema_id;
    int schema_version = 0;
    std::string contract_scope;
    bool strict = false;
    std::string backend;
    std::string mode;
    bool supervise_processes = false;
    bool require_summary = false;
    bool require_status = false;
    bool require_video_sanity = false;
    bool require_protocol_hello = false;
    bool require_frame_identity_proof = false;
    bool require_gop_routing = false;
    bool require_storage_preflight = false;
    SpatialRoiRecorderStoragePreflightPolicyView storage_preflight_policy;
    bool preserve_shard_mp4s = false;
    std::string recording_id;
    std::string session_id;
    std::string recording_identity_token;
    std::string producer_generation;
    std::string spatial_roi_plan_sha256;
    std::string recording_root;
    std::string artifact_root;
    std::string source_cadence;
    std::string source_pixel_format;
    std::uint32_t stream_count = 0;
    std::vector<std::string> stream_order;
    SpatialRoiRecorderIpcView ipc_v2;
    SpatialRoiRecorderAggregateBoundsView aggregate_bounds;
    std::map<std::string, int> analytics_gpu_by_camera_serial;
    std::map<std::string, int> recorder_gpu_by_logical_stream_id;
    std::vector<SpatialRoiRecorderStreamView> streams;
    SpatialRoiRecorderStreamView selected_stream;
};

// Verify one strict spatial ROI contract against its independently supplied,
// digest-verified plan, authoritative recording root, and runtime GPU mapping,
// then select exactly one stream. The accepted contract must equal the exact
// deterministic output of build_spatial_roi_recorder_contract(); merely
// repeating a plan digest inside a mutated contract is not sufficient. An
// empty selector is rejected and there is no legacy/fallback parser.
bool parse_spatial_roi_recorder_contract(
    const nlohmann::json& value,
    const nlohmann::json& verified_plan,
    const std::string& expected_recording_root,
    const SpatialRoiRecorderRuntimeGpuMapping& expected_gpu_mapping,
    const std::string& logical_stream_id,
    SpatialRoiRecorderContractView* contract_out,
    std::string* error_out = nullptr);

// Read and verify the same contract from a bounded regular JSON file. The
// contract path itself may not be a symlink. No output/media file is created;
// lexical artifact validation is not authorization to open those paths.
bool parse_spatial_roi_recorder_contract_file(
    const std::filesystem::path& path,
    const nlohmann::json& verified_plan,
    const std::string& expected_recording_root,
    const SpatialRoiRecorderRuntimeGpuMapping& expected_gpu_mapping,
    const std::string& logical_stream_id,
    SpatialRoiRecorderContractView* contract_out,
    std::string* error_out = nullptr);

}  // namespace orange::session::spatial_roi
