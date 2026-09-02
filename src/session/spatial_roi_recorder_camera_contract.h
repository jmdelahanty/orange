#pragma once

#include "session/spatial_roi_recorder_contract_parser.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orange::session::spatial_roi {

// Gate 2 deliberately introduces a process-level view without changing the
// already authenticated per-stream contract.  The current v4 recorder
// contract's stream_kind/output_kind names are spatial_roi; in this view that
// detector-independent product is classified as fixed_region.  A future
// subject-follow product must not be admitted by this first-slice parser.
inline constexpr const char* kSpatialRoiRecorderCameraContractSchemaId =
    "orange.spatial_roi_recording.camera_contract";
inline constexpr int kSpatialRoiRecorderCameraContractSchemaVersion = 1;
inline constexpr const char* kSpatialRoiRecorderCameraProductKind =
    "fixed_region";

// Exactly one camera and exactly four fixed-region outputs are the first
// camera-level acceptance shape.  The vector is in authenticated
// contract.stream_order, never in connection, JSON-object, or filename order.
struct SpatialRoiRecorderCameraContractView {
    std::string schema_id;
    int schema_version = 0;
    std::string product_kind;
    std::string recording_id;
    std::string session_id;
    std::string recording_identity_token;
    std::string producer_generation;
    std::string spatial_roi_plan_sha256;
    std::string recording_root;
    std::string artifact_root;

    int camera_id = -1;
    std::string camera_serial;
    orange::spatial_roi::SpatialRoiFrameRaster native_raster;
    int analytics_gpu_id = -1;

    std::uint32_t stream_count = 0;
    std::vector<std::string> stream_order;
    SpatialRoiRecorderIpcView ipc_v2;
    SpatialRoiRecorderAggregateBoundsView aggregate_bounds;
    SpatialRoiRecorderStoragePreflightPolicyView storage_preflight_policy;
    std::vector<SpatialRoiRecorderStreamView> streams;

    // These maps are the exact authenticated placement maps from the parent
    // contract.  The first slice contains one analytics entry and four
    // recorder entries, but keeps the complete map available to the process
    // that will construct its four output cores.
    std::map<std::string, int> analytics_gpu_by_camera_serial;
    std::map<std::string, int> recorder_gpu_by_logical_stream_id;
};

// Authenticate and project one camera-level view from a strict v4 recorder
// contract.  Authentication is performed against the independently verified
// plan, expected recording root, and expected runtime GPU mapping by calling
// parse_spatial_roi_recorder_contract(), which rebuilds the deterministic
// contract and compares the complete candidate.  No repeated candidate field
// is used as authority.
//
// This first slice accepts exactly one plan camera and exactly four ROIs.  All
// four streams must be the existing detector-independent spatial_roi output
// shape, which is exposed as product_kind=fixed_region here.  The output is a
// closed, plan-ordered value suitable for constructing one process's vector
// of four output cores; it contains no connection-order state.
bool parse_spatial_roi_recorder_camera_contract(
    const nlohmann::json& candidate_contract,
    const nlohmann::json& independently_verified_plan,
    const std::string& expected_recording_root,
    const SpatialRoiRecorderRuntimeGpuMapping& expected_gpu_mapping,
    SpatialRoiRecorderCameraContractView* view_out,
    std::string* error_out = nullptr);

}  // namespace orange::session::spatial_roi
