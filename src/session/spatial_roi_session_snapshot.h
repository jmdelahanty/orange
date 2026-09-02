#pragma once

#include "json.hpp"
#include "recording_output_descriptor.h"
#include "session/spatial_roi_recorder_camera_contract.h"

#include <cstdint>
#include <string>
#include <vector>

namespace orange::session::spatial_roi {

inline constexpr const char* kSpatialRoiSessionSnapshotSchemaId =
    "orange.spatial_roi_recording.session_snapshot";
// Version 3 retains the complete immutable encoder-profile identity in
// addition to v2's finalized-session receipt gate. Pending/failed snapshots
// retain a JSON null receipt field.
inline constexpr int kSpatialRoiSessionSnapshotSchemaVersion = 3;

// A portable reference to one immutable recording-session authority file.
// relative_path is always relative to the recording root; this type does not
// grant permission to open the referenced file.
struct SpatialRoiSessionArtifactReference final {
    std::string relative_path;
    std::uint64_t size_bytes = 0;
    std::string sha256;
};

// Result of validating an already materialized session.spatial_roi_recording
// object.  The receipt is copied so callers cannot retain a pointer into an
// input JSON document whose lifetime they do not own.
struct SpatialRoiSessionSnapshotValidation final {
    std::string status;
    std::string camera_serial;
    std::vector<std::string> stream_order;
    nlohmann::json finalized_session_receipt = nullptr;
};

// Validate the closed schema-v3 session snapshot, including its identity,
// plan-ordered four-stream ROI list, GPU/authority/artifact shape, and the
// finalized-session receipt gate.  Pending and failed snapshots require a
// JSON-null receipt; complete snapshots require a structurally valid v1
// finalized receipt with four plan-ordered streams and twelve unique safe
// relative artifact receipts per stream.  This parser is intentionally
// portable: receipt identity contains no absolute recording/artifact roots.
// Descriptor-to-receipt coupling remains the responsibility of the v3 output
// updater, which has the descriptor artifact index available.
bool validate_spatial_roi_session_snapshot_json(
    const nlohmann::json& snapshot,
    SpatialRoiSessionSnapshotValidation* validation_out,
    std::string* error_out = nullptr);

// Build the closed value stored at session.spatial_roi_recording. The camera
// view must already have been authenticated against the verified plan and
// runtime GPU mapping. No filesystem access is performed by this builder.
// recorder_process_status and producer_status are optional JSON objects for
// pending/failed snapshots. A complete snapshot must supply the emitted
// headless process/producer status envelopes, including the checked storage
// preflight and stopped, fully submitted producer result.
bool build_spatial_roi_session_snapshot_json(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const SpatialRoiSessionArtifactReference& normalized_config,
    const SpatialRoiSessionArtifactReference& verified_plan,
    const SpatialRoiSessionArtifactReference& recorder_contract,
    const std::string& status,
    const nlohmann::json* recorder_process_status,
    const nlohmann::json* producer_status,
    nlohmann::json* snapshot_out,
    std::string* error_out = nullptr);

// Full builder with the raw finalized-session receipt supplied by the
// recorder-side finalization seam. The builder validates its closed v1 shape
// and identity against camera_contract; it does not open or hash files.
// Complete status requires a non-null, valid receipt. Pending and failed
// statuses reject a supplied receipt and emit finalized_session_receipt=null.
bool build_spatial_roi_session_snapshot_json(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const SpatialRoiSessionArtifactReference& normalized_config,
    const SpatialRoiSessionArtifactReference& verified_plan,
    const SpatialRoiSessionArtifactReference& recorder_contract,
    const std::string& status,
    const nlohmann::json* recorder_process_status,
    const nlohmann::json* producer_status,
    const nlohmann::json* finalized_session_receipt,
    nlohmann::json* snapshot_out,
    std::string* error_out = nullptr);

// Convenience overload for a snapshot without runtime status payloads.
bool build_spatial_roi_session_snapshot_json(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const SpatialRoiSessionArtifactReference& normalized_config,
    const SpatialRoiSessionArtifactReference& verified_plan,
    const SpatialRoiSessionArtifactReference& recorder_contract,
    const std::string& status,
    nlohmann::json* snapshot_out,
    std::string* error_out = nullptr);

}  // namespace orange::session::spatial_roi
