#pragma once

#include "json.hpp"
#include "session/spatial_roi_recorder_camera_contract.h"
#include "session/spatial_roi_recorder_contract.h"

#include <string>

namespace orange::spatial_roi::recording {

inline constexpr const char* kSpatialRoiFinalizedSessionReceiptSchemaId =
    "orange.spatial_roi_recording.finalized_session_receipt";
inline constexpr int kSpatialRoiFinalizedSessionReceiptSchemaVersion = 1;

// Inputs are the authorities retained by the producer/supervisor after the
// recorder child has exited.  The builder reparses the plan and contract and
// compares the supplied camera view with a freshly authenticated projection;
// none of the repeated camera-view fields are used as filesystem authority.
struct SpatialRoiFinalizedSessionReceiptRequest final {
    nlohmann::json recorder_contract = nlohmann::json::object();
    nlohmann::json verified_plan = nlohmann::json::object();
    std::string expected_recording_root;
    orange::session::spatial_roi::SpatialRoiRecorderRuntimeGpuMapping
        expected_gpu_mapping;
    orange::session::spatial_roi::SpatialRoiRecorderCameraContractView
        camera_contract;
};

// Reopen the existing recording root using exactly the artifact paths
// authenticated by recorder_contract, then revalidate all four complete
// fixed-region evidence manifests and every referenced artifact.  The result
// is plan ordered and contains twelve exact receipts per stream: the ten
// finalized output artifacts, evidence JSONL, and the evidence manifest file
// itself.  This function never creates or replaces an artifact.
bool build_spatial_roi_finalized_session_receipt(
    const SpatialRoiFinalizedSessionReceiptRequest& request,
    nlohmann::json* receipt_out,
    std::string* error_out = nullptr);

}  // namespace orange::spatial_roi::recording
