#pragma once

#include "json.hpp"
#include "session/spatial_roi_recorder_contract.h"
#include "spatial_roi_recorder_artifact_root.h"

#include <cstdint>
#include <functional>
#include <string>

namespace orange::spatial_roi::recording {

// Raw block values are retained in the result so the persisted evidence says
// exactly which fstatvfs observation was converted to byte values.
struct SpatialRoiRecorderFilesystemStats final {
    std::uint64_t block_size_bytes = 0;
    std::uint64_t total_blocks = 0;
    std::uint64_t available_blocks = 0;
};

using SpatialRoiRecorderFilesystemQuery = std::function<bool(
    int artifact_root_fd,
    SpatialRoiRecorderFilesystemStats* stats_out,
    std::string* error_out)>;

struct SpatialRoiRecorderStoragePreflightPolicy final {
    std::string schema_id =
        session::spatial_roi::kSpatialRoiRecorderStoragePreflightPolicySchemaId;
    int schema_version =
        session::spatial_roi::kSpatialRoiRecorderStoragePreflightPolicySchemaVersion;
    bool required = true;
    std::uint64_t reserved_free_bytes =
        session::spatial_roi::kSpatialRoiRecorderReservedFreeBytes;
};

// This is a closed, serializable observation. `passed` is true only when the
// retained artifact-root descriptor was valid, the filesystem query and all
// byte conversions succeeded, and available bytes covered every authenticated
// budget plus the nonzero reserved-free policy.
struct SpatialRoiRecorderStoragePreflightResult final {
    std::string schema_id =
        session::spatial_roi::kSpatialRoiRecorderStoragePreflightSchemaId;
    int schema_version =
        session::spatial_roi::kSpatialRoiRecorderStoragePreflightSchemaVersion;
    bool checked = false;
    bool passed = false;
    std::string status = "not_checked";
    std::string error;
    SpatialRoiRecorderStoragePreflightPolicy policy;
    SpatialRoiRecorderArtifactIdentity artifact_root_identity;
    SpatialRoiRecorderFilesystemStats filesystem;
    std::uint64_t capacity_bytes = 0;
    std::uint64_t available_bytes = 0;
    std::uint64_t max_media_bytes_total = 0;
    std::uint64_t max_evidence_bytes_total = 0;
    std::uint64_t required_bytes = 0;
};

// Query the filesystem containing the exact retained artifact-root fd. The
// path held by SpatialRoiRecorderArtifactRoot is intentionally never used.
// The optional query is a deterministic host-test seam; production callers
// leave it empty and use fstatvfs(2).
bool run_spatial_roi_recorder_storage_preflight(
    const SpatialRoiRecorderArtifactRoot& artifact_root,
    std::uint64_t max_media_bytes_total,
    std::uint64_t max_evidence_bytes_total,
    const SpatialRoiRecorderStoragePreflightPolicy& policy,
    SpatialRoiRecorderStoragePreflightResult* result_out,
    const SpatialRoiRecorderFilesystemQuery& filesystem_query = {},
    std::string* error_out = nullptr);

nlohmann::json spatial_roi_recorder_storage_preflight_to_json(
    const SpatialRoiRecorderStoragePreflightResult& result);

}  // namespace orange::spatial_roi::recording
